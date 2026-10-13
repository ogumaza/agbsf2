#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run a Rare GBA sound driver's code under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of Rare's driver. It calls the driver's init, request-tune and
per-frame routines directly (no BIOS, no video), and captures what the driver produces:

    trace       every voice's state after each frame, one line per active voice
    render      the mixer's output: 8-bit mono samples at the driver's rate

    driver_emu.py ROM trace TUNE FRAMES
    driver_emu.py ROM render TUNE FRAMES OUT.wav [--channels 0,9]

The routine and RAM addresses are chosen by the ROM's game code. Those of Donkey Kong Country (A5NE), Sabre Wulf
(AWUE), It's Mr. Pants (BPIE), Banjo-Kazooie: Grunty's Revenge (BKZX), Donkey Kong Country 2 (B2DE), Banjo-Pilot (BAJE)
and Donkey Kong Country 3 (BDQE) are built in. Another game needs its own values; see GAMES and docs/rare.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc)
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
DMA_CONTROL = (0x040000B8, 0x040000C4, 0x040000D0, 0x040000DC)  # each DMA channel's count and control registers
SLOT_SIZE = 0x28

# The music volume, out of 128, at which Donkey Kong Country 3 plays most of its tunes. The driver's init sets 128. The
# game sets the volume for each tune after it requests the tune. agbsf2's model plays every tune at this volume.
GAME_MUSIC_VOLUME = 69


class Addresses:
    """A5NE (Donkey Kong Country)."""
    init = 0x08032294           # sound init
    request = 0x08032248        # r0 = tune: starts it on the next frame
    frame = 0x0803237c          # per-frame entry (from the VCount interrupt)
    tune = 0x03001420           # the tune playing, or -1
    note_slots = 0x03001620     # note records of 40 bytes, slots_per_channel for each of the 16 channels
    slots_per_channel = 6
    fx_slots = 0x03003f30       # sound effect records
    fx_count = 5
    buffer_flag = 0x03004000    # double-buffer half currently played by DMA
    buffers = (0x0809b040, 0x0809b038)  # mixer buffer addresses for a zero and nonzero flag
    samples_per_frame = 0x03001414
    channel_volume = 0x03001560  # 16 controller 7 values
    envelope_settings = None     # in BDQE's revision, 4 bytes for each channel from controllers 20-23
    music_volume = None          # in BDQE, the word that holds the music volume


class AddressesAWUE(Addresses):
    """AWUE (Sabre Wulf)."""
    init = 0x0800dbd4
    request = 0x0800db88
    frame = 0x0800dcbc
    tune = 0x03001630
    note_slots = 0x03001830
    fx_slots = 0x03004140
    buffer_flag = 0x03004210
    buffers = (0x08089868, 0x08089860)
    samples_per_frame = 0x03001624
    channel_volume = 0x03001760


class AddressesBPIE(Addresses):
    """BPIE (It's Mr. Pants)."""
    fx_count = 4
    init = 0x0814a4d4
    request = 0x0814a488
    frame = 0x0814a5bc
    tune = 0x03003ae4
    note_slots = 0x03004398
    fx_slots = 0x03005298
    buffer_flag = 0x03003ae0
    buffers = (0x08169e48, 0x08169e40)
    samples_per_frame = 0x02027a58
    channel_volume = 0x03004378


class AddressesBKZX(Addresses):
    """BKZX (Banjo-Kazooie: Grunty's Revenge, Europe)."""
    fx_count = 4
    init = 0x08004c18
    request = 0x08004bcc
    frame = 0x08004d00
    tune = 0x030043ac
    note_slots = 0x03004a60
    fx_slots = 0x03005960
    buffer_flag = 0x030043a8
    buffers = (0x080a0030, 0x080a0028)
    samples_per_frame = 0x0203f4c8
    channel_volume = 0x03004a40


class AddressesB2DE(Addresses):
    """B2DE (Donkey Kong Country 2)."""
    init = 0x08032ab0
    request = 0x08032a64
    frame = 0x08032b98
    tune = 0x03000c20
    note_slots = 0x03000e20
    fx_slots = 0x03003580
    buffer_flag = 0x03003650
    buffers = (0x080d2c44, 0x080d2c3c)
    samples_per_frame = 0x03000c14
    channel_volume = 0x03000d60


class AddressesBAJE(Addresses):
    """BAJE (Banjo-Pilot): 5 note slots for each channel."""
    slots_per_channel = 5
    fx_count = 4
    init = 0x08002084
    request = 0x08002028
    frame = 0x0800216e
    tune = 0x0203cbbc
    note_slots = 0x030065a4
    fx_slots = 0x03006504
    buffer_flag = 0x03007764
    buffers = (0x0806b8f4, 0x0806b8fc)
    samples_per_frame = 0x0203cc60
    channel_volume = 0x0203cd2c


class AddressesBDQE(Addresses):
    """BDQE (Donkey Kong Country 3)."""
    init = 0x080aed90
    request = 0x080aed34
    frame = 0x080aee82
    tune = 0x030012a0
    note_slots = 0x030014c0
    fx_slots = 0x03003c60
    buffer_flag = 0x03003d30
    buffers = (0x080e38b0, 0x080e38a8)
    samples_per_frame = 0x03001294
    channel_volume = 0x03001380
    envelope_settings = 0x030023f0
    music_volume = 0x030023c4


GAMES = {b'A5NE': Addresses, b'AWUE': AddressesAWUE, b'BPIE': AddressesBPIE, b'BKZX': AddressesBKZX,
         b'B2DE': AddressesB2DE, b'BAJE': AddressesBAJE, b'BDQE': AddressesBDQE}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class Voice:
    """A note or sound effect record, as the driver leaves it after a frame."""

    def __init__(self, raw, index):
        self.index = index
        self.state = raw[0]
        self.key, self.velocity, self.phase, self.pitch_key = raw[2:6]
        self.channel = struct.unpack('<b', raw[6:7])[0]
        self.position, self.fraction, self.level = struct.unpack('<III', raw[12:24])
        self.instrument = struct.unpack('<I', raw[32:36])[0]

    def active(self):
        return self.state in (0x11, 0x12)


class DriverEmulator:
    def __init__(self, rom):
        addr = addresses_for(rom)
        self.addr = addr
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        size = (len(rom) + 0xFFF) & ~0xFFF
        uc.mem_map(ROM_BASE, size)
        uc.mem_write(ROM_BASE, rom)
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_READ, self._io_read, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        self.uc = uc
        self.vcount = 0
        self.muted = None
        self.call(addr.init)

        # The mixer starts sub sp, sp, #0x24 (#0x28 in BDQE); str lr, [sp, #8] in the code the init copies to IWRAM. A
        # hook there can mute voices just before they're mixed.
        iwram = bytes(uc.mem_read(0x03000000, 0x8000))
        at = iwram.find(bytes.fromhex('24d04de208e08de5'))
        if at < 0:
            at = iwram.find(bytes.fromhex('28d04de208e08de5'))
        if at < 0:
            raise SystemExit("the driver's mixer wasn't found in IWRAM")
        uc.hook_add(UC_HOOK_CODE, self._before_mix, begin=0x03000000 + at, end=0x03000000 + at)

        # After mixing, the routine that moves the notes' samples on reads and clears the step the mixer left in each
        # note record: push {lr}; ldr fp, =size; ldr fp, [fp]; ldr sl, =note records; add sl, sl, fp.
        self.steps = [0] * (16 * addr.slots_per_channel)
        self.unmixed_steps = list(self.steps)
        for at in range(0, 0x8000 - 20, 4):
            words = struct.unpack('<5I', iwram[at:at + 20])
            if words[0] == 0xE92D4000 and words[1] & 0xFFFFF000 == 0xE59FB000 and words[2] == 0xE59BB000 and \
                    words[3] & 0xFFFFF000 == 0xE59FA000 and words[4] == 0xE08AA00B:
                literal = at + 12 + 8 + (words[3] & 0xFFF)
                if struct.unpack('<I', iwram[literal:literal + 4])[0] == addr.note_slots:
                    uc.hook_add(UC_HOOK_CODE, self._before_advance, begin=0x03000000 + at, end=0x03000000 + at)
                    break
        else:
            raise SystemExit("the driver's routine that moves the notes on wasn't found in IWRAM")

    def _slot_steps(self):
        a = self.addr
        raw = bytes(self.uc.mem_read(a.note_slots, len(self.steps) * SLOT_SIZE))
        return [struct.unpack('<I', raw[i * SLOT_SIZE + 8:i * SLOT_SIZE + 12])[0] for i in range(len(self.steps))]

    def _before_mix(self, uc, address, size, user):
        # A slot keeps the step that the mixer last left in it when the mixer frees it, and a note that starts there
        # later keeps it until the mixer takes the voice. So only a step that the mixer changes is its own.
        self.unmixed_steps = self._slot_steps()

        # A voice whose velocity byte is 0 is mixed at a level of 0.
        if self.muted is None:
            return
        a = self.addr
        for i in range(16 * a.slots_per_channel):
            if i // a.slots_per_channel in self.muted:
                uc.mem_write(a.note_slots + i * SLOT_SIZE + 3, b'\0')

    def _before_advance(self, uc, address, size, user):
        self.steps = [step if step != before else 0 for step, before in zip(self._slot_steps(), self.unmixed_steps)]

    def _svc(self, uc, intno, user):
        raise SystemExit('the driver made a BIOS call')

    def _unmapped(self, uc, access, address, size, value, user):
        # A one-shot sample that ends while its voice isn't mixed makes the driver store to the address in r8, which is
        # often 0 or outside memory. The hardware ignores such stores, so the page is mapped and the store lands there.
        uc.mem_map(address & ~0xFFF, 0x1000)
        return True

    def _io_read(self, uc, access, address, size, value, user):
        # The init waits for the vertical count to reach the VBlank, so each read moves it on a line.
        if address <= 0x04000006 < address + size:
            self.vcount = (self.vcount + 1) % 228
            uc.mem_write(0x04000006, struct.pack('<H', self.vcount))
        # An immediate DMA transfer is over before the CPU runs again, and the hardware then clears its enable bit.
        for control in DMA_CONTROL:
            if control <= address < control + 4:
                cnt_h = struct.unpack('<H', uc.mem_read(control + 2, 2))[0]
                if cnt_h & 0x8000 and (cnt_h >> 12) & 3 == 0:
                    uc.mem_write(control + 2, struct.pack('<H', cnt_h & 0x7FFF))

    def _io_write(self, uc, access, address, size, value, user):
        # Emulate immediate DMA transfers, which the driver uses to clear its buffers and copy its code and track list.
        for control in DMA_CONTROL:
            if (address == control and size == 4) or (address == control + 2 and size == 2):
                cnt = value if size == 4 else struct.unpack('<H', uc.mem_read(control, 2))[0] | (value << 16)
                if cnt & 0x80000000 and (cnt >> 28) & 3 == 0:
                    src = struct.unpack('<I', uc.mem_read(control - 8, 4))[0]
                    dst = struct.unpack('<I', uc.mem_read(control - 4, 4))[0]
                    unit = 4 if (cnt >> 26) & 1 else 2
                    count_bits = 0xFFFF if control == DMA_CONTROL[3] else 0x3FFF
                    count = (cnt & count_bits) or count_bits + 1
                    step = {0: unit, 1: -unit, 2: 0, 3: unit}  # increment, decrement, fixed, reload
                    src_step, dst_step = step[(cnt >> 23) & 3], step[(cnt >> 21) & 3]
                    for _ in range(count):
                        uc.mem_write(dst, bytes(uc.mem_read(src, unit)))
                        src += src_step
                        dst += dst_step

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def u32(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def s32(self, address):
        return struct.unpack('<i', self.uc.mem_read(address, 4))[0]

    def play(self, tune):
        """Requests `tune`. It starts on the next frame. In BDQE, the music volume is then set as the game sets it for
        most tunes."""
        self.call(self.addr.request, (tune,))
        if self.addr.music_volume is not None:
            self.uc.mem_write(self.addr.music_volume, struct.pack('<I', GAME_MUSIC_VOLUME))

    def frame(self):
        """Runs one frame. Returns the mixer's output for it, as signed 8-bit samples. Afterwards `steps` holds the
        step that the mixer moved each note on by, in 1/2^23 bytes for each output sample, or 0 if it didn't mix it."""
        a = self.addr
        self.steps = [0] * len(self.steps)
        self.call(a.frame)
        buffer = self.u32(a.buffers[0] if self.u32(a.buffer_flag) == 0 else a.buffers[1])
        count = self.u32(a.samples_per_frame)
        return np.frombuffer(bytes(self.uc.mem_read(buffer, count)), dtype=np.int8)

    def playing(self):
        """Returns true while a tune plays."""
        return self.s32(self.addr.tune) >= 0

    def voices(self):
        """Returns the note records, then the sound effect records."""
        a = self.addr
        notes = 16 * a.slots_per_channel
        raw = bytes(self.uc.mem_read(a.note_slots, notes * SLOT_SIZE))
        out = [Voice(raw[i * SLOT_SIZE:(i + 1) * SLOT_SIZE], i) for i in range(notes)]
        raw = bytes(self.uc.mem_read(a.fx_slots, a.fx_count * SLOT_SIZE))
        return out + [Voice(raw[i * SLOT_SIZE:(i + 1) * SLOT_SIZE], notes + i) for i in range(a.fx_count)]

    def channel_volume(self, channel):
        return self.uc.mem_read(self.addr.channel_volume + channel, 1)[0]

    def envelope_settings(self, channel):
        """Returns a channel's attack, decay, sustain and release settings from controllers 20-23, each 0xFF where the
        channel's notes take their instrument's, as in a revision without them."""
        if self.addr.envelope_settings is None:
            return (0xFF,) * 4
        return tuple(self.uc.mem_read(self.addr.envelope_settings + 4 * channel, 4))

    def fade_table(self):
        """Returns the 100 entries of the table of decay and release lengths, which the envelope code in IWRAM loads
        with ldr r2, =table; ldrb r1, [r2, r1]."""
        iwram = bytes(self.uc.mem_read(0x03000000, 0x8000))
        for at in range(0, 0x8000 - 8, 4):
            load, use = struct.unpack('<II', iwram[at:at + 8])
            if load & 0xFFFFF000 == 0xE59F2000 and use == 0xE7D21001:
                table = self.u32(0x03000000 + at + 8 + (load & 0xFFF))
                return bytes(self.uc.mem_read(table, 100))
        raise SystemExit("the driver's fade table wasn't found in IWRAM")

    def rate(self):
        """Returns the mixer's output rate in Hz, from the timer the init started."""
        reload = struct.unpack('<H', self.uc.mem_read(0x04000100, 2))[0]
        return 16777216 / (0x10000 - reload)


def write_wav(path, mono, rate):
    data = np.clip(mono.astype(np.float64) * 256, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(data.tobytes())


def trace(rom, tune, frames):
    emu = DriverEmulator(rom)
    emu.play(tune)
    for f in range(frames):
        emu.frame()
        for v in emu.voices():
            if v.active():
                print(f, v.index, '%02x' % v.state, v.channel, v.key, v.pitch_key, v.velocity - 1, v.phase, v.level,
                      '%08x' % v.instrument, v.position, v.fraction)
        if not emu.playing():
            break


def render(rom, tune, frames, channels=None):
    """Returns the driver's output for the first `frames` frames of `tune`, and its rate in Hz. If `channels` is given,
    the notes of the other channels are mixed at a level of 0. They still count towards the mixer's limit of voices."""
    emu = DriverEmulator(rom)
    if channels is not None:
        emu.muted = set(range(16)) - set(channels)
    emu.play(tune)
    out = [emu.frame() for _ in range(frames)]
    return np.concatenate(out), emu.rate()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('tune', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--channels', help='render: only these MIDI channels (0-15), such as 0,9')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.tune, a.frames)
        return
    if not a.out:
        p.error('an output file is needed')
    channels = [int(c) for c in a.channels.split(',')] if a.channels else None
    mono, rate = render(rom, a.tune, a.frames, channels)
    write_wav(a.out, mono, rate)


if __name__ == '__main__':
    main()
