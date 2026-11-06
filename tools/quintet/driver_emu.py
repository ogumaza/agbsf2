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
(A6SJ) and J (B6JJ) are built in, and those of the games whose driver loads a music file (LoadedFiles). Another game
requires other values; see GAMES and docs/quintet.md.
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
    init_calls = ()  # the driver's routines that the game calls after `sound_init`, before it plays: (routine, args)
    loader = None    # the driver's routine that loads a music file into its tables, or None if the game gives them
    files = ()       # the music files that the loader reads, in agbsf2's order: each file's first word
    programs = ()    # the programs that the game sends to another console, each with its copy of the driver
    image = None     # a program's copy in the ROM. It runs from EWRAM, where its routines' addresses are.


class LoadedFiles(Addresses):
    """The games whose driver of the J revision loads a music file: its songs, its sound effects and its tables. Their
    `sound_init` is the driver's init. The game then calls the routine of `init_calls`. `play` and `play_effect` are
    the driver's routines that play song n and sound effect n of the file that the loader read last. agbsf2 numbers
    each file's songs and sound effects after those of the file before."""
    countdown = 0x1A


class AddressesA2PJ(LoadedFiles):
    """A2PJ (Bouken Yuuki Plaston GP): the loader reads its music file from a literal."""
    sound_init = 0x0803FB38
    init_calls = ((0x08042784, ()),)
    loader = 0x0803FE28
    play = 0x0803FF18
    play_effect = 0x0803FF54
    frame = 0x08041C78
    channels = 0x08747C34
    effect_channels = 0x08747C4C
    files = (0x084D611C,)


class ProgramABRE1(LoadedFiles):
    """The first of the three programs that ABRE sends to another console, at 0x08086F40. Its lookup gives its music
    file, file 2 of archive 0."""
    image = 0x08086F40
    sound_init = 0x0200AD64
    init_calls = ((0x0200D944, ()),)
    loader = 0x0200B054
    play = 0x0200B144
    play_effect = 0x0200B180
    frame = 0x0200CE38
    channels = 0x02028154
    effect_channels = 0x0202816C
    files = (0x0809BA64,)


class ProgramABRE2(ProgramABRE1):
    """The second program that ABRE sends, at 0x080AF2E4."""
    image = 0x080AF2E4
    sound_init = 0x0200ABC4
    init_calls = ((0x0200D7A4, ()),)
    loader = 0x0200AEB4
    play = 0x0200AFA4
    play_effect = 0x0200AFE0
    frame = 0x0200CC98
    channels = 0x0201E3E4
    effect_channels = 0x0201E3FC
    files = (0x080C3C78,)


class ProgramABRE3(ProgramABRE1):
    """The third program that ABRE sends, at 0x080CD918."""
    image = 0x080CD918
    sound_init = 0x0200A77C
    init_calls = ((0x0200D35C, ()),)
    loader = 0x0200AA6C
    play = 0x0200AB5C
    play_effect = 0x0200AB98
    frame = 0x0200C850
    channels = 0x0201CE6C
    effect_channels = 0x0201CE84
    files = (0x080E1C84,)


class AddressesABRE(LoadedFiles):
    """ABRE (Blender Bros.): the game's lookups give its two music files, file 0x48 and file 0x49 of archive 0."""
    sound_init = 0x0805E814
    init_calls = ((0x080613F4, ()),)
    loader = 0x0805EB04
    play = 0x0805EBF4
    play_effect = 0x0805EC30
    frame = 0x080608E8
    channels = 0x0863A278
    effect_channels = 0x0863A290
    files = (0x08117E28, 0x0818B648)
    programs = (ProgramABRE1, ProgramABRE2, ProgramABRE3)


class ProgramARPE(LoadedFiles):
    """The program that ARPE sends to another console, at 0x08066F00. It has a copy of the driver and
    loads its music file from 0x0200F734."""
    image = 0x08066F00
    sound_init = 0x02005118
    init_calls = ((0x02007CF8, ()),)
    loader = 0x02005408
    play = 0x020054F8
    play_effect = 0x02005534
    frame = 0x020071EC
    channels = 0x02010450
    effect_channels = 0x02010468
    files = (0x08076634,)


class AddressesARPE(LoadedFiles):
    """ARPE (Robopon 2: Ring Version): the game picks its music file by number from its table of files."""
    sound_init = 0x08044FBC
    init_calls = ((0x08047B9C, ()),)
    loader = 0x080452AC
    play = 0x0804539C
    play_effect = 0x080453D8
    frame = 0x08047090
    channels = 0x0807BB38
    effect_channels = 0x0807BB50
    files = (0x0834674C,)
    programs = (ProgramARPE,)


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


class AddressesAPJJ(LoadedFiles):
    """APJJ (Bouken Yuuki Densetsu Plaston Gate): the loader reads its music file from a literal. The ROM holds another
    music file, which no code of the game names."""
    sound_init = 0x080D4864
    init_calls = ((0x080D7444, ()),)
    loader = 0x080D4B54
    play = 0x080D4C44
    play_effect = 0x080D4C80
    frame = 0x080D6938
    channels = 0x087FA198
    effect_channels = 0x087FA1B0
    files = (0x0864CEB4, 0x08711F1C)


class AddressesBPDJ(LoadedFiles):
    """BPDJ (Bouken Yuuki PlaStar World: Densetsu no Plust Gate EX): the loader reads its music file from a literal."""
    sound_init = 0x08159098
    init_calls = ((0x0815BC78, ()),)
    loader = 0x08159388
    play = 0x08159478
    play_effect = 0x081594B4
    frame = 0x0815B16C
    channels = 0x08E2B748
    effect_channels = 0x08E2B760
    files = (0x08BB301C,)


class AddressesAVDJ(LoadedFiles):
    """AVDJ (Legend of Dynamic: Goushouden): the loader reads its music file from a literal."""
    sound_init = 0x080C8960
    init_calls = ((0x080CB5AC, ()),)
    loader = 0x080C8C50
    play = 0x080C8D40
    play_effect = 0x080C8D7C
    frame = 0x080CAAA0
    channels = 0x087ED9B4
    effect_channels = 0x087ED9CC
    files = (0x08135E6C,)


class AddressesA8DJ(LoadedFiles):
    """A8DJ (Doubutsu no Shima no Chobi Gurumi): the game's lookup gives its music file, file 4 of archive 0."""
    sound_init = 0x08082B4C
    init_calls = ((0x0808572C, ()),)
    loader = 0x08082E3C
    play = 0x08082F2C
    play_effect = 0x08082F68
    frame = 0x08084C20
    channels = 0x087CE848
    effect_channels = 0x087CE860
    files = (0x080F7BE4,)


class AddressesAAIJ(LoadedFiles):
    """AAIJ and ABIJ (Gachasute! Dyna Device Red and Blue): the game's lookup gives the music file, file 0x16 of
    archive 0. The two have the same driver and music."""
    sound_init = 0x080A8118
    init_calls = ((0x080AACF8, ()),)
    loader = 0x080A8408
    play = 0x080A84F8
    play_effect = 0x080A8534
    frame = 0x080AA1EC
    channels = 0x08747B44
    effect_channels = 0x08747B5C
    files = (0x08131174,)


class ProgramACVE(ProgramARPE):
    """The program that ACVE sends to another console, at 0x08066F08: ARPE's, 8 bytes on."""
    image = 0x08066F08
    files = (0x0807663C,)


class AddressesACVE(LoadedFiles):
    """ACVE (Robopon 2: Cross Version): the game picks its music file by number from its table of files, as in ARPE."""
    sound_init = 0x08044FD0
    init_calls = ((0x08047BB0, ()),)
    loader = 0x080452C0
    play = 0x080453B0
    play_effect = 0x080453EC
    frame = 0x080470A4
    channels = 0x0807BB40
    effect_channels = 0x0807BB58
    files = (0x0834674C,)
    programs = (ProgramACVE,)


class AddressesAJOJ(LoadedFiles):
    """AJOJ (Magical Fengshen): the game picks its music file by number, through tables that it sets up in RAM."""
    sound_init = 0x08048E14
    init_calls = ((0x0804B9F4, ()),)
    loader = 0x08049104
    play = 0x080491F4
    play_effect = 0x08049230
    frame = 0x0804AEE8
    channels = 0x08693EB4
    effect_channels = 0x08693ECC
    files = (0x08074504,)


class AddressesASKJ(Addresses):
    """ASKJ (Suta Komi: Star Communicator), with the S revision of the driver. The game's routines play song n of its
    music file and sound effect n of its sound effects' file."""
    sound_init = 0x08025FF0
    play = 0x08026040
    play_effect = 0x080260A0
    frame = 0x08026110
    channels = 0x08119500
    effect_channels = 0x08119518
    songs = 23


GAMES = {b'ASRJ': Addresses, b'A6SJ': AddressesA6SJ, b'AJ9J': AddressesAJ9J, b'B6JJ': AddressesB6JJ,
         b'A2PJ': AddressesA2PJ, b'A8DJ': AddressesA8DJ, b'AAIJ': AddressesAAIJ, b'ABIJ': AddressesAAIJ,
         b'ABRE': AddressesABRE, b'ACVE': AddressesACVE, b'AJOJ': AddressesAJOJ, b'APJJ': AddressesAPJJ,
         b'ARPE': AddressesARPE, b'ASKJ': AddressesASKJ, b'AVDJ': AddressesAVDJ, b'BPDJ': AddressesBPDJ}


def file_counts(rom, file):
    """Returns the songs and the sound effects of the music file whose first word is at `file`, as the loader counts
    them: each list ends at a length of 0. A song's length gives the start of the next."""
    counts = []
    at = file
    for _ in range(2):
        size = struct.unpack_from('<I', rom, at - ROM_BASE)[0]
        song, n = at + 4, 0
        while True:
            length = struct.unpack_from('<H', rom, song - ROM_BASE)[0] & ~1
            if not length:
                break
            song += length
            n += 1
        counts.append(n)
        at += 4 + (size & ~3)
    return counts


def addresses_for(rom):
    code = rom[0xAC:0xB0]
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


def program_files(rom):
    """Returns each music file of the game in agbsf2's order, as (the addresses of the program that plays it, the
    file's address in the ROM): the game's files, then those of each program that it sends to another console."""
    a = addresses_for(rom)
    return [(a, f) for f in a.files] + [(p, f) for p in a.programs for f in p.files]


def locate(rom, song):
    """Returns the addresses of the program that plays agbsf2's song `song`, the song's music file and its number in
    the file, or the game's addresses, None and `song` if the game has no music files."""
    for program, file in program_files(rom):
        count = sum(file_counts(rom, file))
        if song < count:
            return program, file, song
        song -= count
    return addresses_for(rom), None, song


class DriverEmulator:
    def __init__(self, rom, song=0):
        """Starts the driver of the program that plays agbsf2's song `song`: the game's, or that of a program that the
        game sends to another console."""
        self.addr = locate(rom, song)[0]
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
        if self.addr.image:
            # A program that the game sends to another console runs from the start of EWRAM.
            uc.mem_write(0x02000000, rom[self.addr.image - ROM_BASE:self.addr.image - ROM_BASE + 0x40000])
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_READ, self._io_read, begin=0x04000000, end=0x04000FFF)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        self.uc = uc
        self.rom = rom
        self.writes = []
        self.channel_records = self.records(self.addr.channels)
        self.call(self.addr.sound_init)
        for routine, args in self.addr.init_calls:
            self.call(routine, args)
        self.init_writes = self.writes  # the sound init's writes. A sound effect plays on from them.

    def records(self, table):
        """Returns the addresses of the 6 channel records that the table at `table` points to."""
        return struct.unpack('<6I', self.uc.mem_read(table, 24))

    def rom_address(self, address):
        """Returns the ROM's address of `address` in a program that runs from EWRAM, or `address` itself."""
        if self.addr.image and 0x02000000 <= address < 0x02040000:
            return self.addr.image + address - 0x02000000
        return address

    def ram_address(self, address):
        """Returns the address in EWRAM of a ROM address in a program that runs from there, or `address` itself."""
        return 0x02000000 + address - self.addr.image if self.addr.image else address

    def _svc(self, uc, intno, user):
        raise SystemExit('the driver made a BIOS call')

    def _unmapped(self, uc, access, address, size, value, user):
        raise SystemExit('the driver accessed unmapped memory at %08x' % address)

    def _io_write(self, uc, access, address, size, value, user):
        if address in FIFO_DATA:
            return
        if any(lo <= address < hi for lo, hi in TRACED):
            value &= (1 << (8 * size)) - 1
            if address in (0x040000BC, 0x040000C8) and size == 4:
                # A DMA's source in a program that runs from EWRAM, as the ROM's address of the same data.
                value = self.rom_address(value)
            self.writes.append((address, size, value))

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
        songs = self.addr.songs
        if self.addr.loader:
            # The loader reads the file that holds the song first.
            program, file, song = locate(self.rom, song)
            assert program is self.addr
            self.call(self.addr.loader, (self.ram_address(file),))
            songs = file_counts(self.rom, file)[0]
        if song >= songs:
            self.channel_records = self.records(self.addr.effect_channels)
            self.call(self.addr.play_effect, (song - songs,))
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
    emu = DriverEmulator(rom, song)
    for w in emu.play(song):
        print(format_write(-1, w))
    for f in range(frames):
        for w in emu.frame():
            print(format_write(f, w))
        if not emu.playing():
            break


def channels(rom, song, frames):
    emu = DriverEmulator(rom, song)
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
    emu = DriverEmulator(rom, song)
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
