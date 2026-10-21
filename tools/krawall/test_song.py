#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Copy a Krawall game and add test modules for player features its music may not use.

    test_song.py ROM OUT.gba

The copy has three modules after the game's data. The module scan of `agbsf2 --info` finds them and lists them after
the game's modules:

    S3M          Amiga periods and samples, and every effect of the effect column: speed and tempo, jumps, breaks and
                 the pattern loop, the volume, channel and global volume slides with their fine forms, the S3M
                 portamentos with their fine and extra fine forms, the slide to a note with and without glissando,
                 the vibratos, the tremolo, the tremor, the arpeggio, the offset, the pan and its slides, the panbrello,
                 the retrig with each change of volume, the waves, the note cut and delay, the note off and the mark
    S3M fast     the same patterns, with S3M's fast volume slides
    XM           linear frequencies and instruments: envelopes of volume and pan with sustain points and loops, the
                 fade after the release, the instrument's vibrato, the XM volume slides and portamentos, and the
                 volume column's effects

They play three samples of their own: one that loops forwards, one that loops back and forth, and one that doesn't
loop, and the XM module has three instruments. The copy has new tables of samples and instruments, the game's samples
followed by the new ones and the new instruments, and the player's literals point to them, so the game's modules play
as they do in the game. Run compare_trace.py and compare_notes.py on the copy with --songs for the new modules.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom


class Game:
    """BDGE (Digimon Racing)."""
    samples = 0x087BF3F4      # the game's table of samples
    sample_literals = (0x08037CC0, 0x08037F4C, 0x08038860)  # the player's literals that give the table
    instrument_literals = (0x08037CBC,)  # the player's literal that gives the table of instruments, which is empty


GAMES = {b'BDGE': Game}

# The effects of the effect column.
SPEED, BPM, SPEED_BPM, JUMP, BREAK = 1, 2, 3, 4, 5
VOLSLIDE_S3M, VOLSLIDE_XM, VOLSLIDE_DOWN_FINE, VOLSLIDE_UP_FINE = 6, 7, 8, 9
PORTA_DOWN_XM, PORTA_DOWN_S3M, PORTA_DOWN_FINE, PORTA_DOWN_EFINE = 10, 11, 12, 13
PORTA_UP_XM, PORTA_UP_S3M, PORTA_UP_FINE, PORTA_UP_EFINE = 14, 15, 16, 17
VOLUME, PORTA_NOTE, VIBRATO, TREMOR, ARPEGGIO, VOLSLIDE_VIBRATO, VOLSLIDE_PORTA = 18, 19, 20, 21, 22, 23, 24
CHANNEL_VOLUME, CHANNEL_VOLSLIDE, OFFSET, PANSLIDE, RETRIG, TREMOLO, FINE_VIBRATO = 25, 26, 27, 28, 29, 30, 31
GLOBAL_VOLUME, GLOBAL_VOLSLIDE, PAN, PANBRELLO, MARK, GLISSANDO = 32, 33, 34, 35, 36, 37
WAVE_VIBRATO, WAVE_TREMOLO, WAVE_PANBRELLO, PATTERN_LOOP, NOTE_CUT, NOTE_DELAY = 38, 39, 40, 43, 44, 45
VOLSLIDE_VIBRATO_XM, VOLSLIDE_PORTA_XM = 49, 50

OFF = 0x7F


def note(name):
    """Returns the pattern's note for a name such as C-4: 1 for C-0."""
    names = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-']
    return 1 + names.index(name[:2]) + 12 * int(name[2:])


def pattern(rows, events):
    """Returns a pattern's bytes: the index of every 4th row's data, the row count and the rows. Each event is (row,
    channel, note, instrument, volume byte, effect, operand), with None for what it leaves out."""
    by_row = {}
    for e in events:
        by_row.setdefault(e[0], []).append(e[1:])
    data, index = bytearray(), []
    for row in range(rows):
        if row % 4 == 0:
            index.append(len(data))
        for channel, n, instrument, volume, effect, op in sorted(by_row.get(row, [])):
            follow = channel
            if n is not None:
                follow |= 0x20
            if volume is not None:
                follow |= 0x40
            if effect is not None:
                follow |= 0x80
            data.append(follow)
            if n is not None:
                data += bytes((n << 1 | (instrument or 0) >> 8, (instrument or 0) & 0xFF))
            if volume is not None:
                data.append(volume)
            if effect is not None:
                data += bytes((effect, op))
        data.append(0)
    index += [index[-1]] * (16 - len(index))
    return struct.pack('<16H', *index) + bytes((rows,)) + bytes(data)


def module(channels, orders, speed, tempo, instruments=False, linear=False, fast=False, restart=0,
           global_volume=64, pans=None):
    """Returns a module's header, without the patterns' addresses at its end."""
    header = bytearray(0x16C)
    header[0:3] = bytes((channels, len(orders), restart))
    header[3:3 + len(orders)] = bytes(orders)
    header[3 + len(orders):0x103] = b'\xff' * (0x100 - len(orders))
    for c, p in enumerate(pans or [0] * channels):
        header[0x103 + c] = p & 0xFF
    header[0x163:0x16B] = bytes((global_volume, speed, tempo, instruments, linear, fast, 0, 0))
    return header


def sample(points, loop, loop_length, fine_tune, relative_note, volume, pan, address):
    """Returns a sample's header and points, at `address`."""
    end = address + 0x12 + len(points)
    return struct.pack('<IIIbbBbBB', loop_length, end, 8363, fine_tune, relative_note, volume, pan, loop, 0) + \
        bytes(points)


def wave_points(length, period, shape):
    """Returns 8-bit unsigned points of a wave, with 0x80 for silence."""
    out = []
    for i in range(length):
        x = (i % period) / period
        v = math.sin(2 * math.pi * x) if shape == 'sine' else (1 if x < 0.5 else -1) * (1 - i / length)
        out.append(max(0, min(255, 128 + int(round(100 * v)))))
    return out


def envelope(points, sustain=0, loop_start=0, flags=0):
    """Returns an envelope's 12 points, each its level (0-64) at a tick, with the step to the next, and its last point,
    sustain point, loop start and flags (1 on, 2 sustain, 4 loop)."""
    nodes = bytearray()
    for i in range(12):
        tick, level = points[min(i, len(points) - 1)]
        inc = 0
        if i + 1 < len(points):
            next_tick, next_level = points[i + 1]
            inc = ((next_level - level) * 256 // max(1, next_tick - tick)) & 0xFFFF
        nodes += struct.pack('<HH', level << 9 | tick, inc)
    return nodes + bytes((len(points) - 1, sustain, loop_start, flags))


def instrument(note_samples, volume_env, pan_env, fade, vibrato_depth, vibrato_rate):
    out = struct.pack('<96H', *note_samples) + volume_env + pan_env
    return out + struct.pack('<HBBBB', fade, 0, 0, vibrato_depth, vibrato_rate)


def s3m_patterns(fwd, bidi, one):
    """The S3M modules' patterns: every effect of the effect column, a few rows apart."""
    i = lambda s: s + 1  # a sample's instrument number
    v = lambda x: 0x10 + x
    first = [
        (0, 0, note('C-4'), i(fwd), v(64), SPEED, 3), (0, 1, note('E-4'), i(one), None, BPM, 0x7D),
        (0, 2, note('G-4'), i(bidi), v(40), PAN, 0x20), (0, 3, note('C-5'), i(fwd), v(50), SPEED_BPM, 0x03),
        (1, 0, None, None, None, VOLSLIDE_S3M, 0x04), (1, 1, None, None, None, VOLSLIDE_S3M, 0x40),
        (1, 2, None, None, None, VIBRATO, 0x48), (1, 3, None, None, None, TREMOR, 0x21),
        (2, 0, None, None, None, VOLSLIDE_S3M, 0xF2), (2, 1, None, None, None, VOLSLIDE_S3M, 0x2F),
        (2, 2, None, None, None, FINE_VIBRATO, 0x83), (2, 3, None, None, None, ARPEGGIO, 0x47),
        (3, 0, None, None, None, PORTA_DOWN_S3M, 0x08), (3, 1, None, None, None, PORTA_UP_S3M, 0x08),
        (3, 2, None, None, None, TREMOLO, 0x46), (3, 3, None, None, None, ARPEGGIO, 0x00),
        (4, 0, None, None, None, PORTA_DOWN_S3M, 0xF4), (4, 1, None, None, None, PORTA_UP_S3M, 0xE3),
        (4, 2, None, None, None, PANBRELLO, 0x44), (4, 3, note('C-4'), i(one), v(48), RETRIG, 0x23),
        (5, 0, note('D-4'), i(fwd), None, PORTA_NOTE, 0x10), (5, 1, None, None, None, GLISSANDO, 0x01),
        (5, 2, None, None, None, WAVE_VIBRATO, 0x01), (5, 3, None, None, None, RETRIG, 0xA2),
        (6, 0, None, None, None, PORTA_NOTE, 0x00), (6, 1, note('G-4'), i(one), None, PORTA_NOTE, 0x08),
        (6, 2, None, None, None, VIBRATO, 0x00), (6, 3, None, None, None, RETRIG, 0x62),
        (7, 0, None, None, None, VOLSLIDE_PORTA, 0x02), (7, 1, None, None, None, VOLSLIDE_VIBRATO, 0x20),
        (7, 2, None, None, None, CHANNEL_VOLUME, 0x20), (7, 3, None, None, None, RETRIG, 0x72),
        (8, 0, note('C-4'), i(fwd), None, OFFSET, 0x02), (8, 1, None, None, None, OFFSET, 0x01),
        (8, 2, None, None, None, CHANNEL_VOLSLIDE, 0x04), (8, 3, None, None, None, RETRIG, 0xE2),
        (9, 0, None, None, None, PANSLIDE, 0x40), (9, 1, None, None, None, PANSLIDE, 0x03),
        (9, 2, None, None, None, CHANNEL_VOLSLIDE, 0x30), (9, 3, None, None, None, RETRIG, 0xF2),
        (10, 0, None, None, None, PANSLIDE, 0x2F), (10, 1, None, None, None, PANSLIDE, 0xF3),
        (10, 2, None, None, None, GLOBAL_VOLUME, 0x30), (10, 3, None, None, None, RETRIG, 0x12),
        (11, 0, None, None, None, GLOBAL_VOLSLIDE, 0x02), (11, 2, None, None, None, WAVE_TREMOLO, 0x02),
        (11, 3, None, None, None, WAVE_PANBRELLO, 0x03),
        (12, 0, note('E-4'), i(fwd), None, NOTE_CUT, 0x01), (12, 1, note('E-4'), i(one), None, NOTE_DELAY, 0x02),
        (12, 2, OFF, None, None, None, None), (12, 3, None, None, None, MARK, 0x05),
        (13, 0, note('F-4'), i(fwd), None, NOTE_CUT, 0x00), (13, 1, note('F-4'), i(fwd), None, NOTE_DELAY, 0x00),
        (13, 2, note('A-3'), i(bidi), None, PATTERN_LOOP, 0x00), (13, 3, None, None, None, VOLUME, 0x30),
        (14, 2, None, None, None, TREMOLO, 0x22), (14, 3, None, None, None, PANBRELLO, 0x24),
        (15, 0, None, None, None, VOLSLIDE_DOWN_FINE, 0x03), (15, 1, None, None, None, VOLSLIDE_UP_FINE, 0x04),
        (15, 2, None, None, None, VOLUME, 0x50), (15, 3, None, None, None, VOLSLIDE_DOWN_FINE, 0x00),
        (16, 0, note('C-6'), i(bidi), v(30), PORTA_UP_S3M, 0x30), (16, 1, note('C-2'), i(fwd), None, None, None),
        (17, 1, None, None, None, PORTA_DOWN_S3M, 0x40), (17, 2, None, None, None, GLOBAL_VOLSLIDE, 0x30),
        (18, 3, None, None, None, BREAK, 0x10),
    ]
    second = [
        (0, 0, note('G-3'), i(one), v(64), TREMOR, 0x00), (0, 1, note('D-5'), i(fwd), None, ARPEGGIO, 0x37),
        (1, 2, note('C-4'), i(fwd), None, PATTERN_LOOP, 0x00),
        (3, 2, None, None, None, PATTERN_LOOP, 0x01),
        (6, 3, note('E-3'), i(fwd), v(20), PORTA_UP_S3M, 0xFF),
        (7, 3, None, None, None, PORTA_UP_S3M, 0x00),
        (9, 0, note('B-3'), i(fwd), None, PORTA_NOTE, 0x04), (9, 1, None, None, None, NOTE_CUT, 0x02),
        (11, 0, None, None, None, JUMP, 0x01),
    ]
    return [pattern(20, first), pattern(16, second)]


def xm_patterns():
    """The XM module's patterns: instruments with envelopes, the XM effects and the volume column's effects."""
    v = lambda x: 0x10 + x
    vc = lambda kind, x: 0x60 + (kind << 4) + x
    events = [
        (0, 0, note('C-4'), 1, None, None, None), (0, 1, note('E-4'), 2, None, None, None),
        (0, 2, note('G-4'), 3, v(48), VOLSLIDE_XM, 0x02), (0, 3, note('C-5'), 3, vc(0, 5), None, None),
        (2, 2, None, None, None, PORTA_DOWN_XM, 0x04), (2, 3, None, None, vc(1, 3), None, None),
        (4, 2, None, None, None, PORTA_UP_XM, 0x06), (4, 3, None, None, vc(2, 2), None, None),
        (5, 2, None, None, None, PORTA_DOWN_FINE, 0x03), (5, 3, None, None, vc(3, 2), None, None),
        (6, 2, None, None, None, PORTA_UP_FINE, 0x02), (6, 3, None, None, vc(4, 4), None, None),
        (7, 2, None, None, None, PORTA_DOWN_EFINE, 0x05), (7, 3, None, None, vc(5, 3), None, None),
        (8, 0, OFF, None, None, None, None), (8, 2, None, None, None, PORTA_UP_EFINE, 0x07),
        (8, 3, None, None, vc(6, 8), None, None),
        (9, 2, None, None, None, VOLSLIDE_VIBRATO_XM, 0x20), (9, 3, None, None, vc(7, 2), None, None),
        (10, 2, None, None, None, VIBRATO, 0x33), (10, 3, None, None, vc(8, 2), None, None),
        (11, 2, None, None, None, VOLSLIDE_VIBRATO_XM, 0x03), (11, 3, note('G-4'), 3, vc(9, 4), None, None),
        (12, 1, OFF, None, None, None, None), (12, 2, note('E-4'), 3, None, VOLSLIDE_PORTA_XM, 0x10),
        (13, 2, None, None, None, ARPEGGIO, 0x47), (13, 3, None, None, None, GLISSANDO, 0x01),
        (14, 3, note('D-5'), 3, None, PORTA_NOTE, 0x20),
        (16, 0, note('A-4'), 1, None, None, None), (16, 1, note('A-3'), 2, None, PANBRELLO, 0x48),
        (18, 0, None, None, None, PORTA_DOWN_XM, 0x00),
        (20, 0, OFF, None, None, None, None), (20, 3, note('F-4'), 3, v(60), TREMOLO, 0x24),
        (28, 0, note('C-4'), 1, None, PORTA_DOWN_XM, 0x40), (28, 2, note('C-1'), 3, None, PORTA_DOWN_XM, 0x40),
        (31, 3, None, None, None, JUMP, 0x00),
    ]
    return [pattern(32, events)]


def sample_count(rom, table):
    """Returns the entries of the game's table of samples: the words from its start that give a sample's header, whose
    end comes after it."""
    n = 0
    while True:
        at = struct.unpack_from('<I', rom, table - ROM_BASE + 4 * n)[0]
        if not ROM_BASE <= at < ROM_BASE + len(rom) - 0x12:
            return n
        end = struct.unpack_from('<I', rom, at - ROM_BASE + 4)[0]
        if not at + 0x12 <= end <= ROM_BASE + len(rom):
            return n
        n += 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('out')
    a = p.parse_args()
    rom = bytearray(load_rom(a.rom))
    game = GAMES.get(bytes(rom[0xAC:0xB0]))
    if not game:
        raise SystemExit('no test data known for game code %s' % bytes(rom[0xAC:0xB0]).decode('latin-1'))

    # The new data goes after the ROM's end, which it extends.
    out = bytearray()
    base = ROM_BASE + len(rom)

    def place(data):
        while (base + len(out)) % 4:
            out.append(0)
        at = base + len(out)
        out.extend(data)
        return at

    # Three samples, with fine tunes and relative notes, and each kind of loop.
    samples = []
    for points, loop, length, fine, relative, volume, pan in (
            (wave_points(2400, 40, 'sine'), 1, 400, -24, 0, 48, 0),
            (wave_points(1600, 32, 'square'), 2, 640, 20, 2, 40, -20),
            (wave_points(3000, 50, 'square'), 0, 0, 0, -3, 64, 10)):
        at = base + len(out) + (-(base + len(out)) % 4)
        samples.append(place(sample(points, loop, length, fine, relative, volume, pan, at)))
    first = sample_count(rom, game.samples)
    table = rom[game.samples - ROM_BASE:game.samples - ROM_BASE + 4 * first] + struct.pack('<3I', *samples)
    table_at = place(table)
    for literal in game.sample_literals:
        struct.pack_into('<I', rom, literal - ROM_BASE, table_at)
    fwd, bidi, one = first, first + 1, first + 2

    # Three instruments: envelopes with a sustain point and a loop, a fade and a vibrato; a pan envelope and a volume
    # envelope that ends at 0; and no envelopes.
    flat = envelope([(0, 0)])
    instruments = [
        instrument([fwd] * 48 + [bidi] * 48, envelope([(0, 64), (6, 32), (12, 48), (20, 40)], 1, 2, 1 | 2 | 4),
                   envelope([(0, 32), (10, 48)], 0, 0, 1), 0x0800, 8, 12),
        instrument([one] * 96, envelope([(0, 40), (8, 64), (30, 0)], 0, 0, 1),
                   envelope([(0, 10), (8, 54), (16, 20), (24, 40)], 2, 1, 1 | 2 | 4), 0x0400, 0, 0),
        instrument([bidi] * 40 + [fwd] * 56, flat, flat, 0, 4, 0),
    ]
    table_at = place(struct.pack('<3I', *[place(data) for data in instruments]))
    for literal in game.instrument_literals:
        struct.pack_into('<I', rom, literal - ROM_BASE, table_at)

    # The modules, each a header and its patterns' addresses, after the patterns.
    pans = [-40, 40, -10, 10, 0, 0]
    s3m = [place(p) for p in s3m_patterns(fwd, bidi, one)]
    for fast in (False, True):
        header = module(6, [0, 254, 1], 4, 0x80, fast=fast, global_volume=60, pans=pans)
        place(header + struct.pack('<%dI' % len(s3m), *s3m))
    xm = [place(p) for p in xm_patterns()]
    header = module(4, [0], 3, 0x96, instruments=True, linear=True, pans=[0, 0, 0, 0])
    place(header + struct.pack('<%dI' % len(xm), *xm))

    rom += out
    while len(rom) % 4:
        rom.append(0)
    Path(a.out).write_bytes(bytes(rom))


if __name__ == '__main__':
    main()
