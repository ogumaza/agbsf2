#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Nintendo R&D2's driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-33] [--frames 12000] [-j JOBS]

For every sequence, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. The comparison covers each
voice's state after each frame, the order of the sample voices, and every write to the PSG's registers. It reports
the first difference in each sequence that has one.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and traces.py, in tools/
import driver_emu
import traces
from gbarom import load_rom


def compare(job):
    """Returns a description of the first difference in one sequence, or None if there's none."""
    tool, rom_path, song, frames = job
    driver = traces.printed_lines(driver_emu.trace, load_rom(rom_path), song, frames)
    model = traces.model_lines(tool, 'rd2', rom_path, song, frames)
    return traces.first_difference(driver, model, context=3)


def main():
    a = traces.parse_args(__doc__, 'sequence', 'sequences', '0-33')
    songs = traces.parse_range(a.songs) if a.songs else traces.listed_songs(a.agbsf2, 'rd2', a.rom)
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, a.frames) for s in songs], a.jobs)
    traces.report(songs, results, 'sequence', 'sequences', a.frames)


if __name__ == '__main__':
    main()
