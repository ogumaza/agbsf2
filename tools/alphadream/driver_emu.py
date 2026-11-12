#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run the sound driver of Mario & Luigi: Superstar Saga under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of AlphaDream's driver. The script calls the driver's init, the
mixer's setup, the routine that starts a song on a player and, each frame, the driver's per-frame routine and the
mixer (no BIOS but the LZ77 decompression, no video). It prints what the driver does:

    trace       after each frame, each player that plays (p), each of its tracks that plays (t, its 32 bytes in hex
                without the data pointer), each sample channel whose record changed (h, its 16 bytes without the
                position) and each PSG channel whose record changed (q, its 12 bytes), and the frame's writes to the
                sound registers (w)

    driver_emu.py ROM trace SONG FRAMES [--player N]

SONG is the number of the song's entry in the song table. The game starts its music on player 1. The routine and RAM
addresses come from the ROM's game code. Those of the US release (A88E) and of the two Hamtaro games (B85A and A84J)
are built in; see GAMES and docs/alphadream.md.
"""
import argparse
import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_SP)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
PLAYERS = 3
TRACKS = 12
TRACK_SIZE = 32
PLAYER_SIZE = TRACK_SIZE * TRACKS
SAMPLE_CHANNELS = 8
SAMPLE_CHANNEL_SIZE = 16
PSG_CHANNELS = 4
PSG_CHANNEL_SIZE = 12
SOUND_REGISTERS = (0x04000060, 0x040000A0)  # the PSG's registers, the control registers and the wave RAM


class SuperstarSaga:
    """A88E (Mario & Luigi: Superstar Saga)."""
    sound_info = 0x02000480      # the driver's state, whose address the game stores at 0x03007FF0
    init = 0x0819AFA8            # the driver's init: r0 is the state's address
    mixer_setup = 0x0819A43C     # decompresses the mixer into IWRAM and sets up the timer and DMA
    start = 0x0819B110           # starts song r1 on player r0
    frame = 0x0819B95C           # the per-frame routine: the commands, the tracks and the PSG's output
    mixer = 0x0819A2F0           # runs the sample channels' envelopes and mixes a frame
    mix_buffer = 0x030011E0      # the mixer's buffer of 32-bit points
    fifo_buffers = (0x03001600, 0x03001810)
    mix_points = 264             # a frame's points at 15768 Hz
    sample_channels = 0x03001B50
    psg_channels = 0x4C8         # from the state's address
    player_flags = 0x494         # a byte a player: 1 while it plays
    player_volume = 0x498        # a halfword a player
    player_divisor = 0x4A4       # a word a player: the tempo's divisor


class HamSports(SuperstarSaga):
    """B85A (Tottoko Hamtaro: Hamu-Hamu Sports): notes with a volume, and more of the state before the PSG's."""
    sound_info = 0x02000100
    init = 0x08095A20
    mixer_setup = 0x08094DB4
    start = 0x08095B88
    frame = 0x080964BC
    mixer = 0x08094C68
    mix_buffer = 0x03005B40
    fifo_buffers = (0x03005F60, 0x03006170)
    sample_channels = 0x0200CCF0
    psg_channels = 0x4EC


class Hamtaro4(SuperstarSaga):
    """A84J (Tottoko Hamtaro 4), an older version: notes with a volume, and a fixed tempo divisor."""
    sound_info = 0x02000480
    init = 0x0814AB50
    mixer_setup = 0x08149FD4
    start = 0x0814AC70
    frame = 0x0814B574
    mixer = 0x08149E88
    mix_buffer = 0x03004188
    fifo_buffers = (0x030045A8, 0x030047B8)
    sample_channels = 0x03004AC0
    psg_channels = 0x4B0
    player_divisor = None        # the divisor is a fixed 0x4B
    fixed_divisor = 0x4B


GAMES = {b'A88E': SuperstarSaga, b'B85A': HamSports, b'A84J': Hamtaro4}


def lz77(data, at):
    """Returns what the BIOS's LZ77 decompression makes of the data at offset `at`."""
    size = struct.unpack_from('<I', data, at)[0] >> 8
    out = bytearray()
    at += 4
    while len(out) < size:
        flags = data[at]
        at += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                a, b = data[at], data[at + 1]
                at += 2
                length = (a >> 4) + 3
                back = ((a & 0xF) << 8 | b) + 1
                for _ in range(length):
                    out.append(out[-back])
            else:
                out.append(data[at])
                at += 1
    return bytes(out[:size])


def addresses_for(rom):
    code = bytes(rom[0xAC:0xB0])
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class DriverEmulator:
    def __init__(self, rom, player):
        addr = addresses_for(rom)
        self.addr = addr
        self.rom = rom
        self.player = player
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
        uc.mem_write(0x03007FF0, struct.pack('<I', addr.sound_info))
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=SOUND_REGISTERS[0], end=SOUND_REGISTERS[1] - 1)
        self.uc = uc
        self.writes = []
        self.call(addr.init, (addr.sound_info,))
        self.call(addr.mixer_setup)
        self.last_samples = {}
        self.last_psg = {}

    def _svc(self, uc, intno, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        number = self.rom[pc - 2 - ROM_BASE] if ROM_BASE <= pc < ROM_BASE + len(self.rom) else -1
        if number != 0x11:
            raise SystemExit('the driver made a BIOS call %x at %08x' % (number, pc))
        source, dest = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
        uc.mem_write(dest, lz77(self.rom, source - ROM_BASE))

    def _unmapped(self, uc, access, address, size, value, user):
        # For an unmapped access, Unicorn's emu_start() raises UcError ahead of any exception from this hook.
        # call() raises SystemExit with this message instead.
        self.fault = 'the driver accessed unmapped memory at %08x (pc %08x)' % (address, uc.reg_read(UC_ARM_REG_PC))
        return False

    def _io_write(self, uc, access, address, size, value, user):
        self.writes.append((address, size, value & ((1 << (8 * size)) - 1)))

    def call(self, address, args=()):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | 0x20)  # system mode, Thumb
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for r in range(13):
            uc.reg_write(UC_ARM_REG_R0 + r, args[r] & 0xFFFFFFFF if r < len(args) else 0)
        self.fault = None
        try:
            uc.emu_start(address | 1, RETURN_TRAP, count=50_000_000)
        except UcError:
            if self.fault:
                raise SystemExit(self.fault) from None
            raise

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def start(self, song):
        self.call(self.addr.start, (self.player, song))

    def frame(self):
        """Runs the per-frame routine and the mixer. Returns the frame's writes to the sound registers."""
        self.writes = []
        self.call(self.addr.frame)
        a = self.addr
        self.call(a.mixer, (a.mix_buffer, a.mix_points, a.fifo_buffers[0], a.fifo_buffers[1]))
        return self.writes

    def trace_lines(self, n):
        """Returns the trace's lines for frame n, after the frame has run."""
        a = self.addr
        lines = []
        for p in range(PLAYERS):
            flags = self.read(a.sound_info + a.player_flags + p, 1)[0]
            if not flags & 1:
                continue
            volume = struct.unpack('<H', self.read(a.sound_info + a.player_volume + 2 * p, 2))[0]
            if a.player_divisor is None:
                divisor = a.fixed_divisor
            else:
                divisor = struct.unpack('<I', self.read(a.sound_info + a.player_divisor + 4 * p, 4))[0]
            playing = []
            for t in range(TRACKS):
                track = self.read(a.sound_info + PLAYER_SIZE * p + TRACK_SIZE * t, TRACK_SIZE)
                if track[0] & 1:
                    playing.append('%d t %d.%d %s %s' % (n, p, t, track[:4].hex(), track[8:].hex()))
            if playing:
                lines.append('%d p %d flags %02x volume %04x divisor %d' % (n, p, flags, volume, divisor))
                lines += playing
        for c in range(SAMPLE_CHANNELS):
            data = self.read(a.sample_channels + SAMPLE_CHANNEL_SIZE * c, SAMPLE_CHANNEL_SIZE)
            data = data[:4] + data[8:]
            if self.last_samples.get(c, bytes(12)) != data:
                self.last_samples[c] = data
                lines.append('%d h %d %s' % (n, c, data.hex()))
        for c in range(PSG_CHANNELS):
            data = self.read(a.sound_info + a.psg_channels + PSG_CHANNEL_SIZE * c, PSG_CHANNEL_SIZE)
            if self.last_psg.get(c, bytes(PSG_CHANNEL_SIZE)) != data:
                self.last_psg[c] = data
                lines.append('%d q %d %s' % (n, c, data.hex()))
        return lines


def trace(rom, song, frames, player=1):
    emu = DriverEmulator(rom, player)
    emu.start(song)
    for n in range(frames):
        writes = emu.frame()
        for line in emu.trace_lines(n):
            print(line)
        if writes:
            print('%d w %s' % (n, ' '.join('%03x=%x' % (a & 0xFFF, v) for a, _, v in writes)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('mode', choices=('trace',))
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('--player', type=int, default=1)
    a = p.parse_args()
    trace(load_rom(a.rom), a.song, a.frames, a.player)


if __name__ == '__main__':
    main()
