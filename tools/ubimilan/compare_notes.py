#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against Ubisoft Milan's driver, note by note.

    compare_notes.py ROM AGBSF2 FOLDER [--songs 0-13] [--frames 12000] [-v]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each piece of music, or
NAME_ubimilan_NN.mid and NAME_ubimilan_NN.sf2 if another driver was detected first. A piece that the game starts with
channel 9's kit switched off also has NAME_NN_kit.mid and NAME_NN_kit.sf2, the piece with the kit. The driver plays
the first files with the kit off and the second with it on. Events in these MIDI files follow
the song's beat and may be up to a frame and a half from the driver's timing. The script converts ROM again with
--frame-timing to keep events on the driver's frames. Both conversions must have identical SoundFonts and the same
notes, keys and programs, in the same order on each channel and within 1.65 frames of each other: the frame and a
half, and the rounding of a tick. It then runs the driver under driver_emu.py for every frame of the --frame-timing
MIDI file. Each sample that the driver starts on a voice must be a note on MIDI channel 11:

  - starting on the frame on which the driver starts the sample;
  - at the velocity that the voice's volume gives: 127 times the square root of the volume over 64, a sample's full
    scale;
  - with a zone for its key in the preset of its program in bank 9. The zone's sample holds the driver's points at the
    rate the mixer plays them and loops where the driver's sample loops;
  - ending on the frame on which the driver stops the voice, or on the frame after the one in which the voice reaches
    the end of a sample without a loop, or with the file.

A sample that the driver starts at volume 0 plays nothing, and the MIDI file leaves it out. A key of channel 9 outside
its kit makes the driver play a sample from a mirror of the ROM, which the MIDI file leaves out too; those are counted.

psg_model.py runs the driver's writes to the PSG registers with the hardware envelopes, length counters and sweep. In
each frame, the squares, the wave and the noise have to sound in the MIDI file on channels 1 to 4 where they sound in
the model: while a channel is on, at a volume above 0 or with an envelope that rises from 0, and for the wave while
NR32 doesn't mute it. The CC11 of a sounding frame has to set the channel level: the volume of a square, the 15 wave
steps at the NR32 share, or the noise at full volume. The noise drum contains the noise volume. agbsf2 turns a level
into CC11 with seqlib's LevelsToControllers(). The check does the same. The pitch of the squares and the wave, from
the note key, the zone tuning and the bend, has to be within 1 cent of the frequency setting that the modulation and
the glides write. The noise drum has to contain the channel volume in each frame, within a step. The drum starts its
envelope halfway between two steps of the frame sequencer, and the model counts them from the first frame.
"""
import argparse
import math
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py, gbarom.py and traces.py, in tools/
import conversion
from conversion import midi_file
import driver_emu
import psg_model
import traces
from gbarom import ROM_BASE, load_rom

CPU_CLOCK = 16777216
FRAME_RATE = CPU_CLOCK / 280896
MAX_SHIFT = 1.65  # frames that an event on the beat can be from the driver's frame, with a margin for the MIDI's ticks
MIRROR = 0x0A000000
KIT_MIDI_CHANNEL = 10  # the MIDI channel of the driver's channel 9, MIDI's channel 11, counted from 0
KIT_BANK = 9
POINTS_COMPARED = 256  # points of each sample compared with the SoundFont's
NOISE_MIDI_CHANNEL = 3
PSG_LEVEL = [1.0, 1.0, 30.0, 15.0]  # each channel's level at volume 1, or the wave's and the noise's at full
NOISE_RATE = 65536  # the noise drums' rate
CENTS = 1.0  # the furthest that a PSG note's pitch can be from the driver's


def read_midi(path):
    """Returns the MIDI file's notes as dicts with the channel, key, velocity, bank, program and the frames on which it
    starts and ends, its length in frames, and for each channel the frames and values of its CC11 and bends, and its
    bend range."""
    data = Path(path).read_bytes()
    division = midi_file.division_of(data)
    tracks = midi_file.tracks_of(data)[1]
    events = [(tick, status, args) for _, track_events in tracks for tick, status, args in track_events
              if status != 0xFF]
    tempos = midi_file.tempo_changes(tracks)
    seconds = midi_file.tick_seconds(division, tempos)

    def frames(tick):
        return seconds(tick) * FRAME_RATE

    notes, open_notes, bank, program = [], {}, {}, {}
    controls = {}  # channel -> {'cc11': [(frame, value)], 'bend': [(frame, value)], 'range': semitones}
    rpn = {}
    last = 0
    for tick, status, args in sorted(events, key=lambda e: e[0]):
        last = max(last, tick)
        ch, kind = status & 0xF, status >> 4
        c = controls.setdefault(ch, {'cc11': [], 'bend': [], 'range': 2})
        if kind == 0xB and args[0] in (100, 101):
            rpn[(ch, args[0])] = args[1]
        elif kind == 0xB and args[0] == 6 and rpn.get((ch, 101)) == 0 and rpn.get((ch, 100)) == 0:
            c['range'] = args[1]
        elif kind == 0xB and args[0] == 11:
            c['cc11'].append((frames(tick), args[1]))
        elif kind == 0xE:
            c['bend'].append((frames(tick), args[0] | (args[1] << 7)))
        if kind == 0xB and args[0] == 0:
            bank[ch] = args[1]
        elif kind == 0xC:
            program[ch] = args[0]
        elif kind == 0x9 and args[1]:
            note = dict(channel=ch, key=args[0], velocity=args[1], bank=bank.get(ch, 0),
                        program=program.get(ch, 0), on=frames(tick), off=None)
            open_notes[(ch, args[0])] = note
            notes.append(note)
        elif kind in (0x8, 0x9) and (ch, args[0]) in open_notes:
            open_notes.pop((ch, args[0]))['off'] = frames(tick)
    end = frames(max([last] + [t for t, _ in tempos]))
    for note in notes:
        if note['off'] is None:
            note['off'] = end
    return notes, end, controls


def value_at(changes, frame, default):
    """Returns the value of a controller from `changes`, [(frame, value)] in order, in the middle of `frame`."""
    value = default
    for f, v in changes:
        if f > frame + 0.5:
            break
        value = v
    return value


def read_sf2(path):
    """Returns {(bank, program): {key: sample}}, each sample a dict of its points, rate, and loop if it loops."""
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
    smpl = chunks['smpl']

    def records(name, size):
        raw = chunks[name]
        return [raw[i:i + size] for i in range(0, len(raw), size)]

    phdr, pbag, pgen = records('phdr', 38), records('pbag', 4), records('pgen', 4)
    inst, ibag, igen, shdr = records('inst', 22), records('ibag', 4), records('igen', 4), records('shdr', 46)

    def gens(bags, generators, index):
        first = struct.unpack('<H', bags[index][:2])[0]
        last = struct.unpack('<H', bags[index + 1][:2])[0]
        return {struct.unpack('<H', g[:2])[0]: struct.unpack('<H', g[2:])[0] for g in generators[first:last]}

    def signed(value):
        return value - 0x10000 if value & 0x8000 else value

    presets = {}
    for p in range(len(phdr) - 1):
        program, bank, bag = struct.unpack('<HHH', phdr[p][20:26])
        instrument = gens(pbag, pgen, bag)[41]
        first = struct.unpack('<H', inst[instrument][20:22])[0]
        last = struct.unpack('<H', inst[instrument + 1][20:22])[0]
        keys = {}
        for z in range(first, last):
            g = gens(ibag, igen, z)
            if 53 not in g:
                continue
            start, end, loop_start, loop_end, rate = struct.unpack('<IIIII', shdr[g[53]][20:40])
            sample = dict(points=struct.unpack('<%dh' % (end - start), smpl[2 * start:2 * end]), rate=rate,
                          loop=(loop_start - start, loop_end - start) if g.get(54, 0) & 1 else None,
                          root=g.get(58, shdr[g[53]][40]), cents=100 * signed(g.get(51, 0)) + signed(g.get(52, 0)))
            for key in range(g.get(43, 0x7F00) & 0xFF, (g.get(43, 0x7F00) >> 8) + 1):
                keys[key] = sample
        presets[(bank, program)] = keys
    return presets


def driver_notes(rom, song, frames, kit):
    """Runs the driver for `frames` frames and returns the samples that it starts on its voices, as dicts with the
    voice, the frame of the start, the sample's first point, its length, the volume and the frame on which the voice
    stops, and the PSG's state at each frame's start (psg_frames()). With `kit` false, the kit is off."""
    emu = driver_emu.DriverEmulator(rom)
    emu.play(song, kit)
    start_writes, emu.writes = emu.writes, []
    notes, playing, writes = [], {}, []
    for f in range(frames):
        before = len(emu.voice_events)
        frame_writes, _ = emu.frame()
        for event in emu.voice_events[before:]:
            voice = event[1]
            if voice in playing:
                playing.pop(voice)['off'] = f
            if event[2] == 'play':
                note = dict(voice=voice, on=f, data=event[3], length=event[4], volume=event[5], off=None)
                notes.append(note)
                playing[voice] = note
        for voice in list(playing):
            if emu.voice_state(voice) is None:
                playing.pop(voice)['off'] = f + 1
        writes.append(start_writes + frame_writes if f == 0 else frame_writes)
    return notes, psg_frames(writes)


# The registers of each PSG channel: NRx1 and NRx2, then NRx3 and NRx4.
CONTROL = [0x04000062, 0x04000068, 0x04000072, 0x04000078]


def psg_frames(writes):
    """Plays each frame's writes to the PSG's registers through psg_model.py and returns each frame's state at its
    start: for each channel, whether it sounds, its level and its frequency setting, and the noise's volume. The driver
    sets every channel on both sides at the init, and the PSG at 100%, so a level is the same on each side."""
    apu = psg_model.APU(hardware=True)
    out = []
    for frame_writes in writes:
        for w in frame_writes:
            apu.write(*w)
        state = []
        for c in range(4):
            envelope = apu.half(CONTROL[c]) >> 8
            rises = bool(envelope & 8 and envelope & 7)
            if c < 2:
                ch = apu.square[c]
                volume = ch['env']['vol']
                state.append((ch['on'] and (volume > 0 or rises), PSG_LEVEL[c] * volume, ch['x'], volume))
            elif c == 2:
                w = apu.wave
                share = 0.75 if w['force75'] else [0, 1, 0.5, 0.25][w['volcode']]
                state.append((w['on'] and w['dac'] and share > 0, PSG_LEVEL[c] * share, w['x'], 15))
            else:
                n = apu.noise
                volume = n['env']['vol']
                state.append((n['on'] and (volume > 0 or rises), PSG_LEVEL[c], 0, volume))
        out.append(state)
        apu.advance(psg_model.FRAME_CYCLES)
    return out


def expression(level):
    """Returns the CC11 that seqlib's LevelsToControllers() sets for `level` on both sides. A full-scale FIFO sample
    has a level of 128."""
    if level <= 0:
        return 0
    return min(max(math.floor(127 * math.sqrt(level / 127) + 0.5), 0), 127)


def check_psg(psg, midi_notes, controls, presets, end):
    """Returns the problems with the MIDI file's PSG tracks against the driver's PSG state `psg` (psg_frames())."""
    problems = []
    for c, channel in enumerate([0, 1, 2, NOISE_MIDI_CHANNEL]):
        name = 'the noise' if c == 3 else 'PSG channel %d' % c
        notes = sorted((n for n in midi_notes if n['channel'] == channel), key=lambda n: n['on'])
        ctl = controls.get(channel, {'cc11': [], 'bend': [], 'range': 2})
        reported = 0
        for f in range(min(len(psg), int(math.floor(end + 0.5)))):
            sounds, level, setting, volume = psg[f][c]
            note = next((n for n in notes if n['on'] <= f + 0.5 < n['off']), None)
            problem = None
            if sounds != (note is not None):
                problem = 'frame %d: %s %s in the driver but %s in the MIDI file' % (
                    f, name, 'sounds' if sounds else 'is silent', 'sounds' if note else 'not')
            elif note:
                cc11 = value_at(ctl['cc11'], f, 127)
                zone = presets.get((note['bank'], note['program']), {}).get(note['key'])
                if cc11 != expression(level):
                    problem = 'frame %d: %s has CC11 %d, where its level %.2f gives %d' % (
                        f, name, cc11, level, expression(level))
                elif zone is None:
                    problem = 'frame %d: %s has no zone for key %d of bank %d program %d' % (
                        f, name, note['key'], note['bank'], note['program'])
                elif c < 3:
                    # The zone plays its loop, a cycle of the square or of the wave's 64 steps, at the key's pitch.
                    bend = (value_at(ctl['bend'], f, 8192) - 8192) / 8192 * ctl['range']
                    cycle = zone['rate'] / (zone['loop'][1] - zone['loop'][0])
                    midi_hz = cycle * 2 ** ((note['key'] - zone['root'] + bend) / 12 + zone['cents'] / 1200)
                    driver_hz = 131072 / (2048 - setting) / (4 if c == 2 else 1)
                    cents = 1200 * math.log2(midi_hz / driver_hz)
                    if abs(cents) > CENTS:
                        problem = 'frame %d: %s plays %.2f cents from the driver\'s setting 0x%03X' % (
                            f, name, cents, setting)
                else:
                    # The drum points in the frame contain the channel volume, in 15ths of full scale. A drum that goes
                    # on unchanged loops its end.
                    into = int((f - round(note['on'])) * NOISE_RATE / FRAME_RATE) + 4
                    loop = zone['loop']
                    if loop and into >= loop[1]:
                        into = loop[0] + (into - loop[0]) % (loop[1] - loop[0])
                    points = zone['points'][into:into + int(NOISE_RATE / FRAME_RATE / 8)]
                    held = round(max((abs(p) for p in points), default=0) * 15 / 32767)
                    if abs(held - volume) > 1:
                        problem = 'frame %d: the noise\'s drum holds volume %d, the driver plays %d' % (f, held, volume)
            if problem:
                reported += 1
                if reported <= 3:
                    problems.append(problem)
        if reported > 3:
            problems.append('%s: %d more frames differ' % (name, reported - 3))
    return problems


def check_song(rom, song, framed, frames_limit, kit):
    """Returns the problems with the --frame-timing conversion of `song` and the number of notes checked. With `kit`
    false, the driver plays the song with the kit off."""
    problems = []
    midi_notes, end, controls = read_midi(framed)
    presets = read_sf2(framed.with_suffix('.sf2'))
    frames = min(frames_limit, int(round(end)) + 1)
    notes, psg = driver_notes(rom, song, frames, kit)
    kit_notes = sorted((n for n in midi_notes if n['channel'] == KIT_MIDI_CHANNEL), key=lambda n: n['on'])
    rate = round(CPU_CLOCK / (16 * (0x10000 - driver_emu.addresses_for(rom).timer_reload)) * 8)

    mirrored = sum(1 for n in notes if n['data'] >= MIRROR)
    expected = [n for n in notes if n['data'] < MIRROR and n['volume'] and n['on'] < end - 0.5]
    if len(expected) != len(kit_notes):
        problems.append('the driver starts %d samples that sound, but the MIDI file has %d notes on channel 11' %
                        (len(expected), len(kit_notes)))
    for d, m in zip(expected, kit_notes):
        where = 'frame %d, voice %d' % (d['on'], d['voice'])
        if abs(m['on'] - d['on']) > 0.5:
            problems.append('%s: the MIDI note starts on frame %.2f' % (where, m['on']))
            continue
        velocity = conversion.square_root_value(d['volume'], 64, 1)
        if m['velocity'] != velocity:
            problems.append('%s: velocity %d, where the volume %d gives %d' % (where, m['velocity'], d['volume'],
                                                                                velocity))
        off = min(d['off'] if d['off'] is not None else end, end)
        if abs(m['off'] - off) > 0.5:
            problems.append('%s: the MIDI note ends on frame %.2f, the driver stops it on %d' % (where, m['off'], off))
        if m['bank'] != KIT_BANK:
            problems.append('%s: the MIDI note plays in bank %d instead of bank %d' % (where, m['bank'], KIT_BANK))
        sample = presets.get((m['bank'], m['program']), {}).get(m['key'])
        if sample is None:
            problems.append('%s: the SoundFont has no zone for key %d of program %d' % (where, m['key'],
                                                                                      m['program']))
            continue
        points = min(POINTS_COMPARED, d['length'], len(sample['points']))
        original = [(b - 256 if b > 127 else b) * 256 for b in rom[d['data'] - ROM_BASE:d['data'] - ROM_BASE + points]]
        if list(sample['points'][:points]) != original:
            problems.append('%s: the zone of key %d plays another sample' % (where, m['key']))
        if sample['rate'] != rate:
            problems.append('%s: the sample plays at %d Hz, the mixer at %d' % (where, sample['rate'], rate))

    problems += check_psg(psg, midi_notes, controls, presets, end)
    return problems, len(expected), mirrored


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program')
    p.add_argument('folder', metavar='FOLDER', help="the folder containing agbsf2's output")
    p.add_argument('--songs', help='pieces to check, such as 0-13 (default: every piece with a MIDI file)')
    p.add_argument('--frames', type=int, default=12000, help='maximum frames to check per piece (default: 12000)')
    p.add_argument('-v', '--verbose', action='store_true', help='list every difference, not just the first ten')
    a = p.parse_args()
    rom = load_rom(a.rom)
    midis = {(n, version): path for n, version, path in conversion.midi_files(a.folder, 'ubimilan', versions=True)}
    if not midis:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    # The samples that play as music follow the pieces. They have no sequence for the driver to play.
    pieces = len(driver_emu.DriverEmulator(rom).songs())
    songs = sorted(set(traces.parse_range(a.songs))) if a.songs else sorted({n for n, _ in midis})
    songs = [s for s in songs if s < pieces]
    listed = {n for n, _ in traces.listed(a.agbsf2, 'ubimilan', a.rom)}
    kit_off = {n for n, name in traces.listed_versions(a.agbsf2, 'ubimilan', a.rom) if name == 'with the kit'}
    failed = total = 0
    with tempfile.TemporaryDirectory() as folder:
        conversion.frame_timed(a.agbsf2, a.rom, 'ubimilan', folder, a.songs)
        on_frames = {(n, version): path
                     for n, version, path in conversion.midi_files(folder, 'ubimilan', versions=True)}
        checks = [(n, version) for n in songs for version in ([''] + (['kit'] if n in kit_off else []))]
        for song, version in checks:
            # The first version of a piece that the game starts with the kit off has no kit. Its _kit version has.
            kit = version == 'kit' or song not in kit_off
            label = 'music %d%s' % (song, ' with the kit' if version else (' without the kit' if not kit else ''))
            midi = midis.get((song, version))
            if midi is None:
                print('%s: no MIDI file%s' % (label, '' if song in listed else ', and --info lists no such piece'))
                if version:
                    # --info lists the version, so the conversion must have its files.
                    total += 1
                    failed += 1
                continue
            total += 1
            framed = on_frames.get((song, version))
            if framed is None:
                print('%s: the conversion with --frame-timing has no MIDI file' % label)
                failed += 1
                continue

            problems, worst = conversion.compare_timing(midi, framed, 1 / FRAME_RATE, MAX_SHIFT)
            if midi.with_suffix('.sf2').read_bytes() != framed.with_suffix('.sf2').read_bytes():
                problems.append('the SoundFonts differ')
            if problems:
                print('%s: %d difference%s from the conversion on the driver\'s frames' % (
                    label, len(problems), '' if len(problems) == 1 else 's'))
                for line in problems if a.verbose else problems[:10]:
                    print('    ' + line)
                failed += 1
                continue

            problems, checked, mirrored = check_song(rom, song, framed, a.frames, kit)
            print('%s: on the beat, events at most %.2f frames from the driver\'s; %d notes, %d difference%s%s' %
                  (label, worst, checked, len(problems), '' if len(problems) == 1 else 's',
                   ', %d samples from a mirror of the ROM left out' % mirrored if mirrored else ''))
            for line in problems if a.verbose else problems[:10]:
                print('    ' + line)
            failed += 1 if problems else 0
    if not total:
        raise SystemExit('found none of the pieces to check')
    print('%d of %d conversions match the driver' % (total - failed, total))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
