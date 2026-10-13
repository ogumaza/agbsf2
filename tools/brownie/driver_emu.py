#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Brownie Brown's GBA sound driver under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the driver. It calls the driver's init, its request routine
and its per-frame routine (no BIOS, no video), and the routines that the game's DMA 1 and DMA 2 interrupts call. In the
Sword of Mana revision, it copies the driver's mixer to IWRAM and runs it once for each 16 points the FIFOs play. In
the Magical Vacation revision, each FIFO plays a sample at the rate its timer sets, and a FIFO that holds 16 points or
fewer after playing one asks for more, which runs the interrupt routine that gives it the sample's next 4 points. It
captures what the driver does:

    trace       after each frame, the frame's writes to the sound registers (w), each sound channel's record that's
                in use (c), each voice that isn't clear (v), the mixer's voices or the FIFOs' samples, and the driver's
                other variables when they change (g), the records and variables in hex as the driver keeps them
    render      the sound: the PSG through psg_model.py, and the FIFOs' 8-bit samples

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM render SONG FRAMES OUT.wav [--channels 0,8]

The routine and RAM addresses are chosen by the ROM's game code. Those of Sword of Mana (AVSE) and Magical Vacation
(AMVJ) are built in. Another game needs its own values; see GAMES and docs/brownie.md.
"""
import argparse
import collections
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MEM_WRITE_UNMAPPED, \
    UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R7, \
    UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and psg_model.py, in tools/
import psg_model
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
FRAME_CYCLES = 280896  # CPU cycles from one VBlank to the next
CHANNEL_SIZE = 0x38
VOICE_SIZE = 0x10
POINTS = 16            # points of each FIFO that the Sword of Mana mixer makes at a time
DMA1CNT_H = 0x040000C6
TM0CNT_L = 0x04000100
TM0CNT_H = 0x04000102
SOUNDCNT_H = 0x04000082
IE = 0x04000200
SOUND_REGISTERS = (0x04000060, 0x040000A0)  # the PSG's registers, the control registers and the wave RAM
FIFO_REQUEST = 16      # a FIFO that holds this many points or fewer after playing one asks for more
FIFO_SIZE = 32


class SwordOfMana:
    """AVSE (Sword of Mana): 13 channels, whose samples a mixer plays on 5 voices."""
    channel_count = 13     # 0-3 the music's PSG, 4-7 the sound effects' PSG, 8-12 the music's samples
    voice_count = 5        # the mixer's voices, one for each sample channel
    init = 0x080A1834      # the driver's init, in ARM
    request = 0x080A16EC   # the routine that queues a sound: r0 = its number, in ARM
    frame = 0x080A1900     # the per-frame routine, in ARM, which the VBlank handler calls
    mixer = 0x03003150     # the DMA 1 and 2 interrupt routine, in ARM, which the init's copies put in IWRAM
    copies = ((0x080A0B74, 0x03003150, 0x600), (0x080A1040, 0x03003750, 0x600))  # the mixer, from the ROM to IWRAM
    channels = 0x03002ACC  # the sound channels' records
    voices = 0x03002E0C    # the mixer's voices
    fifos = 0x03002E6C     # 16 points for FIFO A (the right side), then 16 for FIFO B (the left)
    variables = ((0x03002DDC, 0x03002E0C),)  # the request queue, the fade, the mixer's mode and SOUNDCNT_L's copy


class MagicalVacation:
    """AMVJ (Magical Vacation): 12 channels, whose samples play on the FIFOs, a sample on each."""
    channel_count = 12     # 0-3 the music's PSG, 4-7 the sound effects', 8-9 the music's samples, 10-11 the effects'
    voice_count = 2        # FIFO A's sample, which Timer 0 plays, and FIFO B's, which Timer 1 plays
    init = 0x0805BF3C
    request = 0x0805BEAC
    frame = 0x0805C2C8
    interrupts = (0x0805BFD8, 0x0805C150)  # the DMA 1 and DMA 2 interrupt routines, which feed FIFO A and FIFO B
    copies = ()
    channels = 0x02000EF0
    voices = 0x0200119C    # each FIFO's sample: the next point, the end, the loop and the volume
    variables = ((0x02001190, 0x0200119C), (0x020011BC, 0x020011DC))  # the queue; SOUNDCNT_L's copy and the fade


GAMES = {b'AVSE': SwordOfMana, b'AMVJ': MagicalVacation}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class Fifo:
    """A FIFO of the Magical Vacation revision, and the timer that plays it."""

    def __init__(self):
        self.running = False
        self.next = 0                       # the CPU cycle of the timer's next overflow
        self.points = collections.deque()   # the points it holds
        self.owner = None                   # the channel whose note started the timer
        self.played = []                    # (cycle, point) for each point it played


class DriverEmulator:
    def __init__(self, rom):
        addr = addresses_for(rom)
        self.addr = addr
        self.fifo_mode = addr is MagicalVacation
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_write(0x04000088, struct.pack('<H', 0x0200))  # SOUNDBIAS as the BIOS leaves it
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=SOUND_REGISTERS[0], end=SOUND_REGISTERS[1] - 1)
        self.uc = uc
        self.writes = []
        self.fifo = [Fifo(), Fifo()]
        self.now = 0  # the CPU cycle that the code runs at
        if self.fifo_mode:
            uc.hook_add(UC_HOOK_MEM_WRITE, self._fifo_write, begin=0x040000A0, end=0x040000A7)
            uc.hook_add(UC_HOOK_MEM_WRITE, self._timer_write, begin=TM0CNT_L, end=0x04000107)

        # The game's init copies the Sword of Mana mixer to IWRAM with DMA 3 after the driver's init.
        self.call(addr.init, thumb=False)
        for src, dst, size in addr.copies:
            uc.mem_write(dst, bytes(rom[src - ROM_BASE:src - ROM_BASE + size]))
        self.mixed = 0          # the mixer's runs since the FIFOs' DMA started
        self.dma_frame = None   # the frame in which the DMA started, or None while it's stopped
        self.frames = 0

    def _svc(self, uc, intno, user):
        raise SystemExit('the driver made a BIOS call at %08x' % uc.reg_read(UC_ARM_REG_PC))

    def _unmapped(self, uc, access, address, size, value, user):
        # The PSG's rest on a sample channel writes past the end of the driver's table of PSG registers, to an address
        # that the GBA takes as ROM, which ignores it.
        if access == UC_MEM_WRITE_UNMAPPED and address >= 0x10000000:
            uc.mem_map(address & ~0xFFF, 0x1000)
            return True
        raise SystemExit('the driver accessed unmapped memory at %08x' % address)

    def _io_write(self, uc, access, address, size, value, user):
        value &= (1 << (8 * size)) - 1
        self.writes.append((address, size, value))

        # SOUNDCNT_H's bits 11 and 15 empty FIFO A and FIFO B.
        if self.fifo_mode and address == SOUNDCNT_H and size == 2:
            for x, bit in enumerate((0x0800, 0x8000)):
                if value & bit:
                    self.fifo[x].points.clear()

    def _fifo_write(self, uc, access, address, size, value, user):
        fifo = self.fifo[(address - 0x040000A0) // 4]
        for i in range(size):
            if len(fifo.points) < FIFO_SIZE:
                fifo.points.append(struct.unpack('<b', bytes([(value >> (8 * i)) & 0xFF]))[0])

    def _timer_write(self, uc, access, address, size, value, user):
        # A timer that starts runs from its reload value, and its first overflow comes a period later.
        if address not in (TM0CNT_H, TM0CNT_H + 4) or size != 2:
            return
        x = (address - TM0CNT_H) // 4
        fifo = self.fifo[x]
        if value & 0x80 and not fifo.running:
            fifo.running = True
            fifo.next = self.now + self.period(x)
            fifo.owner = uc.reg_read(UC_ARM_REG_R7)  # the per-frame routine's channel
        elif not value & 0x80:
            fifo.running = False

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)

        # The PSG's rest writes whatever r5 holds to the channel's frequency register, which at the start of the
        # per-frame routine is what the code the VBlank interrupted left there. Each call starts with 0, as the model
        # does.
        for r in range(13):
            uc.reg_write(UC_ARM_REG_R0 + r, args[r] if r < len(args) else 0)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def u16(self, address):
        return struct.unpack('<H', self.uc.mem_read(address, 2))[0]

    def period(self, x):
        """Returns the CPU cycles between overflows of the timer that plays FIFO x."""
        return 0x10000 - self.u16(TM0CNT_L + 4 * x)

    def play(self, sound):
        """Queues `sound`, which the driver starts at the end of the next frame."""
        self.call(self.addr.request, (sound,), thumb=False)

    def mix_cycles(self):
        """Returns the CPU cycles between the Sword of Mana mixer's runs: 16 points at the rate Timer 0 sets."""
        return POINTS * (0x10000 - self.u16(TM0CNT_L))

    def frame(self):
        """Runs a frame: the per-frame routine, then the FIFOs until the next frame. Returns the frame's register
        writes, and the points of FIFO A and FIFO B. In the Sword of Mana revision, the mixer runs once for each 16
        points the FIFOs play before the next frame, while Timer 0 and the DMA run, counting from the frame they started
        in, and FIFO A plays the right side and FIFO B the left; afterwards, mixing says whether the mixer ran. In the
        Magical Vacation revision, the FIFOs play at their timers' rates, and each returns its points as (cycle,
        point); afterwards, fifos_before holds each FIFO's (running, period, owner) as the routine left them. Either
        way, voices_before_mix holds the voices, or the FIFOs' samples, as the routine left them."""
        self.writes = []
        self.now = self.frames * FRAME_CYCLES
        self.call(self.addr.frame, thumb=False)
        writes = self.writes
        self.voices_before_mix = [self.voice(v) for v in range(self.addr.voice_count)]
        if self.fifo_mode:
            self.fifos_before = [(f.running, self.period(x), f.owner) for x, f in enumerate(self.fifo)]
            points = [self._play_fifo(x, (self.frames + 1) * FRAME_CYCLES) for x in range(2)]
            self.frames += 1
            return writes, points[0], points[1]

        right, left = [], []
        self.mixing = bool(self.u16(DMA1CNT_H) & 0x8000 and self.u16(TM0CNT_H) & 0x80)
        if not self.mixing:
            self.dma_frame = None
        else:
            if self.dma_frame is None:
                self.dma_frame, self.mixed = self.frames, 0
            due = (self.frames - self.dma_frame + 1) * FRAME_CYCLES // self.mix_cycles()
            while self.mixed < due:
                self.call(self.addr.mixer, thumb=False)
                self.mixed += 1
                points = bytes(self.uc.mem_read(self.addr.fifos, 2 * POINTS))
                right.append(np.frombuffer(points[:POINTS], dtype=np.int8))
                left.append(np.frombuffer(points[POINTS:], dtype=np.int8))
        self.frames += 1
        empty = np.zeros(0, np.int8)
        return writes, (np.concatenate(right) if right else empty), (np.concatenate(left) if left else empty)

    def _play_fifo(self, x, end):
        """Plays FIFO x until CPU cycle `end`: at each overflow of its timer it plays a point, and if it then holds 16
        or fewer, its DMA asks for more and interrupts, while the DMA and its interrupt are on. Returns the points it
        played as (cycle, point)."""
        fifo = self.fifo[x]
        fifo.played = []
        dma = (DMA1CNT_H, 0x040000D2)[x]
        while fifo.running and fifo.next < end:
            self.now = fifo.next
            point = fifo.points.popleft() if fifo.points else 0
            fifo.played.append((fifo.next, point))
            if len(fifo.points) <= FIFO_REQUEST and self.u16(dma) & 0x8000 and self.u16(IE) & (0x200 << x):
                self.call(self.addr.interrupts[x], thumb=False)
            fifo.next += self.period(x)
        return fifo.played

    def channel(self, index):
        return bytes(self.uc.mem_read(self.addr.channels + CHANNEL_SIZE * index, CHANNEL_SIZE))

    def voice(self, index):
        return bytes(self.uc.mem_read(self.addr.voices + VOICE_SIZE * index, VOICE_SIZE))

    def variables(self):
        return b''.join(bytes(self.uc.mem_read(lo, hi - lo)) for lo, hi in self.addr.variables)

    def playing(self):
        """Returns true while a channel is in use or a voice plays."""
        if any(self.channel(c)[0] & 1 for c in range(self.addr.channel_count)):
            return True
        return any(f.running for f in self.fifo) if self.fifo_mode else self.dma_frame is not None


def trace(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    last = None
    for f in range(frames):
        writes, _, _ = emu.frame()
        for address, size, value in writes:
            print('%d w %08x %d %0*x' % (f, address, size, 2 * size, value))
        for c in range(emu.addr.channel_count):
            record = emu.channel(c)
            if record[0]:
                print('%d c%d %s' % (f, c, record.hex()))
        for v in range(emu.addr.voice_count):
            voice = emu.voice(v)
            if any(voice):
                print('%d v%d %s' % (f, v, voice.hex()))
        variables = emu.variables()
        if variables != last:
            print('%d g %s' % (f, variables.hex()))
            last = variables
        if f > 0 and not emu.playing():
            break


def hold(played, start, count, rate, last):
    """Returns `count` points at `rate` from CPU cycle `start` on, each the last point played by then, from (cycle,
    point) pairs in order, with `last` before the first, and the last point played."""
    times = start + np.arange(count) * (16777216 / rate)
    cycles = np.array([c for c, _ in played], dtype=np.float64)
    values = np.array([last] + [p for _, p in played], dtype=np.float64)
    out = values[np.searchsorted(cycles, times, side='right')]
    return out, values[-1]


def render(rom, song, frames, channels=None):
    """Returns the driver's stereo output for the first `frames` frames of `song`, at psg_model.RATE. `channels` lists
    the sound channels to keep: the PSG's 0-3, which the sound effects' 4-7 play on, or the FIFOs' 8 and up, which are
    all kept or all left out."""
    emu = DriverEmulator(rom)
    keep = set(range(emu.addr.channel_count)) if channels is None else set(channels)
    psg_keep = {c & 3 for c in keep if c < 8}
    if len(psg_keep) not in (0, 1, 4):
        raise SystemExit('render keeps all of the PSG channels, one of them or none')
    apu = psg_model.APU(hardware=True)
    for address, size, value in emu.writes:  # the init's, which set the mix and the master volume
        apu.write(address, size, value)
    emu.play(song)
    chunks, acc = [], 0.0
    fifo_start, fifo_points = None, []
    last = [0, 0]  # the point each FIFO last played, in the Magical Vacation revision
    for f in range(frames):
        writes, right, left = emu.frame()
        for address, size, value in writes:
            apu.write(address, size, value)
        acc += psg_model.FRAME_SAMPLES
        n = int(acc)
        acc -= n
        if emu.fifo_mode:
            # Each FIFO plays its points on both sides, at its timer's rate, and holds each until the next.
            both = np.zeros(n)
            for x, points in enumerate((right, left)):
                held, last[x] = hold(points, f * FRAME_CYCLES, n, psg_model.RATE, last[x])
                both += held / 128
            fifo_start = 0
            fifo_points.append(np.stack([both, both], axis=1))
        else:
            if len(right) and fifo_start is None:
                fifo_start = sum(len(c) for c in chunks)
            if fifo_start is not None:
                fifo_points.append(np.stack([left, right], axis=1))
        psg = apu.render(n, None if len(psg_keep) == 4 else next(iter(psg_keep), None))
        if not psg_keep:
            psg[:] = 0
        chunks.append(psg)
    out = np.concatenate(chunks)

    # The Sword of Mana mixer's FIFOs play at 16384 Hz, half the PSG model's rate, from the frame their DMA starts in,
    # at 100%: a point s adds s / 128 on its side. They don't stop and start again within a render.
    if any(c >= 8 for c in keep) and fifo_points:
        fifo = np.concatenate(fifo_points)
        if not emu.fifo_mode:
            fifo = np.repeat(fifo.astype(np.float64) / 128, 2, axis=0)
        m = min(len(out) - fifo_start, len(fifo))
        out[fifo_start:fifo_start + m] += fifo[:m]
    return out


def write_wav(path, stereo, rate):
    data = np.clip(stereo * 32767, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(data.tobytes())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--channels', help='render: only these sound channels, such as 0,8')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames)
        return
    if not a.out:
        p.error('render needs an output file')
    wanted = [int(c) for c in a.channels.split(',')] if a.channels else None
    write_wav(a.out, render(rom, a.song, a.frames, wanted), psg_model.RATE)


if __name__ == '__main__':
    main()
