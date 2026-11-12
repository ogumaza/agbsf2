#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Counts the events of every MIDI file that a song record of WarioWare or Rhythm Tengoku points to.

    smfscan.py ROM SONG_TABLE

ROM is a .gba image or a GSF rip. SONG_TABLE is the song table's address in hex. Each entry of the table is 8 bytes
long: the address of the song's record, then its player. A record starts with the address of the song's MIDI file.
"""
import argparse
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py and gbarom.py, in tools/
from conversion import midi_file  # noqa: E402
from gbarom import ROM_BASE, load_rom  # noqa: E402

LAST_ENTRY = 4000  # the scan stops at the first empty entry from this one on


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('rom', type=Path)
    p.add_argument('song_table', type=lambda s: int(s, 16))
    a = p.parse_args()
    rom = load_rom(a.rom)

    def u32(address):
        return struct.unpack_from('<I', rom, address - ROM_BASE)[0]

    def in_rom(address):
        return ROM_BASE <= address < ROM_BASE + len(rom)

    records = []
    entry = 0
    while True:
        record = u32(a.song_table + 8 * entry)
        if record == 0:
            entry += 1
            if entry > LAST_ENTRY:
                break
            continue
        if not in_rom(record) or not in_rom(u32(record)):
            break
        start = u32(record) - ROM_BASE
        if rom[start:start + 4] != b'MThd':
            break
        records.append((entry, record))
        entry += 1

    cc, cc_values, meta, kinds, sysex, formats, markers = (collections.Counter() for _ in range(7))
    cc_songs = collections.defaultdict(set)
    for song, record in records:
        start = u32(record) - ROM_BASE
        header_size = struct.unpack_from('>I', rom, start + 4)[0]
        form, track_count, division = struct.unpack_from('>HHH', rom, start + 8)
        formats[(form, division)] += 1
        end = start + 8 + header_size
        for _ in range(track_count):
            assert rom[end:end + 4] == b'MTrk', (song, hex(end))
            end += 8 + struct.unpack_from('>I', rom, end + 4)[0]
        for track, (_, events) in enumerate(midi_file.tracks_of(rom[start:end])[1]):
            for tick, status, data in events:
                kind = status & 0xF0
                if status == 0xFF:
                    meta[data[0]] += 1
                    if data[0] == 0x06:
                        size, at = midi_file.read_number(data, 1)
                        markers[data[at:at + size]] += 1
                    if data[0] == 0x2F:
                        break
                elif status in (0xF0, 0xF7):
                    sysex[data.hex()] += 1
                elif kind in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    if kind == 0xB0:
                        cc[data[0]] += 1
                        cc_songs[data[0]].add(song)
                        cc_values[(data[0], data[1])] += 1
                    kinds[kind] += 1
                elif kind in (0xC0, 0xD0):
                    kinds[kind] += 1
                else:
                    raise SystemExit('bad status %x in song %d, track %d, at tick %d' % (status, song, track, tick))

    print('songs', len(records), 'formats', formats)
    print('kinds', {hex(k): v for k, v in kinds.items()})
    print('meta', {hex(k): v for k, v in meta.items()}, 'markers', markers)
    print('sysex', sysex.most_common(10))
    print('cc', sorted((k, v, len(cc_songs[k])) for k, v in cc.items()))
    for k in sorted(cc):
        values = sorted({v for (c, v) in cc_values if c == k})
        print('  cc', k, 'values', values if len(values) < 20 else (len(values), values[:5], values[-5:]))


if __name__ == '__main__':
    main()
