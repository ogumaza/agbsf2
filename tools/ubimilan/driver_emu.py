#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Ubisoft Milan's GBA sound driver under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the driver. It runs the game's routines (no BIOS, no video):
the driver's init with the sound bank, the routine that starts a piece of music, then for each frame the sequencer's
step, which the game's VBlank runs, and the sound update of the game's main loop, then the mixer, copied to IWRAM, once
for each 16 points of output before the next frame, at the rate Timer 0 sets. It captures what the driver does:

    trace       after each frame, the frame's writes to the PSG's registers (w), the track (t: its countdown, next
                command, next sequence, whether it plays and whether an end command was read) and each voice that the
                mixer plays (v0-v3: its next point, its loop's start, its end, its loop's end, its volume and whether
                it loops), in the format of agbsf2 --trace
    render      the mixer's output, which the game plays on both sides

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM render SONG FRAMES OUT.wav

SONG numbers the pieces of music in the order of the sound bank's resources. agbsf2 numbers them the same way. The
routine and RAM addresses are chosen by the ROM's game code. Tomb Raider: The Prophecy's (AL9P) are built in. For
another game, add its values to GAMES; see docs/ubimilan.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, \
    UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
FRAME_CYCLES = 280896  # CPU cycles from one VBlank to the next
RUN_OUTPUT = 16        # points of output that each run of the mixer makes
VOICE_SIZE = 0x28
VOICE_COUNT = 4
PSG_REGISTERS = (0x04000060, 0x040000A0)
DMA3 = 0x040000D4


class TombRaider:
    """AL9P (Tomb Raider: The Prophecy)."""
    file_lookup = 0x08002AA8  # returns the address of a file of the game's archive: r0 = its number
    bank_file = 2             # the archive's file that the game's start hands to the driver as its sound bank
    init = 0x080031A0         # the driver's init: r0 = the sound bank
    voices_init = 0x08001044  # the mixer's voices, which the game's start clears before the driver's init
    start = 0x080044B4        # starts a piece of music: r0 = its resource, r1 = 1 to start it at once
    step = 0x08003280         # the sequencer's step, which the game's VBlank runs
    play_sample = 0x08000F14  # starts a sample on a voice: r0 = its description, r1 = the voice
    stop_voice = 0x080010AC   # stops a voice: r0 = the voice
    update = 0x08003130       # the game's sound update, which its main loop runs
    mixer = 0x0800097C        # the mixer, in ARM, which the game copies to IWRAM: its size is the word at 0x08000F10
    mixer_size = 0x08000F10
    mixer_ram = 0x03001000    # where this copies the mixer, which reads its variables from fixed addresses
    mixer_pointer = 0x03000270
    mixer_flag = 0x0300026C   # set by the game's start, so that the first FIFO interrupt runs the mixer
    track = 0x02003BB0        # the sequencer's track
    voices = 0x03000014       # the mixer's voices
    output = 0x0300022C       # the 16 points of output that each run of the mixer makes
    timer_reload = 0xFDFF     # Timer 0's reload, which the sound init works out from a float: 0xFFFF less half of it


GAMES = {b'AL9P': TombRaider}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
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
        size = (len(rom) + 0xFFF) & ~0xFFF
        # The ROM and its two mirrors, which a key outside channel 9's kit makes the driver read a sample from.
        for base in (ROM_BASE, 0x0A000000, 0x0C000000):
            uc.mem_map(base, size)
            uc.mem_write(base, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=PSG_REGISTERS[0], end=0x040000FF)
        uc.hook_add(UC_HOOK_CODE, self._play, begin=addr.play_sample, end=addr.play_sample)
        uc.hook_add(UC_HOOK_CODE, self._stop, begin=addr.stop_voice, end=addr.stop_voice)
        self.uc = uc
        self.writes = []
        self.voice_events = []  # (frame, voice, 'play', data, length, volume) or (frame, voice, 'stop')
        self.frames = 0
        self.runs = 0

        # The game's start: the mixer in IWRAM, its voices, then the driver's init with the sound bank.
        mixer_size = struct.unpack_from('<I', rom, addr.mixer_size - ROM_BASE)[0]
        uc.mem_write(addr.mixer_ram, bytes(rom[addr.mixer - ROM_BASE:addr.mixer - ROM_BASE + mixer_size]))
        uc.mem_write(addr.mixer_pointer, struct.pack('<I', addr.mixer_ram))
        uc.mem_write(addr.mixer_flag, struct.pack('<I', 1))
        self.call(addr.voices_init)
        self.call(addr.file_lookup, (addr.bank_file,))
        self.bank = uc.reg_read(UC_ARM_REG_R0)
        self.call(addr.init, (self.bank,))
        self.writes = []

    def _svc(self, uc, intno, user):
        raise SystemExit('the driver made a BIOS call at %08x' % uc.reg_read(UC_ARM_REG_PC))

    def _unmapped(self, uc, access, address, size, value, user):
        raise SystemExit('the driver accessed unmapped memory at %08x' % address)

    def _play(self, uc, address, size, user):
        description = uc.reg_read(UC_ARM_REG_R0)
        data, length = struct.unpack('<II', uc.mem_read(description, 8))
        volume = struct.unpack('<H', uc.mem_read(description + 0x14, 2))[0]
        voice = struct.unpack('<h', struct.pack('<H', uc.reg_read(UC_ARM_REG_R1) & 0xFFFF))[0]
        self.voice_events.append((self.frames, voice, 'play', data, length, volume))

    def _stop(self, uc, address, size, user):
        self.voice_events.append((self.frames, uc.reg_read(UC_ARM_REG_R0), 'stop'))

    def _io_write(self, uc, access, address, size, value, user):
        value &= (1 << (8 * size)) - 1
        if DMA3 <= address < DMA3 + 12:
            # DMA 3: the mixer copies each voice's next 8 points with an immediate transfer when it writes the control.
            if address == DMA3 + 8 and size == 4 and value & 0x80000000:
                src, dst = struct.unpack('<II', uc.mem_read(DMA3, 8))
                width = 4 if value & 0x04000000 else 2
                uc.mem_write(dst, bytes(uc.mem_read(src, (value & 0xFFFF) * width)))
            return
        if address < PSG_REGISTERS[1]:
            self.writes.append((address, size, value))

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for r in range(13):
            uc.reg_write(UC_ARM_REG_R0 + r, args[r] if r < len(args) else 0)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def songs(self):
        """Returns the addresses of the pieces of music: the sound bank's resources whose first byte is 0, in order."""
        u16 = lambda a: struct.unpack_from('<H', self.rom, a - ROM_BASE)[0]
        u32 = lambda a: struct.unpack_from('<I', self.rom, a - ROM_BASE)[0]
        table = self.bank + u32(self.bank + 8)
        addresses = [self.bank + u32(table + 4 * i) for i in range(u16(self.bank + 2))]
        return [a for a in addresses if self.rom[a - ROM_BASE] == 0]

    def play(self, song):
        self.call(self.addr.start, (self.songs()[song], 1, 0, 0))

    def frame(self):
        """Runs a frame: the sequencer's step, the game's sound update, then the mixer's runs until the next frame, one
        for each 16 points of output at Timer 0's rate, counted from the first frame. Returns the frame's writes to the
        PSG's registers, and the mixer's output."""
        self.call(self.addr.step)
        self.call(self.addr.update)
        writes, self.writes = self.writes, []
        run_cycles = RUN_OUTPUT * (0x10000 - self.addr.timer_reload)
        due = (self.frames + 1) * FRAME_CYCLES // run_cycles
        output = []
        while self.runs < due:
            self.call(self.addr.mixer_ram, thumb=False)
            output.append(np.frombuffer(bytes(self.uc.mem_read(self.addr.output, RUN_OUTPUT)), dtype=np.int8))
            self.runs += 1
        self.frames += 1
        return writes, (np.concatenate(output) if output else np.zeros(0, np.int8))

    def track_state(self):
        """The track's countdown, next command, next sequence, whether it plays and whether an end was read."""
        raw = bytes(self.uc.mem_read(self.addr.track, 0x14))
        wait = struct.unpack_from('<H', raw)[0]
        pointer, nxt, active, ending = struct.unpack_from('<IIII', raw, 4)
        return wait, pointer, nxt, active, ending

    def voice_state(self, v):
        """Voice v's next point, loop start, end, loop end, volume and loop flag, or None if the mixer doesn't play
        it."""
        raw = bytes(self.uc.mem_read(self.addr.voices + VOICE_SIZE * v, VOICE_SIZE))
        position, loop_start, end, loop_end, volume = struct.unpack_from('<IIIII', raw)
        stopped = struct.unpack_from('<I', raw, 0x1C)[0]
        loops = struct.unpack_from('<I', raw, 0x24)[0]
        return None if stopped else (position, loop_start, end, loop_end, volume, loops)

    def playing(self):
        return self.track_state()[3] != 0 or any(self.voice_state(v) for v in range(VOICE_COUNT))


def trace(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    start_writes, emu.writes = emu.writes, []
    for f in range(frames):
        writes, _ = emu.frame()
        if f == 0:
            writes = start_writes + writes
        for address, size, value in writes:
            print('%d w %08x %d %0*x' % (f, address, size, 2 * size, value))
        print('%d t %d %08x %08x %d %d' % ((f,) + emu.track_state()))
        for v in range(VOICE_COUNT):
            state = emu.voice_state(v)
            if state:
                print('%d v%d %08x %08x %08x %08x %d %d' % ((f, v) + state))
        if f > 0 and not emu.playing():
            break


def render(rom, song, frames):
    """Returns the mixer's output for the first `frames` frames of `song`, as points from -1 to 1 at its rate."""
    emu = DriverEmulator(rom)
    emu.play(song)
    chunks = [emu.frame()[1] for _ in range(frames)]
    return np.concatenate(chunks).astype(np.float64) / 128, 16777216 / (0x10000 - emu.addr.timer_reload)


def write_wav(path, mono, rate):
    data = np.clip(mono * 32767, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(np.repeat(data, 2).tobytes())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames)
        return
    if not a.out:
        p.error('render needs an output file')
    write_wav(a.out, *render(rom, a.song, a.frames))


if __name__ == '__main__':
    main()
