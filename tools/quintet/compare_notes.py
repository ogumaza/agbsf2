#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check a conversion's MIDI files and SoundFonts against the game's driver, frame by frame.

    compare_notes.py ROM AGBSF2 FOLDER [--songs 0-22] [--frames 12000]

FOLDER holds agbsf2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2 for each song, or NAME_quintet_NN.mid and
NAME_quintet_NN.sf2 if detection finds another of the game's drivers first. For every song, the driver runs under
driver_emu.py for as long as the MIDI file lasts, or until all of its channels have ended, and the script works out from
its register writes what each of the 6 channels plays in each frame. The MIDI file and the SoundFont have to play the
same, channel by channel:

  - the frames in which the channel sounds;
  - its pitch: on a square or wave channel, the frequency setting against the one the note's key stands for in the
    driver's frequency table, and on a PCM channel, the rate its FIFO plays at against the one the key stands for in
    the driver's PCM pitch table (the MIDI files keep equal temperament, so the tables' tuning is left out);
  - its level on each side, which CC10 and CC11 carry, to within 1.5 dB;
  - its waveform: a square's duty, the wave channel's 32 or 64 steps, or the bytes of a PCM sample.

A MIDI note on a channel that the driver doesn't have, or one that starts after all of the driver's channels have
ended, counts as a difference.

The noise channel's drums are rendered into the SoundFont from the driver's writes to its registers, so for it the sides
are checked, and the volume that the drum's sample has at each frame against the channel's. While the channel's
envelope runs, a step either way is allowed, since the envelope steps on a clock that isn't tied to the frames.

MIDI notes land on their own ticks, where the driver plays them at the start of the frame that reaches them, and each
frame's changes come as far into the frame as the note's start. So each frame of the driver is compared with that
stretch of the MIDI file, or failing that, with the MIDI file anywhere from half a frame before the frame to half a
frame after the next. The MIDI file is timed as a player plays it, at tempos of whole microseconds a quarter note,
which can put a note a little ahead of the driver's frames or behind them, so a note that starts that close to a
frame's start may belong to the end of the frame before. AGBSF2 is used for its --info report. The report names the
driver's tables.
"""
import argparse
import bisect
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for conversion.py and gbarom.py, in tools/
import conversion
import driver_emu
from compare_trace import parse_range
from gbarom import ROM_BASE, load_rom

FRAME_RATE = 16777216 / 280896
# The MIDI file's tempos are whole microseconds a quarter note, up to half a microsecond off the exact ones, so by a
# note's start, the file can be ahead of the driver or behind it by this many seconds for each quarter note before it.
TEMPO_ROUNDING = 0.5e-6
LEVEL_TOLERANCE_DB = 1.5
PITCH_TOLERANCE_CENTS = 5
# The points around a frame, in frames from its start, at which the MIDI file's state is looked up if the notes around
# it don't play the frame's state where they should. A MIDI note starts at its own tick, up to a frame after the start
# of the frame the driver plays it in, and each frame's changes come as far into the frame as the note's start.
PROBES = (0.5, 0.0, 0.95, 0.25, 0.75, 1.25, 1.5, -0.25, -0.5, 0.125, 0.375, 0.625, 0.875, 0.99, 1.125, 1.375)
PCM_UNITY_INDEX = 24  # the PCM pitch table's entry for a sample's rate, which plays on the sample's root key
CHANNEL_NAMES = ['Square 1', 'Square 2', 'Wave', 'Noise', 'PCM A', 'PCM B']
DUTY_NAMES = ['12.5%', '25%', '50%', '75%']
WAVE_CODES = [0, 1, 0.5, 0.25]


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
        pcm = struct.unpack('<%dh' % (len(parts[b'smpl']) // 2), parts[b'smpl'])

        def records(name, size):
            raw = parts[name]
            return [raw[i:i + size] for i in range(0, len(raw), size)]

        self.samples = []
        for s in records(b'shdr', 46)[:-1]:
            start, end, loop_start, loop_end, rate = struct.unpack('<IIIII', s[20:40])
            self.samples.append(dict(name=s[:20].split(b'\0')[0].decode('latin-1'), rate=rate, root=s[40],
                                     data=pcm[start:end], loop=(loop_start - start, loop_end - start)))

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
    """Returns the notes as dicts of channel, key, on and off seconds, bank, program and drift (the most seconds by
    which the tempos' rounding can have moved the start), each channel's changes of CC10, CC11 and pitch bend as
    {(channel, 10, 11 or 'bend'): (times, values)}, the bend ranges and the length in seconds."""
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

    def seconds(tick):
        s, at, tempo = 0.0, 0, 500000
        for t, value in tempos:
            if t > tick:
                break
            s += (t - at) * tempo / division / 1e6
            at, tempo = t, value
        return s + (tick - at) * tempo / division / 1e6

    notes, open_notes, changes, ranges = [], {}, {}, {}
    bank, program, rpn = {}, {}, {}
    for tick, _, _, kind, ch, a, b in events:
        t = seconds(tick)
        if kind == 'on':
            note = dict(channel=ch, key=a, on=t, off=None, bank=bank.get(ch, 0), program=program.get(ch, 0),
                        drift=tick / division * TEMPO_ROUNDING)
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
        elif kind == 'cc' and a in (10, 11):
            times, values = changes.setdefault((ch, a), ([], []))
            times.append(t)
            values.append(b)
        elif kind == 'bend':
            times, values = changes.setdefault((ch, 'bend'), ([], []))
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
    return notes, changes, ranges, length


def value_at(changes, key, t, default):
    times, values = changes.get(key, ((), ()))
    i = bisect.bisect_right(times, t)
    return values[i - 1] if i else default


def driver_tables(tool, rom_path):
    """Returns the frequency and PCM pitch tables' addresses from agbsf2's --info report."""
    text = subprocess.run([tool, '--driver', 'quintet', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    frequency = int(re.search(r'frequency table 0x([0-9A-F]+)', text).group(1), 16)
    pcm = int(re.search(r'PCM pitch table 0x([0-9A-F]+)', text).group(1), 16)
    return frequency, pcm


class Psg:
    """A PSG channel's on state, volume, length counter, envelope and sweep, from the register writes of each frame."""

    def __init__(self, channel):
        self.channel = channel
        self.on = False
        self.volume = 0
        self.up = False
        self.period = 0
        self.envelope_ticks = 0
        self.length = 0
        self.length_on = False
        self.frequency = 0  # the frequency setting that plays
        self.written = 0    # the one last written
        self.shadow = 0     # the sweep's copy of it
        self.sweeping = False
        self.sweep = (0, 0, False)
        self.sweep_ticks = 0
        self.cycles = 0
        self.step = 0

    def sweep_next(self):
        change = self.shadow >> self.sweep[1]
        return self.shadow - change if self.sweep[2] else self.shadow + change

    def frame(self, regs, restarted):
        """Takes in the registers after a frame's writes. Returns the channel's on state, volume and frequency setting
        at the frame's start, the lowest and highest volume it plays at during the frame (0 while it's off), and the
        frequency settings it plays at, which the sweep can change within the frame."""
        envelope = regs.get([0x62, 0x68, 0x72, 0x78][self.channel], 0)
        control = regs.get([0x64, 0x6C, 0x74, 0x7C][self.channel], 0)
        nr10 = regs.get(0x60, 0)
        self.sweep = (nr10 >> 4 & 7, nr10 & 7, bool(nr10 & 8))
        if restarted:
            self.on = True
            self.volume = 15 if self.channel == 2 else envelope >> 12
            self.up = bool(envelope & 0x800)
            self.period = 0 if self.channel == 2 else envelope >> 8 & 7
            self.envelope_ticks = 0
            self.length_on = bool(control & 0x4000)
            self.length = 256 - (envelope & 0xFF) if self.channel == 2 else 64 - (envelope & 63)
            self.frequency = self.shadow = control & 0x7FF
            self.sweeping = self.channel == 0 and bool(self.sweep[0] or self.sweep[1])
            self.sweep_ticks = 0
            if self.sweeping and self.sweep[1] and self.sweep_next() > 0x7FF:
                self.on = False
        elif control & 0x7FF != self.written:
            self.frequency = control & 0x7FF
        self.written = control & 0x7FF
        if (self.channel == 2 and not regs.get(0x70, 0) & 0x80) or (self.channel != 2 and not envelope & 0xF800):
            self.on = False
        state = (self.on, self.volume, self.frequency)

        # Run the 512 Hz frame sequencer through the frame: the length counter at 256 Hz, the sweep at 128 Hz and the
        # envelope at 64 Hz.
        low = high = self.volume if self.on else 0
        frequencies = [self.frequency]
        self.cycles += 280896
        while self.cycles >= 32768:
            self.cycles -= 32768
            self.step = (self.step + 1) & 7
            if self.step % 2 == 0 and self.length_on and self.on:
                self.length -= 1
                if self.length <= 0:
                    self.on = False
            if self.step in (2, 6) and self.sweeping and self.sweep[0]:
                self.sweep_ticks += 1
                if self.sweep_ticks >= self.sweep[0]:
                    self.sweep_ticks = 0
                    value = self.sweep_next()
                    if value > 0x7FF:
                        self.on = False
                    elif self.sweep[1]:
                        self.shadow = self.frequency = value
                        frequencies.append(value)
                        if self.sweep_next() > 0x7FF:
                            self.on = False
            if self.step == 7 and self.period:
                self.envelope_ticks += 1
                if self.envelope_ticks >= self.period:
                    self.envelope_ticks = 0
                    self.volume = min(self.volume + 1, 15) if self.up else max(self.volume - 1, 0)
            now = self.volume if self.on else 0
            low, high = min(low, now), max(high, now)
        return state + (low, high, frequencies)


def driver_frames(rom, song, frames):
    """Runs the driver and returns, for each frame, what each channel plays: None for silence, or a dict of its levels
    on each side (128 for a full-scale FIFO sample), its frequency setting or FIFO rate, and what it plays."""
    emu = driver_emu.DriverEmulator(rom)
    regs = {}
    wave_ram = [bytearray(16), bytearray(16)]
    fifos = [dict(next=0, start=None, rate=0) for _ in range(2)]
    psgs = [Psg(c) for c in range(4)]
    out = []
    writes = emu.play(song)
    for f in range(frames):
        restarted = [False] * 4
        for address, size, value in (writes if f == 0 else []) + emu.frame():
            for i in range(size):
                a = address + i
                byte = (value >> (8 * i)) & 0xFF
                if 0x04000090 <= a < 0x040000A0:
                    bank = ((regs.get(0x70, 0) >> 6) & 1) ^ 1
                    wave_ram[bank][a - 0x04000090] = byte
            register = address - 0x04000000
            if size == 1 and register == 0x70:
                regs[0x70] = (regs.get(0x70, 0) & 0xFF00) | value
            elif size == 2:
                regs[register] = value
                if register in (0x64, 0x6C, 0x74, 0x7C) and value & 0x8000:
                    restarted[[0x64, 0x6C, 0x74, 0x7C].index(register)] = True
            for k, fifo in enumerate(fifos):
                if address == (0x040000BC, 0x040000C8)[k]:
                    fifo['next'] = value - 32
                elif address == (0x040000C6, 0x040000D2)[k]:
                    if value & 0x8000:
                        fifo['start'] = fifo['next']
                    else:
                        fifo['start'] = None
                elif address == (0x04000100, 0x04000104)[k]:
                    fifo['rate'] = 16777216 / (0x10000 - (value & 0xFFFF)) if value & 0x800000 else 0
        frame = [None] * 6
        nr50, nr51 = regs.get(0x80, 0) & 0xFF, regs.get(0x80, 0) >> 8
        soundcnt_h = regs.get(0x82, 0)
        mix = [0.25, 0.5, 1.0, 1.0][soundcnt_h & 3]
        scale = (mix * ((nr50 >> 4 & 7) + 1) / 8, mix * ((nr50 & 7) + 1) / 8)
        for c in range(4):
            on, volume, frequency, low, high, frequencies = psgs[c].frame(regs, restarted[c])
            if not on and not high:
                continue
            if c == 2:
                nr32 = regs.get(0x72, 0)
                level = 4 * 15 * (0.75 if nr32 & 0x8000 else WAVE_CODES[nr32 >> 13 & 3])
                nr30 = regs.get(0x70, 0)
                bank = nr30 >> 6 & 1
                steps = []
                for b in range(2 if nr30 & 0x20 else 1):
                    for v in wave_ram[(bank + b) & 1]:
                        steps += [v >> 4, v & 15]
                what = tuple(steps)
            elif c == 3:
                # The noise channel's level at its loudest in the frame; its volumes are checked against the drum's.
                # When its envelope runs, the drum may be a step either way, since the hardware's envelope steps on a
                # clock of its own, which a drum can't follow.
                level, what = 2 * high, (low, high)
                if psgs[c].period:
                    what = (max(low - 1, 0), min(high + 1, 15))
            else:
                level = 2 * volume
                what = (regs.get(0x62 if c == 0 else 0x68, 0) >> 6) & 3
            frame[c] = dict(left=level * scale[0] if nr51 & (0x10 << c) else 0,
                            right=level * scale[1] if nr51 & (0x01 << c) else 0,
                            scale=(scale[0] if nr51 & (0x10 << c) else 0, scale[1] if nr51 & (0x01 << c) else 0),
                            pitch=frequency, pitches=frequencies, what=what)
        for k, fifo in enumerate(fifos):
            if fifo['start'] is None or not fifo['rate']:
                continue
            level = 128 if soundcnt_h & (4 << k) else 64
            frame[4 + k] = dict(left=level if soundcnt_h & (0x200 << 4 * k) else 0,
                                right=level if soundcnt_h & (0x100 << 4 * k) else 0,
                                pitch=fifo['rate'], what=fifo['start'])
        out.append(frame)
        if not emu.playing():
            break
    return out


def midi_levels(cc10, cc11):
    """Returns the levels on each side that CC10 and CC11 stand for in agbsf2's LevelsToControllers()."""
    amp = (cc11 / 127) ** 2 * math.sqrt(2) * 127 / 128
    angle = 0.5 if cc10 == 64 else cc10 / 127
    return amp * math.cos(angle * math.pi / 2) * 128, amp * math.sin(angle * math.pi / 2) * 128


def level_db(a, b):
    return 20 * math.log10(max(a, 1e-3) / max(b, 1e-3))


def sample_volume(zone, key, elapsed):
    """Returns the volume (0-15) that a noise drum's sample plays at, `elapsed` seconds into a note on `key`."""
    sample = zone['sample']
    root = zone['g'].get(58, sample['root'])
    position = elapsed * sample['rate'] * 2 ** ((key - root) / 12)
    start, end = sample['loop']
    if zone['g'].get(54, 0) & 1 and position >= end > start:
        position = start + (position - start) % (end - start)
    i = int(position)
    window = sample['data'][max(i - 8, 0):i + 8]
    return round(max((abs(v) for v in window), default=0) * 15 / 32767)


def silent(state):
    """Returns true if a frame's state makes no sound: nothing plays, or it plays at a level of 0 on both sides."""
    if state is None:
        return True
    if 'note' in state:
        if state['zone'] is None:
            return False
        level = max(midi_levels(state['cc10'], state['cc11']))
        if 'volume' in state:
            level *= state['volume'] / 15
        return level < 0.5
    return state['left'] < 0.5 and state['right'] < 0.5


def levels_match(want, have):
    """Returns true if the levels on each side, (left, right), agree within the tolerance."""
    return all(not (w > 0.5 or h > 0.5) or abs(level_db(h, w)) <= LEVEL_TOLERANCE_DB for w, h in zip(want, have))


def check_frame(c, want, have, info):
    """Compares a channel's frame in the driver with the MIDI file's state then. Returns true if they match, and the
    pitch difference in cents, or None for the noise channel or a difference in anything else."""
    if silent(want) or silent(have):
        return silent(want) and silent(have), None
    note, zone, bend = have['note'], have['zone'], have['bend']
    levels = midi_levels(have['cc10'], have['cc11'])

    # A noise drum plays at the channel's full level on its sides, and its sample has the channel's volume in it.
    if c == 3:
        low, high = want['what']
        full = (2 * 15 * want['scale'][0], 2 * 15 * want['scale'][1])
        return levels_match(full, levels) and low <= have['volume'] <= high, None
    if not levels_match((want['left'], want['right']), levels):
        return False, None

    # A square's duty, or the wave channel's steps, and its pitch against the frequency table's entry for the key.
    if c in (0, 1, 2):
        if c == 2:
            steps = want['what']
            # The wave key stands for the pattern's pitch an octave down for 32 steps, two for 64, and 12 up for each
            # time it repeats, which the zone's root key gives, so find the table entry from them.
            index = note['key'] - (12 if len(steps) > 32 else 24) - (zone['g'].get(58, zone['sample']['root']) - 60)
            sample = zone['sample']
            cycle = sample['data'][sample['loop'][0]:sample['loop'][1]]
            got = [cycle[i] for i in range(0, len(cycle), 4)]
            mean = sum(steps) / len(steps)
            expected = [(v - mean) / 15 * 32767 for v in steps]
            if len(got) != len(expected) or max(abs(g - e) for g, e in zip(got, expected)) > 2:
                return False, None
        else:
            index = note['key'] - 36
            if zone['sample']['name'] != 'Square ' + DUTY_NAMES[want['what']]:
                return False, None
        if not 0 <= index < len(info['frequency']):
            return False, None
        # The frame's pitch at its start, or after a step of the sweep within it.
        base = info['frequency'][index]
        cents = min((1200 * math.log2((2048 - base) / (2048 - min(f, 2047))) - bend * 100 for f in want['pitches']),
                    key=abs)
        return abs(cents) <= PITCH_TOLERANCE_CENTS, cents

    # A PCM sample's bytes, and its rate against the one the PCM pitch table gives the key's index.
    sample = zone['sample']
    start = want['what']
    data = [struct.unpack('<b', info['rom'][start - ROM_BASE + i:start - ROM_BASE + i + 1])[0] * 256
            for i in range(min(64, len(sample['data'])))]
    if list(sample['data'][:len(data)]) != data and not have['continued']:
        return False, None
    index = note['key'] - zone['g'].get(58, sample['root']) + PCM_UNITY_INDEX
    if not 0 <= index < len(info['pcm']):
        return False, None
    nominal = sample['rate'] * (1 + info['pcm'][index] / 1000)
    cents = 1200 * math.log2(want['pitch'] / nominal) - bend * 100
    return abs(cents) <= PITCH_TOLERANCE_CENTS, cents


def check_song(rom, info, song, midi_path, sf2_path, frames_limit, report):
    notes, changes, ranges, length = read_midi(midi_path)
    soundfont = SoundFont(sf2_path)
    frames = min(frames_limit, int(length * FRAME_RATE))
    driver = driver_frames(rom, song, frames)
    by_channel = {}
    for n in notes:
        by_channel.setdefault(n['channel'], []).append(n)
    for ch in by_channel.values():
        ch.sort(key=lambda n: n['on'])

    starts = {c: [n['on'] for n in ch] for c, ch in by_channel.items()}

    def note_probes(c, f):
        """Returns the times, in frames from the start of frame f, at which the MIDI notes around it play the frame's
        state: the middle of the stretch that starts as far into the frame as the note's start is into its own frame,
        cut to the note."""
        ch = by_channel.get(c, [])
        out = []
        i = bisect.bisect_right(starts.get(c, []), (f + 2) / FRAME_RATE) - 1
        while i >= 0 and ch[i]['off'] * FRAME_RATE > f - 1:
            on, off = ch[i]['on'] * FRAME_RATE, ch[i]['off'] * FRAME_RATE
            # A start within a rounding error of a frame's start, or as near as the tempos' rounding can have moved it,
            # may come at the end of the frame before.
            boundary = round(on)
            near = abs(on - boundary) < 0.001 + ch[i]['drift'] * FRAME_RATE
            for offset in (on - boundary, on - boundary + 1) if near else (on - math.floor(on),):
                low, high = max(f + offset, on), min(f + 1 + offset, off)
                if low < high:
                    out.append((low + high) / 2 - f)
            i -= 1
        return out

    def midi_state(c, t):
        """Returns what MIDI channel c plays at time t, or None."""
        ch = by_channel.get(c, [])
        i = bisect.bisect_right(starts.get(c, []), t) - 1
        if i < 0 or not ch[i]['on'] <= t < ch[i]['off']:
            return None
        note = ch[i]
        zones = [z for z in soundfont.presets.get((note['bank'], note['program']), [])
                 if z['low'] <= note['key'] <= z['high']]
        if not zones:
            return dict(note=note, zone=None)
        # In an instrument that switches samples, the first zone holds for a time and the second waits for it.
        zone = zones[0]
        if len(zones) > 1:
            delay = 2 ** (signed(zones[1]['g'].get(33, 0x10000 - 12000)) / 1200)
            zone = zones[1] if t - note['on'] >= delay - 1e-6 else zones[0]
        # A controller that the file doesn't set has the value that a player starts the channel with, so that a note
        # on a channel without CC11 sounds, as it does in a player.
        bend = (value_at(changes, (c, 'bend'), t, 8192) - 8192) / 8192 * ranges.get(c, 2)
        state = dict(note=note, zone=zone, cc10=value_at(changes, (c, 10), t, 64),
                     cc11=value_at(changes, (c, 11), t, 127), bend=bend, continued=t - note['on'] > 1.5 / FRAME_RATE)
        if c == 3:
            state['volume'] = sample_volume(zone, note['key'], t - note['on'])
        return state

    problems = []
    worst = 0.0
    checked = 0
    for c in range(6):
        if not any(d[c] for d in driver) and c not in by_channel:
            continue
        for f, d in enumerate(driver):
            if f >= frames - 1:
                break
            want = d[c]
            # The frame's state in the MIDI file anywhere from half a frame before it to half a frame after the next,
            # taking the closest pitch of those that match.
            best = None
            for shift in note_probes(c, f) + list(PROBES):
                have = midi_state(c, (f + shift) / FRAME_RATE)
                if have is not None and have['zone'] is None:
                    continue
                ok, cents = check_frame(c, want, have, info)
                if ok and (best is None or abs(cents or 0) < best):
                    best = abs(cents or 0)
                    if best <= 1:
                        break
            checked += 1
            if best is not None:
                worst = max(worst, best)
            else:
                have = midi_state(c, (f + 0.5) / FRAME_RATE)
                problems.append('frame %d, %s: the driver plays %s, the MIDI file %s' % (
                    f, CHANNEL_NAMES[c], describe(want), describe_midi(have)))

        # The driver starts no more notes once all of its channels have ended, and the frames above stop there, so a
        # note that starts after the last of them plays nothing in the game. A start within a rounding error of a
        # frame's start belongs to that frame, and the tempos' rounding can have moved a start past the last frame.
        for n in by_channel.get(c, []):
            on = n['on'] * FRAME_RATE
            if len(driver) - 0.001 + n['drift'] * FRAME_RATE < on < frames - 1:
                problems.append('frame %.1f, %s: the MIDI file starts key %d after the driver stopped in frame %d' % (
                    on, CHANNEL_NAMES[c], n['key'], len(driver) - 1))

    # The driver has 6 channels, so notes on any other MIDI channel would be checked against nothing.
    for extra in sorted(ch for ch in by_channel if ch >= 6):
        count = len(by_channel[extra])
        problems.append('MIDI channel %d plays %d note%s, but the driver has 6 channels' % (
            extra + 1, count, '' if count == 1 else 's'))
    report(song, checked, problems, worst)
    return not problems


def describe(want):
    if want is None:
        return 'nothing'
    what = want['what']
    if isinstance(what, tuple) and len(what) == 2:
        what = 'noise at volume %d-%d' % what
    elif isinstance(what, tuple):
        what = 'a wave of %d steps' % len(what)
    elif isinstance(what, int) and what > 0x08000000:
        what = 'the sample at 0x%08X' % what
    elif isinstance(what, int):
        what = 'duty ' + DUTY_NAMES[what]
    return '%s at %s, levels %.1f/%.1f' % (what, want['pitch'], want['left'], want['right'])


def describe_midi(have):
    if have is None:
        return 'nothing'
    if have['zone'] is None:
        return 'key %d with no zone' % have['note']['key']
    left, right = midi_levels(have['cc10'], have['cc11'])
    volume = ' at volume %d' % have['volume'] if 'volume' in have else ''
    return 'key %d with %s%s, bend %+.2f, levels %.1f/%.1f' % (have['note']['key'], have['zone']['sample']['name'],
                                                            volume, have['bend'], left, right)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba)')
    p.add_argument('agbsf2', metavar='AGBSF2', help='the agbsf2 program, for its --info report')
    p.add_argument('folder', metavar='FOLDER', help="the folder containing agbsf2's output")
    p.add_argument('--songs', help='songs to check, such as 0-22 (default: every song with a MIDI file)')
    p.add_argument('--frames', type=int, default=12000, help='maximum frames to check per song (default: 12000)')
    p.add_argument('-v', '--verbose', action='store_true', help='list every difference, not just the first ten')
    a = p.parse_args()
    rom = load_rom(a.rom)
    files = conversion.midi_files(a.folder, 'quintet')
    if not files:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)
    frequency_table, pcm_table = driver_tables(a.agbsf2, a.rom)
    def table(address, count, form):
        return list(struct.unpack('<%d%s' % (count, form), rom[address - ROM_BASE:address - ROM_BASE + 2 * count]))

    info = dict(rom=rom, frequency=[v & 0x7FF for v in table(frequency_table, 84, 'H')], pcm=table(pcm_table, 53, 'h'))
    wanted = set(parse_range(a.songs)) if a.songs else None
    failed = total = 0

    def report(song, checked, problems, worst):
        print('song %d: %d channel frames, %d difference%s, pitch within %.2f cents' % (
            song, checked, len(problems), '' if len(problems) == 1 else 's', worst))
        for line in problems if a.verbose else problems[:10]:
            print('    ' + line)

    for song, midi in files:
        if wanted is not None and song not in wanted:
            continue
        total += 1
        if not check_song(rom, info, song, midi, midi.with_suffix('.sf2'), a.frames, report):
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
