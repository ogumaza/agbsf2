#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of MP2K with the game's sound driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-22] [--frames 12000] [-j JOBS]

For every song, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. After each frame, the comparison
covers the sound channels that play: each one's status, track, keys, velocity, priority, envelope and volumes, and then
a DirectSound channel's rate, sample and position in it to a 2^23rd of a point, or a PSG channel's frequency setting,
envelope goal, counter, sustain level, outputs and wave. It reports the first difference in each song that has one.
"""
import gc
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
    # The emulator and its hooks form a reference cycle. Collect it now to avoid keeping emulators in memory for several
    # songs.
    gc.collect()
    model = traces.model_lines(tool, 'mp2k', rom_path, song, frames)
    return traces.first_difference(driver, model)


def main():
    a = traces.parse_args(__doc__, 'song', 'songs', '0-22', 'the game (.gba) or a GSF rip of it')
    # --info lists the songs that have tracks with an address. It gives none for an empty entry.
    listed = [n for n, _ in traces.listed(a.agbsf2, 'mp2k', a.rom)]
    songs = [s for s in traces.parse_range(a.songs) if s in listed] if a.songs else listed
    if not songs:
        raise SystemExit('found none of the songs to check in the song list of agbsf2 --info')
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, a.frames) for s in songs], a.jobs)
    traces.report(songs, results, 'song', 'songs', a.frames)


if __name__ == '__main__':
    main()
