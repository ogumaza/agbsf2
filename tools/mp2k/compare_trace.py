#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of MP2K with the game's sound driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-22] [--frames 12000] [-j JOBS]

For every song, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. After each frame, the comparison
covers the sound channels that play: each one's status, track, keys, velocity, priority, envelope and volumes, and then
a DirectSound channel's rate, sample and position in it to a 2^23rd of a point, or a PSG channel's frequency setting,
envelope goal, counter, sustain level, outputs and wave. It reports the first difference in each song that has one.
"""
import argparse
import contextlib
import gc
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
    # The emulator and its hooks form a reference cycle. Collect it now to avoid keeping emulators in memory for several
    # songs.
    gc.collect()
    return out.getvalue().splitlines()


def model_lines(tool, rom_path, song, frames):
    return subprocess.run([tool, '--driver', 'mp2k', '--trace', str(song), '--trace-frames', str(frames), rom_path],
                          capture_output=True, encoding='utf-8', check=True).stdout.splitlines()


def compare(job):
    """Returns a description of the first difference in one song, or None if there's none."""
    tool, rom_path, song, frames = job
    driver = driver_lines(rom_path, song, frames)
    model = model_lines(tool, rom_path, song, frames)
    for a, b in zip(driver, model):
        if a != b:
            return 'frame %s differs:\n    driver %s\n    agbsf2 %s' % (a.split()[0], a, b)
    if len(driver) != len(model):
        longer, name = (driver, 'the driver') if len(driver) > len(model) else (model, 'agbsf2')
        return 'only %s goes on, at frame %s: %s' % (name, longer[min(len(driver), len(model))].split()[0],
                                                     longer[min(len(driver), len(model))])
    return None


def songs_with_tracks(tool, rom_path):
    """Returns the numbers of the songs that `--info` lists with an address, which leaves out empty entries."""
    text = subprocess.run([tool, '--driver', 'mp2k', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    return [int(n) for n in re.findall(r'^\s+(\d+)\s+0x[0-9A-F]{8}', text, re.MULTILINE)]


def parse_range(text):
    songs = []
    for part in text.split(','):
        a, _, b = part.partition('-')
        songs += list(range(int(a), int(b or a) + 1))
    return songs


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba) or a GSF rip of it')
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program to check')
    p.add_argument('--songs', help='songs to compare, such as 0-22 (default: all)')
    p.add_argument('--frames', type=int, default=12000, help='frames to compare in each song (default: 12000)')
    p.add_argument('-j', '--jobs', type=int, default=1, help='songs to compare at once (default: 1)')
    a = p.parse_args()

    listed = songs_with_tracks(a.agbsf2, a.rom)
    songs = [s for s in parse_range(a.songs) if s in listed] if a.songs else listed
    if not songs:
        raise SystemExit('found none of the songs to check in the song list of agbsf2 --info')
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
            print('song %d: %s' % (song, result))
    print('%d of %d songs match the driver for %d frames' % (len(songs) - failed, len(songs), a.frames))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
