#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Logik State's sound driver (GBAModPlay) from a game under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the driver. The script calls the driver's init, the routine that
plays a module and the routines that set the three volumes: the music's to the volume that the game sets at its start,
and the two others to the full volume. Then,
each frame, it calls the routine that the game calls each frame: it points the DMA at the mix buffer that it filled a
frame earlier and mixes the other one (no BIOS, no video). The build that plays GBAMOD1 modules has no routines that
set the volumes: the script writes the game's options and variables as the game's code does (see Mod1Emulator). It
prints what the driver does:

    trace       after each frame, the player's variables (g) and each mixer channel's record (c0 to c7, its first
                0x3C bytes), in hex, when they have changed. A build with records of 0x54 bytes keeps a channel's
                lengths in words and has a pattern loop for each channel. Its records are printed in the layout of the
                build with records of 0x4C bytes, with the pattern loop's row and count after them. The GBAMOD1 build
                has 6 channels, and its variables are those of Mod1Emulator.lines().
    render      a WAV file of what the mixer plays: each frame's mix buffer, 8 bits, mono, at the mixer's rate

    driver_emu.py ROM trace SONG FRAMES
    driver_emu.py ROM render SONG FRAMES OUT.wav

SONG is the song's number in the list of `agbsf2 --info`. In a game with GBAMOD2 modules, a song is a module, in the
order of the modules' addresses, from its first order with the game's bank of samples. In a game with GBAMOD1 modules,
a song is a piece of a module: the module's order 0, or an order after one whose pattern has a jump, with no range of
orders. The routine and RAM addresses come from the ROM's game code. Those of the US releases of Aero the Acro-Bat
(AAOE) and Zapper (AZPE), and of the European release of Pinball Challenge Deluxe (APLP), are built in; see GAMES.
docs/logikstate.md lists the addresses of Aero's other releases.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_UNMAPPED, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
SIGNATURE = b'GBAMOD2.'
MODULE_CHANNELS = 0x08       # a word: the module's channels
MODULE_RATE = 0x14           # a word: the mixer's rate
MODULE_HEADER = 0x1AF8
CHANNELS = 8
CHANNEL_RECORD = 0x3C        # the part of a channel's record that the driver uses, in the build with short records
LONG_LOOP = 0x50             # in a long record, two halfwords: the pattern loop's row and count
FRAME_SAMPLES = 352          # a mix buffer's bytes
DMA1_SOURCE = 0x040000BC     # where the frame routine points the DMA of FIFO A


class AeroUsa:
    """AAOE (Aero the Acro-Bat: Rascal Rival Revenge, USA)."""
    init = 0x081034E8            # r0 is the memory for the driver's mixer code and two mix buffers
    play = 0x08103418            # plays the module at r0 from order r1 with the bank of samples at r2
    set_master_volume = 0x0810345C  # each of the three sets a volume to r0, up to 64
    set_music_volume = 0x08103468
    set_effects_volume = 0x08103474
    music_volume = 0x30          # the volume that the game sets at its start (0x08026CE6)
    frame = 0x08103480           # the routine that the game calls each frame
    bank = 0x0809B0C8            # the bank of samples that the game passes with every module
    memory = 0x03005D50          # what the game passes to the init
    speed = 0x03006888           # a word: the ticks of a row
    tempo = 0x0300688C           # a word
    loop_row = 0x03006894        # a halfword: the pattern loop's marker
    tick_rate = 0x03006898       # a word: the ticks a second at the tempo
    order = 0x030068A4           # a halfword
    place = 0x030068A8           # a halfword: the next row's first cell, in cells from the pattern's start
    row_samples = 0x030068AC     # a word: a row's length
    row_left = 0x030068B0        # a word: the samples up to the next row
    tick_samples = 0x030068B4    # a word: a tick's length
    tick_left = 0x030068B8       # a word: the samples up to the next tick
    channels = 0x030068C0        # 8 records
    channel_size = 0x4C
    broke = 0x03006B20           # a byte: a row's break has run
    tick = 0x03006B24            # a halfword: the ticks since the row
    arpeggio_tick = 0x03006B5C   # a halfword


class ZapperUsa:
    """AZPE (Zapper: One Wicked Cricket!, USA). The build with records of 0x54 bytes: the variables are in one
    structure at 0x03007130, and the tempo is a halfword. The driver has no marker of a pattern loop for the module."""
    init = 0x080E4780
    play = 0x080E466C
    set_master_volume = 0x080E46B8
    set_music_volume = 0x080E46C4
    set_effects_volume = 0x080E46D0
    music_volume = 0x20          # the default of the game's options (0x080C760A)
    frame = 0x080E46DC
    bank = 0x0805BFEC
    memory = 0x03000180
    speed = 0x03007170
    tempo = 0x030075F8
    tempo_size = 2
    loop_row = None
    tick_rate = 0x03007178
    order = 0x030075FC
    place = 0x03007600
    row_samples = 0x0300718C
    row_left = 0x03007188
    tick_samples = 0x03007198
    tick_left = 0x03007194
    channels = 0x0300733C
    channel_size = 0x54
    broke = 0x030075DC
    tick = 0x030071A0
    arpeggio_tick = 0x030071A2


class PinballEurope:
    """APLP (Pinball Challenge Deluxe, Europe). The build that plays GBAMOD1 modules: 4 channels of music and 2 of
    sound effects, records of 0x3C bytes, and a range of orders in the play routine's r2 and r3."""
    mod1 = True
    init = 0x08004234            # gets the memory of its mixer's code from the game's allocator
    load = 0x08004380            # loads the module at r1 into the state at r0
    play = 0x0800568C            # plays the module at r0 from order r1, with the range of orders r2 to r3
    frame = 0x080088D8           # the routine that the game's handler of the vertical blank calls
    data = (0x083E16D0, 0x02000000, 0x02000E54)  # the initialised data that the entry code copies: from, to, end
    # Where the RAM mixer reads its mask: a mirror of 0x0203FFFC. The entry code clears that word.
    mask = 0x02FFFFFC
    effects_state = 0x02002840   # the state that the game's main loads module 0 into, for the sound effects
    music_option = 0x02000434    # the music's volume in the game's options, for the start routine to copy
    effects_option = 0x02000438
    master_volume = 0x0200070C
    note_slide_scale = 0x020006F0
    music_volume = 63            # the default of the options: level 10, times 0x6666 >> 12 (0x08003960)
    # The scale of the slide to a note for each module, by its place among the modules: 8 for the title's module 1
    # (0x080037B6) and for modules 3 to 6 of tables 0 to 3 (0x080072E2), and 0x28 for the others.
    note_slide_scales = {1: 8, 3: 8, 4: 8, 5: 8, 6: 8}
    order = 0x02002B84           # a halfword
    place = 0x02002B88           # a halfword: the next row's first cell
    row_samples = 0x02002B8C     # a word
    row_left = 0x02002B90
    tick_samples = 0x02002B94
    tick_left = 0x02002B98
    tick = 0x02000700            # a halfword
    arpeggio_tick = 0x02000702   # a halfword
    broke = 0x02000718           # a byte
    first_order = 0x02000708     # halfwords: the range of orders
    last_order = 0x0200070A
    channels = 0x02002BA0        # 6 records of 0x3C bytes


GAMES = {b'AAOE': AeroUsa, b'AZPE': ZapperUsa, b'APLP': PinballEurope}
MOD1_SIGNATURE = b'GBAMOD1.'
MOD1_CHANNELS = 6
MOD1_RATE = 0x10             # a word: the mixer's rate
MOD1_ORDERS = 0x14           # a word: the count of orders
MOD1_PATTERN_BYTES = 0x08    # a word: the size of the patterns
MOD1_PATTERNS = 0x62C        # a word: the count of patterns
MOD1_ORDER_LIST = 0x630      # a byte an order: its pattern
MOD1_ROWS = 0x6B0            # a byte a pattern: its rows
MOD1_HEADER = 0x730


def addresses_for(rom):
    code = bytes(rom[0xAC:0xB0])
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


def modules(rom):
    """Returns the address of each module in the ROM, in order: the signature at a word's start, with a header that
    names up to 8 channels. The driver's code contains the signature too, with instructions after it."""
    found = []
    at = bytes(rom).find(SIGNATURE)
    while at >= 0:
        if at % 4 == 0 and at + MODULE_HEADER <= len(rom):
            if 1 <= struct.unpack_from('<I', rom, at + MODULE_CHANNELS)[0] <= CHANNELS:
                found.append(ROM_BASE + at)
        at = bytes(rom).find(SIGNATURE, at + 1)
    return found


def mod1_modules(rom):
    """Returns the address of each GBAMOD1 module in the ROM, in order: the signature at a word's start, with a header
    whose patterns fill the size that it names. The load routine contains the signature too, with other words after
    it."""
    found = []
    at = bytes(rom).find(MOD1_SIGNATURE)
    while at >= 0:
        if at % 4 == 0 and at + MOD1_HEADER <= len(rom):
            patterns = struct.unpack_from('<I', rom, at + MOD1_PATTERNS)[0]
            rows = sum(rom[at + MOD1_ROWS:at + MOD1_ROWS + patterns]) if 1 <= patterns <= 64 else -1
            if rows >= 0 and struct.unpack_from('<I', rom, at + MOD1_PATTERN_BYTES)[0] == rows * 16:
                found.append(ROM_BASE + at)
        at = bytes(rom).find(MOD1_SIGNATURE, at + 1)
    return found


def mod1_songs(rom):
    """Returns each song of a game with GBAMOD1 modules in the order of agbsf2's detection: (module's place among
    the modules, its address, the order to start at). A song starts at order 0 of each module, and after each order
    whose pattern has a jump (effect B)."""
    songs = []
    for n, module in enumerate(mod1_modules(rom)):
        m = module - ROM_BASE
        orders = struct.unpack_from('<I', rom, m + MOD1_ORDERS)[0]
        rows = rom[m + MOD1_ROWS:m + MOD1_ROWS + 64]
        starts = [sum(rows[:p]) * 16 for p in range(64)]
        songs.append((n, module, 0))
        for order in range(orders - 1):
            pattern = rom[m + MOD1_ORDER_LIST + order]
            cells = range(m + MOD1_HEADER + starts[pattern], m + MOD1_HEADER + starts[pattern] + rows[pattern] * 16, 4)
            if any(rom[cell + 3] == 0xB for cell in cells):
                songs.append((n, module, order + 1))
    return songs


class Emulator:
    """The memory map, and calls of the game's routines in Thumb code."""

    def __init__(self, rom):
        self.addr = addresses_for(rom)
        self.rom = rom
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        # Before the driver starts the timers, it waits while the halfword at 0xFCE2 reads 0x3E and then 0x3D. A GBA
        # has no memory there, and the wait ends at once. Memory of zeros there ends it too.
        uc.mem_map(0x00000000, 0x10000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self.uc = uc
        self.last = {}

    def _unmapped(self, uc, access, address, size, value, user):
        # For an unmapped access, Unicorn's emu_start() raises UcError ahead of any exception from this hook.
        # call() raises SystemExit with this message instead.
        self.fault = 'the driver accessed unmapped memory at %08x (pc %08x)' % (address, uc.reg_read(UC_ARM_REG_PC))
        return False

    def call(self, address, args=()):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | 0x20)  # system mode, Thumb
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for r in range(13):
            uc.reg_write(UC_ARM_REG_R0 + r, args[r] & 0xFFFFFFFF if r < len(args) else 0)
        self.fault = None
        try:
            uc.emu_start(address | 1, RETURN_TRAP, count=50_000_000)
        except UcError:
            if self.fault:
                raise SystemExit(self.fault) from None
            raise

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def write32(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

    def frame(self):
        self.call(self.addr.frame)

    def changed_lines(self, n, records):
        """Returns the trace's lines for frame n: the records that have changed since they were last printed."""
        lines = []
        for name, data in records.items():
            if self.last.get(name, bytes(len(data))) != data:
                self.last[name] = data
                lines.append('%d %s %s' % (n, name, data.hex()))
        return lines


class DriverEmulator(Emulator):
    """A build that plays GBAMOD2 modules."""

    def __init__(self, rom):
        super().__init__(rom)
        self.call(self.addr.init, (self.addr.memory,))

    def rate(self, song):
        return struct.unpack_from('<I', self.rom, modules(self.rom)[song] - ROM_BASE + MODULE_RATE)[0]

    def start(self, song):
        """Plays module `song` from its first order, at the game's volume for its music. The frame routine mixes
        nothing at its first call. That call only hands the DMA the buffer that the play routine cleared."""
        found = modules(self.rom)
        if not 0 <= song < len(found):
            raise SystemExit('the ROM has %d modules, and %d is none of them' % (len(found), song))
        self.call(self.addr.play, (found[song], 0, self.addr.bank))
        self.call(self.addr.set_master_volume, (64,))
        self.call(self.addr.set_music_volume, (self.addr.music_volume,))
        self.call(self.addr.set_effects_volume, (64,))
        self.call(self.addr.frame)

    def channel_record(self, c):
        """Returns channel c's record in the layout of the build with short records. A long record has three words
        for the length, the loop's start and the loop's length, and each later field 4 bytes further on."""
        a = self.addr
        record = self.read(a.channels + a.channel_size * c, a.channel_size)
        if a.channel_size == 0x4C:
            return record[:CHANNEL_RECORD]
        lengths = struct.unpack_from('<III', record, 0x0C)
        if max(lengths) > 0xFFFF:
            raise SystemExit('channel %d has a length of more than 16 bits' % c)
        return (record[:0x0C] + struct.pack('<HHHH', *lengths, 0) + record[0x18:0x18 + CHANNEL_RECORD - 0x14] +
                record[LONG_LOOP:LONG_LOOP + 4])

    def trace_lines(self, n):
        """Returns the trace's lines for frame n, after the frame has run."""
        a = self.addr
        tempo = self.read(a.tempo, getattr(a, 'tempo_size', 4)).ljust(4, b'\0')
        loop_row = self.read(a.loop_row, 2) if a.loop_row else b'\xff\xff'
        records = {'g': b''.join((self.read(a.order, 2), self.read(a.place, 2), self.read(a.speed, 4),
                                  tempo, self.read(a.tick_rate, 4), self.read(a.row_samples, 4),
                                  self.read(a.row_left, 4), self.read(a.tick_samples, 4), self.read(a.tick_left, 4),
                                  self.read(a.tick, 2), self.read(a.arpeggio_tick, 2), self.read(a.broke, 1),
                                  loop_row))}
        for c in range(CHANNELS):
            records['c%d' % c] = self.channel_record(c)
        return self.changed_lines(n, records)


class Mod1Emulator(Emulator):
    """The build that plays GBAMOD1 modules. Like the game's code, the script copies the initialised data to EWRAM,
    calls the init and loads module 0 into the state of the sound effects. The init gets the memory of its mixer's
    code from the game's allocator. To start a song, the script writes the music's volume to the options and the
    module's scale of the slide to a note, calls the play routine, and writes the master volume 64. The start routine
    copies the volume from the options."""

    def __init__(self, rom):
        super().__init__(rom)
        a = self.addr
        uc = self.uc
        source, to, end = a.data
        uc.mem_map(a.mask & ~0xFFF, 0x1000)
        uc.mem_write(to, bytes(rom[source - ROM_BASE:source - ROM_BASE + end - to]))
        self.write32(a.mask, 0)
        self.songs = mod1_songs(rom)
        self.call(a.init)
        self.call(a.load, (a.effects_state, mod1_modules(rom)[0]))

    def rate(self, song):
        return struct.unpack_from('<I', self.rom, self.songs[song][1] - ROM_BASE + MOD1_RATE)[0]

    def start(self, song):
        """Plays song `song` from its order with no range of orders. The frame routine mixes nothing at its first
        call."""
        if not 0 <= song < len(self.songs):
            raise SystemExit('the ROM has %d songs, and %d is none of them' % (len(self.songs), song))
        a = self.addr
        place, module, order = self.songs[song]
        self.write32(a.music_option, a.music_volume)
        self.write32(a.effects_option, a.music_volume)
        self.write32(a.note_slide_scale, a.note_slide_scales.get(place, 0x28))
        self.call(a.play, (module, order, -1, -1))
        self.write32(a.master_volume, 64)
        self.call(a.frame)

    def trace_lines(self, n):
        """Returns the trace's lines for frame n, after the frame has run: the order, the place, a row's samples and
        those up to the next row, a tick's samples and those up to the next tick, the tick, the arpeggio's tick, the
        break's flag and the range of orders, then the 6 channel records."""
        a = self.addr
        records = {'g': b''.join((self.read(a.order, 2), self.read(a.place, 2), self.read(a.row_samples, 4),
                                  self.read(a.row_left, 4), self.read(a.tick_samples, 4), self.read(a.tick_left, 4),
                                  self.read(a.tick, 2), self.read(a.arpeggio_tick, 2), self.read(a.broke, 1),
                                  self.read(a.first_order, 2), self.read(a.last_order, 2)))}
        for c in range(MOD1_CHANNELS):
            records['c%d' % c] = self.read(a.channels + CHANNEL_RECORD * c, CHANNEL_RECORD)
        return self.changed_lines(n, records)


def emulator(rom):
    """Returns the emulator of the game's build of the driver."""
    return Mod1Emulator(rom) if getattr(addresses_for(rom), 'mod1', False) else DriverEmulator(rom)


def trace(rom, song, frames):
    emu = emulator(rom)
    emu.start(song)
    for n in range(frames):
        emu.frame()
        for line in emu.trace_lines(n):
            print(line)


def render(rom, song, frames, path):
    """Writes what the mixer plays in `frames` frames: after each call of the frame routine, the buffer that the DMA
    now plays. The call before mixed it. A WAV file's 8-bit points are unsigned."""
    emu = emulator(rom)
    emu.start(song)
    points = bytearray()
    for _ in range(frames):
        emu.frame()
        buffer = struct.unpack('<I', emu.read(DMA1_SOURCE, 4))[0]
        points += bytes(b ^ 0x80 for b in emu.read(buffer, FRAME_SAMPLES))
    with wave.open(path, 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(1)
        out.setframerate(emu.rate(song))
        out.writeframes(bytes(points))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('mode', choices=('trace', 'render'))
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?', help='the WAV file of render')
    a = p.parse_args()
    if a.mode == 'render':
        if not a.out:
            p.error('render takes the WAV file to write')
        render(load_rom(a.rom), a.song, a.frames, a.out)
    else:
        trace(load_rom(a.rom), a.song, a.frames)


if __name__ == '__main__':
    main()
