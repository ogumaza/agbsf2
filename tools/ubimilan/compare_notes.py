#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against Ubisoft Milan's driver, note by note.

    compare_notes.py ROM AGBSF2 FOLDER [--songs 0-13] [--frames 12000]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each piece of music, or
NAME_ubimilan_NN.mid and NAME_ubimilan_NN.sf2 if another driver was detected first. Events in these MIDI files follow
the song's beat and may be up to a frame and a half from the driver's timing. The script converts ROM again with
--frame-timing to keep events on the driver's frames. Both conversions must have identical SoundFonts and the same
notes, keys and programs, in the same order on each channel and within 1.65 frames of each other: the frame and a
half, and the rounding of a tick. It then runs the driver under driver_emu.py for every frame of the --frame-timing
MIDI file. Each sample that the driver starts on a voice must be a note on MIDI channel 11:

  - starting on the frame on which the driver starts the sample;
  - at the velocity that the voice's volume gives: 127 times the volume over 64, a sample's full scale;
  - with a zone for its key in the preset of its program in bank 9. The zone's sample holds the driver's points at the
    rate the mixer plays them and loops where the driver's sample loops;
  - ending on the frame on which the driver stops the voice, or on the frame after the one in which the voice reaches
    the end of a sample without a loop, or with the file.

A sample that the driver starts at volume 0 plays nothing, and the MIDI file leaves it out. A key of channel 9 outside
its kit makes the driver play a sample from a mirror of the ROM, which the MIDI file leaves out too; those are counted.
The PSG's notes that sound, from the driver's writes to NR12, NR22, NR32 and NR42, must match the MIDI file's notes on
channels 1 to 3 and on channel 11's keys up to 11, frame for frame.
"""
import argparse
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py, gbarom.py and traces.py, in tools/
import conversion
import driver_emu
import traces
from gbarom import ROM_BASE, load_rom

FRAME_RATE = 16777216 / 280896
MAX_SHIFT = 1.65  # frames that an event on the beat can be from the driver's frame, with a margin for the MIDI's ticks
MIRROR = 0x0A000000
KIT_MIDI_CHANNEL = 10  # the MIDI channel of the driver's channel 9, MIDI's channel 11, counted from 0
KIT_BANK = 9
POINTS_COMPARED = 256  # points of each sample compared with the SoundFont's
NR31 = 0x04000072
# The MIDI channel of each register.
PSG_CHANNELS = {0x04000062: 0, 0x04000068: 1, NR31: 2, 0x04000078: KIT_MIDI_CHANNEL}


def read_midi(path):
    """Returns the MIDI file's notes as dicts with the channel, key, velocity, bank, program and the frames on which it
    starts and ends, and its length in frames."""
    data = Path(path).read_bytes()
    division = struct.unpack('>H', data[12:14])[0]
    events = []
    pos = 14
    while pos < len(data):
        size = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        pos += 8 + size
        tick, i, status = 0, 0, 0
        while i < len(body):
            delta = 0
            while True:
                b = body[i]
                i += 1
                delta = (delta << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            tick += delta
            if body[i] == 0xFF:
                kind, length = body[i + 1], body[i + 2]
                if kind == 0x51:
                    events.append((tick, 'tempo', int.from_bytes(body[i + 3:i + 6], 'big')))
                i += 3 + length
                continue
            if body[i] & 0x80:
                status = body[i]
                i += 1
            size = 1 if status >> 4 in (0xC, 0xD) else 2
            args = body[i:i + size]
            i += size
            events.append((tick, status, bytes(args)))

    tempos = sorted((t, v) for t, k, v in events if k == 'tempo')

    def frames(tick):
        seconds, at, tempo = 0.0, 0, 500000
        for t, v in tempos:
            if t > tick:
                break
            seconds += (t - at) * tempo / division / 1e6
            at, tempo = t, v
        return (seconds + (tick - at) * tempo / division / 1e6) * FRAME_RATE

    notes, open_notes, bank, program = [], {}, {}, {}
    last = 0
    for tick, status, args in sorted((e for e in events if e[1] != 'tempo'), key=lambda e: e[0]):
        last = max(last, tick)
        ch, kind = status & 0xF, status >> 4
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
    end = frames(max([last] + [t for t, _, _ in events]))
    for note in notes:
        if note['off'] is None:
            note['off'] = end
    return notes, end


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
                          loop=(loop_start - start, loop_end - start) if g.get(54, 0) & 1 else None)
            for key in range(g.get(43, 0x7F00) & 0xFF, (g.get(43, 0x7F00) >> 8) + 1):
                keys[key] = sample
        presets[(bank, program)] = keys
    return presets


def driver_notes(rom, song, frames):
    """Runs the driver for `frames` frames and returns the samples that it starts on its voices, as dicts with the
    voice, the frame of the start, the sample's first point, its length, the volume and the frame on which the voice
    stops, and the PSG's notes that sound, as (frame, register)."""
    emu = driver_emu.DriverEmulator(rom)
    emu.play(song)
    start_writes, emu.writes = emu.writes, []
    notes, playing, psg = [], {}, []
    for f in range(frames):
        before = len(emu.voice_events)
        writes, _ = emu.frame()
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
        for address, size, value in (start_writes + writes if f == 0 else writes):
            # NRx2 is the high byte of the halfword at NR11, NR21 and NR41. A note sounds if its starting volume isn't
            # 0, or if its envelope rises. NR32 is NR31's high byte, and its volume code is in bits 5 and 6.
            envelope = value >> 8
            if address in PSG_CHANNELS and address != NR31 and size == 2 and (
                    envelope >> 4 or (envelope & 8 and envelope & 7)):
                psg.append((f, address))
            elif address == NR31 and size == 2 and (value >> 13) & 3:
                psg.append((f, address))
    return notes, psg


def check_song(rom, song, framed, frames_limit):
    """Returns the problems with the --frame-timing conversion of `song` and the number of notes checked."""
    problems = []
    midi_notes, end = read_midi(framed)
    presets = read_sf2(framed.with_suffix('.sf2'))
    frames = min(frames_limit, int(round(end)) + 1)
    notes, psg = driver_notes(rom, song, frames)
    kit = sorted((n for n in midi_notes if n['channel'] == KIT_MIDI_CHANNEL and n['key'] > 11), key=lambda n: n['on'])
    rate = round(FRAME_RATE * 280896 / (16 * (0x10000 - driver_emu.addresses_for(rom).timer_reload)) * 8)

    mirrored = sum(1 for n in notes if n['data'] >= MIRROR)
    expected = [n for n in notes if n['data'] < MIRROR and n['volume'] and n['on'] < end - 0.5]
    if len(expected) != len(kit):
        problems.append('the driver starts %d samples that sound, but the MIDI file has %d notes on channel 11' %
                        (len(expected), len(kit)))
    for d, m in zip(expected, kit):
        where = 'frame %d, voice %d' % (d['on'], d['voice'])
        if abs(m['on'] - d['on']) > 0.5:
            problems.append('%s: the MIDI note starts on frame %.2f' % (where, m['on']))
            continue
        velocity = min(127, max(1, round(d['volume'] * 127 / 64)))
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

    # Each PSG channel's notes that sound start on the frames of the driver's writes.
    for address, channel in PSG_CHANNELS.items():
        name = 'the noise' if channel == KIT_MIDI_CHANNEL else 'PSG channel %d' % channel
        driver_frames = sorted(f for f, a in psg if a == address and f < end - 0.5)
        midi_frames = sorted(n['on'] for n in midi_notes
                             if n['channel'] == channel and (channel != KIT_MIDI_CHANNEL or n['key'] <= 11))
        if len(driver_frames) != len(midi_frames):
            problems.append('the driver starts %d notes that sound on %s, the MIDI file has %d' %
                            (len(driver_frames), name, len(midi_frames)))
            continue
        for d, m in zip(driver_frames, midi_frames):
            if abs(m - d) > 0.5:
                problems.append('frame %d: a note of %s starts on frame %.2f in the MIDI file' % (d, name, m))
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
    midis = dict(conversion.midi_files(a.folder, 'ubimilan'))
    if not midis:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    songs = sorted(set(traces.parse_range(a.songs))) if a.songs else sorted(midis)
    listed = {n for n, _ in traces.listed(a.agbsf2, 'ubimilan', a.rom)}
    failed = total = 0
    with tempfile.TemporaryDirectory() as folder:
        on_frames = conversion.frame_timed(a.agbsf2, a.rom, 'ubimilan', folder, a.songs)
        for song in songs:
            midi = midis.get(song)
            if midi is None:
                print('music %d: no MIDI file%s' % (song, '' if song in listed else ', and --info lists no such piece'))
                continue
            total += 1
            framed = on_frames.get(song)
            if framed is None:
                print('music %d: the conversion with --frame-timing has no MIDI file' % song)
                failed += 1
                continue

            problems, worst = conversion.compare_timing(midi, framed, 1 / FRAME_RATE, MAX_SHIFT)
            if midi.with_suffix('.sf2').read_bytes() != framed.with_suffix('.sf2').read_bytes():
                problems.append('the SoundFonts differ')
            if problems:
                print('music %d: %d difference%s from the conversion on the driver\'s frames' % (
                    song, len(problems), '' if len(problems) == 1 else 's'))
                for line in problems if a.verbose else problems[:10]:
                    print('    ' + line)
                failed += 1
                continue

            problems, checked, mirrored = check_song(rom, song, framed, a.frames)
            print('music %d: on the beat, events at most %.2f frames from the driver\'s; %d notes, %d difference%s%s' %
                  (song, worst, checked, len(problems), '' if len(problems) == 1 else 's',
                   ', %d samples from a mirror of the ROM left out' % mirrored if mirrored else ''))
            for line in problems if a.verbose else problems[:10]:
                print('    ' + line)
            failed += 1 if problems else 0
    if not total:
        raise SystemExit('found none of the pieces to check')
    print('%d of %d pieces match the driver' % (total - failed, total))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
