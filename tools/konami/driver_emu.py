#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run a Konami GBA sound driver's code under the Unicorn ARM emulator.

This is the reference for checking agbsf2. It calls the driver's
init, song-start and per-frame routines directly (no BIOS, no video), and
captures what the driver produces:

    trace       the per-frame track output records, in `agbsf2 --trace` format
    render-ds   the DirectSound output (FIFO A = right and FIFO B = left, or both on both sides in the games whose
                drivers say so)
    render-psg  the PSG, rendered by psg_model.py from the driver's register writes

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM render-ds SONG FRAMES OUT.wav [--voices 0,3] [--echo]
    driver_emu.py ROM render-psg SONG FRAMES OUT.wav [--channel N]

The routine and RAM addresses are chosen by the ROM's game code. Those of
Yu-Gi-Oh! Ultimate Masters Edition: World Championship Tournament 2006 (BY6J,
BY6E), Shaman King: Master of Spirits 1 and 2 (BSOE, B2ME), Yu-Gi-Oh! Day of
the Duelist (BY7E), Yu-Gi-Oh! GX: Duel Academy (BYGE), Yu-Gi-Oh! World
Championship Tournament 2004 (BYWP), Rave Master: Special Attack Force (BRME),
Yu-Gi-Oh! The Eternal Duelist Soul (AY5E), Yu-Gi-Oh! Worldwide Edition (AYWE)
and Yu-Gi-Oh! Dungeon Dice Monsters (AYDE) are built in. Another game needs its
own values; see Addresses and docs/konami.md.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py and psg_model.py, in tools/
import psg_model
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
FRAME_CYCLES = 280896  # CPU cycles from one VBlank to the next
DMA_CONTROL = (0x040000B8, 0x040000C4, 0x040000D0, 0x040000DC)  # each DMA channel's count and control registers


class Addresses:
    """BY6J (the Japanese release of Yu-Gi-Oh! Ultimate Masters Edition: World Championship Tournament 2006)."""
    init = 0x0810B470           # sound init(irq slot, irq slot 2, work buffer)
    init_args = (0x03000020, 0x03000024, 0x02006F10)
    request_song = 0x0810CFE4   # r0 = song
    request_args = ()           # the arguments that follow the song
    start_song = 0x0810CE30
    frame = 0x0810CAC8          # per-frame entry (from VBlank)
    output_stage = 0x0810C736   # track output records at sp + record_offset
    tracks = 16                 # tracks in a song, and output records
    record_offset = 8
    record_size = 12
    record_pan = 8              # the two pan bytes of a track output record, or None for records without pan
    mixer = 0x03004C0C          # ARM mixer, copied to IWRAM by init
    mixer_call = None           # the routine through which the per-frame entry runs the mixer, for a ring mixer
    mixer_tick = None           # a ring mixer's DMA 1 interrupt routine, which moves its position on by 16 samples
    mixer_position = None       # a ring mixer's position
    mixer_ring = None           # a ring mixer's buffer size in samples
    fifo_buffer = 0x030055C0    # 16 bytes FIFO A, or a ring mixer's buffer for FIFO A
    fifo_b = 0x20               # FIFO B's buffer, from FIFO A's
    voices = 0x0300546C         # voice records
    voice_count = 12
    voice_size = 0x1C
    voice_flags_at = 0x0E       # a voice record's flags, with 0x80 while it plays and loop_flag for a looped sample
    loop_flag = 0x40
    voice_sample_at = 0x0C      # a voice record's sample number (its low byte)
    voice_levels_at = 0x10      # a voice record's right and left levels
    mono_voices = False         # True if a voice record has one level for both sides, at voice_levels_at
    voice_level_scale = 1       # a voice's level in 1/128 of full scale per unit of its record's levels, or of the
                                # level plus 1 in a mono voice record
    fifo_irqs = None            # the DMA 1 and DMA 2 interrupt routines, if they mix FIFO A's and FIFO B's samples
    fifo_periods = None         # FIFO A's timer period for those routines to apply, and FIFO B's 4 bytes on
    sample_entry_size = 4       # a sample table entry: 4 for a pointer to a header, or 8 for a pointer to the data and
                                # a word with the length in blocks of 16 in bits 20-31
    psg_restarts_on_writes = False  # True if every write of a square channel restarts it, as a pitch change can
    echo_buses = 0x03005608     # 3 echo bus records of 20 bytes, or None for a driver without echo
    echo_mask = 0x03005644      # per-voice bus routing read by the mixer
    echo_bus0_voices = 0xFFF    # bit v: the mixer takes voice v from echo bus 0
    wave_reloads_on_pitch = False  # True if a pitch change reloads the wave RAM, switching banks, as a note does

    @classmethod
    def voice_levels(cls, record):
        """Returns a voice record's (left, right) levels, in units of 1/128 full scale."""
        at = cls.voice_levels_at
        if cls.mono_voices:
            # The mixer plays the voice at (level + 1) times the scale, and skips it at level 0.
            level = (record[at] + 1) * cls.voice_level_scale if record[at] else 0
            return level, level
        return record[at + 1] * cls.voice_level_scale, record[at] * cls.voice_level_scale


class AddressesBY6E(Addresses):
    """BY6E (the US release of Yu-Gi-Oh! Ultimate Masters Edition): the same driver, 0x90 bytes lower in ROM."""
    init = Addresses.init - 0x90
    request_song = Addresses.request_song - 0x90
    start_song = Addresses.start_song - 0x90
    frame = Addresses.frame - 0x90
    output_stage = Addresses.output_stage - 0x90


class AddressesB2ME(Addresses):
    """B2ME (Shaman King: Master of Spirits 2). Its build of the driver has 24-byte voice records, and
    keeps its RAM right after the mixer. The game calls the init's two halves itself."""
    init = 0x08094090
    init_args = (0x03006848, 0x0300684C, 0x02000000)
    request_song = 0x08095BE0
    start_song = 0x08095A2C
    frame = 0x080956C4
    output_stage = 0x0809532E
    mixer = 0x03000000
    fifo_buffer = mixer + 0x944
    voices = mixer + 0x820
    voice_size = 0x18
    echo_buses = mixer + 0x98C
    echo_mask = mixer + 0x9C8


class AddressesBSOE(AddressesB2ME):
    """BSOE (Shaman King: Master of Spirits): the driver build of B2ME."""
    init = 0x0805A5D8
    init_args = (0x03002588, 0x0300258C, 0x02000000)
    request_song = 0x0805C128
    start_song = 0x0805BF74
    frame = 0x0805BC0C
    output_stage = 0x0805B876


class AddressesBY7E(AddressesB2ME):
    """BY7E (Yu-Gi-Oh! Day of the Duelist: World Championship Tournament 2005): the driver build of B2ME, with its RAM
    elsewhere."""
    init = 0x0813C540
    init_args = (0x03000024, 0x03000028, 0x02014820)
    request_song = 0x0813E090
    start_song = 0x0813DEDC
    frame = 0x0813DB74
    output_stage = 0x0813D7DE
    mixer = 0x03004788
    fifo_buffer = mixer + 0x944
    voices = mixer + 0x820
    echo_buses = mixer + 0x98C
    echo_mask = mixer + 0x9C8


class AddressesBYGE(AddressesB2ME):
    """BYGE (Yu-Gi-Oh! GX: Duel Academy): the driver build of B2ME, with its RAM elsewhere."""
    init = 0x080EF234
    init_args = (0x03000024, 0x03000028, 0x020061C0)
    request_song = 0x080F0DA0
    start_song = 0x080F0BEC
    frame = 0x080F0884
    output_stage = 0x080F04F2
    mixer = 0x030006C8
    fifo_buffer = mixer + 0x944
    voices = mixer + 0x820
    echo_buses = mixer + 0x98C
    echo_mask = mixer + 0x9C8


class AddressesBYWP(Addresses):
    """BYWP (Yu-Gi-Oh! World Championship Tournament 2004, Europe): the WCT 2004 revision of the driver. The track
    output stage is part of the sequencer, which leaves the records at sp + 8 there too, with the pan at +10. Its mixer
    plays a voice's level of n at n/16 of full scale, and the routing it hands the mixer keeps only voices 5-11 of echo
    bus 0 (it's masked with 0x1FFF). Every output of the wave track reloads the wave RAM."""
    init = 0x08096694
    init_args = (0x03000020, 0x03000024, 0x020114C0)
    request_song = 0x08098138
    start_song = 0x08097F2C
    frame = 0x08097BE8
    output_stage = 0x080977C6
    record_pan = 10
    mixer = 0x03004F18
    fifo_buffer = 0x03005834
    voices = 0x030056E0
    voice_size = 0x1C
    voice_level_scale = 8
    echo_buses = 0x0300587C
    echo_mask = 0x030058B8
    echo_bus0_voices = 0xFE0
    wave_reloads_on_pitch = True


class AddressesBRME(AddressesBYWP):
    """BRME (Rave Master: Special Attack Force): the Rave Master revision of the driver, whose songs have 12 tracks,
    for 8 voices of 24 bytes. It has no echo, and its mixer plays a voice's level of n at n/15 of full scale."""
    init = 0x0802AF4C
    init_args = (0x03000020, 0x03000024)
    request_song = 0x0802C3EC
    start_song = 0x0802C29C
    frame = 0x0802C018
    output_stage = 0x0802BB98
    tracks = 12
    mixer = 0x03004440
    fifo_buffer = 0x03004400
    voices = 0x0300433C
    voice_count = 8
    voice_size = 0x18
    voice_level_scale = 128 / 15
    echo_buses = None
    echo_mask = None


class AddressesAY5E(AddressesBYWP):
    """AY5E (Yu-Gi-Oh! The Eternal Duelist Soul): the Eternal Duelist revision of the driver, whose songs have 10
    tracks, for 6 voices of 16 bytes. Its track output records are 8 bytes at sp, without pan, and a voice record has
    one level for both sides. It has no echo.

    The per-frame entry ends by running the mixer, which fills a ring buffer for each FIFO up to the position that the
    DMA 1 interrupt moves on 16 samples at a time. The emulator skips that call and runs the mixer itself, as the
    other games' timer interrupts do. Voices 0-2 play through FIFO A and voices 3-5 through FIFO B, and both FIFOs
    play on both sides."""
    init = 0x0807D578
    init_args = (0,)
    request_song = 0x0807E690
    request_args = (0,)         # no fade-in
    start_song = 0x0807E554
    frame = 0x0807E3B0
    output_stage = 0x0807E072
    tracks = 10
    record_offset = 0
    record_size = 8
    record_pan = None
    mixer = 0x0807EAD0          # in ROM
    mixer_call = 0x08080A18
    mixer_tick = 0x0807E324
    mixer_position = 0x0300540C
    mixer_ring = 0x2C0
    fifo_buffer = 0x03005414
    fifo_b = 0x320
    voices = 0x030053AC
    voice_count = 6
    voice_size = 0x10
    voice_levels_at = 0x0F
    mono_voices = True
    voice_level_scale = 8
    echo_buses = None
    echo_mask = None


class AddressesAYWE(AddressesAY5E):
    """AYWE (Yu-Gi-Oh! Worldwide Edition): the driver of AY5E, 0x15A44 bytes higher in ROM and 0x100 bytes lower in
    RAM."""
    init = AddressesAY5E.init + 0x15A44
    request_song = AddressesAY5E.request_song + 0x15A44
    start_song = AddressesAY5E.start_song + 0x15A44
    frame = AddressesAY5E.frame + 0x15A44
    output_stage = AddressesAY5E.output_stage + 0x15A44
    mixer = AddressesAY5E.mixer + 0x15A44
    mixer_call = AddressesAY5E.mixer_call + 0x15A44
    mixer_tick = AddressesAY5E.mixer_tick + 0x15A44
    mixer_position = AddressesAY5E.mixer_position - 0x100
    fifo_buffer = AddressesAY5E.fifo_buffer - 0x100
    voices = AddressesAY5E.voices - 0x100


class AddressesAYDE(Addresses):
    """AYDE (Yu-Gi-Oh! Dungeon Dice Monsters): the Dungeon Dice Monsters revision of the driver, whose songs have 8
    tracks, for 4 voices of 12 bytes. The per-frame entry leaves its 12-byte track output records at sp, without pan,
    and a voice record has one level for both sides. It has no echo.

    It has no mixer. The DMA 1 and DMA 2 interrupt routines each mix the next 16 samples of two voices, 0 and 1 for
    FIFO A and 2 and 3 for FIFO B, at (level + 1) / 32 each, into a buffer that the DMA then hands the FIFO. Each FIFO
    plays at the rate of its timer, which a note start sets to the note's rate, so both of its voices play at the rate
    of the last note started on it. Both FIFOs play on both sides."""
    init = 0x0802985C
    init_args = (0,)            # no interrupt table to fill in
    request_song = 0x0802B264
    request_args = (0,)         # no fade-in
    start_song = 0x0802B04C
    frame = 0x0802AE3C
    output_stage = 0x0802A5EE
    tracks = 8
    record_offset = 0
    record_pan = None
    mixer = None
    fifo_buffer = None
    fifo_irqs = (0x0802A9B4, 0x0802A7C0)
    fifo_periods = 0x03000B4C
    sample_entry_size = 8
    psg_restarts_on_writes = True
    voices = 0x030009AC
    voice_count = 4
    voice_size = 0x0C
    voice_flags_at = 0x0A
    loop_flag = None            # the samples have no loop
    voice_sample_at = 0x08
    voice_levels_at = 0x0B
    mono_voices = True
    voice_level_scale = 4
    echo_buses = None
    echo_mask = None


GAMES = {b'BY6J': Addresses, b'BY6E': AddressesBY6E, b'B2ME': AddressesB2ME, b'BSOE': AddressesBSOE,
         b'BY7E': AddressesBY7E, b'BYGE': AddressesBYGE, b'BYWP': AddressesBYWP, b'BRME': AddressesBRME,
         b'AY5E': AddressesAY5E, b'AYWE': AddressesAYWE, b'AYDE': AddressesAYDE}


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


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
        if addr.mixer_call is not None:
            uc.mem_write(addr.mixer_call, b'\x70\x47')  # bx lr: the per-frame entry doesn't run the mixer
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_READ, self._dma_read, begin=DMA_CONTROL[0], end=DMA_CONTROL[-1] + 3)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        self.uc = uc
        self.frame_no = 0
        self.io_writes = []  # (frame, address, size, value)
        self._records = None
        uc.hook_add(UC_HOOK_CODE, self._capture, begin=addr.output_stage, end=addr.output_stage)
        self.call(addr.init, addr.init_args)

        # The init starts timer 0, which clocks the sound FIFOs and so sets the mixer's output rate.
        self.sample_cycles = None  # CPU cycles per mixer sample
        for _, address, size, value in self.io_writes:
            if address == 0x04000100 and size == 4 and value & 0x800000:
                prescale = [1, 64, 256, 1024][(value >> 16) & 3]
                self.sample_cycles = (0x10000 - (value & 0xFFFF)) * prescale

    def _svc(self, uc, intno, user):
        # The only BIOS call the driver's setup makes is GetBiosChecksum.
        uc.reg_write(UC_ARM_REG_R0, 0xBAAE187F)

    def _io_write(self, uc, access, address, size, value, user):
        self.io_writes.append((self.frame_no, address, size, value))
        # Emulate immediate DMA transfers. The inits copy the mixer with DMA 3, a ring mixer clears its buffer with
        # DMA 0, and AYDE's driver loads the wave RAM with DMA 3, which is recorded as writes to the registers.
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
                        data = bytes(uc.mem_read(src, unit))
                        uc.mem_write(dst, data)
                        if 0x04000000 <= dst < 0x04000400:
                            self.io_writes.append((self.frame_no, dst, unit, int.from_bytes(data, 'little')))
                        src += src_step
                        dst += dst_step

    def _dma_read(self, uc, access, address, size, value, user):
        # An immediate transfer is over before the CPU runs again, and the
        # hardware then clears its enable bit. The driver waits for that after
        # each transfer. (A write hook runs before the store lands, so the bit
        # is cleared here, when the control register is read back.)
        for control in DMA_CONTROL:
            if control <= address < control + 4:
                cnt_h = struct.unpack('<H', uc.mem_read(control + 2, 2))[0]
                if cnt_h & 0x8000 and (cnt_h >> 12) & 3 == 0:
                    uc.mem_write(control + 2, struct.pack('<H', cnt_h & 0x7FFF))

    def _capture(self, uc, address, size, user):
        a = self.addr
        sp = uc.reg_read(UC_ARM_REG_SP)
        raw = bytes(uc.mem_read(sp + a.record_offset, a.tracks * a.record_size))
        self._records = []
        for t in range(a.tracks):
            r = raw[t * a.record_size:(t + 1) * a.record_size]
            pan = (r[a.record_pan], r[a.record_pan + 1]) if a.record_pan is not None else (0, 0)
            self._records.append((struct.unpack('<h', r[0:2])[0], r[2], r[3], r[4], r[5],
                                  struct.unpack('<H', r[6:8])[0]) + pan)

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def play(self, song):
        self.call(self.addr.request_song, (song,) + self.addr.request_args)
        self.call(self.addr.start_song)

    def frame(self):
        """Runs one frame; returns the track output records (None if the driver is idle)."""
        self._records = None
        self.call(self.addr.frame)
        self.frame_no += 1
        return self._records

    def mix_rate(self):
        """Returns the mixer's output rate in Hz."""
        if self.sample_cycles is None:
            raise SystemExit("the driver's init didn't start timer 0, which sets the mixer's rate")
        return 16777216 / self.sample_cycles

    def mix(self, blocks):
        """Runs the mixer for `blocks` blocks of 16 samples; returns their left and right samples, in units of a FIFO's
        sample."""
        a = self.addr
        if not blocks:
            return np.zeros(0, np.int16), np.zeros(0, np.int16)
        if a.mixer_tick is None:
            # The timer interrupt runs the mixer for each block. FIFO A plays on the right, and FIFO B on the left.
            left, right = [], []
            for _ in range(blocks):
                self.call(a.mixer, thumb=False)
                right.append(np.frombuffer(bytes(self.uc.mem_read(a.fifo_buffer, 16)), dtype=np.int8))
                left.append(np.frombuffer(bytes(self.uc.mem_read(a.fifo_buffer + a.fifo_b, 16)), dtype=np.int8))
            return np.concatenate(left).astype(np.int16), np.concatenate(right).astype(np.int16)

        # The DMA 1 interrupt moves a ring mixer's position on for each block, and the mixer fills the ring up to it.
        # Both FIFOs play on both sides.
        start = struct.unpack('<H', self.uc.mem_read(a.mixer_position, 2))[0]
        for _ in range(blocks):
            self.call(a.mixer_tick)
        self.call(a.mixer, thumb=False)
        index = (start + np.arange(16 * blocks)) % a.mixer_ring
        fifo_a, fifo_b = (np.frombuffer(bytes(self.uc.mem_read(a.fifo_buffer + offset, a.mixer_ring)), dtype=np.int8)
                          for offset in (0, a.fifo_b))
        both = fifo_a[index].astype(np.int16) + fifo_b[index]
        return both, both

    def mute_voices_except(self, keep):
        a = self.addr
        for v in range(a.voice_count):
            if v not in keep:
                self.uc.mem_write(a.voices + v * a.voice_size + a.voice_levels_at, bytes(1 if a.mono_voices else 2))

    def disable_echo(self):
        if self.addr.echo_mask is not None:
            self.uc.mem_write(self.addr.echo_mask, b'\0\0\0\0')


def write_wav(path, stereo, rate):
    data = np.clip(stereo * 32767, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(data.tobytes())


def trace(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.play(song)
    for f in range(frames):
        records = emu.frame()
        if records is None:
            break
        for t, r in enumerate(records):
            print(f, t, *r)


class Fifo:
    """A sound FIFO fed 16 samples at a time by the driver's DMA interrupt routine. Tracks queued samples, the
    current output and the next timer tick."""

    def __init__(self, index, irq):
        self.index = index  # 0 for FIFO A, 1 for B
        self.irq = irq
        self.queue = []
        self.value = 0
        self.next_tick = None  # the frame's CPU cycle that the timer next plays a sample on, None while it's stopped

    def period(self, emu):
        """Returns the timer period in CPU cycles, or None if the timer is stopped."""
        timer = (struct.unpack('<H', emu.uc.mem_read(0x04000082, 2))[0] >> (10 + 4 * self.index)) & 1
        reload, control = struct.unpack('<HH', emu.uc.mem_read(0x04000100 + 4 * timer, 4))
        if not control & 0x80:
            return None
        return (0x10000 - reload) * [1, 64, 256, 1024][control & 3]

    def tick(self, emu):
        """Plays the next sample. When 16 or fewer are left, DMA 1 or 2 hands over the next 16, and its interrupt
        routine mixes the 16 after them, or stops the FIFO."""
        if self.queue:
            self.value = self.queue.pop(0)
        control = DMA_CONTROL[1 + self.index]
        if len(self.queue) <= 16 and struct.unpack('<H', emu.uc.mem_read(control + 2, 2))[0] & 0x8000:
            source = struct.unpack('<I', emu.uc.mem_read(control - 8, 4))[0]
            self.queue += [int(v) for v in np.frombuffer(bytes(emu.uc.mem_read(source, 16)), dtype=np.int8)]
            emu.call(self.irq)

    def play_frame(self, emu):
        """Plays the samples its timer ticks on in a frame. Returns the cycles of the ticks and the samples played."""
        ticks, played = [], []
        period = self.period(emu)
        if period is not None and self.next_tick is None:
            self.next_tick = period
        while period is not None and self.next_tick < FRAME_CYCLES:
            self.tick(emu)
            ticks.append(self.next_tick)
            played.append(self.value)
            period = self.period(emu)
            if period is not None:
                self.next_tick += period
        self.next_tick = self.next_tick - FRAME_CYCLES if period is not None else None
        return np.array(ticks, dtype=np.int64), np.array(played, dtype=np.int16)


def render_fifos(emu, frames, voices=None):
    """Returns the output of a driver whose DMA interrupt routines mix each FIFO's samples, for the first `frames`
    frames of the song it plays, at 32768 Hz. The timers set each FIFO's rate, so the frames are played out in CPU
    cycles."""
    fifos = [Fifo(i, irq) for i, irq in enumerate(emu.addr.fifo_irqs)]
    cycles_per_sample = 16777216 // 32768
    out = []
    frame_start = 0  # the CPU cycle each frame starts on, counted from the song's start
    for _ in range(frames):
        emu.io_writes.clear()
        emu.frame()
        if voices is not None:
            emu.mute_voices_except(voices)

        # A FIFO starts empty after a reset, and a write to the FIFO adds its bytes.
        for _, address, size, value in emu.io_writes:
            for i, b in enumerate(value.to_bytes(size, 'little')):
                if address + i == 0x04000083:
                    for fifo in fifos:
                        if b & (8 << 4 * fifo.index):
                            fifo.queue = []
                elif 0x040000A0 <= address + i < 0x040000A8:
                    fifos[(address + i - 0x040000A0) // 4].queue.append((b ^ 0x80) - 0x80)

        # Each output sample uses the latest value from each FIFO, possibly held over from an earlier frame.
        first = -(-frame_start // cycles_per_sample)
        last = -(-(frame_start + FRAME_CYCLES) // cycles_per_sample)
        at = np.arange(first, last, dtype=np.int64) * cycles_per_sample - frame_start
        mix = np.zeros(len(at), dtype=np.int16)
        for fifo in fifos:
            before = fifo.value
            ticks, played = fifo.play_frame(emu)
            index = np.searchsorted(ticks, at, side='right') - 1
            mix += np.where(index >= 0, played[np.maximum(index, 0)], before) if len(played) else before
        out.append(mix)
        frame_start += FRAME_CYCLES

    both = np.concatenate(out)
    return np.stack([both, both], axis=1).astype(np.float32) / 128.0, 32768.0


def render_ds(rom, song, frames, voices=None, echo=False):
    """Returns the driver's DirectSound mix of the first `frames` frames of `song`, and its rate in Hz."""
    emu = DriverEmulator(rom)
    if emu.addr.fifo_irqs is not None:
        emu.play(song)
        return render_fifos(emu, frames, voices)

    rate = emu.mix_rate()
    emu.play(song)

    # The mixer makes 16 samples at a time. When a frame doesn't hold a whole number of blocks, the fraction left over
    # is carried into the next frame.
    blocks_per_frame = FRAME_CYCLES / (16 * emu.sample_cycles)
    out = []
    due = 0.0
    for _ in range(frames):
        emu.frame()
        if not echo:
            emu.disable_echo()
        if voices is not None:
            emu.mute_voices_except(voices)
        due += blocks_per_frame
        blocks = int(due)
        due -= blocks
        left, right = emu.mix(blocks)
        out.append(np.stack([left, right], axis=1))
    return np.concatenate(out).astype(np.float32) / 128.0, rate


def render_psg(rom, song, frames, channel=None):
    emu = DriverEmulator(rom)
    init_writes = [(a, s, v) for _, a, s, v in emu.io_writes]
    emu.io_writes.clear()
    emu.play(song)
    for _ in range(frames):
        emu.frame()
    return psg_model.render_writes(init_writes, emu.io_writes, frames, channel)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render-ds', 'render-psg'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--voices', help='render-ds: only these voices, such as 0,3')
    p.add_argument('--echo', action='store_true', help='render-ds: keep the echo effect')
    p.add_argument('--channel', type=int, help='render-psg: only this PSG channel (0-3)')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames)
        return
    if not a.out:
        p.error('an output file is needed')
    if a.command == 'render-ds':
        voices = [int(v) for v in a.voices.split(',')] if a.voices else None
        mix, rate = render_ds(rom, a.song, a.frames, voices, a.echo)
        write_wav(a.out, mix, rate)
    else:
        write_wav(a.out, render_psg(rom, a.song, a.frames, a.channel), psg_model.RATE)


if __name__ == '__main__':
    main()
