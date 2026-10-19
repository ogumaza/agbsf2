#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against the game's driver, note by note.

    compare_notes.py ROM FOLDER [--songs 0-22] [--frames 12000]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each song, or NAME_rare_NN.mid and
NAME_rare_NN.sf2 if detection finds another of the game's drivers first. For every song, the driver runs under
driver_emu.py for as long as the MIDI file lasts, and each note it plays is matched with a MIDI note on the same channel
and key that starts within a frame and a half of it. For each pair, the script checks:

  - the velocity, and the channel's volume when the note starts;
  - the sample: the SoundFont zone that the note's bank, program and key choose has to play the driver's sample;
  - the envelope: the zone's attack, decay, sustain and release, for each phase that the driver's note reached, have to
    be those that agbsf2 works out from the instrument and the settings from controllers 20-23 that the phase
    took;
  - the pitch on every frame of the note, from the zone's root key, tuning and sample rate and the channel's pitch bend,
    against the step the driver moves the sample on by;
  - the release, which has to come within a frame and a half of the MIDI note's end, unless the MIDI file ends the note
    earlier because the key starts again there.

A MIDI note that the driver doesn't play, or a driver note that the MIDI file doesn't have, counts as a difference. MIDI
events land on their own ticks, where the driver plays them at the start of the frame that reaches them, so a difference
of a frame and a half either way is allowed.
"""
import argparse
import bisect
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py, gbarom.py and traces.py, in tools/
import conversion
import driver_emu
from gbarom import load_rom
from traces import parse_range

FRAME_RATE = 16777216 / 280896


def read_midi(path):
    """Returns the file's notes as (channel, key, velocity, on seconds, off seconds, program, bank), its program
    changes, volume changes and pitch bends as (seconds, channel, value), each channel's bend range in semitones, and
    its length in seconds."""
    data = Path(path).read_bytes()
    division = struct.unpack('>H', data[12:14])[0]
    events = []
    pos = 14
    track = 0
    while pos < len(data):
        size = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        pos += 8 + size
        tick, i, status = 0, 0, 0
        index = 0
        while i < len(body):
            index += 1
            delta = 0
            while True:
                b = body[i]
                i += 1
                delta = (delta << 7) | (b & 0x7F)
                if b < 0x80:
                    break
            tick += delta
            if body[i] >= 0x80:
                status = body[i]
                i += 1
            if status == 0xFF:
                kind = body[i]
                length = 0
                i += 1
                while True:
                    b = body[i]
                    i += 1
                    length = (length << 7) | (b & 0x7F)
                    if b < 0x80:
                        break
                if kind == 0x51:
                    events.append((tick, track, index, 'tempo', 0, int.from_bytes(body[i:i + 3], 'big'), 0))
                elif kind == 0x2F:
                    events.append((tick, track, index, 'end', 0, 0, 0))
                i += length
                continue
            kind, ch = status & 0xF0, status & 0x0F
            n = 1 if kind in (0xC0, 0xD0) else 2
            args = body[i:i + n]
            i += n
            if kind == 0x90 and args[1] > 0:
                events.append((tick, track, index, 'on', ch, args[0], args[1]))
            elif kind in (0x80, 0x90):
                events.append((tick, track, index, 'off', ch, args[0], 0))
            elif kind == 0xC0:
                events.append((tick, track, index, 'program', ch, args[0], 0))
            elif kind == 0xB0:
                events.append((tick, track, index, 'cc', ch, args[0], args[1]))
            elif kind == 0xE0:
                events.append((tick, track, index, 'bend', ch, args[0] | args[1] << 7, 0))
        track += 1

    # A player takes the events of a tick track by track, each track's in the order the file gives them.
    events.sort(key=lambda e: (e[0], e[1], e[2]))

    # Ticks to seconds, through the tempo changes.
    tempos = [(0, 500000)] + [(e[0], e[5]) for e in events if e[3] == 'tempo']

    def seconds(tick):
        s, at, tempo = 0.0, 0, 500000
        for t, value in tempos:
            if t > tick:
                break
            s += (t - at) * tempo / division / 1e6
            at, tempo = t, value
        return s + (tick - at) * tempo / division / 1e6

    notes, programs, volumes, bends = [], [], [], []
    end = seconds(max(e[0] for e in events))
    open_notes = {}
    rpn = {}
    ranges = {}
    program = {}
    bank = {}
    for tick, _, _, kind, ch, a, b in events:
        t = seconds(tick)
        if kind == 'on':
            open_notes[(ch, a)] = [ch, a, b, t, None, program.get(ch, 0), bank.get(ch, 0)]
            notes.append(open_notes[(ch, a)])
        elif kind == 'cc' and a == 0:
            bank[ch] = b
        elif kind == 'off' and (ch, a) in open_notes:
            open_notes.pop((ch, a))[4] = t
        elif kind == 'program':
            programs.append((t, ch, a))
            program[ch] = a
        elif kind == 'bend':
            bends.append((t, ch, a))
        elif kind == 'cc' and a == 7:
            volumes.append((t, ch, b))
        elif kind == 'cc' and a in (100, 101):
            rpn[(ch, a)] = b
        elif kind == 'cc' and a == 6 and rpn.get((ch, 101)) == 0 and rpn.get((ch, 100)) == 0:
            ranges[ch] = b
    return notes, programs, volumes, bends, ranges, end


def read_sf2(path):
    """Returns the presets as {(bank, program): [zone, ...]}, each zone a dict of its key range and generators, with its
    sample's name, rate and root key."""
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
    samples = []
    for s in shdr[:-1]:
        name = s[:20].split(b'\0')[0].decode('latin-1')
        start, end = struct.unpack('<II', s[20:28])
        rate, root = struct.unpack('<I', s[36:40])[0], s[40]
        samples.append({'name': name, 'rate': rate, 'root': root, 'length': end - start})

    def gens(bag_records, gen_records, index):
        first = struct.unpack('<H', bag_records[index][:2])[0]
        last = struct.unpack('<H', bag_records[index + 1][:2])[0]
        return {struct.unpack('<H', g[:2])[0]: struct.unpack('<h', g[2:])[0] for g in gen_records[first:last]}

    presets = {}
    for p in range(len(phdr) - 1):
        program, bank, bag = struct.unpack('<HHH', phdr[p][20:26])
        instrument = gens(pbag, pgen, bag)[41]
        first = struct.unpack('<H', inst[instrument][20:22])[0]
        last = struct.unpack('<H', inst[instrument + 1][20:22])[0]
        zones = []
        for z in range(first, last):
            g = gens(ibag, igen, z)
            if 53 not in g:
                continue  # the global zone
            low, high = g.get(43, 0x7F00) & 0xFF, (g.get(43, 0x7F00) >> 8) & 0xFF
            zones.append(dict(g=g, low=low, high=high, sample=samples[g[53]]))
        presets[(bank, program)] = zones
    return presets


def by_channel(changes):
    """Returns a list of (time, channel, value) changes in time order as each channel's times and values."""
    out = {}
    for at, ch, v in changes:
        times, values = out.setdefault(ch, ([], []))
        times.append(at)
        values.append(v)
    return out


def value_at(changes, ch, t, default):
    """Returns a channel's value at time `t` from the changes grouped by by_channel()."""
    times, values = changes.get(ch, ((), ()))
    i = bisect.bisect_right(times, t)
    return values[i - 1] if i else default


def driver_notes(rom, song, frames):
    """Runs the driver and returns its notes, each channel's volume on each frame, the mixer's rate in Hz and the fade
    table. The notes are dicts of channel, key, velocity, start and release frames (the end, for a note the driver
    didn't release), whether the driver released the note, its instrument's and sample's addresses, the step on each
    frame, and the settings from controllers 20-23 that each phase of its envelope took, for the phases it reached."""
    emu = driver_emu.DriverEmulator(rom)
    emu.play(song)
    count = 16 * emu.addr.slots_per_channel
    notes, current = [], [None] * count
    last = [None] * count
    volume = []
    for f in range(frames):
        emu.frame()
        volume.append([emu.channel_volume(c) for c in range(16)])
        # Each phase of an envelope starts after the frame's tracks, so it takes the settings they leave.
        settings = [emu.envelope_settings(c) for c in range(16)]
        voices = emu.voices()[:count]
        for v in voices:
            prev = last[v.index]
            # A note that the driver releases and starts again in one frame shows as its envelope starting again: its
            # phase goes back, or it's in its first frame's phase 2 again, or its attack falls.
            active = v.active()
            restarted = prev is not None and (v.phase < prev.phase or v.phase == prev.phase == 2 or
                                              (v.phase == prev.phase == 1 and v.level < prev.level))
            begins = active and (prev is None or not prev.active() or
                                 (prev.state == 0x12 and v.state == 0x11) or prev.instrument != v.instrument or
                                 prev.key != v.key or restarted)
            note = current[v.index]
            # The decay starts in the frame after the one that leaves the note in phase 2.
            if note and not begins and prev is not None and prev.phase == 2 and 'decay' not in note['envelope']:
                note['envelope'].update(decay=settings[prev.channel][1], sustain=settings[prev.channel][2])
            if note and (not active or begins):
                if note['release'] is None:
                    note['release'] = f
                current[v.index] = None
            if begins:
                sample = struct.unpack('<I', rom[v.instrument - 0x08000000 + 16:v.instrument - 0x08000000 + 20])[0] \
                    if 0x08000000 <= v.instrument < 0x08000000 + len(rom) else 0
                note = dict(channel=v.channel, key=v.key, velocity=v.velocity - 1, start=f, release=None,
                            released=False, instrument=v.instrument, sample=sample, steps={},
                            envelope=dict(attack=settings[v.channel][0]))
                notes.append(note)
                current[v.index] = note
            else:
                note = current[v.index]
            if note:
                if v.state == 0x12 and note['release'] is None:
                    note['release'] = f
                    note['released'] = True
                    note['envelope']['release'] = settings[v.channel][3]
                # The mixer leaves no step for a voice it doesn't mix, when more voices play than it can mix.
                if emu.steps[v.index]:
                    note['steps'][f] = emu.steps[v.index]
            last[v.index] = v
        if not emu.playing():
            break
    for note in notes:
        if note['release'] is None:
            note['release'] = f + 1
    return notes, volume, emu.rate(), emu.fade_table()


def lround(x):
    """Rounds half away from zero, as std::lround does."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def timecents(seconds):
    return -12000 if seconds <= 0.001 else max(-12000, min(8000, lround(1200 * math.log2(seconds))))


def fade_seconds(frames):
    """The length that agbsf2 gives a SoundFont fade for a decay or release of `frames` frames to silence."""
    return min(6.0, max(3.0, 3.0 + 3.0 * (frames - 5) / 11.0)) * frames / FRAME_RATE


def expected_envelope(rom, instrument, envelope, fade):
    """Returns the generators that agbsf2 gives the volume envelope of the instrument at `instrument` (attack,
    decay, sustain and release, each None where the zone leaves the default), for the settings that `envelope` gives
    each phase. A setting of 0x80 or more, or one missing, leaves the instrument's."""
    at = instrument - 0x08000000
    attack, decay, sustain, release = struct.unpack('<4I', rom[at + 36:at + 52])

    def frames(setting, index):
        entry = setting if setting < 0x80 else (fade[index] if index < 100 else 0)
        return entry + 1 if entry else 0

    steps = envelope.get('attack', 0xFF)
    steps = steps if steps < 0x80 else ((99 - min(attack, 99)) << 5) // 99
    level = envelope.get('sustain', 0xFF)
    if level < 0x80:
        level <<= 8
    else:
        value = min(sustain, 99)
        level = ((value << 7) // 99) << 8 | ((((value << 7) % 99) << 8) // 99)
    decay_frames = frames(envelope.get('decay', 0xFF), decay)
    release_frames = frames(envelope.get('release', 0xFF), release)

    out = [timecents((steps + 1) / FRAME_RATE) if steps else None, None, None,
           timecents(fade_seconds(release_frames)) if release_frames else None]
    if level < 0x8000 and level << 4 < 0x1000:
        out[1:3] = [timecents(fade_seconds(decay_frames)) if decay_frames else None, 1440]
    elif level < 0x8000:
        attenuation = min(1440, lround(-200 * math.log10(level / 32768)))
        out[1:3] = [timecents(1000 / attenuation * decay_frames / FRAME_RATE) if decay_frames and attenuation else None,
                    attenuation or None]
    return [None if v == -12000 else v for v in out]


def check_song(rom, song, midi_path, sf2_path, frames_limit, report):
    notes, programs, volumes, bends, ranges, length = read_midi(midi_path)
    volumes, bends = by_channel(volumes), by_channel(bends)
    presets = read_sf2(sf2_path)
    frames = min(frames_limit, int(length * FRAME_RATE))
    played, volume, mix_rate, fade = driver_notes(rom, song, frames)

    midi = [dict(channel=n[0], key=n[1], velocity=n[2], on=n[3] * FRAME_RATE,
                 off=(n[4] if n[4] is not None else length) * FRAME_RATE, program=n[5], bank=n[6], matched=False)
            for n in notes]
    by_key = {}
    for m in midi:
        by_key.setdefault((m['channel'], m['key']), []).append(m)

    # A key that a channel plays twice on one frame is one MIDI note, as loud as the voices that last past the frame
    # together. A voice that the driver releases on the frame it starts on sounds for a frame or two, and only counts
    # if no other does.
    groups = {}
    for d in played:
        if d['start'] < frames - 1:
            groups.setdefault((d['channel'], d['key'], d['start']), []).append(d)

    problems = []
    worst_cents = 0.0
    checked = 0
    for (channel, key, start), group in sorted(groups.items(), key=lambda item: item[0][2]):
        lasting = [d for d in group if d['release'] > d['start']]
        voices = lasting or group
        velocity = min(127, sum(d['velocity'] + 1 for d in voices) - 1)
        candidates = [m for m in by_key.get((channel, key), []) if not m['matched'] and -1.5 <= m['on'] - start <= 1.5]
        if not candidates:
            problems.append('frame %d: the driver plays key %d on channel %d, which the MIDI file doesn\'t' % (
                start, key, channel))
            continue

        # Notes of the same key can start a frame apart, and a MIDI note can be short because the next note on its key
        # cuts it, or because the driver released it on the frame it started. So the closest MIDI note with the right
        # velocity that lasts goes first, then one with the right velocity, then one that lasts.
        def rank(c):
            lasts = not lasting or c['off'] - c['on'] > 1
            right = c['velocity'] == velocity
            return (not (lasts and right), not right, not lasts, abs(c['on'] - start))

        m = min(candidates, key=rank)
        m['matched'] = True
        checked += len(group)
        where = 'frame %d, channel %d key %d' % (start, channel, key)
        if m['velocity'] != velocity:
            problems.append('%s: velocity %d, the driver\'s %d' % (where, m['velocity'], velocity))

        # The driver mixes the frame at the volume the channel has at its end.
        driver_volume = volume[start][channel]
        volumes_seen = [value_at(volumes, channel, (start + 1 + shift) / FRAME_RATE, 100)
                        for shift in (-1.000001, -0.000001, 0.999999)]
        if driver_volume not in volumes_seen:
            problems.append('%s: volume %d, the driver\'s %d' % (where, volumes_seen[1], driver_volume))

        # The zone that the note's bank, program and key choose.
        zones = [z for z in presets.get((m['bank'], m['program']), []) if z['low'] <= key <= z['high']]
        if not zones:
            problems.append('%s: bank %d program %d has no zone for the key' % (where, m['bank'], m['program']))
            continue
        zone = zones[0]
        name = zone['sample']['name']
        if not all(name.endswith('%08X' % d['sample']) for d in voices):
            problems.append('%s: plays %s; the driver plays the sample at 0x%08X' % (where, name, voices[0]['sample']))
            continue

        # The envelope, in each phase that the driver's note reached: the instrument's, with the settings from
        # controllers 20-23 that the phase took. Timecents can come out a step apart from floating point.
        want = expected_envelope(rom, voices[0]['instrument'], voices[0]['envelope'], fade)
        have = [zone['g'].get(op) for op in (34, 36, 37, 38)]
        reached = [True, 'decay' in voices[0]['envelope'], 'decay' in voices[0]['envelope'],
                   'release' in voices[0]['envelope']]
        for phase, w, h, r in zip(('attack', 'decay', 'sustain', 'release'), want, have, reached):
            if not r:
                continue
            if (w is None) != (h is None) or (w is not None and abs(w - h) > 1):
                problems.append('%s: the %s is %s, where the driver\'s envelope gives %s' % (where, phase, h, w))

        # Check the pitch on each frame in which the driver advances the note.
        g = zone['g']
        scale = g.get(56, 100)
        root = g.get(58, zone['sample']['root'])
        cents = scale * (key - root) + 100 * g.get(51, 0) + g.get(52, 0)
        bend_range = ranges.get(channel, 2)
        for d in voices:
            for f, step in sorted(d['steps'].items()):
                if f >= frames - 1 or f >= d['release'] or step <= 0:
                    continue
                want = step * mix_rate / (1 << 23)
                # A bend lands on its tick, which can be up to a frame after the start of the frame the driver plays it
                # on, and a frame's step of vibrato on the tick at or just before the frame's start. So the pitch is
                # read across the frame and the frames either side, and the closest counts.
                errors = []
                for step_of_frame in range(-30, 60):
                    bend = value_at(bends, channel, (f + step_of_frame / 30 + 1e-6) / FRAME_RATE, 8192)
                    total = cents + (bend - 8192) / 8192 * bend_range * 100
                    have = zone['sample']['rate'] * 2 ** (total / 1200)
                    errors.append(1200 * math.log2(have / want))
                error = min(errors, key=abs)
                worst_cents = max(worst_cents, abs(error))
                if abs(error) > 2:
                    problems.append('%s: %+.1f cents on frame %d' % (where, error, f))
                    break

        # The release, unless the note ended by itself first, or the MIDI note ended first because the key was played
        # again.
        release = max(d['release'] for d in voices)
        cut = m['off'] < release - 1.5 and any(o is not m and abs(o['on'] - m['off']) <= 1.5
                                               for o in by_key[(channel, key)])
        if any(d['released'] for d in voices) and abs(m['off'] - release) > 1.5 and not cut:
            problems.append('%s: released on frame %d, the MIDI note ends at %.1f' % (where, release, m['off']))

    # The driver can start and finish a short sample without a loop inside one frame, between two looks at it. A note
    # that comes just before its channel's first program change, on the same tick, plays the instrument the channel had
    # in the song before, which the driver run here doesn't have.
    first_program = {}
    for t, ch, _ in programs:
        first_program.setdefault(ch, t)
    for m in midi:
        if m['matched'] or m['on'] >= frames - 2 or m['off'] - m['on'] <= 1:
            continue
        if abs(first_program.get(m['channel'], -1) - m['on'] / FRAME_RATE) < 1e-9:
            continue
        zones = [z for z in presets.get((m['bank'], m['program']), []) if z['low'] <= m['key'] <= z['high']]
        if zones and zones[0]['g'].get(54, 0) == 0:
            zone = zones[0]
            ratio = 2 ** (zone['g'].get(56, 100) * (m['key'] - zone['g'].get(58, zone['sample']['root'])) / 1200)
            if zone['sample']['length'] / (zone['sample']['rate'] * ratio) < 1.5 / FRAME_RATE:
                continue
        problems.append('%.1f: the MIDI file plays key %d on channel %d, which the driver doesn\'t' % (
            m['on'], m['key'], m['channel']))
    report(song, checked, problems, worst_cents)
    return not problems


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('folder', metavar='FOLDER', help="the folder containing agbsf2's output")
    p.add_argument('--songs', help='songs to check, such as 0-22 (default: every song with a MIDI file)')
    p.add_argument('--frames', type=int, default=12000, help='maximum frames to check per song (default: 12000)')
    p.add_argument('-v', '--verbose', action='store_true', help='list every difference, not just the first ten')
    a = p.parse_args()
    rom = load_rom(a.rom)
    files = conversion.midi_files(a.folder, 'rare')
    if not files:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    wanted = set(parse_range(a.songs)) if a.songs else None
    failed = total = 0

    def report(song, checked, problems, worst):
        print('song %d: %d notes, %d difference%s, pitch within %.2f cents' % (
            song, checked, len(problems), '' if len(problems) == 1 else 's', worst))
        for line in problems if a.verbose else problems[:10]:
            print('    ' + line)

    for song, midi in files:
        if wanted is not None and song not in wanted:
            continue
        total += 1
        if not check_song(rom, song, midi, midi.with_suffix('.sf2'), a.frames, report):
            failed += 1
    # agbsf2 writes no files for a song that plays no notes.
    for song in sorted((wanted or set()) - {s for s, _ in files}):
        print('song %d: no MIDI file to check' % song)
    if not total:
        raise SystemExit('found none of the songs to check')
    print('%d of %d songs match the driver' % (total - failed, total))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
