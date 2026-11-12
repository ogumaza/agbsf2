#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Logik State's sound driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-21] [--frames 12000] [-j JOBS]

For every song, the script runs `AGBSF2 --trace` and the driver under driver_emu.py, or under driver_emu_mod30.py for a
game with GBAMOD30 modules. Frame by frame, the comparison covers the player's variables and each mixer channel's
record, byte for byte. It reports the first difference in each song that has one.
"""
import sys
from pathlib import Path

# For driver_emu.py, in this folder, and for gbarom.py and traces.py, in tools/. Python's isolated mode leaves the
# script's folder out of the path.
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parent.parent)]
import driver_emu
import driver_emu_mod30
import traces
from gbarom import load_rom


def compare(job):
    """Returns a description of the first difference in one song, or None if there's none."""
    tool, rom_path, song, frames = job
    rom = load_rom(rom_path)
    emulator = driver_emu_mod30 if driver_emu_mod30.has_modules(rom) else driver_emu
    return traces.first_difference(traces.printed_lines(emulator.trace, rom, song, frames),
                                   traces.model_lines(tool, 'logikstate', rom_path, song, frames), context=3)


def main():
    a = traces.parse_args(__doc__, 'song', 'songs', '0-21')
    songs = traces.parse_range(a.songs) if a.songs else traces.listed_songs(a.agbsf2, 'logikstate', a.rom)
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, a.frames) for s in songs], a.jobs)
    traces.report(songs, results, 'song', 'songs', a.frames)


if __name__ == '__main__':
    main()
