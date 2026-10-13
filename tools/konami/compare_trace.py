#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's sequencer with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-22] [--frames 12000]

For every song, the script runs `AGBSF2 --trace` and the driver under
driver_emu.py and reports the frames where any track's output record differs.
After an FF 00 stop the driver emits one more frame that silences everything.
agbsf2 ends the song instead. The comparison leaves that frame out. A song
whose traces differ in length in any other way fails, with both frame counts
shown. That happens when a loop takes no time: the driver hangs, and agbsf2
stops the song there.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

from unicorn import UcError

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from driver_emu import DriverEmulator
from gbarom import load_rom


def driver_records(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    out = []
    for _ in range(frames):
        records = emu.frame()
        if records is None:
            break
        out.append(records)
    return out


def agbsf2_records(tool, rom_path, song, frames):
    text = subprocess.run([tool, '--driver', 'konami', '--trace', str(song), '--trace-frames', str(frames), rom_path],
                          capture_output=True, encoding='utf-8', check=True).stdout
    out = []
    for line in text.splitlines():
        v = [int(x) for x in line.split()]
        while len(out) <= v[0]:
            out.append([None] * 16)
        out[v[0]][v[1]] = tuple(v[2:])
    return out


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
    p.add_argument('--songs', default=None, help='the songs to check, such as 0-22 or 1,4,9 (default: all)')
    p.add_argument('--frames', type=int, default=12000, help='frames to compare in each song (default: 12000)')
    a = p.parse_args()
    rom = load_rom(a.rom)
    songs = parse_range(a.songs) if a.songs else None
    if songs is None:
        info = subprocess.run([a.agbsf2, '--driver', 'konami', '--info', a.rom], capture_output=True,
                              encoding='utf-8', check=True).stdout
        songs = [int(m.group(1)) for m in re.finditer(r'^\s+(\d+)\s+0x[0-9A-F]{8}\s', info, re.M)]
        if not songs:
            raise SystemExit('found no songs in the song list of agbsf2 --info')
    failures = 0
    for song in songs:
        try:
            drv = driver_records(rom, song, a.frames)
        except UcError as e:
            print('song %2d: the emulated driver faulted (%s)' % (song, e))
            failures += 1
            continue
        mine = agbsf2_records(a.agbsf2, a.rom, song, a.frames)
        bad = [(f, t) for f in range(min(len(drv), len(mine))) for t in range(len(drv[f])) if drv[f][t] != mine[f][t]]
        # agbsf2's trace ends early when the song stops. After an FF 00 stop the driver has one more frame.
        extra_expected = 1 if len(mine) < a.frames else 0
        ok = not bad and len(drv) - len(mine) == extra_expected
        failures += not ok
        where = ' first at frame %d track %d: driver %s agbsf2 %s' % (
            bad[0][0], bad[0][1], drv[bad[0][0]][bad[0][1]], mine[bad[0][0]][bad[0][1]]) if bad else ''
        if len(drv) - len(mine) != extra_expected:
            where += ' (the driver gave %d frames, agbsf2 %d)' % (len(drv), len(mine))
        print('song %2d: %d frames compared, %d differences%s' % (song, min(len(drv), len(mine)), len(bad), where))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
