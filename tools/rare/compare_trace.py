#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Rare's driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-22] [--frames 12000] [-j JOBS]

For every song, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. After each frame, the comparison
covers the note slots that are playing: each one's state, channel, keys, velocity, envelope phase and level, instrument,
and sample position to a 2^23rd of a byte. It reports the first difference in each song that has one.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and traces.py, in tools/
import driver_emu
import traces
from gbarom import load_rom


def compare(job):
    """Returns a description of the first difference in one song, or None if there's none."""
    tool, rom_path, song, frames = job
    driver = traces.printed_lines(driver_emu.trace, load_rom(rom_path), song, frames)
    model = traces.model_lines(tool, 'rare', rom_path, song, frames)
    return traces.first_difference(driver, model)


def main():
    a = traces.parse_args(__doc__, 'song', 'songs', '0-22', 'the game (.gba) or a GSF rip of it')
    songs = traces.parse_range(a.songs) if a.songs else traces.listed_songs(a.agbsf2, 'rare', a.rom)
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, a.frames) for s in songs], a.jobs)
    traces.report(songs, results, 'song', 'songs', a.frames)


if __name__ == '__main__':
    main()
