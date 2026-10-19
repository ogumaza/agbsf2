#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against the game's build of Krawall, tick by tick.

    compare_notes.py ROM AGBSF2 FOLDER [--songs 0-20]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each module, or NAME_krawall_NN.mid and
NAME_krawall_NN.sf2 if another driver was detected first. For each module listed by `AGBSF2 --info`, the script runs the
game's code under driver_emu.py for as long as the module's MIDI file lasts. A module without a MIDI file runs for the
length that --info reports. After every player tick, it reads each module channel's mixer channel, provided it still
belongs to that channel's last note and is playing. It records the sample, position, step and levels on both sides,
along with position changes made by kramSetPos() for retriggers or offsets. A note starts when the mixer channel starts
a sample or changes its position. It ends when the sample stops or the next note starts. The MIDI file and SoundFont
must match:

  - each note on the channel's MIDI channel, starting and ending within a millisecond of the driver's ticks, the MIDI
    file starting on the module's first tick;
  - its preset's sample: the game's sample, its points (8-bit, unsigned in the game), its loop, with a loop back and
    forth played forwards and then backwards, and the point the note starts from;
  - its pitch at each tick, the rate at which the mixer steps through the sample, to within 5 cents;
  - its level on each side at each tick, which CC10 and CC11 carry, to within a step of each.

A module named in --songs that has no MIDI file mustn't play anything.
"""
import argparse
import bisect
import collections
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_FP, UC_ARM_REG_R0, UC_ARM_REG_R1

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py, gbarom.py and traces.py, in tools/
import conversion
import driver_emu
from gbarom import ROM_BASE, load_rom
from traces import parse_range

TIME_TOLERANCE = 0.001   # seconds that an event can be from the driver's tick, for the MIDI tempos' rounding
PITCH_TOLERANCE_CENTS = 5
DRUM_CHANNEL = 9


def midi_channel(c):
    n = c % 15
    return n if n < DRUM_CHANNEL else n + 1


def read_sf2(path):
    """Returns the presets as {(bank, program): (sample, offset)}, each sample a dict of its name, rate, root key,
    points and loop."""
    data = Path(path).read_bytes()
    chunks = {}
    pos = 12
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        if cid == b'LIST':
            inner, end = pos + 12, pos + 8 + size
            while inner < end:
                sid, ssize = data[inner:inner + 4], struct.unpack('<I', data[inner + 4:inner + 8])[0]
                chunks[sid.decode()] = data[inner + 8:inner + 8 + ssize]
                inner += 8 + ssize + (ssize & 1)
        pos += 8 + size + (size & 1)

    def records(name, size):
        raw = chunks[name]
        return [raw[i:i + size] for i in range(0, len(raw), size)]

    phdr, pbag, pgen = records('phdr', 38), records('pbag', 4), records('pgen', 4)
    inst, ibag, igen, shdr = records('inst', 22), records('ibag', 4), records('igen', 4), records('shdr', 46)
    smpl = chunks['smpl']
    samples = []
    for s in shdr[:-1]:
        name = s[:20].split(b'\0')[0].decode('latin-1')
        start, end, loop_start, loop_end, rate = struct.unpack('<5I', s[20:40])
        points = struct.unpack('<%dh' % (end - start), smpl[2 * start:2 * end])
        samples.append({'name': name, 'rate': rate, 'root': s[40], 'points': points,
                        'loop': (loop_start - start, loop_end - start) if loop_end > loop_start else None})

    def gens(bag_records, gen_records, index):
        first = struct.unpack('<H', bag_records[index][:2])[0]
        last = struct.unpack('<H', bag_records[index + 1][:2])[0]
        return {struct.unpack('<H', g[:2])[0]: struct.unpack('<h', g[2:])[0] for g in gen_records[first:last]}

    presets = {}
    for p in range(len(phdr) - 1):
        program, bank, bag = struct.unpack('<HHH', phdr[p][20:26])
        instrument = gens(pbag, pgen, bag)[41]

        # The instrument's global zone comes first and holds no sample. The zone after it plays the sample.
        first, last = (struct.unpack('<H', inst[i][20:22])[0] for i in (instrument, instrument + 1))
        g = next(z for z in (gens(ibag, igen, b) for b in range(first, last)) if 53 in z)
        sample = dict(samples[g[53]])
        if not g.get(54, 0) & 1:
            sample['loop'] = None
        presets[(bank, program)] = (sample, g.get(0, 0) + 32768 * g.get(4, 0))
    return presets


def game_sample(rom, header):
    """Returns the points of the game's sample as the SoundFont should hold them, and its loop."""
    loop_length, end = struct.unpack_from('<II', rom, header - ROM_BASE)
    loop = rom[header + 0x10 - ROM_BASE]
    data = header + 0x12
    points = [(b - 128) * 256 for b in rom[data - ROM_BASE:end - ROM_BASE]]
    length = len(points)
    if not loop or not 0 < loop_length <= length:
        return points, None
    if loop == 2:
        points += points[length - loop_length:][::-1]
    return points, (length - loop_length, len(points))


def driver_ticks(rom, play, frames):
    """Runs the game's code as krapPlay() plays `play`, a (module, mode, song), and returns each tick's time in the
    mixer's samples and the state of each of the module's channels after it."""
    module, mode, song = play
    emu = driver_emu.KrawallEmulator(rom)
    a = emu.addr
    channels = rom[module - ROM_BASE]
    moves = collections.Counter()
    ticks = []
    frame = [0]

    def mix_record(handle):
        if (handle & 0xFF) >= driver_emu.MIX_CHANNELS:
            return None
        return emu.read(a.mixer + driver_emu.MIX_SIZE * (handle & 0xFF), driver_emu.MIX_SIZE)

    def on_set_pos(uc, address, size, user):
        handle, pos = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
        m = mix_record(handle)
        if m and struct.unpack_from('<I', m, 0x18)[0] == handle:
            start, end = struct.unpack_from('<II', m, 4)[0], struct.unpack_from('<I', m, 0xC)[0]
            if (start + pos) & 0xFFFFFFFF < end:
                moves[handle] += 1

    def after_tick(uc, address, size, user):
        # The worker keeps the left buffer's pointer, moved on past the samples mixed so far, below its frame pointer.
        left = struct.unpack('<I', emu.read(uc.reg_read(UC_ARM_REG_FP) - 0x1C, 4))[0]
        time = frame[0] * a.frame_samples + left - driver_emu.LEFT
        states = []
        for c in range(channels):
            handle = struct.unpack('<I', emu.read(a.player + driver_emu.PLAYER_SIZE + driver_emu.CHANNEL_SIZE * c,
                                                  4))[0]
            m = mix_record(handle)
            if handle == 0 or m is None or struct.unpack_from('<I', m, 0x18)[0] != handle or m[2] != 1:
                states.append(None)
                continue
            start, pos = struct.unpack_from('<II', m, 4)
            inc = struct.unpack_from('<i', m, 0x14)[0]
            states.append({'handle': handle, 'moves': moves[handle], 'start': start, 'offset': pos - start,
                           'rate': abs(inc) * 16384 / 65536, 'left': m[0x1E], 'right': m[0x1F]})
        ticks.append((time, states))

    emu.uc.hook_add(UC_HOOK_CODE, on_set_pos, begin=a.set_pos, end=a.set_pos)
    emu.uc.hook_add(UC_HOOK_CODE, after_tick, begin=a.after_tick, end=a.after_tick)
    emu.play(module, mode, song)
    for f in range(frames):
        frame[0] = f
        emu.frame()
    return ticks


def expected_notes(ticks, c, end):
    """Returns the notes of channel `c` up to tick `end`: dicts of their first and last tick, their sample's start,
    their point and each tick's state."""
    notes, last = [], None
    for i, (_, states) in enumerate(ticks[:end + 1]):
        s = states[c]
        started = s is not None and (last is None or s['handle'] != last['handle'] or s['moves'] != last['moves'])
        if notes and notes[-1]['off'] is None and (s is None or started or i == end):
            notes[-1]['off'] = i
        if i == end:
            break
        if s is not None and (not notes or notes[-1]['off'] is not None) and s['rate'] > 0:
            notes.append({'on': i, 'off': None, 'start': s['start'], 'offset': s['offset'], 'states': []})
        if notes and notes[-1]['off'] is None:
            notes[-1]['states'].append((i, s))
        last = s
    return notes


def levels_to_controllers(left, right):
    """Returns the CC10 and CC11 that agbsf2's LevelsToControllers() works out for levels on each side (full scale
    128). CC10 is None when both levels are 0."""
    gl, gr = left / 128, right / 128
    amp = math.sqrt(gl * gl + gr * gr)
    if amp <= 0:
        return None, 0
    cc10 = 64 if left == right else min(127, max(0, round(math.atan2(gr, gl) / (math.pi / 2) * 127)))
    loudest = math.sqrt(2) * 127 / 128
    return cc10, min(127, max(0, round(127 * math.sqrt(amp / loudest))))


def midi_length(path):
    """Returns the seconds to the end of a MIDI file's longest track."""
    data = Path(path).read_bytes()
    division = struct.unpack('>H', data[12:14])[0]
    tempos, end, pos = [], 0, 14
    while pos < len(data):
        size = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        body, pos = data[pos + 8:pos + 8 + size], pos + 8 + size
        tick, i, status = 0, 0, 0
        while i < len(body):
            delta = 0
            while True:
                delta, i = delta << 7 | (body[i] & 0x7F), i + 1
                if body[i - 1] < 0x80:
                    break
            tick += delta
            if body[i] >= 0x80:
                status, i = body[i], i + 1
            if status == 0xFF:
                kind, length, i = body[i], body[i + 1], i + 2
                if kind == 0x51:
                    tempos.append((tick, int.from_bytes(body[i:i + 3], 'big')))
                i += length
            else:
                i += 1 if status & 0xF0 in (0xC0, 0xD0) else 2
        end = max(end, tick)
    seconds, at, tempo = 0.0, 0, 500000
    for when, value in sorted(tempos):
        seconds += (min(when, end) - at) * tempo / division / 1e6
        at, tempo = min(when, end), value
    return seconds + (end - at) * tempo / division / 1e6


def value_at(changes, ch, kind, t, default):
    values = changes.get((ch, kind), [])
    i = bisect.bisect_right([v[0] for v in values], t + TIME_TOLERANCE)
    return values[i - 1][1] if i else default


def check_song(rom, info, song, midi_path, sf2_path, report):
    module, scale = info['modules'][song], info['scale']
    seconds = midi_length(midi_path) if midi_path else info['seconds'][song]
    frames = int(seconds * 16384 / 276) + 3
    ticks = driver_ticks(rom, info['plays'][song], frames)
    first = ticks[0][0]
    times = [(t - first) / 16384 for t, _ in ticks]
    end = min(range(len(times)), key=lambda i: abs(times[i] - seconds))
    channels = rom[module - ROM_BASE]
    expected = {c: expected_notes(ticks, c, end) for c in range(channels)}
    if midi_path is None:
        playing = [c for c, notes in expected.items() if notes]
        if playing:
            report('module %d: no MIDI file, but channels %s play' % (song, playing))
            return False
        return True

    notes, changes = conversion.read_timing(midi_path)
    presets = read_sf2(sf2_path)
    headers = {}
    problems = 0

    def problem(text):
        nonlocal problems
        problems += 1
        if problems <= 10:
            report('module %d: %s' % (song, text))

    for c in range(channels):
        ch = midi_channel(c)
        got = notes.get(ch, [])
        want = expected[c]
        if len(got) != len(want):
            problem('channel %d has %d notes, the driver %d' % (c, len(got), len(want)))
        bend_range = value_at(changes, ch, ('cc', 6), 0, 2)
        for n, (start, stop, key, bank, program) in zip(want, got):
            if abs(start - times[n['on']]) > TIME_TOLERANCE or abs(stop - times[n['off']]) > TIME_TOLERANCE:
                problem('channel %d: note at %.4f-%.4f s, the driver %.4f-%.4f s' % (c, start, stop, times[n['on']],
                                                                                     times[n['off']]))
                continue

            # The preset's sample and the point the note starts from.
            sample, offset = presets[(bank, program)]
            header = n['start'] - 0x12
            if header not in headers:
                headers[header] = game_sample(rom, header)
            points, loop = headers[header]
            if list(sample['points'][:len(points)]) != points or sample['loop'] != loop:
                problem('channel %d: note at %.4f s plays %s, which isn\'t the sample at 0x%08X' % (
                    c, start, sample['name'], header))
            if offset != n['offset']:
                problem('channel %d: note at %.4f s starts at point %d, the driver %d' % (c, start, offset,
                                                                                          n['offset']))

            # Each tick's pitch and levels.
            for i, s in n['states']:
                t = times[i]
                bend = (value_at(changes, ch, 'bend', t, 8192) - 8192) / 8192 * bend_range
                rate = sample['rate'] * 2 ** ((key + bend - sample['root']) / 12)
                if s['rate'] > 0 and abs(1200 * math.log2(rate / s['rate'])) > PITCH_TOLERANCE_CENTS:
                    problem('channel %d: at %.4f s plays at %.1f Hz, the driver %.1f Hz' % (c, t, rate, s['rate']))
                    break
                cc10, cc11 = levels_to_controllers(s['left'] * scale, s['right'] * scale)
                got10 = value_at(changes, ch, ('cc', 10), t, 64)
                got11 = value_at(changes, ch, ('cc', 11), t, 0)
                if abs(got11 - cc11) > 1 or (cc10 is not None and cc11 > 0 and abs(got10 - cc10) > 1):
                    problem('channel %d: at %.4f s has CC10 %d and CC11 %d, the driver\'s levels %d and %d need %s '
                            'and %d' % (c, t, got10, got11, s['left'], s['right'], cc10, cc11))
                    break

    for ch in notes:
        if ch not in [midi_channel(c) for c in range(channels)]:
            problem('MIDI channel %d stands for none of the module\'s channels' % ch)
    if problems > 10:
        report('module %d: %d more problems' % (song, problems - 10))
    return problems == 0


def read_info(tool, rom_path):
    """Returns the modules' addresses and lengths, and the level of the mix's output, from `--info`."""
    text = subprocess.run([tool, '--driver', 'krawall', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    modules, seconds = {}, {}
    for n, address, minutes, secs in re.findall(r'^\s+(\d+)\s+0x([0-9A-F]{8})\s+\d+\s+(\d+):(\d+\.\d+)', text,
                                                re.MULTILINE):
        modules[int(n)] = int(address, 16)
        seconds[int(n)] = int(minutes) * 60 + float(secs)
    if not modules:
        raise SystemExit('found no modules in the list of agbsf2 --info')
    master = int(re.search(r'master volume 0x([0-9A-F]+)', text).group(1), 16)
    shift = master >> 16 or 5
    return {'modules': modules, 'seconds': seconds, 'scale': (master & 0xFF) / (1 << (3 + shift))}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program, for --info')
    p.add_argument('folder', metavar='FOLDER', help="agbsf2's conversion of ROM")
    p.add_argument('--songs', help='modules to check, such as 0-20 (default: all)')
    a = p.parse_args()

    rom = load_rom(a.rom)
    info = read_info(a.agbsf2, a.rom)
    info['plays'] = driver_emu.song_plays(rom, info['modules'])
    files = dict(conversion.midi_files(a.folder, 'krawall'))
    if not files:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    songs = parse_range(a.songs) if a.songs else sorted(info['modules'])
    failed = 0
    for song in songs:
        midi = files.get(song)
        sf2 = midi.with_suffix('.sf2') if midi else None
        if check_song(rom, info, song, midi, sf2, print) is False:
            failed += 1
    print('%d of %d modules match the driver' % (len(songs) - failed, len(songs)))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
