# SPDX-License-Identifier: MIT

"""Find a driver's files in a folder of agbsf2's output. Compare a conversion on the song's beat with one on the
driver's frames. Give the MIDI value that agbsf2 writes for a driver's level.

A conversion names each song's MIDI file and SoundFont after the game and the song's number, such as game_05.mid and
game_05.sf2. In a game with more than one driver, it converts each one's songs, and the files of every driver but the
first that detection finds have the driver's --driver name after the game's, such as game_mp2k_005.mid. A song that the
game plays in two versions gets a second pair of files, with the version's name after the song's number, such as
game_12_underwater.mid.

For drivers that count frames, the default conversion estimates the beat from the notes and moves events onto it.
With --frame-timing, events stay on the frames where the driver plays them. The tools check this conversion against
the driver, frame by frame, then compare the default conversion with it.
"""
import math
import re
import struct
import subprocess
from pathlib import Path

DRIVERS = ('konami', 'rare', 'quintet', 'rd2', 'mp2k', 'brownie', 'krawall', 'ubimilan')
VERSIONS = ('underwater',)  # the names of the songs' other versions in the files' names

_STEM = re.compile(r'(.+?)(?:_(%s))?_(\d+)(?:_(%s))?' % ('|'.join(DRIVERS), '|'.join(VERSIONS)))


def midi_files(folder, driver, versions=False):
    """Returns (song, path) for each of a driver's MIDI files in the folder, in song order: the files with the driver's
    name, if there are any, or else the ones without a driver's name. Those of a song's other versions are left out.
    With `versions`, it returns (song, version, path) for every file, with a version of '' for a song as the game
    starts it."""
    found = {}
    for path in Path(folder).glob('*.mid'):
        m = _STEM.fullmatch(path.stem)
        if m and (versions or m.group(4) is None):
            entry = (int(m.group(3)), m.group(4) or '', path) if versions else (int(m.group(3)), path)
            found.setdefault(m.group(2), []).append(entry)
    return sorted(found.get(driver) or found.get(None, []))


def square_root_value(level, full, lowest):
    """Returns the MIDI value that agbsf2's SquareRootValue() writes for a level of `level` / `full`: 127 × √(level /
    full), rounded to the nearest with halves up, from `lowest` to 127. A SoundFont player squares velocity and
    controller 7 by default. The level then plays in a straight line."""
    value = 127 * math.sqrt(max(0, level) / full)
    whole = math.floor(value)
    return min(127, max(lowest, whole + (1 if value - whole >= 0.5 else 0)))


def frame_timed(tool, rom, driver, folder, songs=None):
    """Converts ROM into FOLDER with --frame-timing, keeping events on the driver's frames.
    Returns {song: path} for the driver's MIDI files."""
    command = [tool, '--driver', driver, '--frame-timing', '-q', '-o', str(folder)]
    subprocess.run(command + (['-s', songs] if songs else []) + [str(rom)], check=True)
    return dict(midi_files(folder, driver))


def read_timing(path):
    """Returns a MIDI file's notes on each channel, as (start, end, key, bank, program), and its changes of each
    controller, pitch bend and program on each channel, as {(channel, what): [(time, value)]}, with times in seconds.
    Repeated values, such as settings restored at a loop start, do not count as changes."""
    data = Path(path).read_bytes()
    division = struct.unpack('>H', data[12:14])[0]
    events = []
    pos, track = 14, 0
    while pos < len(data):
        size = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        pos += 8 + size
        tick, i, status, index = 0, 0, 0, 0
        while i < len(body):
            delta = 0
            while True:
                b = body[i]
                i += 1
                delta = delta << 7 | (b & 0x7F)
                if b < 0x80:
                    break
            tick += delta
            index += 1
            if body[i] >= 0x80:
                status = body[i]
                i += 1
            if status == 0xFF:
                kind, length = body[i], 0
                i += 1
                while True:
                    b = body[i]
                    i += 1
                    length = length << 7 | (b & 0x7F)
                    if b < 0x80:
                        break
                if kind == 0x51:
                    events.append((tick, 0, track, index, 'tempo', 0, int.from_bytes(body[i:i + 3], 'big')))
                i += length
                continue
            kind, ch = status & 0xF0, status & 0x0F
            n = 1 if kind in (0xC0, 0xD0) else 2
            args = body[i:i + n]
            i += n
            if kind == 0x90 and args[1]:
                events.append((tick, 5, track, index, 'on', ch, args[0]))
            elif kind in (0x80, 0x90):
                events.append((tick, 1, track, index, 'off', ch, args[0]))
            elif kind == 0xC0:
                events.append((tick, 2, track, index, 'program', ch, args[0]))
            elif kind == 0xB0:
                events.append((tick, 3, track, index, ('cc', args[0]), ch, args[1]))
            elif kind == 0xE0:
                events.append((tick, 4, track, index, 'bend', ch, args[0] | args[1] << 7))
        track += 1
    events.sort()

    tempos = [(0, 500000)] + [(e[0], e[6]) for e in events if e[4] == 'tempo']

    def seconds(t):
        s, at, tempo = 0.0, 0, 500000
        for when, value in tempos:
            if when > t:
                break
            s += (when - at) * tempo / division / 1e6
            at, tempo = when, value
        return s + (t - at) * tempo / division / 1e6

    notes, changes, sounding, bank, program = {}, {}, {}, {}, {}
    for tick, _, _, _, kind, ch, value in events:
        t = seconds(tick)
        if kind == 'on':
            # Retriggering a sounding key ends the previous note, as it does in MIDI players.
            if (ch, value) in sounding:
                sounding.pop((ch, value))[1] = t
            note = [t, None, value, bank.get(ch, 0), program.get(ch, 0)]
            notes.setdefault(ch, []).append(note)
            sounding[(ch, value)] = note
        elif kind == 'off':
            if (ch, value) in sounding:
                sounding.pop((ch, value))[1] = t
        elif kind != 'tempo':
            if kind == ('cc', 0):
                bank[ch] = value << 7 | (bank.get(ch, 0) & 0x7F)
            elif kind == ('cc', 32):
                bank[ch] = (bank.get(ch, 0) & ~0x7F) | value
            elif kind == 'program':
                program[ch] = value
            values = changes.setdefault((ch, kind), [])
            if not values or values[-1][1] != value:
                values.append((t, value))
    end = seconds(events[-1][0]) if events else 0.0
    for note in sounding.values():
        note[1] = end
    return {ch: [tuple(n) for n in list_] for ch, list_ in notes.items()}, changes


def compare_timing(beat_path, frame_path, frame_seconds, max_shift):
    """Returns the differences between a MIDI file on the song's beat and the same conversion on the driver's frames,
    and the largest number of frames an event moved by: they need the same notes, with the same keys and programs, and
    the same changes of each controller, pitch bend and program, in the same order on each channel, each at most
    `max_shift` frames from the other's."""
    beat_notes, beat_changes = read_timing(beat_path)
    frame_notes, frame_changes = read_timing(frame_path)
    problems = []
    worst = 0.0

    def shift(a, b, what):
        nonlocal worst
        moved = abs(a - b) / frame_seconds
        worst = max(worst, moved)
        if moved > max_shift:
            problems.append('%s is %.2f frames from frame %.2f' % (what, moved, b / frame_seconds))

    for ch in sorted(set(beat_notes) | set(frame_notes)):
        on_beat, on_frames = beat_notes.get(ch, []), frame_notes.get(ch, [])
        if len(on_beat) != len(on_frames):
            problems.append('MIDI channel %d: %d notes on the beat, %d on the frames' % (
                ch + 1, len(on_beat), len(on_frames)))
            continue
        for i, (a, b) in enumerate(zip(on_beat, on_frames)):
            if a[2:] != b[2:]:
                problems.append('MIDI channel %d, note %d: key and program %s on the beat, %s on the frames' % (
                    ch + 1, i, a[2:], b[2:]))
                break
            shift(a[0], b[0], 'MIDI channel %d, note %d\'s start' % (ch + 1, i))
            shift(a[1], b[1], 'MIDI channel %d, note %d\'s end' % (ch + 1, i))

    for key in sorted(set(beat_changes) | set(frame_changes), key=str):
        a, b = beat_changes.get(key, []), frame_changes.get(key, [])
        if [v for _, v in a] != [v for _, v in b]:
            problems.append('MIDI channel %d: the %s changes differ' % (key[0] + 1, key[1]))
            continue
        for i, ((ta, _), (tb, _)) in enumerate(zip(a, b)):
            shift(ta, tb, 'MIDI channel %d: %s change %d' % (key[0] + 1, key[1], i))
    return problems, worst
