#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Krawall with the game's build of it, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-20] [--frames 12000] [-j JOBS]

For every module that `agbsf2 --info` lists, the script runs `AGBSF2 --trace` and the game's code under
driver_emu.py. Frame by frame, the comparison covers the mixer's variables, the player's record, each of the module's
channels and each mixer channel. It reports the first difference in each module that has one.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and traces.py, in tools/
import driver_emu
import traces
from gbarom import load_rom


def compare(job):
    """Returns a description of the first difference in one module, or None if there's none."""
    tool, rom_path, number, (module, mode, song), frames = job
    driver = traces.printed_lines(driver_emu.trace, load_rom(rom_path), module, frames, mode, song)
    model = traces.model_lines(tool, 'krawall', rom_path, number, frames)
    return traces.first_difference(driver, model, context=3)


def main():
    a = traces.parse_args(__doc__, 'module', 'modules', '0-20')
    modules = dict(traces.listed(a.agbsf2, 'krawall', a.rom))
    if not modules:
        raise SystemExit('found no modules in the list of agbsf2 --info')
    plays = driver_emu.song_plays(load_rom(a.rom), modules)
    songs = traces.parse_range(a.songs) if a.songs else sorted(modules)
    results = traces.compare_all(compare, [(a.agbsf2, a.rom, s, plays[s], a.frames) for s in songs], a.jobs)
    traces.report(songs, results, 'module', 'modules', a.frames)


if __name__ == '__main__':
    main()
