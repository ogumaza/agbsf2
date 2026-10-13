#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Copy a ROM and replace song 0 with a test song for commands and modes the original songs may not use.

    test_song.py ROM OUT.gba

The test song takes the place of song 0, in free space at the end of the ROM, and plays the game's instruments.
It uses mono mode (controllers 126 and 127), which makes a channel play every note in its first slot, the note on with
command number 4, channel pressure (command 9), the command that does nothing (12), and delays of one, two and three
bytes. It also uses controllers 20-23, which set the attack, decay, sustain and release of a channel's notes in the
revision of Donkey Kong Country 3 and do nothing in the others: an attack of several frames, settings that change
while a note plays, a sustain of 0, and a program change, which takes them away. Check it against the driver with

    python tools/rare/compare_trace.py OUT.gba AGBSF2 --songs 0
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom


def thumb_literal(rom, at):
    insn = struct.unpack_from('<H', rom, at - ROM_BASE)[0]
    return struct.unpack_from('<I', rom, ((at + 4) & ~3) + (insn & 0xFF) * 4 - ROM_BASE)[0]


def find_driver(rom):
    """Returns the tune table and whether commands give their channel in their high nibble. Both come from where
    agbsf2's detection finds them: the init's copy of the driver's code and the table setup after it."""
    for o in range(0, len(rom) - 12, 2):
        h = struct.unpack_from('<6H', rom, o)
        if (h[0] >> 8, h[1] >> 8, h[2] >> 8, h[3], h[4] >> 8, h[5]) != (0x49, 0x4A, 0x4B, 0x681B, 0x4C, 0x42A3):
            continue
        copy = ROM_BASE + o
        code = thumb_literal(rom, copy)
        size = struct.unpack_from('<I', rom, thumb_literal(rom, copy + 4) - ROM_BASE)[0]
        driver = rom[code - ROM_BASE:code - ROM_BASE + size]
        nibble = bytes.fromhex('0100dbe42012a0e1f000c0e3') in driver
        for at in range(copy + 12, copy + 0x60, 2):
            if struct.unpack_from('<H', rom, at - ROM_BASE + 4)[0] == 0x6001:
                return thumb_literal(rom, at + 2), nibble
    raise SystemExit("no Rare sound driver found")


class Track:
    """A track's commands, in either format."""

    def __init__(self, nibble):
        self.nibble = nibble
        self.data = bytearray()

    def command(self, number, channel=None, args=b''):
        if self.nibble:
            self.data.append(number | ((channel or 0) << 4))
        else:
            self.data.append(number)
            if channel is not None:
                self.data.append(channel)
        self.data += args
        return self

    def tempo(self, micros):
        return self.command(0, None, micros.to_bytes(3, 'little'))

    def wait(self, ticks, size=None):
        size = size or (1 if ticks < 0x100 else 2 if ticks < 0x10000 else 3)
        return self.command(size, None, ticks.to_bytes(size, 'little'))

    def on(self, ch, key, velocity, number=5):
        return self.command(number, ch, bytes([key, velocity]))

    def off(self, ch, key):
        return self.command(6, ch, bytes([key, 0x40]))

    def control(self, ch, controller, value):
        return self.command(7, ch, bytes([controller, value]))

    def program(self, ch, program):
        return self.command(8, ch, bytes([program]))

    def pressure(self, ch, value):
        return self.command(9, ch, bytes([value]))

    def bend(self, ch, value):
        return self.command(10, ch, value.to_bytes(2, 'little'))

    def nop(self):
        return self.command(12)

    def end(self):
        return bytes(self.command(11).data)


def instrument_programs(rom, header):
    """Returns two lists of programs from song `header`'s bank: looped sample instruments and all other instruments."""
    program_map, table = struct.unpack_from('<II', rom, header + 12 - ROM_BASE)
    looped, others = [], []
    for program in range(128):
        index = rom[program_map - ROM_BASE + program]
        if index == 0xFF:
            continue
        inst = struct.unpack_from('<I', rom, table + 4 * index - ROM_BASE)[0]
        kind, mode = struct.unpack_from('<II', rom, inst - ROM_BASE)
        (looped if kind in (0x20, 0x21) and mode in (2, 4) else others).append(program)
    return looped, others


def test_tracks(nibble, sustained, other):
    t0 = Track(nibble).tempo(500000).wait(1920).wait(0x10000, 3).end()

    # Mono mode: every note takes the channel's first slot, cutting the one before. Then poly mode again, with the
    # note on of command number 4, channel pressure and the command that does nothing.
    t1 = Track(nibble).program(0, sustained).control(0, 7, 100)
    t1.control(0, 126, 0).on(0, 60, 100).wait(30).on(0, 64, 90).wait(30).off(0, 60).wait(15).off(0, 64)
    t1.on(0, 67, 80).wait(20).on(0, 69, 80).wait(20).off(0, 69).wait(10).control(0, 127, 0)
    t1.on(0, 60, 100, number=4).on(0, 64, 100).pressure(0, 50).nop().wait(40).off(0, 60).off(0, 64)
    t1.wait(30).bend(0, 0x3000).on(0, 72, 110).wait(0x40, 2).bend(0, 0x2000).off(0, 72)

    # Mono mode on a second channel, with notes of another instrument, while the first channel plays on.
    t2 = Track(nibble).program(1, other).wait(10).control(1, 126, 0)
    t2.on(1, 48, 127).wait(12).on(1, 50, 127).wait(12).on(1, 52, 127).wait(40).off(1, 52).control(1, 127, 0)
    t2.on(1, 48, 127).on(1, 50, 127).wait(40).off(1, 50).off(1, 48)

    # The envelope controllers on a third channel, at 16 ticks a frame: an attack of 7 frames, a decay of 10 to a
    # sustain level of 40/128 and a release of 6. Then an instant attack, with a decay and a sustain of 0 set in the
    # frame before the decay starts, which stops the note, and a release that changes while a note plays. A program
    # change takes them all away. A sustain level comes with a decay each time, since the driver reads a decay without
    # its setting from a register that holds whatever its code left there.
    t3 = Track(nibble).program(2, sustained).control(2, 7, 100)
    t3.control(2, 20, 6).control(2, 21, 9).control(2, 22, 40).control(2, 23, 5)
    t3.on(2, 60, 100).wait(320).off(2, 60).wait(160)
    t3.control(2, 20, 0).on(2, 62, 100).wait(16).control(2, 21, 3).control(2, 22, 0).wait(160).off(2, 62).wait(16)
    t3.control(2, 22, 60).on(2, 64, 100).wait(48).control(2, 23, 1).wait(16).off(2, 64).wait(48)
    t3.program(2, sustained).on(2, 65, 100).wait(64).off(2, 65)
    return [t0, t1.end(), t2.end(), t3.end()]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    rom = bytearray(load_rom(sys.argv[1]))
    table, nibble = find_driver(rom)
    header = struct.unpack_from('<I', rom, table - ROM_BASE)[0]
    looped, others = instrument_programs(rom, header)
    if not looped:
        raise SystemExit("the first song's bank has no sample instrument with a loop")
    tracks = test_tracks(nibble, looped[0], (others or looped)[0])

    # Free space at the end of the ROM: a run of 0xFF or 0x00 bytes.
    blob = bytearray()
    needed = sum(len(t) for t in tracks) + 4 * len(tracks) + 20 + 16
    end = len(rom)
    start = end
    while start > 0 and rom[start - 1] == rom[-1] and rom[-1] in (0x00, 0xFF):
        start -= 1
    if end - start < needed:
        rom += bytes(needed)
        start = end
    base = ROM_BASE + ((start + 3) & ~3)

    # The tracks, their list and the header, with the first song's program map and instruments.
    addresses = []
    for t in tracks:
        addresses.append(base + len(blob))
        blob += t
    while len(blob) % 4:
        blob.append(0)
    track_list = base + len(blob)
    blob += struct.pack('<%dI' % len(tracks), *addresses)
    new_header = base + len(blob)
    program_map, instruments = struct.unpack_from('<II', rom, header + 12 - ROM_BASE)
    blob += struct.pack('<5I', len(tracks), 480, track_list, program_map, instruments)
    rom[base - ROM_BASE:base - ROM_BASE + len(blob)] = blob
    struct.pack_into('<I', rom, table - ROM_BASE, new_header)
    Path(sys.argv[2]).write_bytes(bytes(rom))
    print('wrote %s: song 0 is the test song, at 0x%08X' % (sys.argv[2], new_header))


if __name__ == '__main__':
    main()
