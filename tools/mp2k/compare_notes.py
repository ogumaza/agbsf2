#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against the game's MP2K sound driver, note by note.

    compare_notes.py ROM FOLDER [--songs 0-22] [--frames 12000] [-j JOBS]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each song, or NAME_mp2k_NN.mid and
NAME_mp2k_NN.sf2 if detection finds another of the game's drivers first. For every song, the driver runs under
driver_emu.py for as long as the MIDI file lasts and two frames more, and each note it starts is matched with a MIDI
note on the MIDI channel of its track, with the same key, that starts within a frame and a half of it. For each pair,
the script checks:

  - the velocity, and the track's CC10 and CC11 when the note starts, against the driver's velocity and levels on
    each side. A velocity of v is written as 127 × √(v / 127);
  - the sound: the SoundFont zone that the note's program and key choose has to play the driver's sample, square
    wave duty, wave pattern or noise type. A note on the drum channel takes its zones from bank 128;
  - the pitch on every frame of the note until its release, from the zone's root key, tuning and sample rate and the
    channel's pitch bend, against the driver's rate for a sample, or its frequency setting for a PSG channel, which can
    be two steps of the 11-bit register from equal temperament;
  - the release, which has to come within a frame and a half of the MIDI note's end, unless the MIDI note ends where
    the key starts again, or the file ends before the driver releases the note.

A MIDI note that the driver doesn't play, or a driver note that the MIDI file doesn't have, counts as a difference. A
driver note that starts in the file's last half frame or after it is left out, since it can be the loop starting again.
MIDI events land on their own ticks, where the driver plays them on the frame whose ticks reach them, so a difference of
a frame and a half either way is allowed. A song named in --songs that has no MIDI file mustn't play any notes.
"""
import argparse
import bisect
import gc
import io
import math
import multiprocessing
import struct
import sys
from pathlib import Path

from unicorn import UC_HOOK_CODE

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py, gbarom.py and traces.py, in tools/
import conversion
import driver_emu
from gbarom import ROM_BASE, load_rom
from traces import parse_range

FRAME_RATE = 16777216 / 280896
TRACK_LEVELS = 16  # a track's right and left levels (volMR and volML), in its MusicPlayerTrack
TRACK_PITCH = 8  # a track's key and fine pitch offsets, in its MusicPlayerTrack
SPECIAL_TEST = bytes.fromhex('0100d4e5300010e3')  # the newer mixer's test for reversed and compressed samples
CAMELOT_PULSE = bytes.fromhex('0260d3e5062c82e00460d3e5066c92e00660e041')  # Camelot's mixer, which has synth voices
CAMELOT_CHANNEL_SEARCH = bytes.fromhex('0a235034c72700220026032021783942')  # Camelot's note start
MONO_VOLUME = bytes.fromhex('a078e17840186843400aa072')  # the mono mixer's volume for a channel
FREQ_TABLE = [2147483648, 2275179671, 2410468894, 2553802834, 2705659852, 2866546760, 3037000500, 3217589947,
              3408917802, 3611622603, 3826380858, 4053909305]  # the driver's rates for the 12 semitones, from key 168


def read_midi(path):
    """Returns the file's notes as (channel, key, velocity, on seconds, off seconds, program, seconds at the start of
    the driver's tick before the off, a 24th of a quarter note), its changes of CC10 and CC11 and its pitch bends as
    (seconds, channel, value), each channel's bend range in semitones, its length in seconds, and the MIDI channel of
    each of the song's tracks. The file's track names give the song's tracks."""
    data = Path(path).read_bytes()
    division = struct.unpack('>H', data[12:14])[0]
    events = []
    names, channels = {}, {}
    pos = 14
    track = 0
    while pos < len(data):
        size = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        pos += 8 + size
        tick, i, status, index = 0, 0, 0, 0
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
                elif kind == 0x03:
                    names[track] = body[i:i + length].decode('latin-1')
                i += length
                continue
            kind, ch = status & 0xF0, status & 0x0F
            channels.setdefault(track, ch)
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

    # A player takes the events of a tick track by track, and within a track in the file's order, which puts a note
    # that plays with the program before a change ahead of the change.
    events.sort(key=lambda e: (e[0], e[1], e[2]))

    tempos = [(0, 500000)] + [(e[0], e[5]) for e in events if e[3] == 'tempo']

    def seconds(tick):
        s, at, tempo = 0.0, 0, 500000
        for t, value in tempos:
            if t > tick:
                break
            s += (t - at) * tempo / division / 1e6
            at, tempo = t, value
        return s + (tick - at) * tempo / division / 1e6

    notes, pans, expressions, bends = [], [], [], []
    end = seconds(max(e[0] for e in events))
    open_notes, rpn, ranges, program = {}, {}, {}, {}
    for tick, _, _, kind, ch, a, b in events:
        t = seconds(tick)
        if kind == 'on':
            open_notes[(ch, a)] = [ch, a, b, t, None, program.get(ch, 0), None]
            notes.append(open_notes[(ch, a)])
        elif kind == 'off' and (ch, a) in open_notes:
            note = open_notes.pop((ch, a))
            note[4], note[6] = t, seconds(max(tick - division // 24, 0))
        elif kind == 'program':
            program[ch] = a
        elif kind == 'bend':
            bends.append((t, ch, a))
        elif kind == 'cc' and a == 10:
            pans.append((t, ch, b))
        elif kind == 'cc' and a == 11:
            expressions.append((t, ch, b))
        elif kind == 'cc' and a in (100, 101):
            rpn[(ch, a)] = b
        elif kind == 'cc' and a == 6 and rpn.get((ch, 101)) == 0 and rpn.get((ch, 100)) == 0:
            ranges[ch] = b
    song_channels = {}
    for t, name in names.items():
        words = name.split()
        if len(words) == 2 and words[0] == 'Track' and words[1].isdigit() and t in channels:
            song_channels[int(words[1])] = channels[t]
    return notes, pans, expressions, bends, ranges, end, song_channels


def read_sf2(path):
    """Returns the presets as {(bank, program): [zone, ...]}, each zone a dict of its key range and generators, with its
    sample's name, rate and root key, and whether a modulator of its own keeps the pitch wheel from moving it."""
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
    imod = records('imod', 10)
    samples = []
    for s in shdr[:-1]:
        name = s[:20].split(b'\0')[0].decode('latin-1')
        rate, root = struct.unpack('<I', s[36:40])[0], s[40]
        samples.append({'name': name, 'rate': rate, 'root': root})

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
            first_mod = struct.unpack('<H', ibag[z][2:4])[0]
            last_mod = struct.unpack('<H', ibag[z + 1][2:4])[0]
            no_bend = any(struct.unpack('<HHhHH', m) == (0x020E, 52, 0, 0x0010, 0) for m in imod[first_mod:last_mod])
            zones.append(dict(g=g, low=low, high=high, sample=samples[g[53]], no_bend=no_bend))
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


def round_half_up(x):
    """Rounds a value of 0 or more to the nearest whole number. Halves go up. C++'s std::lround() does the same."""
    whole = math.floor(x)
    return int(whole) + (1 if x - whole >= 0.5 else 0)


def levels_to_controllers(left, right):
    """Returns CC10 and CC11 for levels on the left and right. 128 is full scale. seqlib's LevelsToControllers() gives
    the same. CC10 is None when both levels are 0. Only CC11 changes then."""
    gl, gr = left / 128.0, right / 128.0
    amp = math.sqrt(gl * gl + gr * gr)
    if amp <= 0:
        return None, 0
    cc10 = 64 if left == right else min(max(round_half_up(math.atan2(gr, gl) / (math.pi / 2) * 127), 0), 127)
    loudest_centred = math.sqrt(2.0) * 127.0 / 128.0
    return cc10, min(max(round_half_up(127 * math.sqrt(amp / loudest_centred)), 0), 127)


def track_controllers(left, right, psg, mono, camelot_sequencer):
    """Returns CC10 and CC11 for a track's levels. agbsf2's TrackControllers() gives the same. A sample note plays at
    the levels. A mono mixer plays it at their average on both sides. A PSG note plays on one side alone where that
    side is at least twice the other, or else on both. The driver's CgbPan decides that, here for a note at full
    velocity. Each side that the note plays on gets the average of the two levels."""
    centre = (left + right) / 2.0
    if not psg:
        return levels_to_controllers(centre, centre) if mono else levels_to_controllers(left, right)
    note_right = min((127 * 128 * right) >> 14, 255)
    note_left = min((127 * (128 if camelot_sequencer else 127) * left) >> 14, 255)
    right_only = note_right >= note_left and note_right // 2 >= note_left
    left_only = note_right < note_left and note_left // 2 >= note_right
    return levels_to_controllers(0 if right_only else centre, 0 if left_only else centre)


def driver_notes(rom, song, frames):
    """Runs the driver and returns its notes and the mixer's rate in Hz. The notes are dicts of track, key, velocity,
    start and release frames (the end, for a note the driver didn't release), the track's levels on the left and right
    at the start, whether it's a PSG note, what the note plays, its frequency on each frame, whether it sounds, whether
    its channel stopped by itself before the driver released it, and the frames in which its pitch is behind its
    track's. The channels are read just before the PSG and mixer routines run each frame, when the notes the tracks
    started in the frame still have their start flag."""
    emu = driver_emu.emulator(rom)
    emu.play(song)
    snapshot = []

    def before_output(uc, address, size, user):
        snapshot[:] = emu.channels()

    psg_routine = emu.u32(emu.info + 40) & ~1
    emu.uc.hook_add(UC_HOOK_CODE, before_output, begin=psg_routine, end=psg_routine)
    player_tracks = emu.player[1]

    notes, current, pitches = [], {}, {}
    for f in range(frames):
        snapshot[:] = []
        emu.frame()
        after = {(c.kind, c.index): c for c in emu.channels()}
        before = pitches
        pitches = {t: bytes(emu.uc.mem_read(player_tracks + driver_emu.TRACK_SIZE * t + TRACK_PITCH, 2))
                   for t in range(emu.player[2])}
        started = set()
        for c in snapshot:
            name = (c.kind, c.index)
            note = current.get(name)
            starts = bool(c.status & 0x80)
            if note and (starts or not c.status & 0xC7):
                if note['release'] is None:
                    note['release'] = f
                    note['stopped'] = not starts
                current.pop(name)
            if starts and not c.status & 0x40 and c.track >= 0:
                at = player_tracks + driver_emu.TRACK_SIZE * c.track + TRACK_LEVELS
                levels = (emu.u8(at + 1), emu.u8(at))
                note = dict(track=c.track, key=c.midi_key, play_key=c.key, velocity=c.velocity, start=f,
                            release=None, type=c.type, wave=c.wav, levels=levels, psg=c.kind != 'd', frequencies={},
                            sounds=False, stopped=False, lagging=False, behind=set())
                notes.append(note)
                current[name] = note
                started.add(c.track)
            note = current.get(name)
            if note:
                if c.status & 0x40 and note['release'] is None:
                    note['release'] = f
                note['frequencies'][f] = c.frequency
                # A sample that ends during the frame stops its channel, but the mixer mixed it up to there at the
                # envelope's level. A PSG channel that stops in the frame never sounded in it.
                late = after[name]
                note['sounds'] = note['sounds'] or bool(late.envelope and (late.status or c.kind == 'd'))

        # A note that a track starts clears the track's change of pitch, so the driver doesn't pass the change on to
        # the track's other notes, which stay behind until a later change of the track's pitch reaches them.
        for note in current.values():
            track = note['track']
            if note['start'] < f and pitches.get(track) != before.get(track, pitches.get(track)):
                note['lagging'] = track in started
            if note['lagging']:
                note['behind'].add(f)
        if not emu.playing():
            break
    for note in notes:
        if note['release'] is None:
            note['release'] = f + 1
    return notes, emu.rate()


def synth_type(rom, note, camelot):
    """Returns the type of synth voice that Camelot's mixer (`camelot`) plays for a note, from its sample's data: 0 for
    a pulse wave, 1 for a saw wave and anything else for a triangle wave, or None for a note that isn't a synth."""
    offset = note['wave'] - ROM_BASE
    if not camelot or note['type'] & 0x0F or not 0 <= offset <= len(rom) - 18:
        return None
    if rom[offset + 12:offset + 16] != bytes(4):
        return None
    return struct.unpack('<b', rom[offset + 17:offset + 18])[0]


def sound_name(rom, note, special, camelot):
    """Returns the name the SoundFont gives the note's sound, from its type and wave field. Only a mixer that tests for
    them (`special`) plays reversed samples backwards, and only Camelot's mixer (`camelot`) has synth voices, which get
    a sample for each key they play at, unless they play a triangle wave."""
    psg = note['type'] & 7
    synth = synth_type(rom, note, camelot)
    if synth in (0, 1):
        return '%s %08X %d' % ('Pulse' if synth == 0 else 'Saw', note['wave'], note['play_key'])
    if synth is not None:
        return 'Triangle %08X' % note['wave']
    if psg == 0:
        offset = note['wave'] - ROM_BASE
        reversed_sample = note['type'] & 0x10 and special
        return ('Reversed %08X' if reversed_sample else 'Sample %08X') % note['wave'] if 0 <= offset < len(rom) \
            else None
    if psg in (1, 2):
        return 'Square ' + ('12.5%', '25%', '50%', '75%')[note['wave'] & 3]
    if psg == 3:
        return 'Wave %08X' % note['wave']
    return 'Noise 7-bit' if note['wave'] & 1 else 'Noise 15-bit'


def driver_hz(note, frequency, mix_rate):
    """Returns the pitch the driver plays a note at: a sample's rate, a PSG wave's cycle frequency, or the noise
    generator's step rate. A PSG square or wave channel's setting is its 11-bit register value."""
    psg = note['type'] & 7
    if psg == 0:
        return mix_rate if note['type'] & 8 else frequency
    if psg in (1, 2):
        return 131072 / (2048 - (frequency & 0x7FF))
    if psg == 3:
        return 65536 / (2048 - (frequency & 0x7FF))
    ratio, shift = frequency & 7, (frequency >> 4) & 15
    return 524288 / (ratio if ratio else 0.5) / (2 << shift)


def register_step_cents(note, frequency):
    """Returns the cents between a PSG square or wave channel's frequency and the next register value's, or 0 for other
    channels. The driver's settings land within two such steps of equal temperament: its table rounds down, and so does
    its interpolation between the table's entries for a bend."""
    if note['type'] & 7 not in (1, 2, 3):
        return 0.0
    period = 2048 - (frequency & 0x7FF)
    return 1200 * math.log2(period / (period - 1)) if period > 1 else 0.0


def synth_rate(rom, name):
    """Returns the rate in Hz that the driver gives a sample for the key that a pulse or saw synth voice's sample is
    made for, from the sample's name. The sample plays the key at that pitch at the mixer's rate."""
    _, wave, key = name.split()
    header = int(wave, 16) - ROM_BASE
    frequency = struct.unpack('<I', rom[header + 4:header + 8])[0]
    return frequency * (FREQ_TABLE[int(key) % 12] >> (14 - int(key) // 12)) >> 32


def zone_hz(zone, key, cents_from_bend):
    """Returns the rate a zone plays its sample at for `key` and a pitch bend, in the units driver_hz() uses: a square
    sample has 64 points a cycle, a wave pattern 32, and noise and samples one point for each step."""
    g = zone['g']
    scale = g.get(56, 100)
    root = g.get(58, zone['sample']['root'])
    cents = scale * (key - root) + 100 * g.get(51, 0) + g.get(52, 0) + cents_from_bend
    rate = zone['sample']['rate'] * 2 ** (cents / 1200)
    name = zone['sample']['name']
    if name.startswith('Square'):
        return rate / 64
    if name.startswith('Wave'):
        return rate / 32
    return rate


def check_song(rom, song, midi_path, sf2_path, frames_limit, report):
    """Checks one song, and passes report() its number, the notes checked, the differences and the largest pitch
    error. Returns true if there are no differences."""
    notes, pans, expressions, bends, ranges, length, channel_of = read_midi(midi_path)
    pans, expressions, bends = by_channel(pans), by_channel(expressions), by_channel(bends)
    presets = read_sf2(sf2_path)
    # The file ends at frame `end`. The driver plays a note up to a frame after the file has it, so it runs two frames
    # longer, for the notes at the end.
    end = length * FRAME_RATE
    frames = min(frames_limit, math.ceil(end) + 2)
    played, mix_rate = driver_notes(rom, song, frames)
    special = SPECIAL_TEST in rom
    camelot = CAMELOT_PULSE in rom
    camelot_sequencer = CAMELOT_CHANNEL_SEARCH in rom
    mono = MONO_VOLUME in rom

    midi = [dict(channel=n[0], key=n[1], velocity=n[2], on=n[3] * FRAME_RATE,
                 off=(n[4] if n[4] is not None else length) * FRAME_RATE, program=n[5],
                 last_tick=(n[6] if n[6] is not None else length) * FRAME_RATE, matched=False) for n in notes]
    by_key = {}
    for m in midi:
        by_key.setdefault((m['channel'], m['key']), []).append(m)

    # Notes of one key that a track starts on one frame are one MIDI note, as loud as them together. A note whose
    # envelope never rises above 0, such as one on a track's default voice before it chooses one, doesn't count.
    groups = {}
    for d in played:
        if d['start'] < min(end + 1, frames - 1) and d['sounds']:
            groups.setdefault((d['track'], d['key'], d['start']), []).append(d)

    problems = []
    worst_cents = 0.0
    checked = 0
    for (track, key, start), group in sorted(groups.items(), key=lambda item: item[0][2]):
        driver_velocity = min(127, sum(d['velocity'] for d in group))
        velocity = conversion.square_root_value(driver_velocity, 127, 1)
        channel = channel_of.get(track, track)
        candidates = [m for m in by_key.get((channel, key), []) if not m['matched'] and -1.5 <= m['on'] - start <= 1.5]
        if not candidates:
            # A note in the file's last half frame or after it can be the loop starting again, which the file leaves
            # out.
            if start < end - 0.5:
                problems.append('frame %d: the driver plays key %d on track %d, which the MIDI file doesn\'t' % (
                    start, key, track))
            continue
        # The driver's notes go in order, so each takes the earliest MIDI note left that fits, preferring notes of the
        # same velocity.
        m = min(candidates, key=lambda c: (c['velocity'] != velocity, c['on']))
        m['matched'] = True
        checked += len(group)
        where = 'frame %d, track %d key %d' % (start, track, key)
        if m['velocity'] != velocity:
            problems.append('%s: velocity %d for the driver\'s %d, not %d' % (where, m['velocity'], driver_velocity,
                                                                              velocity))

        # The driver's track levels are read at the end of the frame, after all of its ticks. A PSG note's track
        # follows the PSG's rule unless a sample note of the track plays too. Either rule's values pass for a PSG note.
        # One step either way is allowed for rounding.
        left, right = group[0]['levels']
        wanted = {track_controllers(left, right, False, mono, camelot_sequencer)}
        if group[0]['psg']:
            wanted.add(track_controllers(left, right, True, mono, camelot_sequencer))
        seen = [(value_at(pans, channel, (start + shift / 10) / FRAME_RATE, 64),
                 value_at(expressions, channel, (start + shift / 10) / FRAME_RATE, 127)) for shift in range(-10, 16)]
        if not any(abs(s[1] - w[1]) <= 1 and (w[0] is None or abs(s[0] - w[0]) <= 1) for s in seen for w in wanted):
            pan = value_at(pans, channel, start / FRAME_RATE, 64)
            expression = value_at(expressions, channel, start / FRAME_RATE, 127)
            problems.append('%s: CC10 %d and CC11 %d, where the driver\'s levels %d and %d give %s' % (
                where, pan, expression, left, right, ' or '.join('%s and %d' % w for w in sorted(wanted, key=str))))

        # The zone that the note's program and key choose has to play the driver's sound. The drum channel takes its
        # presets from bank 128. Track 9 plays there only in a song that plays on all 16 channels.
        bank = 128 if channel == 9 else 0
        zones = [z for z in presets.get((bank, m['program']), []) if z['low'] <= key <= z['high']]
        if not zones:
            problems.append('%s: program %d in bank %d has no zone for the key' % (where, m['program'], bank))
            continue
        zone = zones[0]
        name = sound_name(rom, group[0], special, camelot)
        if zone['sample']['name'] != name:
            problems.append('%s: plays %s; the driver plays %s' % (where, zone['sample']['name'], name))
            continue
        if name.startswith(('Pulse', 'Saw')):
            zone = dict(zone, sample=dict(zone['sample'], rate=synth_rate(rom, name)))

        # The pitch on each frame until the release, or until the file's last half frame. A bend lands on its tick,
        # which can be up to a frame from the frame the driver plays it on, so the pitch is read across the frame and
        # the frames either side, and the closest counts. A zone that keeps the pitch wheel from moving it doesn't
        # follow the bend. While the note is behind its track's pitch, which the MIDI channel shares with the track's
        # newer note, it isn't compared.
        bend_range = 0 if zone['no_bend'] else ranges.get(channel, 2)
        for d in group:
            for f, frequency in sorted(d['frequencies'].items()):
                if f >= min(end - 0.5, frames - 1) or f >= d['release'] or f in d['behind']:
                    continue
                want = driver_hz(d, frequency, mix_rate)
                errors = []
                for step in range(-30, 60):
                    bend = value_at(bends, channel, (f + step / 30 + 1e-6) / FRAME_RATE, 8192)
                    have = zone_hz(zone, key, (bend - 8192) / 8192 * bend_range * 100)
                    errors.append(1200 * math.log2(have / want))
                error = min(errors, key=abs)
                worst_cents = max(worst_cents, abs(error))
                limit = 2 + 2 * register_step_cents(d, frequency)
                if abs(error) > limit:
                    problems.append('%s: %+.1f cents on frame %d (%s)' % (where, error, f, name))
                    break

        # The release, unless the driver hadn't released the note when the check stopped. The MIDI note can end first
        # where the key starts again, or at the end of the file. A channel that stops by itself, at the end of a sample
        # without a loop, can stop between ticks, and then the MIDI note ends on the first tick after it.
        release = max(d['release'] for d in group)
        ends_file = m['off'] >= end - 1.5
        again = any(o is not m and abs(o['on'] - m['off']) < 0.01 for o in by_key[(channel, key)])
        stopped = all(d['stopped'] for d in group) and m['last_tick'] <= release + 1.5
        late = m['off'] > release + 1.5 and not stopped
        early = m['off'] < release - 1.5 and not again and not ends_file
        if (late or early) and release < frames - 1:
            problems.append('%s: released on frame %d, the MIDI note ends at %.1f' % (where, release, m['off']))

    for m in midi:
        if not m['matched'] and m['on'] < min(end, frames - 2):
            problems.append('%.1f: the MIDI file plays key %d on channel %d, which the driver doesn\'t' % (
                m['on'], m['key'], m['channel']))
    report(song, checked, problems, worst_cents)
    return not problems


def has_tracks(rom, song):
    """Returns true if the song table's entry for `song` has a header with tracks and a music player. The songs that
    agbsf2's --info lists with an address have both."""
    def u32(address):
        o = address - ROM_BASE
        return struct.unpack('<I', rom[o:o + 4])[0] if 0 <= o <= len(rom) - 4 else 0

    addr = driver_emu.addresses_for(rom)
    players = 0
    while players < 64 and 0x02000000 <= u32(addr.player_table + 12 * players) < 0x03008000:
        players += 1
    header = u32(addr.song_table + 8 * song) - ROM_BASE
    player = u32(addr.song_table + 8 * song + 4) & 0xFFFF
    return 0 <= header < len(rom) and rom[header] > 0 and player < players


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('folder', metavar='FOLDER', help="the folder containing agbsf2's output")
    p.add_argument('--songs', help='songs to check, such as 0-22 (default: every song with a MIDI file)')
    p.add_argument('--frames', type=int, default=12000, help='maximum frames to check per song (default: 12000)')
    p.add_argument('-v', '--verbose', action='store_true', help='list every difference, not just the first ten')
    p.add_argument('-j', '--jobs', type=int, default=1, help='songs to check at once (default: 1)')
    a = p.parse_args()
    midis = dict(conversion.midi_files(a.folder, 'mp2k'))
    if not midis:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    songs = sorted(set(parse_range(a.songs))) if a.songs else sorted(midis)
    jobs = [(a.rom, song, str(midis[song]) if song in midis else None, a.frames, a.verbose) for song in songs]
    if a.jobs > 1:
        with multiprocessing.Pool(a.jobs) as pool:
            results = pool.map(check_job, jobs)
    else:
        results = [check_job(j) for j in jobs]

    for text, _ in results:
        print(text, end='')
    results = [ok for _, ok in results if ok is not None]
    if not results:
        raise SystemExit('found none of the songs to check')
    failed = results.count(False)
    print('%d of %d songs match the driver' % (len(results) - failed, len(results)))
    raise SystemExit(1 if failed else 0)


def check_job(job):
    """Checks one song for main(), and returns its report and whether it matches, or None for a song that has neither a
    MIDI file nor tracks. The driver mustn't play any notes in a song that has tracks but no MIDI file."""
    # The last song's emulator and its hooks form a reference cycle. Collect it now to avoid keeping emulators in memory
    # for several songs.
    gc.collect()
    rom_path, song, midi, frames, verbose = job
    rom = load_rom(rom_path)
    if midi is None:
        if not has_tracks(rom, song):
            return 'song %d: no MIDI file, and the song has no tracks\n' % song, None
        played = sum(1 for d in driver_notes(rom, song, frames)[0] if d['sounds'])
        if played:
            return 'song %d: no MIDI file, but the driver plays %d notes\n' % (song, played), False
        return 'song %d: no MIDI file, and the driver plays no notes\n' % song, True
    out = io.StringIO()

    def report(song, checked, problems, worst):
        out.write('song %d: %d notes, %d difference%s, pitch within %.2f cents\n' % (
            song, checked, len(problems), '' if len(problems) == 1 else 's', worst))
        for line in problems if verbose else problems[:10]:
            out.write('    ' + line + '\n')

    ok = check_song(rom, song, Path(midi), Path(midi).with_suffix('.sf2'), frames, report)
    return out.getvalue(), ok


if __name__ == '__main__':
    main()
