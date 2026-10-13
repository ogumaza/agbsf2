#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run a game's MusicPlayer2000 (MP2K) sound engine under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the engine. It calls the engine's init, song start and
per-frame routines directly (no video, and of the BIOS only the CpuSet, CpuFastSet and Div calls that the engine
makes), and captures what the engine produces:

    trace       every sound channel's state after each frame, one line per channel that plays
    render      the DirectSound mixer's output: 8-bit stereo samples at the engine's rate, or 9-bit ones in Camelot's

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM render SONG FRAMES OUT.wav [--tracks 0,3]

The routine addresses are chosen by the ROM's game code. Those of Pokemon Emerald (BPEE), The Legend of Zelda: A Link
to the Past & Four Swords (AZLE), The Legend of Zelda: The Minish Cap (BZME), Kingdom Hearts: Chain of Memories (B8CE),
Super Robot Taisen: Original Generation 2 (B2RE) and Golden Sun: The Lost Age (AGFE), whose version of the engine
Camelot changed, are built in. Another game needs its own values; see GAMES and docs/mp2k.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, \
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
CAMELOT_SCRATCH = 0x0F100000  # where CamelotEmulator mixes the channels of muted tracks
DMA3_CONTROL = 0x040000DC
SOUND_INFO_PTR = 0x03007FF0  # the engine keeps its SoundInfo's address here

# Offsets and sizes in the engine's structures.
SOUND_INFO_CHANS = 0x50
SOUND_INFO_PCM_BUFFER = 0x350
PCM_DMA_BUF_SIZE = 1584
CHANNEL_SIZE = 0x40
TRACK_SIZE = 0x50


class Addresses:
    """BPEE (Pokemon Emerald)."""
    init = 0x082e0070        # m4aSoundInit
    song_start = 0x082e0130  # m4aSongNumStart: r0 = song
    main = 0x082e0124        # m4aSoundMain, which the VBlank interrupt calls
    vsync = 0x082dfa10       # m4aSoundVSync, which the VCount interrupt calls
    song_table = 0x086b49f0
    player_table = 0x086b49c0


class AddressesAZLE(Addresses):
    """AZLE (The Legend of Zelda: A Link to the Past & Four Swords): MP2K plays Four Swords and the menus."""
    init = 0x08134684
    song_start = 0x08134708
    main = 0x081346fc
    vsync = 0x08134028
    song_table = 0x083c3bbc
    player_table = 0x083c3a3c


class AddressesBZME(Addresses):
    """BZME (The Legend of Zelda: The Minish Cap), with the mono mixer."""
    init = 0x080aff48
    song_start = 0x080affcc
    main = 0x080affc0
    vsync = 0x080b0674
    song_table = 0x08a11dbc
    player_table = 0x08a11c3c


class AddressesB8CE(Addresses):
    """B8CE (Kingdom Hearts: Chain of Memories)."""
    init = 0x0811fdec
    song_start = 0x0811fe70
    main = 0x0811fe64
    vsync = 0x08120558
    song_table = 0x09d6f744
    player_table = 0x09d6f60c


class AddressesB2RE(Addresses):
    """B2RE (Super Robot Taisen: Original Generation 2), with the mono mixer."""
    init = 0x08001ae0
    song_start = 0x08001b64
    main = 0x08001b58
    vsync = 0x080021ec
    song_table = 0x084730cc
    player_table = 0x0847309c


class AddressesAGFE(Addresses):
    """AGFE (Golden Sun: The Lost Age), with Camelot's sequencer and mixer, which CamelotEmulator runs."""
    init = 0x081c0c1c        # the game's sound init: m4aSoundInit, then the mixer's rate
    song_start = 0x081c1fc0
    main = 0x08000630        # Camelot's SoundMain, which the VBlank handler calls
    vsync = 0x080005e8       # Camelot's m4aSoundVSync
    song_table = 0x081c4530
    player_table = 0x081c44d0
    camelot = True
    iwram_copies = ((0x080006b8, 0x03000100, 0x1000), (0x080178b4, 0x030001e4, 0x38))  # the boot code's, to IWRAM
    code_ranges = ((0x08000000, 0x08020000), (0x081c0000, 0x081c4000))  # where calls through LR need fixing
    mixer_code = (0x03000100, 0x03001100)  # the mixer, in IWRAM, which writes instructions into its own loops
    fixed_loop = 0x03000c74  # the last instruction before the fixed-pitch loop, which it has just written
    mix_channel = 0x03000880  # where the mixer starts to mix a DirectSound channel (r4) into its sums (r5), in ARM
    output = 0x03000d40      # the mixer's output stage, which the VBlank handler calls after SoundMain
    output_place = 0x03000d24  # where the output stage keeps the frame's place in the right FIFO's buffer
    dma_counter = 0x03001139  # where the VBlank handler copies the DMA counter for SoundMain


GAMES = {b'BPEE': Addresses, b'AZLE': AddressesAZLE, b'BZME': AddressesBZME, b'B8CE': AddressesB8CE,
         b'B2RE': AddressesB2RE, b'AGFE': AddressesAGFE}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no engine addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class Channel:
    """A DirectSound channel (kind 'd') or one of the four PSG channels (kind 'c'), as the engine leaves it after a
    frame."""

    def __init__(self, raw, kind, index, track):
        self.kind = kind
        self.index = index
        self.track = track
        self.status, self.type, self.right, self.left = raw[0:4]
        self.attack, self.decay, self.sustain, self.release = raw[4:8]
        self.key, self.envelope = raw[8], raw[9]
        self.echo_volume, self.echo_length = raw[12], raw[13]
        self.gate, self.midi_key, self.velocity, self.priority = raw[16:20]
        self.rhythm_pan = struct.unpack('<b', raw[20:21])[0]
        if kind == 'd':
            self.count, self.fraction, self.frequency, self.wav, self.position = struct.unpack('<iIIII', raw[24:44])
        else:
            self.goal, self.counter = raw[10], raw[11]
            self.sustain_goal, self.n4, self.pan, self.pan_mask, self.modify, self.length, self.sweep = raw[25:32]
            self.frequency, self.wav = struct.unpack('<II', raw[32:40])

    def line(self, frame):
        """Returns the channel's line in the trace."""
        common = '%d %s%d %02x %d %d %d %d %d %d %d %d' % (frame, self.kind, self.index, self.status, self.track,
                                                          self.midi_key, self.key, self.velocity, self.priority,
                                                          self.envelope, self.right, self.left)
        if self.kind == 'd':
            offset = self.position - (self.wav + 16)
            return common + ' %d %08x %d %d %d' % (self.frequency, self.wav, self.count, offset, self.fraction)
        return common + ' %d %d %d %d %02x %08x' % (self.frequency, self.goal, self.counter, self.sustain_goal,
                                                  self.pan, self.wav)


class EngineEmulator:
    def __init__(self, rom):
        addr = addresses_for(rom)
        self.addr = addr
        self.rom = rom
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        size = (len(rom) + 0xFFF) & ~0xFFF
        uc.mem_map(ROM_BASE, size)
        uc.mem_write(ROM_BASE, rom)
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_READ, self._io_read, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._swi)
        self.uc = uc
        self.vcount = 0
        self.fixed_vcount = None
        self.muted = set()
        self.call(addr.init)
        self.info = self.u32(SOUND_INFO_PTR)
        # SoundMain runs the sequencer, then the PSG routine, then the mixer, so muted tracks are silenced at the start
        # of the PSG routine.
        psg_routine = self.u32(self.info + 40) & ~1
        uc.hook_add(UC_HOOK_CODE, self._mute, begin=psg_routine, end=psg_routine)
        # The mono mixer's init sends FIFO A to both sides and leaves FIFO B out of the sound control register.
        self.mono = not self.u16(0x04000082) & 0x3000
        self.players = []
        for i in range(64):
            entry = addr.player_table + 12 * i
            player, tracks, count = self.u32(entry), self.u32(entry + 4), rom[entry + 8 - ROM_BASE]
            if not 0x02000000 <= player < 0x03008000:
                break
            self.players.append((player, tracks, count))

    def _swi(self, uc, intno, user):
        # Unicorn calls this after the SWI instruction, without taking the exception. The engine's init copies its
        # mixer to IWRAM and clears its structures with CpuSet, and some games' compilers divide with Div.
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        number = self.u16(pc - 2) & 0xFF if thumb else (self.u32(pc - 4) >> 16) & 0xFF
        src, dst, control = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if number == 0x06:
            # r0 / r1, rounded toward zero, with the remainder in r1 and the quotient's magnitude in r3.
            numerator = src - (1 << 32) if src & 0x80000000 else src
            denominator = dst - (1 << 32) if dst & 0x80000000 else dst
            quotient = abs(numerator) // abs(denominator) * (-1 if (numerator < 0) != (denominator < 0) else 1)
            uc.reg_write(UC_ARM_REG_R0, quotient & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R1, (numerator - quotient * denominator) & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R3, abs(quotient) & 0xFFFFFFFF)
            return
        if number == 0x0B:
            unit = 4 if control & (1 << 26) else 2
            count = control & 0x1FFFFF
        elif number == 0x0C:
            unit = 4
            count = ((control & 0x1FFFFF) + 7) & ~7
        else:
            raise SystemExit('the engine made BIOS call %02x' % number)
        fill = control & (1 << 24)
        value = bytes(uc.mem_read(src, unit))
        for i in range(count):
            uc.mem_write(dst + i * unit, value if fill else bytes(uc.mem_read(src + i * unit, unit)))

    def _unmapped(self, uc, access, address, size, value, user):
        uc.mem_map(address & ~0xFFF, 0x1000)
        return True

    def _io_read(self, uc, access, address, size, value, user):
        # SampleFreqSet waits for the vertical count to reach line 159, so each read during the init moves it on a
        # line. During a frame it stays at the start of the VBlank, so that a game with a time limit on its mixer never
        # runs out of time.
        if address <= 0x04000006 < address + size:
            if self.fixed_vcount is None:
                self.vcount = (self.vcount + 1) % 228
                uc.mem_write(0x04000006, struct.pack('<H', self.vcount))
            else:
                uc.mem_write(0x04000006, struct.pack('<H', self.fixed_vcount))

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def u8(self, address):
        return self.uc.mem_read(address, 1)[0]

    def u16(self, address):
        return struct.unpack('<H', self.uc.mem_read(address, 2))[0]

    def u32(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def play(self, song):
        """Starts `song` on its music player, which plays it from the next frame."""
        self.call(self.addr.song_start, (song,))
        entry = self.addr.song_table + 8 * song - ROM_BASE
        self.player = self.players[struct.unpack('<H', self.rom[entry + 4:entry + 6])[0]]

    def frame(self):
        """Runs one frame. Returns the mixer's output for it as two arrays of signed 8-bit samples, right and left."""
        info = self.info
        self.fixed_vcount = 160
        self.call(self.addr.vsync)
        counter = self.u8(info + 4)
        period = self.u8(info + 11)
        per_frame = self.u32(info + 16)
        offset = (period - (counter - 1)) * per_frame if counter > 1 else 0
        self.call(self.addr.main)
        self.fixed_vcount = None
        buffer = info + SOUND_INFO_PCM_BUFFER + offset
        right = np.frombuffer(bytes(self.uc.mem_read(buffer, per_frame)), dtype=np.int8)
        if self.mono:
            return right, right
        left = np.frombuffer(bytes(self.uc.mem_read(buffer + PCM_DMA_BUF_SIZE, per_frame)), dtype=np.int8)
        return right, left

    def _mute(self, uc, address, size, user):
        # A channel whose volumes are 0 is mixed silently. The sequencer sets them again when its track starts a note
        # on it or changes its volume or pan, so they're cleared after the sequencer in every frame.
        if not self.muted:
            return
        for c in self.channels():
            if c.kind == 'd' and c.track in self.muted:
                base = self.info + SOUND_INFO_CHANS + CHANNEL_SIZE * c.index
                self.uc.mem_write(base + 2, b'\0\0')

    def playing(self):
        """Returns true while the song's player has tracks that haven't ended, or any channel still sounds."""
        status = self.u32(self.player[0] + 4)
        if status & 0xFFFF and not status & 0x80000000:
            return True
        return any(c.status for c in self.channels())

    def track_index(self, track):
        """Returns the index of a track in the song's player, or -1."""
        first, count = self.player[1], self.player[2]
        if track and first <= track < first + TRACK_SIZE * count:
            return (track - first) // TRACK_SIZE
        return -1

    def channels(self):
        """Returns the DirectSound channels up to the engine's limit, then the PSG channels."""
        info = self.info
        out = []
        limit = self.u8(info + 6)
        raw = bytes(self.uc.mem_read(info + SOUND_INFO_CHANS, CHANNEL_SIZE * limit))
        for i in range(limit):
            r = raw[i * CHANNEL_SIZE:(i + 1) * CHANNEL_SIZE]
            out.append(Channel(r, 'd', i, self.track_index(struct.unpack('<I', r[44:48])[0])))
        cgb = self.u32(info + 28)
        if cgb:
            raw = bytes(self.uc.mem_read(cgb, CHANNEL_SIZE * 4))
            for i in range(4):
                r = raw[i * CHANNEL_SIZE:(i + 1) * CHANNEL_SIZE]
                out.append(Channel(r, 'c', i, self.track_index(struct.unpack('<I', r[44:48])[0])))
        return out

    def rate(self):
        """Returns the mixer's output rate in Hz, from the timer the init started."""
        reload = struct.unpack('<H', self.uc.mem_read(0x04000100, 2))[0]
        return 16777216 / (0x10000 - reload)


class CamelotEmulator(EngineEmulator):
    """Camelot's version of the engine: its mixer is in the code the game copies to IWRAM at boot, and the VBlank
    handler copies the DMA counter for it, runs SoundMain and then the mixer's output stage."""

    def __init__(self, rom):
        addr = addresses_for(rom)
        # Camelot calls a routine with `mov lr, rN` and the second half of a BL alone, which the ARM7 runs as a jump to
        # LR. Unicorn's cores either take it for the first half of a Thumb-2 instruction or switch to ARM mode, so each
        # such call is changed to `blx lr`, which does the same.
        rom = bytearray(rom)
        for lo, hi in addr.code_ranges:
            for at in range(lo + 2, hi, 2):
                o = at - ROM_BASE
                if rom[o:o + 2] == b'\x00\xf8' and (rom[o - 2] | rom[o - 1] << 8) & 0xFFC7 == 0x4686:
                    rom[o:o + 2] = b'\xf0\x47'
        self.pending_copies = addr.iwram_copies
        self.restarted = False
        super().__init__(bytes(rom))

    def call(self, address, args=(), thumb=True):
        if self.pending_copies:
            self.copy_mixer()
        super().call(address, args, thumb)

    def copy_mixer(self):
        """Copies the code to IWRAM, as the game's boot code does before the engine's init."""
        uc = self.uc
        for src, dst, size in self.pending_copies:
            uc.mem_write(dst, bytes(uc.mem_read(src, size)))
        self.pending_copies = ()

        # The mixer writes instructions into its loops, which the ARM7 runs at once, having no cache. Unicorn runs a
        # translation of the code, so each write drops the translations it changes. The fixed-pitch loop's first pass
        # is translated with the writes before it, so a new translation starts at the loop's last instruction before it.
        lo, hi = self.addr.mixer_code
        uc.hook_add(UC_HOOK_MEM_WRITE, self._code_write, begin=lo, end=hi - 1)
        uc.hook_add(UC_HOOK_CODE, self._fixed_loop, begin=self.addr.fixed_loop, end=self.addr.fixed_loop)

        # The mixer leaves out a channel whose volumes come to 0, which then doesn't move on through its sample, so a
        # muted track's channels are mixed into sums of their own instead.
        uc.mem_map(CAMELOT_SCRATCH, 0x1000)
        uc.hook_add(UC_HOOK_CODE, self._mix_channel, begin=self.addr.mix_channel, end=self.addr.mix_channel)

        # The mixer has DMA channel 3 copy the points it needs for a frame to the stack, and mixes them from there.
        uc.hook_add(UC_HOOK_MEM_WRITE, self._dma3, begin=DMA3_CONTROL, end=DMA3_CONTROL + 3)

    def _code_write(self, uc, access, address, size, value, user):
        uc.ctl_remove_cache(address, address + size)

    def _fixed_loop(self, uc, address, size, user):
        if self.restarted:
            self.restarted = False
            return
        self.restarted = True
        uc.ctl_remove_cache(address, address + 0x40)
        uc.reg_write(UC_ARM_REG_PC, address)

    def _dma3(self, uc, access, address, size, value, user):
        # A write that enables the channel with an immediate start copies at once, from the source to the destination
        # address, a word or a halfword at a time, each moving up, down or not at all.
        control = value if address == DMA3_CONTROL and size == 4 else None
        if control is None or not control & 0x80000000 or control & 0x30000000:
            return
        unit = 4 if control & 0x04000000 else 2
        count = control & 0xFFFF or 0x10000
        src, dst = self.u32(DMA3_CONTROL - 8), self.u32(DMA3_CONTROL - 4)
        steps = {0: unit, 1: -unit, 2: 0, 3: unit}
        dst_step, src_step = steps[(control >> 21) & 3], steps[(control >> 23) & 3]
        for i in range(count):
            uc.mem_write(dst + i * dst_step, bytes(uc.mem_read(src + i * src_step, unit)))

    def _mute(self, uc, address, size, user):
        # The mixer leaves out a channel whose volumes are 0, so _mix_channel() mutes tracks instead.
        pass

    def _mix_channel(self, uc, address, size, user):
        if self.muted and self.track_index(self.u32(uc.reg_read(UC_ARM_REG_R4) + 44)) in self.muted:
            uc.reg_write(UC_ARM_REG_R5, CAMELOT_SCRATCH)

    def frame(self):
        """Runs one frame. Returns the mixer's output for it as two arrays, right and left. The output stage writes two
        8-bit points for each of the mixer's, which add up to a 9-bit point, and each array holds their average."""
        self.fixed_vcount = 160
        self.call(self.addr.vsync)
        self.uc.mem_write(self.addr.dma_counter, bytes([self.u8(self.info + 4)]))
        self.call(self.addr.main)
        self.call(self.addr.output, (8,))
        self.fixed_vcount = None
        place = self.u32(self.addr.output_place)
        count = 2 * self.u32(self.info + 16)
        right = np.frombuffer(bytes(self.uc.mem_read(place, count)), dtype=np.int8).astype(np.float64)
        left = np.frombuffer(bytes(self.uc.mem_read(place + 2 * PCM_DMA_BUF_SIZE, count)), dtype=np.int8)
        left = left.astype(np.float64)
        return (right[0::2] + right[1::2]) / 2, (left[0::2] + left[1::2]) / 2

    def rate(self):
        """Returns the mixer's rate in Hz: the timer runs at twice the rate, for the output's two points each."""
        return super().rate() / 2


def emulator(rom):
    """Returns the engine of a game, ready to play a song."""
    return CamelotEmulator(rom) if getattr(addresses_for(rom), 'camelot', False) else EngineEmulator(rom)


def write_wav(path, right, left, rate):
    stereo = np.stack([left, right], axis=1).astype(np.float64) * 256
    data = np.clip(stereo, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(data.tobytes())


def trace(rom, song, frames):
    emu = emulator(rom)
    emu.play(song)
    for f in range(frames):
        emu.frame()
        for c in emu.channels():
            if c.status:
                print(c.line(f))
        if not emu.playing():
            break


def render(rom, song, frames, tracks=None):
    """Returns the mixer's output for the first `frames` frames of `song` as right and left arrays, and its rate in Hz.
    If `tracks` is given, the DirectSound channels of the song's other tracks are mixed silently. They still take up
    channels, so the result is what the game plays with those tracks muted. Camelot's mixer adds an echo of its earlier
    output, which the muted tracks leave out too."""
    emu = emulator(rom)
    emu.play(song)
    if tracks is not None:
        emu.muted = set(range(16)) - set(tracks)
    right, left = [], []
    for _ in range(frames):
        r, l = emu.frame()
        right.append(r)
        left.append(l)
    return np.concatenate(right), np.concatenate(left), emu.rate()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--tracks', help='render: only these tracks of the song, such as 0,3')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames)
        return
    if not a.out:
        p.error('an output file is needed')
    tracks = [int(t) for t in a.tracks.split(',')] if a.tracks else None
    right, left, rate = render(rom, a.song, a.frames, tracks)
    write_wav(a.out, right, left, rate)


if __name__ == '__main__':
    main()
