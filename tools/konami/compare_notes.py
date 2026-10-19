#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare agbsf2's MIDI files and SoundFonts with the game's driver, note by note.

    compare_notes.py ROM AGBSF2 FOLDER [--songs 0-22]

FOLDER holds AGBSF2's conversion of ROM: NAME_NN.mid and NAME_NN.sf2
for song NN, or NAME_konami_NN.mid and NAME_konami_NN.sf2 if another driver
was detected first. Events in these MIDI files follow the song's beat and
may be up to a frame and a half from the driver's timing. The script
converts ROM again with --frame-timing to keep events on the driver's
frames. Both conversions must have identical SoundFonts and the same notes,
keys, programs, controller changes and pitch bends, in the same order on
each channel and within 1.65 frames of each other: a frame and a half, and
the rounding of a tick. An event that gives a controller, pitch bend or
program the value it already has, as the conversions do where a loop starts,
isn't compared. For each song, it then runs the driver under driver_emu.py
for the duration of the --frame-timing MIDI file. On every frame, it
compares the driver's DirectSound voice records and PSG registers with what
that MIDI file plays through its SoundFont:

* Notes start and stop on exactly the frames where the driver starts and stops
  a voice or channel. A voice or channel that the MIDI file has no track for
  has to be silent in the driver, and each track with notes has to stand for
  a different voice or channel of the driver. Where every write of a square
  channel restarts it, as in AYDE, a PSG note can also start on any frame that
  writes its channel.
* A sample note plays the driver's sample, with the same data and loop, at the
  driver's pitch. The driver's pitch is a whole number of steps, so a
  difference of up to two steps (a few cents) is allowed. Where a FIFO plays
  its voices at its timer's rate, as in AYDE, the difference can be 2 cents.
* A PSG note plays the driver's duty cycle, wave RAM image or noise setting,
  within one step of the frequency register the driver wrote.
* CC10 and CC11 give the driver's level on each side to within 1.5 dB, and
  CC91 follows the driver's echo routing, which PSG channels aren't part of.

agbsf2 writes no MIDI file for a song with no notes. If
`AGBSF2 --info` lists a song missing from FOLDER, the driver must be
silent for its reported duration.

It prints the first 10 differences in each song and the largest pitch and level
deviations, and exits with status 1 if anything differs.
"""
import argparse
import functools
import math
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from unicorn import UC_HOOK_MEM_WRITE

# For conversion.py, gbarom.py, psg_model.py and traces.py, in tools/
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import conversion
from driver_emu import DriverEmulator
from gbarom import ROM_BASE, load_rom
from psg_model import DUTY
from traces import parse_range

LEVEL_TOLERANCE_DB = 1.5
PSG_TRACKS = {'Square 1': 0, 'Square 2': 1, 'Wave': 2, 'Noise': 3}
DUTY_NAMES = ['12.5%', '25%', '50%', '75%']
FRAME_SECONDS = 280896 / 16777216
MAX_SHIFT = 1.65  # frames that an event on the beat can be from the driver's frame, with a margin for the MIDI's ticks

# The level agbsf2 plays every wave note at (kWaveRowLevel): the level at the 100% volume code. The wave RAM image holds
# the note's volume.
WAVE_LEVEL = 60

# The share of the PSG in the mix for each setting of SOUNDCNT_H bits 0-1 (the last is prohibited).
PSG_RATIOS = [0.25, 0.5, 1.0, 0.0]

# The first register of each PSG channel's NR registers, and the one after its last: NR10-NR14, NR21-NR24, NR31-NR34
# (NR30 turns the wave channel on and switches its banks) and NR41-NR44.
CHANNEL_REGISTERS = [(0x04000060, 0x04000068), (0x04000068, 0x04000070), (0x04000072, 0x04000078),
                     (0x04000078, 0x04000080)]


def chunks(data, pos, end):
    while pos + 8 <= end:
        size = struct.unpack_from('<I', data, pos + 4)[0]
        yield data[pos:pos + 4], pos + 8, size
        pos += 8 + size + (size & 1)


class SoundFont:
    """The samples, instruments and presets of an SF2 file."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        c = {}
        for cid, pos, size in chunks(data, 12, len(data)):
            if cid == b'LIST':
                for sub, sub_pos, sub_size in chunks(data, pos + 4, pos + size):
                    c[sub] = data[sub_pos:sub_pos + sub_size]
        self.samples = []
        for i in range(len(c[b'shdr']) // 46 - 1):
            name, start, end, loop_start, loop_end, rate, key = struct.unpack_from('<20sIIIIIB', c[b'shdr'], i * 46)
            pcm = struct.unpack_from('<%dh' % (end - start), c[b'smpl'], start * 2)
            self.samples.append(dict(name=name.rstrip(b'\0').decode('latin-1'), pcm=pcm, rate=rate, key=key,
                                     loop=(loop_start - start, loop_end - start)))
        self.instruments = [self._zones(c[b'inst'], 22, 20, c[b'ibag'], c[b'igen'], i)
                            for i in range(len(c[b'inst']) // 22 - 1)]
        self.presets = {}
        for i in range(len(c[b'phdr']) // 38 - 1):
            program, bank = struct.unpack_from('<HH', c[b'phdr'], i * 38 + 20)
            zones = self._zones(c[b'phdr'], 38, 24, c[b'pbag'], c[b'pgen'], i)
            self.presets[(bank, program)] = zones[0][41]

    @staticmethod
    def _zones(headers, size, bag_offset, bags, gens, i):
        """Returns one generator dictionary per zone in header i."""
        first, end = (struct.unpack_from('<H', headers, j * size + bag_offset)[0] for j in (i, i + 1))
        zones = []
        for bag in range(first, end):
            g0, g1 = (struct.unpack_from('<H', bags, b * 4)[0] for b in (bag, bag + 1))
            zones.append(dict(struct.unpack_from('<HH', gens, g * 4) for g in range(g0, g1)))
        return zones

    def zone(self, bank, program, key):
        """Returns the sample and root key that play `key` on preset bank:program, or None."""
        instrument = self.presets.get((bank, program))
        if instrument is None:
            return None
        # The instrument's global zone comes first and holds no sample.
        for z in self.instruments[instrument]:
            key_range = z.get(43, 0x7F00)
            if 53 in z and key_range & 0xFF <= key <= key_range >> 8:
                sample = self.samples[z[53]]
                return dict(sample, root=z.get(58, sample['key']), looped=bool(z.get(54, 0) & 1))
        return None


def read_midi(path):
    """Returns [(track name, [(frame, status, data...)])], leaving out meta events, and the length in frames, with each
    tick at the frame it stands for under the file's tempo map."""
    data = Path(path).read_bytes()
    division = struct.unpack_from('>H', data, 12)[0]
    tempos = [(0, 500000)]
    tracks = []
    length = 0
    pos = 14
    while pos < len(data):
        end = pos + 8 + struct.unpack_from('>I', data, pos + 4)[0]
        pos += 8
        tick, status, name, events = 0, 0, '', []

        def varlen():
            nonlocal pos
            value = 0
            while True:
                b = data[pos]
                pos += 1
                value = (value << 7) | (b & 0x7F)
                if not b & 0x80:
                    return value

        while pos < end:
            tick += varlen()
            if data[pos] == 0xFF:
                meta = data[pos + 1]
                pos += 2
                size = varlen()
                if meta == 0x03:
                    name = data[pos:pos + size].decode('latin-1')
                if meta == 0x51:
                    tempos.append((tick, int.from_bytes(data[pos:pos + 3], 'big')))
                pos += size
                continue
            if data[pos] & 0x80:
                status = data[pos]
                pos += 1
            count = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
            events.append((tick, status & 0xF0) + tuple(data[pos:pos + count]))
            pos += count
        tracks.append((name, events))
        length = max(length, tick)

    def frame(t):
        seconds, at, tempo = 0.0, 0, 500000
        for when, value in sorted(tempos, key=lambda tempo: tempo[0]):
            if when > t:
                break
            seconds += (when - at) * tempo / division / 1e6
            at, tempo = when, value
        return round((seconds + (t - at) * tempo / division / 1e6) / FRAME_SECONDS)

    return [(name, [(frame(e[0]),) + e[1:] for e in events]) for name, events in tracks], frame(length)


def midi_frames(events, frames):
    """Returns, for each frame once its events are done: (True if a note started, the sounding note (key, bank,
    program) or None, bend in semitones, CC10, CC11, CC91)."""
    out = []
    i = 0
    bank = program = cc10 = cc11 = cc91 = 0
    bend, bend_range, rpn = 8192, 2, (127, 127)
    note = None
    for f in range(frames):
        started = False
        while i < len(events) and events[i][0] == f:
            _, kind, *args = events[i]
            i += 1
            if kind == 0xB0:
                cc, value = args
                if cc == 0:
                    bank = value
                elif cc == 10:
                    cc10 = value
                elif cc == 11:
                    cc11 = value
                elif cc == 91:
                    cc91 = value
                elif cc == 101:
                    rpn = (value, rpn[1])
                elif cc == 100:
                    rpn = (rpn[0], value)
                elif cc == 6 and rpn == (0, 0):
                    bend_range = value
            elif kind == 0xC0:
                program = args[0]
            elif kind == 0xE0:
                bend = args[0] | (args[1] << 7)
            elif kind == 0x90 and args[1] > 0:
                note = (args[0], bank, program)
                started = True
            elif kind in (0x80, 0x90) and note and note[0] == args[0]:
                note = None
        out.append((started, note, (bend - 8192) / 8192 * bend_range, cc10, cc11, cc91))
    return out


def midi_levels(cc10, cc11):
    """Returns the per-side levels (full scale 128) that CC10 and CC11 stand for. The function inverts agbsf2's
    LevelsToControllers."""
    amplitude = (cc11 / 127) ** 2 * math.sqrt(2) * 127 / 128
    angle = cc10 / 127 * math.pi / 2
    return amplitude * math.cos(angle) * 128, amplitude * math.sin(angle) * 128


class Psg:
    """Game Boy APU registers and wave RAM after the driver's writes."""

    def __init__(self):
        self.reg = {}
        self.wave_ram = [bytearray(16), bytearray(16)]
        self.on = [False] * 4

    def half(self, address):
        return self.reg.get(address, 0) | (self.reg.get(address + 1, 0) << 8)

    def write(self, address, size, value, triggers, writes=None):
        """Applies a register write. Adds restarted channels and wave bank switches ('wave') to triggers.
        If writes is supplied, adds each channel whose registers were written."""
        for i in range(size):
            a, b = address + i, (value >> (8 * i)) & 0xFF
            old = self.reg.get(a, 0)
            self.reg[a] = b
            for channel, (first, end) in enumerate(CHANNEL_REGISTERS):
                if writes is not None and first <= a < end:
                    writes.add(channel)
            if 0x04000090 <= a < 0x040000A0:
                # The CPU writes the bank that isn't playing.
                self.wave_ram[1 - self.wave_bank()][a - 0x04000090] = b
            elif a == 0x04000070 and (old ^ b) & 0x40:
                triggers.add('wave')
            elif a in (0x04000065, 0x0400006D, 0x0400007D) and b & 0x80:
                triggers.add({0x04000065: 0, 0x0400006D: 1, 0x0400007D: 3}[a])

    def wave_bank(self):
        return (self.reg.get(0x04000070, 0) >> 6) & 1

    def wave_volume(self):
        """Returns the wave channel's volume fraction: forced 75%, or the volume code's value from 0 to 100%."""
        nr32 = self.half(0x04000072)
        return 0.75 if nr32 & 0x8000 else [0, 1, 0.5, 0.25][(nr32 >> 13) & 3]

    def side_scales(self):
        """Returns the scale of a PSG channel's level on the left and on the right. The scale is relative to the
        master volume 7 and the 100% PSG share that agbsf2 bases its levels on."""
        nr50 = self.reg.get(0x04000080, 0)
        ratio = PSG_RATIOS[self.reg.get(0x04000082, 0) & 3]
        return ((nr50 >> 4 & 7) + 1) / 8 * ratio, ((nr50 & 7) + 1) / 8 * ratio

    def update(self, triggers):
        """Updates the active channels after a frame's register writes."""
        for channel, envelope in ((0, 0x04000062), (1, 0x04000068), (3, 0x04000078)):
            dac = self.half(envelope) & 0xF800 != 0
            self.on[channel] = dac and (self.on[channel] or channel in triggers)
        self.on[2] = bool(self.reg.get(0x04000070, 0) & 0x80) and self.wave_volume() > 0


def run_driver(rom, song, frames):
    """Runs the driver for `frames` frames of `song`. Returns per frame: the voice records, the echo bus records, the
    voices that started a note, and the PSG state with the channels triggered; the mixer rate, or None for a driver
    whose FIFOs play at their voices' rates; and the game's addresses and settings in driver_emu.py."""
    emu = DriverEmulator(rom)
    addr = emu.addr
    voices, record_size = addr.voices, addr.voice_size
    starts = set()

    def on_voice_write(uc, access, address, size, value, user):
        # A note start writes the voice's sample pointer. A driver whose interrupt routines mix the voices writes 0
        # there to stop one.
        if (address - voices) % record_size == 0 and (addr.fifo_irqs is None or value):
            starts.add((address - voices) // record_size)

    count = emu.addr.voice_count
    emu.uc.hook_add(UC_HOOK_MEM_WRITE, on_voice_write, begin=voices, end=voices + count * record_size - 1)
    psg = Psg()
    for _, address, size, value in emu.io_writes:
        psg.write(address, size, value, set())
    emu.io_writes.clear()

    emu.play(song)
    starts.clear()
    out = []
    for _ in range(frames):
        emu.frame()
        triggers, writes = set(), set()
        for _, address, size, value in emu.io_writes:
            psg.write(address, size, value, triggers, writes)
        emu.io_writes.clear()
        psg.update(triggers)
        records = [bytes(emu.uc.mem_read(voices + v * record_size, record_size)) for v in range(count)]
        buses = bytes(emu.uc.mem_read(addr.echo_buses, 3 * 20)) if addr.echo_buses else None
        periods = struct.unpack('<HxxH', emu.uc.mem_read(addr.fifo_periods, 6)) if addr.fifo_periods else None
        wave = (bytes(psg.wave_ram[psg.wave_bank()]), psg.half(0x04000074))
        state = dict(on=list(psg.on), nr51=psg.reg.get(0x04000081, 0), scales=psg.side_scales(), wave=wave,
                     wave_volume=psg.wave_volume(), writes=writes, periods=periods,
                     squares=[(psg.half(0x04000062), psg.half(0x04000064)),
                              (psg.half(0x04000068), psg.half(0x0400006C))],
                     noise=(psg.half(0x04000078), psg.half(0x0400007C)))
        out.append((records, buses, set(starts), state, triggers))
        starts.clear()
    return out, emu.mix_rate() if addr.fifo_irqs is None else None, addr


@functools.lru_cache(maxsize=None)
def noise_samples(nr43):
    """Returns the noise sample that agbsf2 writes for an NR43 value, and the sample's loop. The sample comes from an
    integer model of the LFSR at 32768 Hz."""
    shift, ratio, narrow = nr43 >> 4, nr43 & 7, nr43 & 8
    if shift >= 14:
        points = [0] * 64
    else:
        # Each output point advances the LFSR 16 / den steps.
        den = (2 * ratio if ratio else 1) << shift
        length = min((127 if narrow else 32767) * den // math.gcd(den, 16), 65536)
        lfsr, done, points = 0x7FFF, 0, []
        for k in range(length):
            while done < k * 16 // den:
                bit = (lfsr ^ (lfsr >> 1)) & 1
                lfsr = (lfsr >> 1) | (bit << 14)
                if narrow:
                    lfsr = (lfsr & ~0x40) | (bit << 6)
                done += 1
            points.append(-32767 if lfsr & 1 else 32767)
    return tuple(points[-8:] + points + points[:8]), (8, 8 + len(points))


def register_step_cents(x):
    """Returns how far one step of an 11-bit GB frequency register moves the pitch at x, in cents."""
    return 1200 * math.log2((2048 - x) / (2047 - x))


class Report:
    """The differences found, and the largest pitch and level deviations."""

    def __init__(self):
        self.differences = []
        self.worst = {}

    def differ(self, where, text):
        self.differences.append('%s: %s' % (where, text))

    def deviation(self, what, value, where):
        if what not in self.worst or abs(value) > abs(self.worst[what][0]):
            self.worst[what] = (value, where)

    def level(self, what, got, want, where):
        """Checks a MIDI level against the driver's; 0 has to stay 0."""
        if want == 0:
            if got > 0.5:
                self.differ(where, '%s level %.1f where the driver has 0' % (what, got))
            return
        error = 20 * math.log10(max(got, 1e-9) / want)
        self.deviation(what + ' level (dB)', error, where)
        if abs(error) > LEVEL_TOLERANCE_DB:
            self.differ(where, '%s level %.1f, driver %g (%+.1f dB)' % (what, got, want, error))


def sample_header(rom, sample_table, index, addr):
    """Returns the address of sample `index`'s data, its length and its loop start (-1 for none)."""
    if addr.sample_entry_size == 8:
        data, word = struct.unpack_from('<II', rom, sample_table - ROM_BASE + 8 * index)
        return data, (word >> 20) * 16, -1
    header = struct.unpack_from('<I', rom, sample_table - ROM_BASE + 4 * index)[0]
    _, length, loop = struct.unpack_from('<Iii', rom, header - ROM_BASE)
    return header + 12, length, loop


def zone(soundfont, note, report, where):
    """Returns the zone for a MIDI note (key, bank, program). Reports a missing zone and returns None."""
    z = soundfont.zone(note[1], note[2], note[0])
    if z is None:
        report.differ(where, 'no preset zone plays key %d on bank %d program %d' % note)
    return z


def check_sample_track(rom, sample_table, soundfont, voice, midi, driver, rate, addr, report, name):
    """Checks the MIDI track that plays DirectSound voice `voice`, frame by frame. Returns how many notes it plays."""
    notes = 0
    for f, ((started, note, bend, cc10, cc11, cc91), (records, buses, starts, state, _)) in enumerate(
            zip(midi, driver)):
        where = '%s frame %d' % (name, f)
        record = records[voice]
        pointer = struct.unpack_from('<I', record)[0]
        flags = record[addr.voice_flags_at]
        playing = bool(flags & 0x80)
        if started != (voice in starts):
            report.differ(where, 'note-on %s, driver starts a note %s' % (started, voice in starts))
        if (note is not None) != playing:
            report.differ(where, 'note sounding %s, driver voice playing %s' % (note is not None, playing))

        # CC91 stands in for the driver's echo: it's bus 0's target feedback while the voice is routed to bus 0. The
        # mixer takes a voice routed to buses 0 and 1 as routed to bus 1 alone, and some drivers hand it only some
        # voices of bus 0. A driver without echo has no buses.
        send = 0
        if buses is not None:
            bus0, bus1 = (struct.unpack_from('<I', buses, bus * 20 + 0x0C)[0] >> ((11 - voice) * 2) & 1
                          for bus in (0, 1))
            wet = bus0 and not bus1 and addr.echo_bus0_voices >> voice & 1
            send = min(127, buses[0x10]) if wet else 0
        if cc91 != send:
            report.differ(where, 'CC91 %d, driver echo send %d' % (cc91, send))

        if note is None or not playing:
            continue
        z = zone(soundfont, note, report, where)
        if z is None:
            continue
        if started:
            notes += 1
            index = int(z['name'].split()[1])
            data, length, loop = sample_header(rom, sample_table, index, addr)
            if data != pointer or index & 0xFF != record[addr.voice_sample_at]:
                report.differ(where, '%s, driver plays the sample at %08X' % (z['name'], pointer))
            pcm = [((b ^ 0x80) - 0x80) * 256 for b in rom[data - ROM_BASE:data - ROM_BASE + length]]
            if list(z['pcm'][:length]) != pcm:
                report.differ(where, '%s holds different data from the ROM' % z['name'])
            looped = 0 <= loop < length
            flagged = bool(flags & addr.loop_flag) if addr.loop_flag else looped
            if looped != z['looped'] or looped != flagged or (looped and z['loop'] != (loop, length)):
                report.differ(where, '%s loops differently from the driver' % z['name'])

        # Mixer pitch uses integer steps; allow an error of two steps. A FIFO plays at its timer rate. agbsf2 rounds
        # that rate to the nearest 1/32 semitone.
        if state['periods'] is not None:
            period = state['periods'][voice >> 1] or 0x10000
            driver_rate, tolerance, unit = 16777216 / period, 2.0, 'period %d' % period
        else:
            step = struct.unpack_from('<H', record, 8)[0]
            driver_rate, tolerance, unit = rate * step / 4096, 1200 * math.log2(1 + 2 / step) + 0.5, 'step %d' % step
        cents = 1200 * math.log2(z['rate'] * 2 ** ((note[0] - z['root'] + bend) / 12) / driver_rate)
        report.deviation('sample pitch (cents)', cents, where)
        if abs(cents) > tolerance:
            report.differ(where, 'pitch %+.1f cents from the driver\'s (%s)' % (cents, unit))
        left, right = midi_levels(cc10, cc11)
        driver_left, driver_right = addr.voice_levels(record)
        report.level('sample left', left, driver_left, where)
        report.level('sample right', right, driver_right, where)
    return notes


def check_psg_track(soundfont, channel, midi, driver, addr, report, name):
    """Checks the MIDI track that plays PSG channel `channel`, frame by frame. Returns how many notes it plays."""
    # A driver that reloads the wave RAM for a pitch change switches banks without starting a note. Its bank switches
    # only have to start a MIDI note where they change the image, and the image is checked on every frame instead.
    reloads = channel == 2 and addr.wave_reloads_on_pitch
    notes = 0
    was_on = False
    for f, ((started, note, bend, cc10, cc11, cc91), (_, _, _, state, triggers)) in enumerate(zip(midi, driver)):
        where = '%s frame %d' % (name, f)
        on = state['on'][channel]
        triggered = on and ('wave' if channel == 2 else channel) in triggers
        if addr.psg_restarts_on_writes:
            # Square channels restart on every write, including pitch changes. The wave channel restarts only for
            # a new wave. Allow a note on any write; require one when a silent channel starts, the wave changes or
            # the noise restarts.
            wrote = on and (channel in state['writes'] or triggered)
            needed = on and (not was_on or (channel >= 2 and triggered))
            if started and not wrote:
                report.differ(where, 'note-on, but the driver doesn\'t write the channel')
            if needed and not started:
                report.differ(where, 'no note-on where the driver starts the channel')
        elif started != triggered and not (reloads and triggered):
            report.differ(where, 'note-on %s, driver triggers the channel %s' % (started, triggered))
        was_on = on
        if (note is not None) != on:
            report.differ(where, 'note sounding %s, driver channel on %s' % (note is not None, on))
        if cc91 != 0:
            report.differ(where, 'CC91 %d, but the driver\'s echo only takes DirectSound voices' % cc91)
        if note is None or not on:
            continue
        z = zone(soundfont, note, report, where)
        if z is None:
            continue
        if started:
            notes += 1

        nr51 = state['nr51']
        sides = (bool(nr51 >> (channel + 4) & 1), bool(nr51 >> channel & 1))
        left, right = midi_levels(cc10, cc11)
        if (left > 0.5, right > 0.5) != sides:
            report.differ(where, 'plays on (left, right) %s, driver %s' % ((left > 0.5, right > 0.5), sides))

        # A PSG channel at volume v plays as loud as a DirectSound voice at level 2v, at the master volume and PSG
        # share that the driver sets. Other settings scale that.
        left_scale, right_scale = state['scales']
        if channel == 2:
            level = WAVE_LEVEL * state['wave_volume']
            report.level('wave left', left, level * left_scale if sides[0] else 0, where)
            report.level('wave right', right, level * right_scale if sides[1] else 0, where)
            ram, nr34 = state['wave']
            steps = [s for b in ram for s in (b >> 4, b & 15)]
            mean = sum(steps) / 32
            # 4 points per step, rounded half away from zero as std::lround does.
            points = [int(abs((s - mean) / 15.0 * 32767) + 0.5) * (1 if s >= mean else -1) for s in steps]
            cycle = [p for p in points for _ in range(4)]
            if (started or reloads) and (list(z['pcm']) != cycle * 3 or z['loop'] != (128, 256)):
                report.differ(where, '%s differs from the driver\'s wave RAM %s' % (z['name'], ram.hex()))
            x, period_points = nr34 & 0x7FF, 128
            driver_hz = 65536 / (2048 - x)
        else:
            envelope, frequency = state['noise'] if channel == 3 else state['squares'][channel]
            volume = envelope >> 12
            report.level('PSG left', left, 2 * volume * left_scale if sides[0] else 0, where)
            report.level('PSG right', right, 2 * volume * right_scale if sides[1] else 0, where)
            if channel == 3:
                nr43 = frequency & 0xFF
                if z['name'] != 'Noise %02X' % nr43:
                    report.differ(where, 'plays %s, driver\'s noise setting %02X' % (z['name'], nr43))
                elif started and (z['pcm'], z['loop']) != noise_samples(nr43):
                    report.differ(where, '%s differs from the LFSR model' % z['name'])
                continue
            duty = envelope >> 6 & 3
            pattern = [32767 if DUTY[duty] >> (7 - i // 8) & 1 else -32767 for i in range(64)]
            if z['name'] != 'Square ' + DUTY_NAMES[duty]:
                report.differ(where, 'plays %s, driver\'s duty is %s' % (z['name'], DUTY_NAMES[duty]))
            elif started and (list(z['pcm']), z['loop']) != (pattern * 3, (64, 128)):
                report.differ(where, '%s differs from the duty pattern' % z['name'])
            x, period_points = frequency & 0x7FF, 64
            driver_hz = 131072 / (2048 - x)
        midi_hz = z['rate'] / period_points * 2 ** ((note[0] - z['root'] + bend) / 12)
        cents = 1200 * math.log2(midi_hz / driver_hz)
        report.deviation('PSG pitch beyond one register step (cents)', max(0.0, abs(cents) - register_step_cents(x)),
                         where)
        report.deviation('PSG pitch (cents)', cents, where)
        if abs(cents) > register_step_cents(x) + 0.5:
            report.differ(where, 'pitch %+.1f cents from the driver\'s (register %d)' % (cents, x))
    return notes


def check_silent(name, driver, addr, report):
    """Checks that the driver plays nothing on the voice or channel of track `name`, which has no notes in the MIDI
    file."""
    for f, (records, _, starts, state, _) in enumerate(driver):
        if name in PSG_TRACKS:
            playing = state['on'][PSG_TRACKS[name]]
        else:
            voice = int(name.split()[1])
            playing = voice in starts or bool(records[voice][addr.voice_flags_at] & 0x80)
        if playing:
            report.differ('%s frame %d' % (name, f), 'the driver plays a note, but there\'s no %s track' % name)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba) or a GSF rip of it')
    p.add_argument('agbsf2', metavar='AGBSF2', help="the agbsf2 program, for the sample table's address")
    p.add_argument('folder', metavar='FOLDER', help='the conversion to check')
    p.add_argument('--songs', help='the songs to check, such as 0-22 or 1,4,9 (default: every song that --info '
                                   'lists or FOLDER has a MIDI file for)')
    a = p.parse_args()
    rom = load_rom(a.rom)
    info = subprocess.run([a.agbsf2, '--driver', 'konami', '--info', a.rom], capture_output=True,
                          encoding='utf-8', check=True).stdout
    sample_table = int(re.search(r'sample table 0x([0-9A-F]{8})', info).group(1), 16)
    midi_files = dict(conversion.midi_files(a.folder, 'konami'))
    if not midi_files:
        raise SystemExit('found no MIDI files to check in %s' % a.folder)

    # The songs --info lists, each with its length in frames, which --info gives in minutes and seconds.
    lengths = {int(m.group(1)): round((int(m.group(2)) * 60 + float(m.group(3))) / FRAME_SECONDS)
               for m in re.finditer(r'^\s+(\d+)\s+0x[0-9A-F]{8}\s+\d+\s+(\d+):(\d+\.\d+)', info, re.M)}
    if not lengths:
        raise SystemExit('found no songs in the song list of agbsf2 --info')
    songs = parse_range(a.songs) if a.songs else sorted(set(lengths) | set(midi_files))

    # The same conversion with each event on the driver's frame.
    temporary = tempfile.TemporaryDirectory()
    on_frames = conversion.frame_timed(a.agbsf2, a.rom, 'konami', temporary.name, a.songs)

    total = Report()
    checked = 0
    for song in songs:
        report = Report()
        midi_path = midi_files.get(song)
        if midi_path is not None:
            framed = on_frames.get(song)
            if framed is None:
                report.differ('song %d' % song, 'the conversion with --frame-timing has no MIDI file')
                midi_path = None
            else:
                problems, worst = conversion.compare_timing(midi_path, framed, FRAME_SECONDS, MAX_SHIFT)
                if midi_path.with_suffix('.sf2').read_bytes() != framed.with_suffix('.sf2').read_bytes():
                    problems.append('the SoundFonts differ')
                for problem in problems:
                    report.differ('song %d on the beat' % song, problem)
                report.deviation('move onto the beat (frames)', worst, '')
        if midi_path is not None:
            soundfont = SoundFont(midi_path.with_suffix('.sf2'))
            tracks, frames = read_midi(framed)  # the song ends, and its notes are released, on the last tick
            driver, rate, addr = run_driver(rom, song, frames)
            track_names = list(PSG_TRACKS) + ['Voice %d' % v for v in range(addr.voice_count)]
            present = dict(tracks[1:])
            # The tracks are checked by name, so notes on the first track, on a track of another name, or on one whose
            # name a later track has too, would go unchecked.
            for i, (name, events) in enumerate(tracks):
                if i and name in track_names and present[name] is events:
                    continue
                if any(e[1] == 0x90 and e[3] for e in events):
                    why = ('a later track has the same name' if i and name in track_names else
                           'it stands for none of the driver\'s voices and channels')
                    report.differ('song %d' % song, 'MIDI track %d (%s) has notes, but %s' % (i, name, why))
            notes = 0
            for name in track_names:
                if name not in present:
                    check_silent(name, driver, addr, report)
                    continue
                midi = midi_frames(present[name], frames)
                if name in PSG_TRACKS:
                    notes += check_psg_track(soundfont, PSG_TRACKS[name], midi, driver, addr, report, name)
                else:
                    voice = int(name.split()[1])
                    notes += check_sample_track(rom, sample_table, soundfont, voice, midi, driver, rate, addr,
                                                report, name)
            played = '%5d notes' % notes
        elif song in lengths:
            # agbsf2 writes no files for a song that plays no notes.
            frames = max(1, lengths[song])
            driver, _, addr = run_driver(rom, song, frames)
            track_names = list(PSG_TRACKS) + ['Voice %d' % v for v in range(addr.voice_count)]
            for name in track_names:
                check_silent(name, driver, addr, report)
            played = 'no MIDI file'
        else:
            continue

        checked += 1
        print('song %2d: %5d frames, %s, %d differences' % (song, frames, played, len(report.differences)),
              flush=True)
        for d in report.differences[:10]:
            print('    ' + d)
        total.differences += report.differences
        for what, (value, where) in report.worst.items():
            total.deviation(what, value, 'song %d %s' % (song, where))

    temporary.cleanup()
    if not checked:
        raise SystemExit('found none of the songs to check')
    print()
    for what, (value, where) in sorted(total.worst.items()):
        print('largest %s: %+.2f (%s)' % (what, value, where))
    raise SystemExit(1 if total.differences else 0)


if __name__ == '__main__':
    main()
