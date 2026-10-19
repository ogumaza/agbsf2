# SPDX-License-Identifier: MIT

"""The parts that the compare_trace.py scripts share.

Each script runs a game's driver under the driver_emu.py in the script's folder. driver_emu.py prints a trace in the
same format as `agbsf2 --trace`. These functions run agbsf2, compare the two traces line by line and report the
differences. They also read the song list of `agbsf2 --info` and parse a list of song numbers such as 0-3,7. The
compare_notes.py scripts use parse_range() too.
"""
import argparse
import contextlib
import io
import multiprocessing
import re
import subprocess


def parse_range(text):
    """Returns the numbers in a list such as 0-3,7, in the list's order."""
    songs = []
    for part in text.split(','):
        a, _, b = part.partition('-')
        songs += list(range(int(a), int(b or a) + 1))
    return songs


def parse_args(doc, unit, units, example, rom_help='the game (.gba)'):
    """Returns the arguments of a compare_trace.py script. `unit` and `units` name what the driver plays, such as song
    and songs. `example` is a range of their numbers."""
    p = argparse.ArgumentParser(description=doc, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help=rom_help)
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program to check')
    p.add_argument('--songs', help='%s to compare, such as %s (default: all)' % (units, example))
    p.add_argument('--frames', type=int, default=12000, help='frames to compare in each %s (default: 12000)' % unit)
    p.add_argument('-j', '--jobs', type=int, default=1, help='%s to compare at once (default: 1)' % units)
    return p.parse_args()


def listed(tool, driver_name, rom_path):
    """Returns the number and address of each song that `agbsf2 --info` lists with an address."""
    text = subprocess.run([tool, '--driver', driver_name, '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    return [(int(n), int(a, 16)) for n, a in re.findall(r'^\s+(\d+)\s+0x([0-9A-F]{8})', text, re.MULTILINE)]


def listed_songs(tool, driver_name, rom_path):
    """Returns the numbers of the songs that `agbsf2 --info` lists. Exits if the list is empty."""
    songs = [n for n, _ in listed(tool, driver_name, rom_path)]
    if not songs:
        raise SystemExit('found no songs in the song list of agbsf2 --info')
    return songs


def printed_lines(function, *args):
    """Calls the function with the arguments and returns the lines that it prints."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        function(*args)
    return out.getvalue().splitlines()


def model_lines(tool, driver_name, rom_path, song, frames):
    """Returns the lines of `agbsf2 --trace` for one song."""
    command = [tool, '--driver', driver_name, '--trace', str(song), '--trace-frames', str(frames), rom_path]
    return subprocess.run(command, capture_output=True, encoding='utf-8', check=True).stdout.splitlines()


def first_difference(driver, model, context=0):
    """Returns a description of the first difference between the traces of the driver and agbsf2, or None if there's
    none. With `context`, the description also shows up to that many of the lines before the difference. Both traces
    share those lines."""
    for i, (a, b) in enumerate(zip(driver, model)):
        if a != b:
            lines = ['frame %s differs:' % a.split()[0]]
            if context:
                lines.append('\n'.join('    both   %s' % line for line in driver[max(0, i - context):i]))
            return '\n'.join(lines + ['    driver %s' % a, '    agbsf2 %s' % b])
    if len(driver) != len(model):
        longer, name = (driver, 'the driver') if len(driver) > len(model) else (model, 'agbsf2')
        line = longer[min(len(driver), len(model))]
        return 'only %s goes on, at frame %s: %s' % (name, line.split()[0], line)
    return None


def compare_all(compare, jobs, workers):
    """Returns compare(job) for each job, in order. With more than one worker, that many jobs run at once."""
    if workers > 1:
        with multiprocessing.Pool(workers) as pool:
            return pool.map(compare, jobs)
    return [compare(job) for job in jobs]


def report(songs, results, label, units, frames):
    """Prints the difference in each song that has one and a count of the songs that match. Exits with 1 if any song
    differs, or else with 0. `label` comes before each song's number. `units` names the songs in the count."""
    failed = 0
    for song, result in zip(songs, results):
        if result:
            failed += 1
            print('%s %d: %s' % (label, song, result))
    print('%d of %d %s match the driver for %d frames' % (len(songs) - failed, len(songs), units, frames))
    raise SystemExit(1 if failed else 0)
