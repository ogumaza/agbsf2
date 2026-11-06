#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of AlphaDream's sound driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 1-50] [--frames 12000] [-j JOBS]

For every song, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. Frame by frame, the comparison
covers the player, each track that plays, each sample channel's record and the records of the PSG's four channels,
byte for byte. The script leaves out the writes to the sound registers, which the model doesn't trace. It reports the
first difference in each song that has one.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and traces.py, in tools/
import driver_emu
import traces
from gbarom import load_rom


def compared(line):
    """Returns True for a line of the player, a track, a sample channel or a PSG channel."""
    return line.split()[1] in ('p', 't', 'h', 'q')


def driver_lines(rom_path, song, frames):
    """Returns the driver's trace of the player, the tracks, the sample channels and the PSG's channels."""
    lines = traces.printed_lines(driver_emu.trace, load_rom(rom_path), song, frames)
    return [line for line in lines if compared(line)]


def compare(job):
    """Returns a description of the first difference in one song, or None if there's none."""
    tool, rom_path, song, frames = job
    return traces.first_difference(driver_lines(rom_path, song, frames),
                                   traces.model_lines(tool, 'alphadream', rom_path, song, frames), context=3)


def main():
    a = traces.parse_args(__doc__, 'song', 'songs', '1-50')
    songs = traces.parse_range(a.songs) if a.songs else traces.listed_songs(a.agbsf2, 'alphadream', a.rom)
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, a.frames) for s in songs], a.jobs)
    traces.report(songs, results, 'song', 'songs', a.frames)


if __name__ == '__main__':
    main()
