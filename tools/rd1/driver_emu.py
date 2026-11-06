#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Nintendo R&D1's MIDI-file sound driver under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the driver. WarioWare, Inc. and Rhythm Tengoku play their music
and sound effects with it. The script calls the driver's init, the game's call that sets the echo, the routine that
starts a song and the per-frame routine (no BIOS, no video). The per-frame routine runs the players, the voices and
the mixer for the frame. It prints what the driver does:

    trace       after each frame, each player that plays a song (p), each track of it that hasn't ended (t), each of
                its channels whose record changed (c, in hex as the driver keeps it), each voice in use (v) and each
                PSG voice in use (q), with the mixer's voice (m) after each sample voice, and the frame's writes to
                the sound registers (w)
    psg         the PSG's register writes alone, one frame a line

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM psg SONG FRAMES

SONG is the number of the song's entry in the song table, as agbsf2 numbers it. The routine and RAM addresses come
from the ROM's game code. Those of WarioWare, Inc. (AZWE) and Rhythm Tengoku (BRIJ) are built in; see GAMES and
docs/rd1.md.
"""
import argparse
import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
PLAYER_INIT_SIZE = 20  # an entry of the table that the init sets the players up from
PLAYER_SIZE = 0x30
BUS_SIZE = 0x30
CHANNEL_SIZE = 32
VOICE_SIZE = 32
PSG_VOICES = 4
SOUND_REGISTERS = (0x04000060, 0x040000A0)  # the PSG's registers, the control registers and the wave RAM


class WarioWare:
    """AZWE (WarioWare, Inc.: Mega Microgame$!)."""
    init = 0x080F4090        # the driver's init
    echo = 0x080F3C60        # sets the echo's four settings: r0-r3
    echo_settings = (0x23, 2, 2, 4)  # what the game's init passes to it, at 0x080002E0
    play = 0x080F2E48        # starts entry r0 of the song table
    frame = 0x080F3A90       # the per-frame routine, which the game's main loop calls
    song_table = 0x08402990  # 8 bytes an entry: the song's record, then its player
    player_init = 0x08406378 # 20 bytes a player: what the init sets each player up from
    players = 9
    track_size = 0x24            # a track's record
    voices_pointer = 0x0300721C  # holds the address of the sample voices
    voice_count = 0x030068EC     # a halfword: the sample voices
    mixer_pointer = 0x030068E8   # holds the address of the mixer's voices
    psg_voices = 0x03006600      # 4 voices, one a PSG channel


class RhythmTengoku:
    """BRIJ (Rhythm Tengoku)."""
    init = 0x0804C778
    echo = 0x0804C340
    echo_settings = (0x23, 2, 2, 4)  # the game's init, at 0x0800035A
    play = 0x0804B534
    frame = 0x0804C170
    song_table = 0x08AA06F8
    player_init = 0x08AA4358
    players = 13
    track_size = 0x1C
    voices_pointer = 0x030064BC
    voice_count = 0x03005B8C
    mixer_pointer = 0x03005B88
    psg_voices = 0x030056A0


GAMES = {b'AZWE': WarioWare, b'BRIJ': RhythmTengoku}


def u32_at(rom, address):
    return struct.unpack_from('<I', rom, address - ROM_BASE)[0]


def literal(rom, address):
    """Returns the literal that the Thumb `ldr rX, [pc, #n]` at `address` loads."""
    op = struct.unpack_from('<H', rom, address - ROM_BASE)[0]
    assert op >> 11 == 0x09, 'no ldr at %08x' % address
    return u32_at(rom, ((address + 4) & ~3) + (op & 0xFF) * 4)


def addresses_for(rom):
    code = bytes(rom[0xAC:0xB0])
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class DriverEmulator:
    def __init__(self, rom):
        addr = addresses_for(rom)
        self.addr = addr
        self.rom = rom
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_write(0x04000088, struct.pack('<H', 0x0200))  # SOUNDBIAS as the BIOS leaves it
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=SOUND_REGISTERS[0], end=SOUND_REGISTERS[1] - 1)
        self.uc = uc
        self.writes = []
        self.call(addr.init)
        self.call(addr.echo, addr.echo_settings)
        self.players = []
        for i in range(addr.players):
            entry = addr.player_init + PLAYER_INIT_SIZE * i
            head = struct.unpack_from('<H', rom, entry - ROM_BASE)[0]
            self.players.append({'channels': (head >> 5) & 0x1F, 'channel_records': u32_at(rom, entry + 4),
                                 'bus': u32_at(rom, entry + 8), 'tracks': u32_at(rom, entry + 12),
                                 'player': u32_at(rom, entry + 16)})
        self.last_channels = {}

    def _svc(self, uc, intno, user):
        raise SystemExit('the driver made a BIOS call at %08x' % uc.reg_read(UC_ARM_REG_PC))

    def _unmapped(self, uc, access, address, size, value, user):
        raise SystemExit('the driver accessed unmapped memory at %08x (pc %08x)' % (address, uc.reg_read(UC_ARM_REG_PC)))

    def _io_write(self, uc, access, address, size, value, user):
        self.writes.append((address, size, value & ((1 << (8 * size)) - 1)))

    def call(self, address, args=()):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | 0x20)  # system mode, Thumb
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for r in range(13):
            uc.reg_write(UC_ARM_REG_R0 + r, args[r] & 0xFFFFFFFF if r < len(args) else 0)
        uc.emu_start(address | 1, RETURN_TRAP, count=50_000_000)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]

    def u16(self, address):
        return struct.unpack('<H', self.read(address, 2))[0]

    def play(self, song):
        self.call(self.addr.play, (song,))

    def frame(self):
        """Runs the per-frame routine. Returns the frame's writes to the sound registers."""
        self.writes = []
        self.call(self.addr.frame)
        return self.writes

    def trace_lines(self, n):
        """Returns the trace's lines for frame n, after the frame has run."""
        lines = []
        for i, p in enumerate(self.players):
            player = self.read(p['player'], PLAYER_SIZE)
            record = struct.unpack_from('<I', player, 0xC)[0]
            if not record:
                continue
            head, = struct.unpack_from('<I', player, 0)
            smf = u32_at(self.rom, record)
            lines.append('%d p %d song %08x tracks %d bpm %d step %d volume %d fade %04x/%04x mode %d' % (
                n, i, record, (head >> 5) & 0x1F, (head >> 12) & 0x1FF, struct.unpack_from('<I', player, 0x10)[0],
                struct.unpack_from('<H', player, 0x26)[0], struct.unpack_from('<H', player, 0x28)[0], struct.unpack_from('<H', player, 0x2A)[0],
                (player[3] >> 3) & 7))
            for t in range((head >> 5) & 0x1F):
                track = self.read(p['tracks'] + self.addr.track_size * t, self.addr.track_size)
                if track[0] & 1:
                    lines.append('%d t %d.%d at +%x wait %d status %02x' % (
                        n, i, t, struct.unpack_from('<I', track, 8)[0] - smf, struct.unpack_from('<I', track, 0xC)[0],
                        (struct.unpack_from('<H', track, 0)[0] >> 2) & 0xFF))
            for c in range(p['channels']):
                data = self.read(p['channel_records'] + CHANNEL_SIZE * c, CHANNEL_SIZE)
                if self.last_channels.get((i, c)) != data:
                    self.last_channels[(i, c)] = data
                    lines.append('%d c %d.%d %s' % (n, i, c, data.hex()))
        voices = self.u32(self.addr.voices_pointer)
        mixer = self.u32(self.addr.mixer_pointer)
        bus_of = {p['channel_records'] + CHANNEL_SIZE * c: '%d.%d' % (i, c)
                  for i, p in enumerate(self.players) for c in range(p['channels'])}
        for v in range(self.u16(self.addr.voice_count)):
            lines += self._voice_lines(n, 'v', v, self.read(voices + VOICE_SIZE * v, VOICE_SIZE), bus_of)
            if lines and lines[-1].split()[1] == 'v' and lines[-1].split()[2] == str(v):
                m = self.read(mixer + VOICE_SIZE * v, VOICE_SIZE)
                lines.append('%d m %d on %d volume %d left %d right %d step %x base %x at %x' % (
                    n, v, m[0] & 1, m[1], m[2], m[3], struct.unpack_from('<I', m, 0x18)[0],
                    struct.unpack_from('<I', m, 0x1C)[0], struct.unpack_from('<I', m, 0xC)[0]))
        for v in range(PSG_VOICES):
            lines += self._voice_lines(n, 'q', v, self.read(self.addr.psg_voices + VOICE_SIZE * v, VOICE_SIZE), bus_of)
        return lines

    def _voice_lines(self, n, kind, v, data, bus_of):
        if not data[0] & 1:
            return []
        head, = struct.unpack_from('<I', data, 0)
        instrument, _, channel = struct.unpack_from('<III', data, 4)
        envelope, = struct.unpack_from('<I', data, 0x1C)
        return ['%d %s %d key %d velocity %d channel %s instrument %08x pitch %d bend %d/%d mod %d pan %d '
                'envelope %d level %06x' % (
                    n, kind, v, (head >> 1) & 0x7F, data[1] & 0x7F, bus_of.get(channel, '%08x' % channel), instrument,
                    head >> 15, struct.unpack_from('<h', data, 0x10)[0], struct.unpack_from('<h', data, 0x12)[0],
                    struct.unpack_from('<h', data, 0x14)[0], struct.unpack_from('<h', data, 0x18)[0], envelope & 0xFF,
                    envelope >> 8)]


def trace(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    for n in range(frames):
        writes = emu.frame()
        for line in emu.trace_lines(n):
            print(line)
        if writes:
            print('%d w %s' % (n, ' '.join('%03x=%x' % (a & 0xFFF, v) for a, _, v in writes)))


def psg(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    for n in range(frames):
        writes = emu.frame()
        print('%d %s' % (n, ' '.join('%03x=%x' % (a & 0xFFF, v) for a, _, v in writes)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('mode', choices=('trace', 'psg'))
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    a = p.parse_args()
    rom = load_rom(a.rom)
    {'trace': trace, 'psg': psg}[a.mode](rom, a.song, a.frames)


if __name__ == '__main__':
    main()
