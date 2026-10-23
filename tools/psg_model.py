# SPDX-License-Identifier: MIT

"""Game Boy APU model (as on the GBA) driven by captured register writes.

By default a channel plays at the volume last written to its envelope register, without the hardware's envelope. With
`hardware`, it plays the hardware's envelopes, length counters and square 1's frequency sweep, which the 512 Hz frame
sequencer steps: the length counters at 256 Hz, the sweep at 128 Hz and the envelopes at 64 Hz, counting the CPU's
cycles from the start of the first frame. A channel then takes its envelope's volume when it starts.

Output is normalised like the DirectSound output of konami/driver_emu.py and quintet/driver_emu.py: one PSG channel at
volume v contributes v * 8 * (master + 1) / 4 in mGBA's units, and a FIFO sample s contributes s * 4, so both are
divided by 4 * 128. With `hardware`, the output is also scaled by the PSG's share of the mix that SOUNDCNT_H sets: 25%,
50% or 100%.
"""
import numpy as np

RATE = 32768
FRAME_CYCLES = 280896  # one frame of the GBA's video, in the CPU's cycles
FRAME_SAMPLES = RATE * FRAME_CYCLES / 16777216
DUTY = [0x01, 0x81, 0x87, 0x7E]
CYCLES_PER_SAMPLE = 16777216 // RATE  # 512
SEQUENCER_CYCLES = 32768  # one step of the frame sequencer, at 512 Hz


def new_envelope():
    return dict(vol=0, up=False, period=0, ticks=0)


class APU:
    def __init__(self, hardware=False):
        self.hardware = hardware
        self.reg = {}
        self.square = [dict(duty=0, dac=False, on=False, x=0, phase=0.0, length=0, length_on=False, env=new_envelope())
                       for _ in range(2)]
        self.wave = dict(dac=False, bank=0, volcode=0, force75=False, x=0, on=False, pos=0.0,
                         ram=[bytearray(16), bytearray(16)], length=0, length_on=False)
        self.noise = dict(dac=False, on=False, nr43=0, lfsr=0x7FFF, acc=0.0, length=0, length_on=False,
                          env=new_envelope())
        self.sweep = dict(time=0, down=False, shift=0, shadow=0, ticks=0, on=False)
        self.nr50 = 0x77
        self.nr51 = 0xFF
        self.cycles = 0
        self.step = 0

    def write(self, address, size, value):
        for i in range(size):
            self._write8(address + i, (value >> (8 * i)) & 0xFF)

    def _half(self, base):
        return self.reg.get(base, 0) | (self.reg.get(base + 1, 0) << 8)

    @staticmethod
    def _trigger_envelope(env, nrx2):
        env.update(vol=nrx2 >> 12, up=bool(nrx2 & 0x800), period=(nrx2 >> 8) & 7, ticks=0)

    def _sweep_next(self):
        s = self.sweep
        change = s['shadow'] >> s['shift']
        return s['shadow'] - change if s['down'] else s['shadow'] + change

    def _write8(self, a, b):
        self.reg[a] = b
        if a in (0x04000060, 0x04000061):
            v = self._half(0x04000060)
            self.sweep.update(time=(v >> 4) & 7, down=bool(v & 8), shift=v & 7)
        elif a in (0x04000062, 0x04000063, 0x04000068, 0x04000069):
            ch = 0 if a < 0x04000068 else 1
            v = self._half(0x04000062 if ch == 0 else 0x04000068)
            sq = self.square[ch]
            sq.update(duty=(v >> 6) & 3, dac=(v & 0xF800) != 0)
            if not self.hardware:
                sq['env']['vol'] = v >> 12
            if a & 1 == 0:
                sq['length'] = 64 - (v & 63)
            if not sq['dac']:
                sq['on'] = False
        elif a in (0x04000064, 0x04000065, 0x0400006C, 0x0400006D):
            ch = 0 if a < 0x04000068 else 1
            v = self._half(0x04000064 if ch == 0 else 0x0400006C)
            sq = self.square[ch]
            sq['x'] = v & 0x7FF
            sq['length_on'] = bool(v & 0x4000)
            if a & 1 and v & 0x8000:
                sq['on'] = sq['dac']
                sq['phase'] = 0.0
                if self.hardware:
                    self._trigger_envelope(sq['env'], self._half(0x04000062 if ch == 0 else 0x04000068))
                if sq['length'] == 0:
                    sq['length'] = 64
                if ch == 0:
                    s = self.sweep
                    s.update(shadow=sq['x'], ticks=0, on=bool(s['time'] or s['shift']))
                    if s['on'] and s['shift'] and self._sweep_next() > 0x7FF:
                        sq['on'] = False
        elif a == 0x04000070:
            w = self.wave
            w.update(dac=bool(b & 0x80), bank=(b >> 6) & 1)
            if not w['dac']:
                w['on'] = False
        elif a in (0x04000072, 0x04000073):
            v = self._half(0x04000072)
            self.wave.update(volcode=(v >> 13) & 3, force75=bool(v & 0x8000))
            if a & 1 == 0:
                self.wave['length'] = 256 - (v & 0xFF)
        elif a in (0x04000074, 0x04000075):
            v = self._half(0x04000074)
            w = self.wave
            w['x'] = v & 0x7FF
            w['length_on'] = bool(v & 0x4000)
            if a & 1 and v & 0x8000:
                w['on'] = w['dac']
                w['pos'] = 0.0
                if w['length'] == 0:
                    w['length'] = 256
        elif 0x04000090 <= a < 0x040000A0:
            # The CPU writes the bank that isn't playing.
            self.wave['ram'][1 - self.wave['bank']][a - 0x04000090] = b
        elif a in (0x04000078, 0x04000079):
            v = self._half(0x04000078)
            n = self.noise
            n['dac'] = (v & 0xF800) != 0
            if not self.hardware:
                n['env']['vol'] = v >> 12
            if a & 1 == 0:
                n['length'] = 64 - (v & 63)
            if not n['dac']:
                n['on'] = False
        elif a in (0x0400007C, 0x0400007D):
            v = self._half(0x0400007C)
            n = self.noise
            n['nr43'] = v & 0xFF
            n['length_on'] = bool(v & 0x4000)
            if a & 1 and v & 0x8000:
                n.update(on=n['dac'], lfsr=0x7FFF, acc=0.0)
                if self.hardware:
                    self._trigger_envelope(n['env'], self._half(0x04000078))
                if n['length'] == 0:
                    n['length'] = 64
        elif a == 0x04000080:
            self.nr50 = b
        elif a == 0x04000081:
            self.nr51 = b

    def _sequence(self):
        """Takes one step of the frame sequencer."""
        self.step = (self.step + 1) & 7
        if self.step % 2 == 0:
            for ch in self.square + [self.wave, self.noise]:
                if ch['length_on'] and ch['on']:
                    ch['length'] -= 1
                    if ch['length'] <= 0:
                        ch['on'] = False
        s = self.sweep
        sq = self.square[0]
        if self.step in (2, 6) and s['on'] and s['time']:
            s['ticks'] += 1
            if s['ticks'] >= s['time']:
                s['ticks'] = 0
                value = self._sweep_next()
                if value > 0x7FF:
                    sq['on'] = False
                elif s['shift']:
                    s['shadow'] = sq['x'] = value
                    if self._sweep_next() > 0x7FF:
                        sq['on'] = False
        if self.step == 7:
            for env in (self.square[0]['env'], self.square[1]['env'], self.noise['env']):
                if env['period']:
                    env['ticks'] += 1
                    if env['ticks'] >= env['period']:
                        env['ticks'] = 0
                        env['vol'] = min(env['vol'] + 1, 15) if env['up'] else max(env['vol'] - 1, 0)

    def advance(self, cycles):
        """Moves the frame sequencer on by `cycles` of the CPU's cycles, without rendering. With `hardware`, the
        envelopes, length counters and sweep step as they do in render()."""
        self.cycles += cycles
        while self.hardware and self.cycles >= SEQUENCER_CYCLES:
            self.cycles -= SEQUENCER_CYCLES
            self._sequence()

    def render(self, count, only=None):
        out = np.zeros((count, 2), dtype=np.float32)
        for i in range(count):
            self.cycles += CYCLES_PER_SAMPLE
            while self.hardware and self.cycles >= SEQUENCER_CYCLES:
                self.cycles -= SEQUENCER_CYCLES
                self._sequence()
            vals = [0, 0, 0, 0]
            for c in range(2):
                sq = self.square[c]
                if sq['on']:
                    step = int(sq['phase']) & 7
                    vals[c] = sq['env']['vol'] if (DUTY[sq['duty']] >> (7 - step)) & 1 else 0
                    sq['phase'] += 1048576.0 / (2048 - sq['x']) / RATE
            w = self.wave
            if w['on'] and w['dac']:
                idx = int(w['pos']) & 31
                byte = w['ram'][w['bank']][idx >> 1]
                s = byte >> 4 if idx % 2 == 0 else byte & 15
                vals[2] = s * 3 // 4 if w['force75'] else [0, s, s >> 1, s >> 2][w['volcode']]
                w['pos'] += 2097152.0 / (2048 - w['x']) / RATE
            n = self.noise
            if n['on']:
                vals[3] = n['env']['vol'] if (~n['lfsr'] & 1) else 0
                shift, ratio = n['nr43'] >> 4, n['nr43'] & 7
                if shift < 14:
                    n['acc'] += 524288.0 / (ratio if ratio else 0.5) / (1 << (shift + 1)) / RATE
                while n['acc'] >= 1:
                    n['acc'] -= 1
                    bit = (n['lfsr'] ^ (n['lfsr'] >> 1)) & 1
                    n['lfsr'] = (n['lfsr'] >> 1) | (bit << 14)
                    if n['nr43'] & 8:
                        n['lfsr'] = (n['lfsr'] & ~0x40) | (bit << 6)
            left = right = 0
            for c in range(4):
                if only is not None and c != only:
                    continue
                if (self.nr51 >> (c + 4)) & 1:
                    left += vals[c]
                if (self.nr51 >> c) & 1:
                    right += vals[c]
            out[i, 0] = left * 8 * (((self.nr50 >> 4) & 7) + 1) / 4 / 4 / 128
            out[i, 1] = right * 8 * ((self.nr50 & 7) + 1) / 4 / 4 / 128
        if self.hardware:
            # SOUNDCNT_H's share of the mix for the PSG: 25%, 50% or 100%.
            out *= [0.25, 0.5, 1.0, 1.0][self.reg.get(0x04000082, 2) & 3]
        return out


def render_writes(init_writes, frame_writes, frames, only=None):
    """Returns the APU's stereo output at RATE for `frames` frames. init_writes [(address, size, value)] are applied
    first, and frame_writes [(frame, address, size, value)] at the start of their frame. `only` renders one channel."""
    apu = APU()
    for a, s, v in init_writes:
        apu.write(a, s, v)
    by_frame = {}
    for f, a, s, v in frame_writes:
        by_frame.setdefault(f, []).append((a, s, v))
    chunks, acc = [], 0.0
    for f in range(frames):
        for a, s, v in by_frame.get(f, []):
            apu.write(a, s, v)
        acc += FRAME_SAMPLES
        n = int(acc)
        acc -= n
        chunks.append(apu.render(n, only))
    return np.concatenate(chunks)
