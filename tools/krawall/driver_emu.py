#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run a game's build of Krawall, the XM and S3M player, under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of Krawall. It copies the game's code and data to IWRAM and
EWRAM as its start-up code does, calls the game's sound init and krapPlay() (no BIOS, no video), and calls the mixer's
worker once a frame, as the game's VBlank handler does, with getDmaAddress() replaced by a routine that hands it the
same two buffers and a frame's samples each time. It captures what the player and the mixer do:

    trace       after each frame, the mixer's variables (g), the player's record (p), each of the module's channels (c0
                and on) and each mixer channel (m0 and on), in hex as Krawall keeps them in RAM, when they've changed
                since the line that last gave them. The player's record leaves out its pointers to the routines that
                work out periods and frequencies.
    render      the mix's 8-bit output, left and right

    driver_emu.py ROM trace MODULE FRAMES [--song N]
    driver_emu.py ROM render MODULE FRAMES OUT.wav [--song N]

MODULE is the address of the module's header in hex. `agbsf2 --info` lists the addresses. krapPlay() plays the
module in loop mode. With --song, krapPlay() plays song N of a module whose orders separate songs with +++, in song
mode. Song mode loops back to the song's start at the song's end. The routine and RAM addresses are chosen by the ROM's
game code. Those of Digimon Racing (BDGE) are built in. Another game requires its values in GAMES (see
docs/krawall.md).
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_MEM_READ_UNMAPPED, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, \
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
LEFT, RIGHT = 0x02030000, 0x02031000  # the buffers that the stand-in for getDmaAddress() hands the worker
PLAYER_SIZE = 0x34
CHANNEL_SIZE = 0x60
MIX_SIZE = 0x2C
MIX_CHANNELS = 32
PERIOD_POINTERS = (0x24, 0x2C)  # the player's pointers to the routines that work out periods and frequencies
MODE_LOOP, MODE_SONG = 1, 2  # krapPlay()'s modes: loop the module, and play one of its songs
ORDER_SKIP = 254  # the order that separates songs (+++)


class DigimonRacing:
    """BDGE (Digimon Racing): the 2003/09/01 build of Krawall."""
    iwram = (0x087F2730, 0x1598)  # the code and data that the start-up code copies to IWRAM, and its size
    ewram = (0x087F3CC8, 0x27F8)  # the data it copies to EWRAM
    sound_init = 0x080130B4       # the game's sound init: kragInit(1), kramSetMasterVol(0x40080), kramQualityMode(0)
    play = 0x08038398             # krapPlay(module, mode, song)
    worker = 0x03001068           # kramWorker(), in ARM, which the game calls once a frame
    after_tick = 0x0300111C       # where the worker goes on after a tick of the player
    set_pos = 0x03000578          # kramSetPos(), in ARM, which moves a mixer channel's point
    dma_address = 0x080373A8      # getDmaAddress(), which the worker calls for the buffers and the samples to mix
    frame_samples = 276
    player = 0x02001644           # the player's record, then its channels
    mixer = 0x020008B4            # the mixer's channels
    master = 0x03000090           # the master volumes of the music and the sound effects, hqMode and hqRamp
    timer = 0x0200163C            # the sound timer's step and count
    music_volume = 0x02001640
    plays = 0x03000600            # the count of plays, which the mixer's handles hold


GAMES = {b'BDGE': DigimonRacing}


def addresses_for(rom):
    code = bytes(rom[0xAC:0xB0])
    if code not in GAMES:
        raise SystemExit('no Krawall addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class KrawallEmulator:
    def __init__(self, rom):
        a = self.addr = addresses_for(rom)
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        for (source, size), destination in ((a.iwram, 0x03000000), (a.ewram, 0x02000000)):
            uc.mem_write(destination, bytes(rom[source - ROM_BASE:source - ROM_BASE + size]))

        # getDmaAddress(&left, &right) stores the buffers' addresses and returns the frame's samples.
        assert a.frame_samples % 2 == 0 and a.frame_samples >> 1 < 256
        stub = struct.pack('<8H2I', 0x4A03, 0x6002, 0x4A03, 0x600A, 0x2000 | (a.frame_samples >> 1), 0x0040, 0x4770,
                           0x46C0, LEFT, RIGHT)
        uc.mem_write(a.dma_address, stub)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        self.uc = uc
        self.call(a.sound_init)

    def _unmapped(self, uc, access, address, size, value, user):
        # A note on a channel without a sample has the mixer read its points from address 0 on, and a sample near the
        # ROM's end can have it read past the end, where the hardware reads open bus. agbsf2 reads 0 there, and
        # so does the emulator. The points don't change the mixer's positions, which the trace compares.
        if access == UC_MEM_READ_UNMAPPED:
            uc.mem_map(address & ~0xFFF, 0x1000 if (address & 0xFFF) + size <= 0x1000 else 0x2000)
            return True
        raise SystemExit('unmapped access at %08x (pc %08x)' % (address, uc.reg_read(UC_ARM_REG_PC)))

    def _svc(self, uc, intno, user):
        """The BIOS calls that Krawall makes: Div, and the CpuSet and CpuFastSet that clear and copy its records."""
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        if thumb:
            number = uc.mem_read(pc - 2, 1)[0]
        else:
            number = (struct.unpack('<I', uc.mem_read(pc - 4, 4))[0] >> 16) & 0xFF
        r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if number == 6:
            n = r0 - (1 << 32) if r0 & 0x80000000 else r0
            d = r1 - (1 << 32) if r1 & 0x80000000 else r1
            q = int(n / d) if d else 0
            uc.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R1, (n - q * d) & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R3, abs(q) & 0xFFFFFFFF)
        elif number in (0x0B, 0x0C):
            fill = r2 & (1 << 24)
            words = number == 0x0C or r2 & (1 << 26)
            count = r2 & 0x1FFFFF
            if number == 0x0C:
                count = (count + 7) & ~7
            unit = 4 if words else 2
            value = bytes(uc.mem_read(r0, unit))
            for i in range(count):
                if not fill:
                    value = bytes(uc.mem_read(r0 + i * unit, unit))
                uc.mem_write(r1 + i * unit, value)
        else:
            raise SystemExit('BIOS call %02x at %08x' % (number, pc))

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | (1 if thumb else 0))
        for i, v in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=100_000_000)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def play(self, module, mode=MODE_LOOP, song=0):
        self.call(self.addr.play, (module, mode, song))

    def frame(self):
        """Runs the worker once and returns the frame's output, left and right, as signed 8-bit points."""
        self.call(self.addr.worker, thumb=False)
        n = self.addr.frame_samples
        return self.read(LEFT, n), self.read(RIGHT, n)

    def records(self, module):
        """Returns the mixer's variables, the player's record and each channel's, as the trace prints them."""
        a = self.addr
        timer = self.read(a.timer, 4)
        variables = self.read(a.master, 4) + timer + self.read(a.music_volume, 4) + self.read(a.plays, 4)
        player = bytearray(self.read(a.player, PLAYER_SIZE))
        player[PERIOD_POINTERS[0]:PERIOD_POINTERS[1]] = bytes(PERIOD_POINTERS[1] - PERIOD_POINTERS[0])
        channels = self.read(module, 1)[0]
        lines = [('g', variables), ('p', bytes(player))]
        for c in range(channels):
            lines.append(('c%d' % c, self.read(a.player + PLAYER_SIZE + CHANNEL_SIZE * c, CHANNEL_SIZE)))
        for m in range(MIX_CHANNELS):
            lines.append(('m%d' % m, self.read(a.mixer + MIX_SIZE * m, MIX_SIZE)))
        return lines

    def playing(self):
        return struct.unpack('<I', self.read(self.addr.player + 4, 4))[0] != 0


def song_count(rom, module):
    """Returns the number of songs that a module's orders separate with +++, or 0 if they have no marker. A song is a
    run of orders between markers. agbsf2 counts them the same way."""
    count = rom[module + 1 - ROM_BASE]
    orders = rom[module + 3 - ROM_BASE:module + 3 + count - ROM_BASE]
    songs, in_song = 0, False
    for o in orders:
        if o < ORDER_SKIP and not in_song:
            songs += 1
        in_song = o < ORDER_SKIP
    return min(songs, 64) if ORDER_SKIP in orders else 0


def song_plays(rom, modules):
    """Returns how krapPlay() plays each piece that agbsf2 numbers: a (module, mode, song) for each number. `modules`
    maps the numbers to the modules' addresses. A module without markers plays in loop mode. Each song of a module with
    markers plays in song mode, in the order of the numbers."""
    plays, seen = {}, {}
    for n in sorted(modules):
        module = modules[n]
        song = seen.get(module, 0)
        seen[module] = song + 1
        plays[n] = (module, MODE_LOOP | MODE_SONG, song) if song_count(rom, module) else (module, MODE_LOOP, 0)
    return plays


def trace(rom, module, frames, mode=MODE_LOOP, song=0):
    emu = KrawallEmulator(rom)
    emu.play(module, mode, song)
    last = {}
    for f in range(frames):
        emu.frame()
        for name, record in emu.records(module):
            if record != last.get(name, bytes(len(record))):
                print('%d %s %s' % (f, name, record.hex()))
                last[name] = record
        if not emu.playing():
            break


def render(rom, module, frames, path, mode=MODE_LOOP, song=0):
    emu = KrawallEmulator(rom)
    emu.play(module, mode, song)
    out = bytearray()
    for _ in range(frames):
        left, right = emu.frame()
        for l, r in zip(left, right):
            out += bytes(((l + 128) & 0xFF, (r + 128) & 0xFF))
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(1)
        w.setframerate(16384)
        w.writeframes(bytes(out))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('module', help="the module's address, in hex")
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--song', type=int, help='play this song of a module that separates songs with +++, in song mode')
    a = p.parse_args()
    rom = load_rom(a.rom)
    module = int(a.module, 16)
    mode, song = (MODE_LOOP | MODE_SONG, a.song) if a.song is not None else (MODE_LOOP, 0)
    if a.command == 'trace':
        trace(rom, module, a.frames, mode, song)
        return
    if not a.out:
        p.error('render needs an output file')
    render(rom, module, a.frames, a.out, mode, song)


if __name__ == '__main__':
    main()
