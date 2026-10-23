#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Quintet's GBA sound driver under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the driver. It calls the game's sound init, play-song
and per-frame routines (no BIOS, no video, no interrupts), and captures what the driver does with the hardware:

    trace       every write to the sound, DMA 1 and 2 and timer 0 and 1 registers, frame by frame, except the bytes a
                note writes into a FIFO before its DMA starts
    channels    each channel's read position and countdown after each frame
    render      the sound: the PSG through psg_model.py, and each FIFO's sample at its timer's rate

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM channels SONG FRAMES
    driver_emu.py ROM render SONG FRAMES OUT.wav [--channels 0,4]

SONG is agbsf2's number for a song. agbsf2 numbers the songs of the music file first, then the sound effects of the
sound effects' file. The script plays a sound effect through the game's routine for sound effects, with no music
playing. The sound effect channels then take the place of the music channels in the channel list.

The routine and RAM addresses are chosen by the ROM's game code. Those of Super Robot Taisen A (ASRJ), R (AJ9J), D
(A6SJ) and J (B6JJ) are built in. Another game needs its own values; see GAMES and docs/quintet.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import (UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM,
                     Uc)
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and psg_model.py, in tools/
import psg_model
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
FIFO_DATA = range(0x040000A0, 0x040000A8)
TRACED = ((0x04000060, 0x040000A8), (0x040000BC, 0x040000D4), (0x04000100, 0x04000108))

# The bits of the sound registers the driver reads back that the hardware returns. SOUNDCNT_H's FIFO reset bits read as
# 0, and so do NR30's bits 0-4.
READ_MASKS = {0x04000082: 0x770F, 0x04000070: 0xE0}


class Addresses:
    """ASRJ (Super Robot Taisen A)."""
    sound_init = 0x080034F0  # the game's sound init: the driver's init, then the tables it loads from the game's files
    play = 0x08003550        # the game's play-song routine: r0 = song
    play_effect = 0x08003594  # the game's routine for sound effects: r0 = the sound effect
    frame = 0x08003544       # the game's per-frame sound routine
    channels = 0x087F0F60    # the table of pointers to the 6 music channel records
    effect_channels = 0x087F0F78  # the table of pointers to the 6 sound effect channel records
    songs = 38               # the number of songs in the music file. agbsf2 numbers the sound effects after them.
    countdown = 0x18         # the offset in a channel record of its frames left before its next note


class AddressesA6SJ(Addresses):
    """A6SJ (Super Robot Taisen D)."""
    sound_init = 0x08004A30
    play = 0x08004A94
    play_effect = 0x08004AD8
    frame = 0x08004A84
    channels = 0x087FE2E4
    effect_channels = 0x087FE2FC
    songs = 47


class AddressesAJ9J(Addresses):
    """AJ9J (Super Robot Taisen R)."""
    sound_init = 0x08004AB4
    play = 0x08004B18
    play_effect = 0x08004B5C
    frame = 0x08004B08
    channels = 0x08771BB8
    effect_channels = 0x08771BD0
    songs = 43


class AddressesB6JJ(Addresses):
    """B6JJ (Super Robot Taisen J), with a later revision of the driver."""
    sound_init = 0x080083D0
    play = 0x08008440
    play_effect = 0x08008498
    frame = 0x08008430
    channels = 0x08FB19B0
    effect_channels = 0x08FB19C8
    songs = 52
    countdown = 0x1A


GAMES = {b'ASRJ': Addresses, b'A6SJ': AddressesA6SJ, b'AJ9J': AddressesAJ9J, b'B6JJ': AddressesB6JJ}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class DriverEmulator:
    def __init__(self, rom):
        self.addr = addresses_for(rom)
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_write(0x04000088, struct.pack('<H', 0x0200))  # SOUNDBIAS as the BIOS leaves it
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, rom)
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_READ, self._io_read, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        self.uc = uc
        self.rom = rom
        self.writes = []
        self.channel_records = self.records(self.addr.channels)
        self.call(self.addr.sound_init)
        self.init_writes = self.writes  # the sound init's writes. A sound effect plays on from them.

    def records(self, table):
        """Returns the addresses of the 6 channel records that the table at `table` points to."""
        return struct.unpack('<6I', self.rom[table - ROM_BASE:table - ROM_BASE + 24])

    def _svc(self, uc, intno, user):
        raise SystemExit('the driver made a BIOS call')

    def _unmapped(self, uc, access, address, size, value, user):
        raise SystemExit('the driver accessed unmapped memory at %08x' % address)

    def _io_write(self, uc, access, address, size, value, user):
        if address in FIFO_DATA:
            return
        if any(lo <= address < hi for lo, hi in TRACED):
            self.writes.append((address, size, value & ((1 << (8 * size)) - 1)))

    def _io_read(self, uc, access, address, size, value, user):
        # Mask what's stored before the read sees it, as the register would read on the hardware.
        for register, mask in READ_MASKS.items():
            if address <= register < address + size:
                width = 2 if mask > 0xFF else 1
                stored = int.from_bytes(uc.mem_read(register, width), 'little')
                uc.mem_write(register, (stored & mask).to_bytes(width, 'little'))

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def u16(self, address):
        return struct.unpack('<H', self.uc.mem_read(address, 2))[0]

    def u32(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def play(self, song):
        """Starts `song` at once, or the sound effect that agbsf2 numbers `song`. Returns the register writes that
        starting it made."""
        self.writes = []
        if song >= self.addr.songs:
            self.channel_records = self.records(self.addr.effect_channels)
            self.call(self.addr.play_effect, (song - self.addr.songs,))
        else:
            self.call(self.addr.play, (song,))
        return self.writes

    def frame(self):
        """Runs one frame. Returns its register writes as (address, size, value)."""
        self.writes = []
        self.call(self.addr.frame)
        return self.writes

    def positions(self):
        """Returns each channel's read position, or 0 once it has ended."""
        return [self.u32(c + 4) for c in self.channel_records]

    def countdowns(self):
        """Returns each channel's frames left before it reads its next note."""
        return [self.u16(c + self.addr.countdown) for c in self.channel_records]

    def playing(self):
        return any(self.positions())


def format_write(frame, write):
    address, size, value = write
    return '%d %08x %d %0*x' % (frame, address, size, 2 * size, value)


def trace(rom, song, frames):
    emu = DriverEmulator(rom)
    for w in emu.play(song):
        print(format_write(-1, w))
    for f in range(frames):
        for w in emu.frame():
            print(format_write(f, w))
        if not emu.playing():
            break


def channels(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    for f in range(frames):
        emu.frame()
        print(f, ' '.join('%08x:%d' % pc for pc in zip(emu.positions(), emu.countdowns())))
        if not emu.playing():
            break


class FifoModel:
    """Plays a FIFO's sample from the ROM at its timer's rate, as the driver's DMA and timer writes set it up. A note's
    first 32 bytes go into the FIFO before its DMA starts, so the sample plays from 32 bytes before the DMA's source."""

    def __init__(self, rom, index):
        self.rom = rom
        self.source = 0x040000BC if index == 0 else 0x040000C8
        self.control = self.source + 10
        self.timer = 0x04000100 if index == 0 else 0x04000104
        self.next_start = 0
        self.position = None
        self.step = 0.0

    def write(self, address, value):
        if address == self.source:
            self.next_start = value - 32
        elif address == self.control:
            self.position = float(self.next_start) if value & 0x8000 else None
        elif address == self.timer:
            reload = value & 0xFFFF
            running = (value >> 16) & 0x80
            self.step = 16777216 / (0x10000 - reload) / psg_model.RATE if running else 0.0

    def render(self, count):
        out = np.zeros(count, dtype=np.float32)
        for i in range(count):
            if self.position is None or not self.step:
                break
            offset = int(self.position) - ROM_BASE
            if not 0 <= offset < len(self.rom):
                self.position = None
                break
            out[i] = struct.unpack('<b', self.rom[offset:offset + 1])[0] / 128
            self.position += self.step
        return out


def render(rom, song, frames, channels_wanted=None):
    """Returns the driver's stereo output for the first `frames` frames of `song`, at psg_model.RATE. `channels_wanted`
    lists the channels to keep: 0-3 the PSG's, 4 and 5 the FIFOs."""
    emu = DriverEmulator(rom)
    keep = set(range(6)) if channels_wanted is None else set(channels_wanted)
    psg_keep = keep & {0, 1, 2, 3}
    if len(psg_keep) not in (0, 1, 4):
        raise SystemExit('render keeps all of the PSG channels, one of them or none')
    apu = psg_model.APU()
    fifos = [FifoModel(rom, 0), FifoModel(rom, 1)]
    soundcnt_h = 0
    chunks, acc = [], 0.0
    started = emu.play(song)
    for f in range(frames):
        writes = (started if f == 0 else []) + emu.frame()
        for address, size, value in writes:
            apu.write(address, size, value)
            for fifo in fifos:
                fifo.write(address, value)
            if address == 0x04000082:
                soundcnt_h = value
        acc += psg_model.FRAME_SAMPLES
        n = int(acc)
        acc -= n

        # The PSG channels kept, then each FIFO at 100% or 50%, on the sides SOUNDCNT_H sends it to. The PSG model has
        # to run every frame either way.
        psg = apu.render(n, None if len(psg_keep) == 4 else next(iter(psg_keep), None))
        if not psg_keep:
            psg[:] = 0
        for i, fifo in enumerate(fifos):
            data = fifo.render(n)
            if 4 + i not in keep:
                continue
            level = (1.0 if soundcnt_h & (4 << i) else 0.5)
            if soundcnt_h & (0x200 << (4 * i)):
                psg[:, 0] += data * level
            if soundcnt_h & (0x100 << (4 * i)):
                psg[:, 1] += data * level
        chunks.append(psg)
    return np.concatenate(chunks)


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
    p.add_argument('command', choices=['trace', 'channels', 'render'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--channels', help='render: only these channels (0-5), such as 0,4')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames)
    elif a.command == 'channels':
        channels(rom, a.song, a.frames)
    else:
        if not a.out:
            p.error('render needs an output file')
        wanted = [int(c) for c in a.channels.split(',')] if a.channels else None
        write_wav(a.out, render(rom, a.song, a.frames, wanted), psg_model.RATE)


if __name__ == '__main__':
    main()
