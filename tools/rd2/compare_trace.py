#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Nintendo R&D2's driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-33] [--frames 12000] [-j JOBS]

For every sequence, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. The comparison covers each
voice's state after each frame, the order of the sample voices, and every write to the PSG's registers. It reports
the first difference in each sequence that has one.
"""
import argparse
import contextlib
import io
import multiprocessing
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
import driver_emu
from gbarom import load_rom


def driver_lines(rom_path, song, frames):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        driver_emu.trace(load_rom(rom_path), song, frames)
    return out.getvalue().splitlines()


def model_lines(tool, rom_path, song, frames):
    return subprocess.run([tool, '--driver', 'rd2', '--trace', str(song), '--trace-frames', str(frames), rom_path],
                          capture_output=True, encoding='utf-8', check=True).stdout.splitlines()


def compare(job):
    """Returns a description of the first difference in one sequence, or None if there's none."""
    tool, rom_path, song, frames = job
    driver = driver_lines(rom_path, song, frames)
    model = model_lines(tool, rom_path, song, frames)
    for i, (a, b) in enumerate(zip(driver, model)):
        if a != b:
            before = '\n'.join('    both   %s' % line for line in driver[max(0, i - 3):i])
            return 'frame %s differs:\n%s\n    driver %s\n    agbsf2 %s' % (a.split()[0], before, a, b)
    if len(driver) != len(model):
        longer, name = (driver, 'the driver') if len(driver) > len(model) else (model, 'agbsf2')
        return 'only %s goes on, at frame %s: %s' % (name, longer[min(len(driver), len(model))].split()[0],
                                                     longer[min(len(driver), len(model))])
    return None


def listed_songs(tool, rom_path):
    """Returns the numbers of the songs that `--info` lists."""
    text = subprocess.run([tool, '--driver', 'rd2', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    songs = [int(n) for n in re.findall(r'^\s+(\d+)\s+0x[0-9A-F]{8}', text, re.MULTILINE)]
    if not songs:
        raise SystemExit('found no songs in the song list of agbsf2 --info')
    return songs


def parse_range(text):
    songs = []
    for part in text.split(','):
        a, _, b = part.partition('-')
        songs += list(range(int(a), int(b or a) + 1))
    return songs


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program to check')
    p.add_argument('--songs', help='sequences to compare, such as 0-33 (default: all)')
    p.add_argument('--frames', type=int, default=12000, help='frames to compare in each sequence (default: 12000)')
    p.add_argument('-j', '--jobs', type=int, default=1, help='sequences to compare at once (default: 1)')
    a = p.parse_args()

    songs = parse_range(a.songs) if a.songs else listed_songs(a.agbsf2, a.rom)
    jobs = [(a.agbsf2, a.rom, s, a.frames) for s in songs]
    if a.jobs > 1:
        with multiprocessing.Pool(a.jobs) as pool:
            results = pool.map(compare, jobs)
    else:
        results = [compare(j) for j in jobs]

    failed = 0
    for song, result in zip(songs, results):
        if result:
            failed += 1
            print('sequence %d: %s' % (song, result))
    print('%d of %d sequences match the driver for %d frames' % (len(songs) - failed, len(songs), a.frames))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
