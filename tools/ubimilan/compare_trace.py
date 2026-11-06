#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's model of Ubisoft Milan's driver with the game's driver, frame by frame.

    compare_trace.py ROM AGBSF2 [--songs 0-13] [--frames 12000] [-j JOBS]

For every piece of music, the script runs `AGBSF2 --trace` and the driver under driver_emu.py. Frame by frame, the
comparison covers every write to the PSG's registers, in order, the track, and each voice that the mixer plays. It
reports the first difference in each piece that has one.

A piece that agbsf2 --info lists with a version "with the kit" is one that the game starts with channel 9's kit
switched off. agbsf2 --trace follows a piece's first version, so the driver then starts the piece with the kit off.

A key of channel 9 outside its kit makes the driver read a sample from past the end of the sound bank's resource table,
which lands in a mirror of the ROM, at 0x0A000000 or above. The model leaves such a note out, so the driver's voices
that play there aren't compared; each piece that has them says how many frames they play for.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and traces.py, in tools/
import driver_emu
import traces
from gbarom import load_rom

MIRROR = 0x0A000000


def driver_lines(rom_path, song, frames, kit):
    """Returns the driver's trace, and the number of its voice lines that play from a mirror of the ROM, left out."""
    kept, skipped = [], 0
    for line in traces.printed_lines(driver_emu.trace, load_rom(rom_path), song, frames, kit):
        fields = line.split()
        if fields[1].startswith('v') and int(fields[2], 16) >= MIRROR:
            skipped += 1
        else:
            kept.append(line)
    return kept, skipped


def compare(job):
    """Returns a description of the first difference in one piece, or None if there's none, and the driver's voice
    lines that play from a mirror of the ROM."""
    tool, rom_path, song, frames, kit = job
    driver, skipped = driver_lines(rom_path, song, frames, kit)
    model = traces.model_lines(tool, 'ubimilan', rom_path, song, frames)
    return traces.first_difference(driver, model, context=3), skipped


def main():
    a = traces.parse_args(__doc__, 'piece', 'pieces', '0-13')
    # The samples that play as music follow the pieces. They have no sequence for the driver to play.
    pieces = len(driver_emu.DriverEmulator(load_rom(a.rom)).songs())
    songs = traces.parse_range(a.songs) if a.songs else traces.listed_songs(a.agbsf2, 'ubimilan', a.rom)
    songs = [s for n, s in enumerate(songs) if s < pieces and s not in songs[:n]]
    kit_off = {s for s, name in traces.listed_versions(a.agbsf2, 'ubimilan', a.rom) if name == 'with the kit'}
    jobs = [(a.agbsf2, a.rom, s, a.frames, s not in kit_off) for s in songs]
    results = traces.compare_all(compare, jobs, a.jobs)

    failed = 0
    for song, (result, skipped) in zip(songs, results):
        if result:
            failed += 1
            print('music %d%s: %s' % (song, ' without the kit' if song in kit_off else '', result))
        if skipped:
            print('music %d: agbsf2 leaves out a voice that plays from a mirror of the ROM for %d frames' %
                  (song, skipped))
    print('%d of %d pieces match the driver for %d frames' % (len(songs) - failed, len(songs), a.frames))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
