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

A sequence named in --songs that has no MIDI file mustn't play any notes. AGBSF2 is used for its --info report. The
report names the driver's tables and gives each sequence's length.
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
from gbarom import ROM_BASE, load_rom
from traces import parse_range

FRAME_RATE = 16777216 / 280896
UNITY_INDEX = 0x30   # the pitch index that plays a sample at its own rate
TOP_INDEX = 0x78     # the highest pitch index the driver reads
DRUM_CHANNEL = 9     # the MIDI channel the conversion leaves out
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
    bends as {channel: (times, values)}, the bend ranges and the length in seconds."""
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

    notes, open_notes, bends, ranges = [], {}, {}, {}
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
        elif kind == 'cc' and a in (100, 101):
            rpn[(ch, a)] = b
        elif kind == 'cc' and a == 6 and rpn.get((ch, 101)) == 0 and rpn.get((ch, 100)) == 0:
            ranges[ch] = b
    length = seconds(max(e[0] for e in events)) if events else 0
    for note in notes:
        if note['off'] is None:
            note['off'] = length
    return notes, bends, ranges, length


def bend_at(bends, ch, t):
    times, values = bends.get(ch, ((), ()))
    i = bisect.bisect_right(times, t + 1e-9)
    return values[i - 1] if i else 8192


def driver_info(tool, rom_path):
    """Returns the pitch and frequency tables' addresses, the table of banks and the frames that each listed sequence
    lasts. All come from agbsf2's --info report. The report gives each sequence's length in minutes and seconds."""
    text = subprocess.run([tool, '--driver', 'rd2', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    pitch = int(re.search(r'pitch table: 0x([0-9A-F]+)', text).group(1), 16)
    frequency = int(re.search(r'frequency table: 0x([0-9A-F]+)', text).group(1), 16)
    banks = re.search(r'banks: 0x([0-9A-F]+) \((\d+)\)', text)
    lengths = {int(m.group(1)): math.ceil((int(m.group(2)) * 60 + float(m.group(3))) * FRAME_RATE)
               for m in re.finditer(r'^\s+(\d+)\s+0x[0-9A-F]{8}\s+\d+\s+(\d+):(\d+\.\d+)', text, re.M)}
    return (pitch, frequency), (int(banks.group(1), 16), int(banks.group(2))), lengths


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


def check_song(rom, raw_rom, tables, bank_starts, song, midi_path, sf2_path, frames_limit):
    """Returns the notes checked and the problems found. A note whose region is in `bank_starts` is one that its
    instrument has no region for, which the driver plays from its bank's table of offsets and the conversion leaves
    out."""
    notes, bends, ranges, length = read_midi(midi_path)

    sf = SoundFont(sf2_path)
    frames = min(frames_limit, int(length * FRAME_RATE) + 2)
    played = driver_emu.notes(raw_rom, song, frames)
    problems = []
    end_frame = length * FRAME_RATE

    # The MIDI notes of each key on each channel, in order.
    by_channel = {}
    for n in notes:
        by_channel.setdefault((n['channel'], n['key']), []).append(n)

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

    # Each of the driver's notes, from the frame it starts to the frame its voice releases it, stops or plays another.
    checked = 0
    merged = {}  # (frame, track, key) -> the MIDI note of the first note that a track starts on the key in the frame
    for f, (started, voices, _) in enumerate(played):
        for i, note in enumerate(started):
            if note['frame'] >= end_frame - MARGIN or note['velocity'] == 0 or note['region'] in bank_starts:
                continue
            v = note['voice']
            key, note_pitch = expected(rom, tables, note)
            channel = note['track'] if note['track'] < DRUM_CHANNEL else note['track'] + 1
            velocity = conversion.square_root_value(note['velocity'], 127, 1)

            # The MIDI note: on the track's channel, with the key, starting in the frame or the one before. Notes that a
            # track starts on the same key and tick are one MIDI note, the first's, which lasts as long as the longest
            # of them, so where the MIDI file has fewer notes than the driver starts on the key in the frame, the
            # first takes the ends of the others, and they need only their bends checked against it.
            group = [j for j, n in enumerate(started) if n['track'] == note['track'] and n['velocity'] and
                     n['region'] not in bank_starts and expected(rom, tables, n)[0] == key]
            candidates = [n for n in by_channel.get((channel, key), ())
                          if not n['matched'] and f - 1 - MARGIN < n['on'] * FRAME_RATE <= f + MARGIN]
            end = note_end(f, i)
            midi_end = end  # where the MIDI note has to end, if this note decides it
            check_end = True
            if candidates:
                m = candidates[0]
                m['matched'] = True
                if i == group[0] and len(candidates) < len(group):
                    ends = [note_end(f, j) for j in group]
                    midi_end = None if None in ends else max(ends)
                    merged[(f, note['track'], key)] = m
                if m['velocity'] != velocity:
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
    tables, (bank_table, bank_count), lengths = driver_info(a.agbsf2, a.rom)
    bank_starts = {bank_table + rom.u32(bank_table + 4 * i) for i in range(bank_count)}
    midis = dict(conversion.midi_files(a.folder, 'rd2'))
    if not midis:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    songs = sorted(set(parse_range(a.songs))) if a.songs else sorted(midis)
    count = failed = total = 0
    for song in songs:
        midi = midis.get(song)
        if midi is None:
            # A sequence without a MIDI file mustn't play any notes.
            if song not in lengths:
                print('sequence %d: no MIDI file, and --info lists no such sequence' % song)
                continue
            count += 1
            played = sum(1 for started, _, _ in driver_emu.notes(raw_rom, song, min(a.frames, lengths[song] + 2))
                         for note in started if note['velocity'] and note['region'] not in bank_starts)
            if played:
                failed += 1
                print('sequence %d: no MIDI file, but the driver plays %d notes' % (song, played))
            else:
                print('sequence %d: no MIDI file, and the driver plays no notes' % song)
            continue
        count += 1
        checked, problems = check_song(rom, raw_rom, tables, bank_starts, song, midi, midi.with_suffix('.sf2'),
                                       a.frames)
        total += checked
        if problems:
            failed += 1
            print('sequence %d: %d problems in %d notes' % (song, len(problems), checked))
            for problem in problems[:10]:
                print('    ' + problem)
        else:
            print('sequence %d: all %d notes match' % (song, checked))
    if not count:
        raise SystemExit('found none of the sequences to check')
    print('%d of %d sequences match the driver, %d notes checked' % (count - failed, count, total))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
