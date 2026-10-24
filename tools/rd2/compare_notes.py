#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against the game's driver, note by note.

    compare_notes.py ROM AGBSF2 FOLDER [--songs 0-33] [--frames 12000]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each sequence, or NAME_rd2_NN.mid and
NAME_rd2_NN.sf2 if detection finds another of the game's drivers first. For every sequence, the driver runs under
driver_emu.py for as long as the MIDI file lasts, with hooks in its note on routine, and each note it starts with a
voice has to be in the MIDI file, and the other way round:

  - on its track's channel, with the key and velocity the conversion gives it, starting within the frame the driver
    starts it in or the frame before (a MIDI note keeps to its own tick, where the driver waits for the next frame);
  - ending within a frame of the driver's release or stop of its voice, or where the MIDI file has to end it early:
    at its end, or where the channel plays the key again;
  - with a SoundFont zone for its key that plays the driver's sample at the pitch the driver plays the note at, to
    within a cent, or a square, wave or noise voice's sound;
  - and in each frame that its voice is the newest of its track's, with the pitch bend for the voice's pitch in that
    frame, which slides, the track's bend and the LFO change.

A PSG voice's note plays at velocity 127 instead. It ends where its PSG channel falls silent, or where another note
takes the channel. In each frame until then, its CC11 has to give the channel's volume at the frame's start. The wave
channel's volume is 15 times the share of its full level that NR32 sets. psg_model.py plays the driver's writes to the
PSG's registers with the hardware's envelopes, length counters and sweep. agbsf2 works out the CC11 from the volume
and the sides that NR51 gives the channel (PsgExpression() in src/rd2/convert.cpp). The check works it out the same
way.

A sequence named in --songs that has no MIDI file mustn't play any notes. AGBSF2 is used for its --info report. The
report names the driver's tables and gives each sequence's length.

A sequence that the game plays with other tracks muted for another music number has a second pair of files for it,
named after the music number, such as NAME_30_music18.mid. The driver plays each file's version: the sequence starts
through the game's play-music routine as the file's music number, or by default as the first one that plays it. A
sequence with tracks that play only while the player rides Yoshi has a pair named NAME_NN_yoshi.mid. The driver plays
it with the player riding Yoshi, and the first version without Yoshi (see driver_emu.DriverEmulator.play()).

agbsf2 numbers the sound effects after the sequences. Each is checked in the same way. The conversion leaves out the
notes that it can't make a sound for. They are a note at velocity 0, a note that its instrument has no region for and a
note whose sample can't be read. The check leaves them out too (see converted()).

The driver plays a velocity of up to 255. A track whose sample notes play above 127 gets a scale. Its notes play at
their velocity divided by the scale. Its CC11 raises its levels by the scale (VelocityScales() in src/rd2/convert.cpp).
The scale is the loudest sample note's velocity over 127. Where CC11 can't raise the track's levels that far, the scale
is smaller. CC11 then reaches 127. The velocities of such a track have to fit one scale, from 1 up to the loudest
note's velocity over 127.
"""
import argparse
import bisect
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py, gbarom.py and traces.py, in tools/
import conversion
import driver_emu
import psg_model
from gbarom import ROM_BASE, load_rom
from traces import parse_range

FRAME_RATE = 16777216 / 280896
UNITY_INDEX = 0x30   # the pitch index that plays a sample at its own rate
TOP_INDEX = 0x78     # the highest pitch index the driver reads
DRUM_CHANNEL = 9     # the MIDI channel the conversion leaves out
PSG_VOICES = 7       # the driver's first PSG voice: square 1, then square 2, the wave and the noise
VOLUME_TYPES = (1, 2, 3, 4)  # the voice types whose notes follow their PSG channel's volume: every PSG voice's
WAVE_SHARES = (0.0, 1.0, 0.5, 0.25)  # the wave channel's share of its full level for each of NR32's volume codes
TUNE_TOLERANCE_CENTS = 1
BEND_TOLERANCE_CENTS = 2
# The MIDI file's tempos are whole microseconds a quarter note. Timed by them, its events drift from the driver's frames
# by up to half a microsecond a quarter note: at tempo 240, up to 0.024 frames in 12000 frames. read_midi() therefore
# works out each tempo's frame length in ticks from its microseconds, and times the events in frames from that. Times
# are compared with this margin, in frames.
MARGIN = 0.01


def chunks(data, pos, end):
    while pos + 8 <= end:
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        yield cid, pos + 8, size
        pos += 8 + size + (size & 1)


class SoundFont:
    """The presets as {(bank, program): [zone, ...]}: each zone a dict of its key range, generators and sample."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        parts = {}
        for cid, pos, size in chunks(data, 12, len(data)):
            if cid == b'LIST':
                for sid, spos, ssize in chunks(data, pos + 4, pos + size):
                    parts[sid] = data[spos:spos + ssize]

        def records(name, size):
            raw = parts[name]
            return [raw[i:i + size] for i in range(0, len(raw), size)]

        self.samples = []
        for s in records(b'shdr', 46)[:-1]:
            rate = struct.unpack('<I', s[36:40])[0]
            self.samples.append(dict(name=s[:20].split(b'\0')[0].decode('latin-1'), rate=rate, root=s[40]))

        def gens(bags, gen_records, index):
            first, last = struct.unpack('<H', bags[index][:2])[0], struct.unpack('<H', bags[index + 1][:2])[0]
            out = {}
            for g in gen_records[first:last]:
                op, amount = struct.unpack('<HH', g)
                out[op] = amount
            return out

        phdr, pbag, pgen = records(b'phdr', 38), records(b'pbag', 4), records(b'pgen', 4)
        inst, ibag, igen = records(b'inst', 22), records(b'ibag', 4), records(b'igen', 4)
        self.presets = {}
        for p in range(len(phdr) - 1):
            program, bank, bag = struct.unpack('<HHH', phdr[p][20:26])
            i = gens(pbag, pgen, bag)[41]
            first, last = struct.unpack('<H', inst[i][20:22])[0], struct.unpack('<H', inst[i + 1][20:22])[0]
            zones = []
            for z in range(first, last):
                g = gens(ibag, igen, z)
                if 53 in g:
                    zones.append(dict(g=g, low=g.get(43, 0x7F00) & 0xFF, high=g.get(43, 0x7F00) >> 8,
                                      sample=self.samples[g[53]]))
            self.presets[(bank, program)] = zones


def signed(v):
    return v - 0x10000 if v >= 0x8000 else v


def read_midi(path):
    """Returns the notes as dicts of channel, key, velocity, on and off seconds, bank and program, each channel's pitch
    bends and CC11 as {channel: (times, values)}, the bend ranges and the length in seconds."""
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
            index += 1
            delta = 0
            while True:
                b = body[i]
                i += 1
                delta = delta << 7 | (b & 0x7F)
                if b < 0x80:
                    break
            tick += delta
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
                    events.append((tick, track, index, 'tempo', 0, int.from_bytes(body[i:i + 3], 'big'), 0))
                events.append((tick, track, index, 'meta', 0, 0, 0))
                i += length
                continue
            kind, ch = status & 0xF0, status & 0x0F
            n = 1 if kind in (0xC0, 0xD0) else 2
            args = body[i:i + n]
            i += n
            if kind == 0x90 and args[1]:
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

    # The order of events on a tick: the writer's, which puts note offs first and note ons last.
    order = {'tempo': 0, 'meta': 0, 'off': 1, 'program': 2, 'cc': 3, 'bend': 4, 'on': 5}
    events.sort(key=lambda e: (e[0], order[e[3]], e[1], e[2]))
    tempos = [(0, 500000)] + [(e[0], e[5]) for e in events if e[3] == 'tempo']

    # A frame at the driver's tempo T lasts T ticks. The MIDI file's tempo is the frame's length in microseconds times
    # division / T, rounded to a whole microsecond. Rounding the ticks a frame back to a whole number undoes that.
    def frame_ticks(tempo):
        return max(1, round(division * 1e6 / (tempo * FRAME_RATE)))

    def seconds(tick):
        frames, at, tempo = 0.0, 0, 500000
        for t, value in tempos:
            if t > tick:
                break
            frames += (t - at) / frame_ticks(tempo)
            at, tempo = t, value
        return (frames + (tick - at) / frame_ticks(tempo)) / FRAME_RATE

    notes, open_notes, bends, expressions, ranges = [], {}, {}, {}, {}
    bank, program, rpn = {}, {}, {}
    for tick, _, _, kind, ch, a, b in events:
        t = seconds(tick)
        if kind == 'on':
            note = dict(channel=ch, key=a, velocity=b, on=t, off=None, bank=bank.get(ch, 0),
                        program=program.get(ch, 0), matched=False)
            open_notes[(ch, a)] = note
            notes.append(note)
        elif kind == 'off' and (ch, a) in open_notes:
            open_notes.pop((ch, a))['off'] = t
        elif kind == 'program':
            program[ch] = a
        elif kind == 'cc' and a == 0:
            # agbsf2 gives the SoundFont's bank number in CC0 alone and sets CC32 to 0. FluidSynth reads the bank from
            # CC0 by default.
            bank[ch] = b
        elif kind == 'bend':
            times, values = bends.setdefault(ch, ([], []))
            times.append(t)
            values.append(a)
        elif kind == 'cc' and a == 11:
            times, values = expressions.setdefault(ch, ([], []))
            times.append(t)
            values.append(b)
        elif kind == 'cc' and a in (100, 101):
            rpn[(ch, a)] = b
        elif kind == 'cc' and a == 6 and rpn.get((ch, 101)) == 0 and rpn.get((ch, 100)) == 0:
            ranges[ch] = b
    length = seconds(max(e[0] for e in events)) if events else 0
    for note in notes:
        if note['off'] is None:
            note['off'] = length
    return notes, bends, expressions, ranges, length


def bend_at(bends, ch, t):
    times, values = bends.get(ch, ((), ()))
    i = bisect.bisect_right(times, t + 1e-9)
    return values[i - 1] if i else 8192


def expression_at(expressions, ch, t):
    times, values = expressions.get(ch, ((), ()))
    i = bisect.bisect_right(times, t + 1e-9)
    return values[i - 1] if i else 127


def psg_frames(writes, frames):
    """Returns ([square 1, square 2, wave, noise], nr51) for each frame: the volume of each PSG channel at the frame's
    start (0 to 15, and 0 while the channel is off) and NR51 after the frame's writes. The wave's volume is 15 times the
    share of its full level that NR32 sets. Each of `writes` is one of the driver's writes to the PSG's registers:
    (frame, address, size, value), with frame -1 before the first frame."""
    apu = psg_model.APU(hardware=True)
    by_frame = {}
    for f, address, size, value in writes:
        by_frame.setdefault(f, []).append((address, size, value))
    for w in by_frame.get(-1, ()):
        apu.write(*w)
    out = []
    for f in range(frames):
        for w in by_frame.get(f, ()):
            apu.write(*w)
        volumes = [ch['env']['vol'] if ch['on'] else 0 for ch in (apu.square[0], apu.square[1])]
        w = apu.wave
        share = 0.75 if w['force75'] else WAVE_SHARES[w['volcode']]
        volumes.append(15 * share if w['on'] and w['dac'] else 0)
        out.append((volumes + [apu.noise['env']['vol'] if apu.noise['on'] else 0], apu.nr51))
        apu.advance(psg_model.FRAME_CYCLES)
    return out


def psg_expression(volume, nr51, c, share):
    """Returns the CC11 that agbsf2 gives a note at full velocity on PSG channel `c` at `volume`, on the sides that
    `nr51` gives the channel. Volume 15 plays at `share` of full scale. seqlib's LevelsToControllers() turns the two
    sides' levels into the CC11 that plays them under the SoundFont's default modulator."""
    level = volume / share
    left = level if (nr51 >> (c + 4)) & 1 else 0.0
    right = level if (nr51 >> c) & 1 else 0.0
    amp = math.hypot(left / 128, right / 128)
    if amp <= 0:
        return 0
    loudest_centred = math.sqrt(2.0) * 127 / 128
    return min(max(math.floor(127 * math.sqrt(amp / loudest_centred) + 0.5), 0), 127)


def driver_info(tool, rom_path):
    """Returns the pitch and frequency tables' addresses, the table of banks, the frames that each listed sequence
    lasts and the PSG's share of full scale. All come from agbsf2's --info report. The report gives each sequence's
    length in minutes and seconds, and the driver's revision. agbsf2 gives the PSG a share for each revision
    (PsgShare() in src/rd2/soundfont.h)."""
    text = subprocess.run([tool, '--driver', 'rd2', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    revision = re.search(r'^\s+(.+) revision of the driver', text, re.M).group(1)
    share = 15 / 47 if revision == 'A Link to the Past' else 15 / 31
    pitch = int(re.search(r'pitch table: 0x([0-9A-F]+)', text).group(1), 16)
    frequency = int(re.search(r'frequency table: 0x([0-9A-F]+)', text).group(1), 16)
    banks = re.search(r'banks: 0x([0-9A-F]+) \((\d+)\)', text)
    lengths = {int(m.group(1)): math.ceil((int(m.group(2)) * 60 + float(m.group(3))) * FRAME_RATE)
               for m in re.finditer(r'^\s+(\d+)\s+0x[0-9A-F]{8}\s+\d+\s+(\d+):(\d+\.\d+)', text, re.M)}
    return (pitch, frequency), (int(banks.group(1), 16), int(banks.group(2))), lengths, share


class Rom:
    def __init__(self, data):
        self.data = data

    def u8(self, a):
        o = a - ROM_BASE
        return self.data[o] if 0 <= o < len(self.data) else 0

    def u16(self, a):
        return self.u8(a) | self.u8(a + 1) << 8

    def u32(self, a):
        return self.u16(a) | self.u16(a + 2) << 16

    def contains(self, a, size):
        return 0 <= a - ROM_BASE and a - ROM_BASE + size <= len(self.data)


def converted(rom, bank_starts, note):
    """Returns true if the conversion keeps one of the driver's notes. A note at velocity 0 is left out. So is a note
    whose region is in `bank_starts`. Its instrument has no region for it. The driver plays it from its bank's table of
    offsets. A sample voice's note is left out if agbsf2 can't make a SoundFont sample from the sample's header
    (SampleReadable() in src/rd2/soundfont.cpp)."""
    if note['velocity'] == 0 or note['region'] in bank_starts:
        return False
    if note.get('type') != 0:
        return True
    sample = note['sample']
    length, rate, loop_start, loop_end = (rom.u32(sample + 4 * i) for i in range(4))
    points = loop_end or length
    return (0 < points <= 1 << 24 and rate != 0 and rom.contains(sample + 16, points) and
            (not loop_end or loop_start < loop_end))


def midi_channel(track):
    """Returns the MIDI channel of one of the player's tracks. The conversion leaves out the drum channel."""
    return track if track < DRUM_CHANNEL else track + 1


def expected(rom, tables, note):
    """Returns the MIDI key a note plays on and the pitch of its note without its slide, bend or LFO."""
    kind = note['type']
    # A voice in legato keeps the region of its first note, but plays each note with its own region's tuning.
    region = note['region']
    in_rom = region >= ROM_BASE
    tuning = rom.u8(region + 7) if in_rom else UNITY_INDEX
    raw = (note['pitch_note'] + UNITY_INDEX - tuning) & 0xFFFF
    raw = raw - 0x10000 if raw >= 0x8000 else raw
    index = min(max(raw, 0), TOP_INDEX)
    pitch_table, frequency_table = tables
    if kind == 0:
        return note['key'], rom.u32(pitch_table + 4 * index)
    if kind in (1, 2):
        return index + 12, rom.u16(frequency_table + 2 * index)
    if kind == 3:
        return index, rom.u16(frequency_table + 2 * index)
    return note['key'], 0


def semitones(kind, pitch, note_pitch):
    if kind == 0:
        return 12 * math.log2(pitch / note_pitch) if pitch and note_pitch else 0.0
    if kind == 4 or pitch >= 2048 or note_pitch >= 2048:
        return 0.0
    return 12 * math.log2((2048 - note_pitch) / (2048 - pitch))


def zone_cents(zone, key, rate):
    """Returns how far a SoundFont zone's pitch for `key` is from playing at `rate`, in cents."""
    g = zone['g']
    root = g.get(58, zone['sample']['root'])
    coarse = signed(g.get(51, 0))
    fine = signed(g.get(52, 0))
    played = zone['sample']['rate'] * 2 ** ((key - root + coarse + fine / 100) / 12)
    return 1200 * math.log2(played / rate)


def check_song(rom, raw_rom, tables, bank_starts, share, song, midi_path, sf2_path, frames_limit, music=None,
               yoshi=False):
    """Returns the notes checked and the problems found. The notes that converted() leaves out aren't checked.
    `share` is the PSG's share of full scale at volume 15. The sequence starts as music number `music`, with the player
    riding Yoshi with `yoshi` (see driver_emu.DriverEmulator.play())."""
    notes, bends, expressions, ranges, length = read_midi(midi_path)

    sf = SoundFont(sf2_path)
    frames = min(frames_limit, int(length * FRAME_RATE) + 2)
    writes = []
    played = driver_emu.notes(raw_rom, song, frames, writes, music, yoshi)
    psg = psg_frames(writes, len(played))
    problems = []
    end_frame = length * FRAME_RATE

    # The MIDI notes of each key on each channel, in order.
    by_channel = {}
    for n in notes:
        by_channel.setdefault((n['channel'], n['key']), []).append(n)

    # Each track's scale for its sample notes' velocities, and the tracks whose CC11 reaches 127 (see the docstring).
    loudest = {}
    for started, _, _ in played:
        for note in started:
            if note['frame'] < end_frame and note['type'] not in VOLUME_TYPES and converted(rom, bank_starts, note):
                loudest[note['track']] = max(loudest.get(note['track'], 0), note['velocity'])
    scales = {track: max(1.0, v / 127.0) for track, v in loudest.items()}
    capped = {track for track, scale in scales.items()
              if scale > 1 and 127 in expressions.get(midi_channel(track), ((), ()))[1]}
    capped_velocities = {}  # track -> (frame, key, the driver's velocity, the MIDI note's) for each of its sample notes

    def note_end(f, i):
        """Returns the frame where the voice of note `i` of frame `f` stops playing it: a release, a stop, or another
        note on the voice. A run that stops before the note ends doesn't say where it ends, and gives None."""
        started, voices, _ = played[f]
        v = started[i]['voice']

        # A note whose voice another note takes in the same frame ends at once, and isn't heard.
        if voices[v] is None or any(later['voice'] == v for later in started[i + 1:]):
            return f
        for g in range(f + 1, len(played)):
            later = played[g]
            if any(n['voice'] == v for n in later[0]) or later[1][v] is None or later[1][v][0] != 1:
                return g
        return len(played) if len(played) >= end_frame else None

    def sound_end(f, i):
        """Returns the frame where the PSG channel of note `i` of frame `f` falls silent, for a PSG voice's note: the one
        after the last frame that starts with the channel sounding, before another note takes the channel. A note whose
        voice another note takes in the same frame ends at once. A run that stops before the channel falls silent gives
        None."""
        started = played[f][0]
        v = started[i]['voice']
        if any(later['voice'] == v for later in started[i + 1:]):
            return f
        until = next((g for g in range(f + 1, len(played)) if any(n['voice'] == v for n in played[g][0])),
                     len(played))
        silent = f
        for g in range(f, until):
            if psg[g][0][v - PSG_VOICES] > 0:
                silent = g + 1
        return None if silent >= len(played) and len(played) < end_frame else silent

    def midi_note_end(f, i):
        """Returns the frame where the MIDI note of note `i` of frame `f` ends, or None if the run doesn't say."""
        return sound_end(f, i) if played[f][0][i]['type'] in VOLUME_TYPES else note_end(f, i)

    # Each of the driver's notes, from the frame it starts to the frame its voice releases it, stops or plays another.
    checked = 0
    merged = {}  # (frame, track, key) -> the MIDI note of the first note that a track starts on the key in the frame
    for f, (started, voices, _) in enumerate(played):
        for i, note in enumerate(started):
            if note['frame'] >= end_frame - MARGIN or not converted(rom, bank_starts, note):
                continue
            v = note['voice']
            key, note_pitch = expected(rom, tables, note)
            channel = midi_channel(note['track'])
            follows = note['type'] in VOLUME_TYPES
            scale = scales.get(note['track'], 1.0)
            velocity = 127 if follows else conversion.square_root_value(note['velocity'], 127 * scale, 1)

            # The MIDI note: on the track's channel, with the key, starting in the frame or the one before. Notes that a
            # track starts on the same key and tick are one MIDI note, the first's, which lasts as long as the longest
            # of them, so where the MIDI file has fewer notes than the driver starts on the key in the frame, the
            # first takes the ends of the others, and they need only their bends checked against it.
            group = [j for j, n in enumerate(started) if n['track'] == note['track'] and
                     converted(rom, bank_starts, n) and expected(rom, tables, n)[0] == key]
            candidates = [n for n in by_channel.get((channel, key), ())
                          if not n['matched'] and f - 1 - MARGIN < n['on'] * FRAME_RATE <= f + MARGIN]
            end = note_end(f, i)
            midi_end = midi_note_end(f, i)  # where the MIDI note has to end, if this note decides it
            check_end = True
            if candidates:
                m = candidates[0]
                m['matched'] = True
                if i == group[0] and len(candidates) < len(group):
                    ends = [midi_note_end(f, j) for j in group]
                    midi_end = None if None in ends else max(ends)
                    merged[(f, note['track'], key)] = m
                if not follows and note['track'] in capped:
                    capped_velocities.setdefault(note['track'], []).append((f, key, note['velocity'], m['velocity']))
                elif m['velocity'] != velocity:
                    problems.append('frame %d: track %d key %d has velocity %d, not %d' %
                                    (f, note['track'], key, m['velocity'], velocity))
            elif (f, note['track'], key) in merged:
                m = merged[(f, note['track'], key)]
                check_end = False
            else:
                problems.append('frame %d: track %d plays key %d, which the MIDI file has no note for' %
                                (f, note['track'], key))
                continue
            checked += 1

            # Its end: within a frame of the voice's, or earlier where the MIDI file has to end it.
            off = m['off'] * FRAME_RATE
            cut = off >= end_frame - MARGIN or any(
                o is not m and o['on'] <= m['off'] + 1e-9 and o['on'] >= m['on'] - 1e-9 and o['off'] > m['off']
                for o in by_channel.get((channel, key), ()))
            near = midi_end is not None and midi_end - 1 - MARGIN <= off <= midi_end + 1 + MARGIN
            if check_end and midi_end is not None and not near and not (cut and off < midi_end + 1 + MARGIN):
                problems.append('frame %d: track %d key %d ends at frame %.2f, where the driver ends it at %d' %
                                (f, note['track'], key, off, midi_end))

            # A PSG voice's CC11 in each frame until its channel falls silent. Each frame's value holds
            # from the start of the frame on the note's time line until the start of the next frame or the note off.
            # The next note on the channel can start within the frame.
            if follows and check_end and midi_end is not None:
                c = v - PSG_VOICES
                stop = min(m['off'] * FRAME_RATE, end_frame)
                for g in range(f, midi_end):
                    step = m['on'] * FRAME_RATE + (g - f)
                    if step >= stop - MARGIN:
                        break
                    at = min(step + 0.1, (step + stop) / 2)
                    volume, nr51 = psg[g]
                    want = psg_expression(volume[c], nr51, c, share)
                    have = expression_at(expressions, channel, at / FRAME_RATE)
                    if have != want:
                        problems.append('frame %d: track %d key %d has CC11 %d, where volume %d gives %d' %
                                        (g, note['track'], key, have, volume[c], want))
                        break

            # Its zone in the SoundFont, and the zone's pitch.
            zones = [z for z in sf.presets.get((m['bank'], m['program']), ()) if z['low'] <= key <= z['high']]
            if not zones:
                problems.append('frame %d: track %d key %d has no zone in its preset' % (f, note['track'], key))
                continue
            zone = zones[0]
            kind = note['type']
            if kind == 0:
                name = 'Sample %08X' % note['sample']
                sample_rate = rom.u32(note['sample'] + 4)
                rate = sample_rate * note_pitch / 0x8000
            elif kind in (1, 2):
                name = 'Square'
            elif kind == 3:
                name = 'Wave'
            else:
                name = 'Noise'
            if not zone['sample']['name'].startswith(name):
                problems.append('frame %d: track %d key %d plays %s, not %s' %
                                (f, note['track'], key, zone['sample']['name'], name))
            elif kind == 0:
                cents = zone_cents(zone, key, rate)
                if abs(cents) > TUNE_TOLERANCE_CENTS:
                    problems.append('frame %d: track %d key %d plays %.1f cents off' % (f, note['track'], key, cents))

            # Each frame's bend while its voice is the newest of its track's.
            bend_range = ranges.get(channel, 2)
            for g in range(f, min(len(played) if end is None else end, len(played), int(end_frame))):
                if played[g][2][note['track']] != v or played[g][1][v] is None:
                    continue
                want = semitones(kind, played[g][1][v][1], note_pitch)
                have = (bend_at(bends, channel, max((g + MARGIN) / FRAME_RATE, m['on'])) - 8192) / 8192 * bend_range
                if abs(want - have) * 100 > BEND_TOLERANCE_CENTS + (bend_range / 8192 * 100):
                    problems.append('frame %d: track %d key %d bends %.1f cents, where the driver bends %.1f' %
                                    (g, note['track'], key, have * 100, want * 100))
                    break

    # The scales that give each note of a capped track its MIDI velocity: 127 √(v / 127 k) rounds to it. A velocity of 1
    # can come from any smaller value. One of 127 can come from any larger.
    for track, pairs in capped_velocities.items():
        low, high = 1.0, scales[track]
        for _, _, v, midi in pairs:
            if midi > 1:
                high = min(high, 127 * v / (midi - 0.5) ** 2)
            if midi < 127:
                low = max(low, 127 * v / (midi + 0.5) ** 2)
        if low > high:
            problems.append('track %d: no one scale from 1 to %.3f gives its sample notes their velocities' %
                            (track, scales[track]))

    for n in notes:
        if not n['matched'] and n['on'] * FRAME_RATE < frames - 1:
            problems.append('%.2f s: channel %d key %d is a note the driver doesn\'t play' % (n['on'], n['channel'],
                                                                                         n['key']))
    return checked, problems


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program that made the conversion')
    p.add_argument('folder', metavar='FOLDER', help='the conversion: NAME_NN.mid and NAME_NN.sf2, or NAME_rd2_NN')
    p.add_argument('--songs', help='sequences to check, such as 0-33 (default: every one in FOLDER)')
    p.add_argument('--frames', type=int, default=12000, help='frames to check in each sequence (default: 12000)')
    a = p.parse_args()

    raw_rom = load_rom(a.rom)
    rom = Rom(raw_rom)
    tables, (bank_table, bank_count), lengths, share = driver_info(a.agbsf2, a.rom)
    bank_starts = {bank_table + rom.u32(bank_table + 4 * i) for i in range(bank_count)}
    files = conversion.midi_files(a.folder, 'rd2', versions=True)
    midis = {song: path for song, version, path in files if not version}
    if not files:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    songs = sorted(set(parse_range(a.songs))) if a.songs else sorted({song for song, _, _ in files})
    count = failed = total = 0
    for song in songs:
        midi = midis.get(song)
        if midi is None and song in lengths:
            # A sequence without a MIDI file mustn't play any notes.
            count += 1
            played = sum(1 for started, _, _ in driver_emu.notes(raw_rom, song, min(a.frames, lengths[song] + 2))
                         for note in started if converted(rom, bank_starts, note))
            if played:
                failed += 1
                print('sequence %d: no MIDI file, but the driver plays %d notes' % (song, played))
            else:
                print('sequence %d: no MIDI file, and the driver plays no notes' % song)
        elif midi is None:
            print('sequence %d: no MIDI file, and --info lists no such sequence' % song)

        # The sequence as the game starts it, then each of its other versions. A version's files are named after the
        # music number that plays it, or after the player riding Yoshi.
        checks = [('sequence %d' % song, midi, None, False)] if midi else []
        for version, path in ((v, p) for s, v, p in files if s == song and v):
            music = re.fullmatch(r'music(\d+)', version)
            if music:
                checks.append(('sequence %d %s' % (song, version), path, int(music.group(1)), False))
            elif version == 'yoshi':
                checks.append(('sequence %d %s' % (song, version), path, None, True))
            else:
                count += 1
                failed += 1
                print('sequence %d %s: no music number plays this version' % (song, version))
        for name, path, music, yoshi in checks:
            count += 1
            checked, problems = check_song(rom, raw_rom, tables, bank_starts, share, song, path,
                                           path.with_suffix('.sf2'), a.frames, music, yoshi)
            total += checked
            if problems:
                failed += 1
                print('%s: %d problems in %d notes' % (name, len(problems), checked))
                for problem in problems[:10]:
                    print('    ' + problem)
            else:
                print('%s: all %d notes match' % (name, checked))
    if not count:
        raise SystemExit('found none of the sequences to check')
    print('%d of %d sequences match the driver, %d notes checked' % (count - failed, count, total))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
