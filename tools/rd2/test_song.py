#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Copy a ROM and replace sequence 0 with a test sequence for commands and voices the original sequences may not use.

    test_song.py ROM OUT.gba

The test sequence takes the place of sequence 0's data, and a bank of test instruments goes in free space at the end
of the ROM, as bank 1, which plays the game's first sample set. A ROM that ends before the free space is padded with
0xFF up to it. The sequence's list of banks starts with the test bank, and its second entry is the game's music bank.

Between its 10 tracks, the sequence uses every command apart from CA, and in the Super Mario Advance 2 revision of the
driver, which has no player volume, EA: bends and their range, the echo send, slides of each kind, legato in which a
note changes the instrument, notes that wait for their length, the transpose, calls inside calls, a track that another
starts with F8, a switch of bank, priorities that make notes take each other's voices, and notes of no length. Its
instruments cover every kind of region: samples, both squares with and without a table of duties and with the frequency
sweep, the wave voice, noise with and without a table of widths, a drum kit with pans of its own, an instrument with a
sample for each key, and a key split. Check it against the driver with

    python tools/rd2/compare_trace.py OUT.gba AGBSF2 --songs 0
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

# The test data's address and size: free space at the end of the ROM, which has to hold 0xFF.
FREE = 0x08700000
FREE_SIZE = 0x8000

# The start of the driver's init routine, as Thumb halfwords (None for any).
INIT_PATTERN = [0xB530, None, 0x6008, None, 0x2000, 0x7008, 0x2080, 0x7008, 0x3904]

# The start of the routine that works out a sample voice's level in the Super Mario Advance 2 revision, whose tempo
# command takes a byte and which has no player volume (EA).
MARIO_PATTERN = [0xB530, 0x1C05, 0x7868, 0x2801, None, 0x7A6C, 0x0224]


def u32(rom, address):
    return struct.unpack_from('<I', rom, address - ROM_BASE)[0]


def find(rom, pattern):
    """Returns the offset of the first match of a pattern of Thumb halfwords, or None."""
    for o in range(0, len(rom) - 2 * len(pattern), 2):
        if all(p is None or struct.unpack_from('<H', rom, o + 2 * i)[0] == p for i, p in enumerate(pattern)):
            return o
    return None


def find_settings(rom):
    """Returns the game's settings for the driver: the literal of the ldr r0 before the call to the driver's init."""
    init = find(rom, INIT_PATTERN)
    if init is None:
        raise SystemExit('no Nintendo R&D2 sound driver found')
    target = ROM_BASE + init
    for o in range(0, len(rom) - 6, 2):
        hi, lo = struct.unpack_from('<HH', rom, o + 2)
        if hi >> 11 != 0x1E or lo >> 11 != 0x1F:
            continue
        offset = ((hi & 0x7FF) << 12 | (lo & 0x7FF) << 1)
        if offset & 0x400000:
            offset -= 0x800000
        if ROM_BASE + o + 2 + 4 + offset == target and struct.unpack_from('<H', rom, o)[0] >> 8 == 0x48:
            insn = struct.unpack_from('<H', rom, o)[0]
            return u32(rom, ((ROM_BASE + o + 4) & ~3) + (insn & 0xFF) * 4)
    raise SystemExit("no call to the driver's init found")


class Track:
    """A track's commands, with labels for the addresses that F0, F4 and F8 go to."""

    def __init__(self, mario):
        self.data = bytearray()
        self.labels = {}
        self.fixups = []  # (position, label): a halfword to fill in with a label's offset in the sequence
        self.mario = mario  # the Super Mario Advance 2 revision, whose tempo is a byte and which has no player volume

    def raw(self, *values):
        self.data += bytes(values)
        return self

    def label(self, name):
        self.labels[name] = len(self.data)
        return self

    @staticmethod
    def length(value):
        return bytes([value]) if value < 0x80 else bytes([0x80 | value >> 8, value & 0xFF])

    def note(self, key, length=None, velocity=None):
        if length is None:
            return self.raw(key)
        self.data += bytes([0x60 + key]) + self.length(length) + bytes([velocity])
        return self

    def wait(self, ticks=None):
        if ticks is None:
            return self.raw(0xC0)
        self.data += bytes([0xC1]) + self.length(ticks)
        return self

    def tempo(self, tempo):
        self.data += bytes([0xE4]) + (bytes([tempo]) if self.mario else self.length(tempo))
        return self

    def volume(self, volume):
        """The player's volume, which the Super Mario Advance 2 revision doesn't have."""
        return self if self.mario else self.raw(0xEA, volume)

    def go(self, op, label, *before):
        self.raw(op, *before)
        self.fixups.append((len(self.data), label))
        self.data += b'\0\0'
        return self


class Bank:
    """A bank: a table of instrument offsets, then the instruments, envelopes and tables, all from the bank's start."""

    def __init__(self, count):
        self.data = bytearray(2 * count)

    def add(self, data):
        while len(self.data) % 2:
            self.data.append(0)
        at = len(self.data)
        self.data += data
        return at

    def instrument(self, index, data):
        struct.pack_into('<H', self.data, 2 * index, self.add(data))


def region(kind, flags, arg, envelope, release, tuning, sweep=None):
    data = struct.pack('<BBHHBB', kind, flags, arg, envelope, release, tuning)
    return data + (bytes([sweep]) if sweep is not None else b'')


def envelope(*points):
    return b''.join(struct.pack('<hh', frames, level) for frames, level in points) + struct.pack('<hh', -1, 0)


def build_bank():
    bank = Bank(16)
    slow = bank.add(envelope((3, 32767), (20, 20000), (40, 0)))
    held = bank.add(envelope((1, 32767)))
    decay = bank.add(envelope((4, 32767), (30, 16000)))
    fade = bank.add(envelope((1, 20000), (60, 0)))
    steps = bank.add(envelope((2, 10000), (2, 32767), (5, 0)))
    duties = bank.add(struct.pack('<H', 4) + bytes([0, 1, 2, 3]))
    widths = bank.add(struct.pack('<H', 3) + bytes([1, 0, 1]))
    wave = bank.add(bytes.fromhex('0123456789abcdeffedcba9876543210'))

    # Samples from the game's first sample set.
    bank.instrument(0, region(0, 0, 28, slow, 0x46, 0x37))
    bank.instrument(1, region(0, 0x10, 5, held, 0xFF, 0x11))
    bank.instrument(2, region(1, 0, 2, decay, 0x60, 0x30, sweep=8))
    bank.instrument(3, region(1, 1, duties, held, 0x20, 0x30, sweep=0x17))
    bank.instrument(4, region(2, 0, 1, fade, 0x00, 0x30))
    bank.instrument(5, region(2, 1, duties, steps, 0xE0, 0x30))
    bank.instrument(6, region(3, 0, wave, decay, 0x40, 0x30))
    bank.instrument(7, region(4, 0, 0, decay, 0x40, 0x30))
    bank.instrument(8, region(4, 1, widths, steps, 0x80, 0x30))

    # A drum kit from note 36: a sample panned left, a square panned right, noise in the centre and a sample tuned down.
    drums = [bank.add(region(0, 0, 38, held, 0, 0x30)), bank.add(region(1, 0, 3, decay, 0x40, 0x30, sweep=8)),
             bank.add(region(4, 0, 0, decay, 0x20, 0x30)), bank.add(region(0, 0, 27, steps, 0x10, 0x24))]
    kit = bank.add(b''.join(struct.pack('<HBB', d, pan, 0) for d, pan in zip(drums, (0x20, 0x60, 0x40, 0x48))))
    bank.instrument(9, struct.pack('<BBHBBBB', 0x10, 0, kit, 36, 0, 0, 0))

    # An instrument with a sample for each key, and a key split between a sample and the wave.
    keys = bank.add(b''.join(struct.pack('<H', (3, 27, 38, 22)[k % 4]) for k in range(128)))
    bank.instrument(10, struct.pack('<BBHBBBB', 0x11, 0, keys, 0, 0, 0, 0))
    low = bank.add(region(0, 0, 22, decay, 0x50, 0x3C))
    high = bank.add(region(3, 0, wave, held, 0x30, 0x30))
    split = bank.add(struct.pack('<BBH', 40, 0, low) + struct.pack('<BBH', 255, 0, high))
    bank.instrument(11, struct.pack('<BBHBBBB', 0x12, 0, split, 0, 0, 0, 0))
    return bank.data


def build_tracks(mario):
    tracks = [Track(mario) for _ in range(10)]

    # 0: tempo and the player's volume, and F8, which starts track 9 with this track's settings.
    t = tracks[0]
    t.tempo(120).volume(100).wait(48).tempo(200).wait(48).volume(60).tempo(160).raw(0xC3, 0x30, 0xE0, 0x70)
    t.go(0xF8, 'nine', 9).wait(96).volume(0x80).tempo(140).wait(200).raw(0xFF)

    # 1: a sample with bends, the echo send, the LFO, slides of each kind, legato, notes that wait, the transpose and
    # calls inside calls, then a loop.
    t = tracks[1]
    t.raw(0xC2, 0, 0xE0, 0xA0, 0xC3, 0x30, 0xE3, 0x7F).note(40, 20, 100).wait(24)
    t.raw(0xE2, 4, 0xE1, 40).note(42, 10, 90).wait(12).raw(0xE1, 0xC4).note(44, 10, 120).wait(12).raw(0xE1, 0)
    t.raw(0xE5, 4, 0xE6, 0x30, 0xE7, 0x40).note(47, 30, 110).wait(32).raw(0xE7, 0)
    t.raw(0xD2, 52, 0x80).note(45, 20, 100).wait(20)
    t.raw(0xD0, 38, 0xC0).note(45, 20, 100).wait(20)
    t.raw(0xD5, 40, 0x40, 3).note(43, 8, 100).wait(8).note(47, 8, 100).wait(8).raw(0xE8).note(49, 8, 100).wait(8)
    t.raw(0xC2, 1, 0xC5).note(40, 6, 100).wait(6).note(42, 6, 100).wait(6).raw(0xC2, 0).note(44, 12, 100).wait(12)
    t.raw(0xC6).wait(4)
    t.raw(0xC8).note(40, 6, 100).note(43, 6, 100).note(47, 12, 100).raw(0xC9)
    t.raw(0xE9, 12).note(40, 8, 100).wait(8).raw(0xE9, 0xF4).note(52, 8, 100).wait(8).raw(0xE9, 0)
    t.label('loop').go(0xF4, 'outer').note(45, 0, 80).wait(4).go(0xF0, 'loop')
    t.label('outer').note(41, 6, 90).wait(6).go(0xF4, 'inner').note(43).wait().raw(0xFF)
    t.label('inner').note(46, 6, 90).wait(6).raw(0xFF)

    # 2: square 1 in each pan, with a bend, the LFO, a slide and notes of no length.
    t = tracks[2]
    t.raw(0xC2, 2, 0xC3, 0x40).note(48, 10, 127).wait(12).raw(0xC3, 0x20).note(52, 10, 100).wait(12)
    t.raw(0xC3, 0x60).note(55, 10, 80).wait(12).raw(0xC3, 0x40, 0xE2, 2, 0xE1, 0x50).note(60, 10, 127).wait(12)
    t.raw(0xE1, 0xB0).note(60, 10, 127).wait(12).raw(0xE1, 0, 0xE5, 2, 0xE6, 0x40, 0xE7, 0x60)
    t.note(57, 20, 120).wait(24).raw(0xE7, 0, 0xD0, 50, 0x80).note(62, 16, 120).wait(16)
    t.note(64, 0, 120).wait(8).note(65, 1, 120).wait(8).raw(0xC2, 3).note(50, 30, 120).wait(32)
    t.go(0xF0, 'two')
    t.label('two').note(53, 12, 100).wait(48).go(0xF0, 'two')

    # 3: square 2, from a table of duties and from a duty of its own, on the same voice.
    t = tracks[3]
    t.raw(0xC2, 5, 0xC3, 0x50).note(48, 20, 127).wait(24).raw(0xC2, 4).note(50, 20, 100).wait(24)
    t.raw(0xC2, 5).note(52, 40, 110).wait(44).raw(0xC2, 4, 0xC3, 0x10).note(53, 8, 60).wait(60).raw(0xFF)

    # 4: the wave voice, which fades by itself after its release, panned and centred.
    t = tracks[4]
    t.raw(0xC2, 6).note(48, 12, 127).wait(30).raw(0xC3, 0x70).note(55, 8, 90).wait(40).raw(0xC3, 0x40)
    t.note(60, 4, 50).wait(30).note(36, 20, 127).wait(60).raw(0xFF)

    # 5: noise, with and without a table of widths.
    t = tracks[5]
    t.raw(0xC2, 7).note(40, 6, 127).wait(10).note(50, 6, 100).wait(10).raw(0xC2, 8).note(45, 20, 120).wait(24)
    t.raw(0xC2, 7, 0xC3, 0x10).note(60, 30, 127).wait(40).raw(0xFF)

    # 6: the drum kit, the instrument with a sample for each key, the key split, and the game's bank through C7.
    t = tracks[6]
    t.raw(0xC2, 9)
    for key in (36, 37, 38, 39):
        t.note(key, 8, 110).wait(10)
    t.raw(0xC2, 10).note(40, 10, 100).wait(10).note(41, 10, 100).wait(10).note(42, 10, 100).wait(10)
    t.raw(0xC2, 11).note(36, 10, 100).wait(10).note(48, 10, 100).wait(10)
    t.raw(0xC7, 1, 0xC2, 10).note(50, 20, 100).wait(24).raw(0xC7, 0, 0xC2, 1).note(41, 10, 100).wait(10).raw(0xFF)

    # 7 and 8: chords at a low priority, which notes at a high one take voices from.
    t = tracks[7]
    t.raw(0xC4, 1, 0xC2, 1).wait(60)
    for key in (36, 40, 43, 47, 50, 53, 57):
        t.note(key, 90, 80)
    t.wait(100).raw(0xFF)
    t = tracks[8]
    t.raw(0xC4, 9, 0xC2, 0).wait(70).note(60, 20, 127).note(64, 20, 127).wait(30).raw(0xC4, 0)
    t.note(67, 20, 127).wait(40).raw(0xFF)

    # 9: started by track 0, with its settings.
    t = tracks[9]
    t.label('nine').raw(0xC2, 0).note(52, 12, 100).wait(14).note(55, 12, 100).wait(14).raw(0xFF)
    return tracks


def build_sequence(mario):
    """Returns the sequence's bytes: the header, then each track."""
    tracks = build_tracks(mario)
    data = bytearray(struct.pack('<bB', len(tracks), 0) + b'\0\0' * len(tracks))
    starts = []
    for t in tracks:
        starts.append(len(data))
        data += t.data
    labels = {name: starts[i] + at for i, t in enumerate(tracks) for name, at in t.labels.items()}
    for i, t in enumerate(tracks):
        # Track 9 has no start of its own: track 0 starts it.
        struct.pack_into('<H', data, 2 + 2 * i, 0 if i == 9 else starts[i])
        for at, name in t.fixups:
            struct.pack_into('<H', data, starts[i] + at, labels[name])
    return data


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    rom = bytearray(load_rom(sys.argv[1]))
    rom += b'\xFF' * max(0, FREE + FREE_SIZE - ROM_BASE - len(rom))
    if any(b != 0xFF for b in rom[FREE - ROM_BASE:FREE - ROM_BASE + FREE_SIZE]):
        raise SystemExit('the ROM has no free space at 0x%08X' % FREE)

    settings = find_settings(rom)
    banks, sequences = u32(rom, settings + 4), u32(rom, settings + 8)
    bank_sets, bank_lists = u32(rom, settings + 16), u32(rom, settings + 20)

    # The test bank replaces bank 1, and plays sample set 0.
    bank_at = FREE
    bank = build_bank()
    rom[bank_at - ROM_BASE:bank_at - ROM_BASE + len(bank)] = bank
    struct.pack_into('<I', rom, banks + 4 - ROM_BASE, bank_at - banks)
    struct.pack_into('<H', rom, bank_sets + 2 - ROM_BASE, 0)

    # The test sequence replaces sequence 0's data, and its list of banks starts with bank 1.
    list_at = bank_lists + u32(rom, bank_lists)
    struct.pack_into('<H', rom, list_at - ROM_BASE, 1)
    sequence_at = sequences + u32(rom, sequences)
    room = sequences + u32(rom, sequences + 4) - sequence_at
    sequence = build_sequence(find(rom, MARIO_PATTERN) is not None)
    if len(sequence) > room:
        raise SystemExit('the test sequence needs %d bytes, and sequence 0 has %d' % (len(sequence), room))
    rom[sequence_at - ROM_BASE:sequence_at - ROM_BASE + len(sequence)] = sequence

    Path(sys.argv[2]).write_bytes(bytes(rom))
    print('test sequence at 0x%08X, test bank at 0x%08X' % (sequence_at, bank_at))


if __name__ == '__main__':
    main()
