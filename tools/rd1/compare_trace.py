#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Nintendo R&D1's MIDI-file driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-100] [--frames 12000] [-j JOBS]

For every song, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. Frame by frame, the comparison
covers the player, each track that hasn't ended and each channel's record, byte for byte. The script leaves out the
driver's voices and its writes to the PSG, which the model doesn't keep. It also leaves out each channel's bytes 0x1A
and 0x1B. CC82 gives each note a random pitch there, which the model doesn't follow. It reports the first difference
in each song that has one.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and traces.py, in tools/
import driver_emu
import traces
from gbarom import load_rom


RANDOM_PITCH = slice(2 * 0x1A, 2 * 0x1C)  # the hex digits of a channel's bytes 0x1A and 0x1B


def masked(lines):
    """Returns the lines of the player, the tracks and the channels, with each channel's random pitch blanked. Both
    traces print a channel's line only when its record changes. A line that changed only in the random pitch is left
    out."""
    out = []
    last = {}
    for line in lines:
        words = line.split()
        if words[1] not in ('p', 't', 'c'):
            continue
        if words[1] == 'c':
            data = words[3]
            data = data[:RANDOM_PITCH.start] + '----' + data[RANDOM_PITCH.stop:]
            if last.get(words[2]) == data:
                continue
            last[words[2]] = data
            words[3] = data
            line = ' '.join(words)
        out.append(line)
    return out


def driver_lines(rom_path, song, frames):
    """Returns the driver's trace of the player, the tracks and the channels."""
    return masked(traces.printed_lines(driver_emu.trace, load_rom(rom_path), song, frames))


def compare(job):
    """Returns a description of the first difference in one song, or None if there's none."""
    tool, rom_path, song, frames = job
    return traces.first_difference(driver_lines(rom_path, song, frames),
                                   masked(traces.model_lines(tool, 'rd1', rom_path, song, frames)), context=3)


def main():
    a = traces.parse_args(__doc__, 'song', 'songs', '0-100')
    songs = traces.parse_range(a.songs) if a.songs else traces.listed_songs(a.agbsf2, 'rd1', a.rom)
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, a.frames) for s in songs], a.jobs)

    failed = 0
    for song, result in zip(songs, results):
        if result:
            failed += 1
            print('song %d: %s' % (song, result))
    print('%d of %d songs differ' % (failed, len(songs)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
