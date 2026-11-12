#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Logik State's later sound drivers (GBAModPlay 2002 and 2003, LS_Play 2003) from a game under the Unicorn ARM
emulator. These drivers play GBAMOD30 modules.

This is the reference for checking agbsf2's model of the drivers. The script calls the driver's routines as the
game's code does: the init with the game's bank, rate and sound effect channels, the three volumes and the loop flag
with the game's values, the routine that plays a module, and each frame the routines that the game calls each frame.
It runs no game code and no BIOS. It handles the BIOS calls that the drivers make (division, the copies and the LZ77
unpacking). It prints what the driver does:

    trace       after each frame, the player's variables (g) and each channel's record (c0 on), in hex, when they
                have changed. The variables are in a fixed order for every build: the order, the row, the speed, the
                tempo, the ticks a second, a row's length and the samples left of it, a tick's length and the samples
                left of it, the ticks of the row, the arpeggio's ticks, the break's flag, the playing flag and the
                fade. A record has the layout of the game's build, with the fields that agbsf2's model keeps and zeros
                elsewhere.

    driver_emu_mod30.py ROM trace SONG FRAMES

SONG is the song's number in the game's order, as `agbsf2 --info` lists them. The routine and RAM addresses come from
each game's code. See GAMES.
"""
import argparse
import struct
import sys
from pathlib import Path

from unicorn import (UC_ARCH_ARM, UC_HOOK_BLOCK, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM,
                     Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R3, UC_ARM_REG_SP)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
TRANSLATIONS = 32 << 20  # the bytes of Unicorn's buffer of translated code
STACK_TOP = 0x03007E00
SIGNATURE = b'GBAMOD30'

# The player's variables of the trace, with their sizes.
GLOBALS = (('order', 4), ('row', 4), ('speed', 4), ('tempo', 4), ('hz', 4), ('row_length', 4), ('row_left', 4),
           ('tick_length', 4), ('tick_left', 4), ('tick', 2), ('arpeggio_tick', 2), ('broke', 1), ('playing', 2),
           ('fade', 4))

# The fields of a channel's record that the model keeps: (offset, size) in each layout. Every layout starts with seven
# words: the points, the position, the step, the length, the loop's start and length, and the volume.
WORDS = [(0x00, 4), (0x04, 4), (0x08, 4), (0x0C, 4), (0x10, 4), (0x14, 4), (0x18, 4)]
RECORD_FIELDS = {
    # 0x80 bytes (group A): the relative note to the target, the pattern loop, the packed flag, the linear slide and
    # its operand, the volume column.
    'A': WORDS + [(0x30, 2), (0x34, 2), (0x38, 2), (0x3A, 2), (0x3C, 4)] + [(o, 2) for o in range(0x40, 0x58, 2)] +
         [(0x68, 2), (0x6A, 2), (0x6C, 2), (0x70, 4), (0x74, 4), (0x78, 4)],
    # 0x90 bytes (group B): as A with the vibrato's wave type at 0x50 left out, then the linear vibrato, the linear
    # period and the linear target.
    'B': WORDS + [(0x30, 2), (0x34, 2), (0x38, 2), (0x3A, 2), (0x3C, 4)] + [(o, 2) for o in range(0x40, 0x50, 2)] +
         [(o, 2) for o in range(0x52, 0x5A, 2)] +
         [(0x6C, 2), (0x6E, 2), (0x70, 2), (0x74, 4), (0x78, 4), (0x7C, 4), (0x80, 4), (0x88, 4), (0x8C, 4)],
    # 0x94 bytes (groups C, C' and D).
    'C': WORDS + [(0x38, 2), (0x3C, 2), (0x40, 2), (0x42, 2), (0x44, 4)] + [(o, 2) for o in range(0x48, 0x58, 2)] +
         [(o, 2) for o in range(0x5A, 0x62, 2)] +
         [(0x74, 2), (0x76, 2), (0x78, 2), (0x7C, 4), (0x80, 4), (0x84, 4), (0x88, 4), (0x90, 4)],
    # 0x98 bytes (groups E1, E2 and E3).
    'E': WORDS + [(0x3C, 2), (0x40, 2), (0x44, 2), (0x46, 2), (0x48, 4)] + [(o, 2) for o in range(0x4C, 0x5C, 2)] +
         [(o, 2) for o in range(0x5E, 0x66, 2)] +
         [(0x78, 2), (0x7A, 2), (0x7C, 2), (0x80, 4), (0x84, 4), (0x88, 4), (0x8C, 4), (0x94, 4)],
}
RECORD_SIZE = {'A': 0x80, 'B': 0x90, 'C': 0x94, 'E': 0x98}


def lz77(read, source):
    """Unpacks the BIOS's LZ77 data at `source`. `read(address, size)` returns bytes."""
    header = struct.unpack('<I', read(source, 4))[0]
    size = header >> 8
    data = read(source + 4, size * 2 + 16)
    out = bytearray()
    at = 0
    while len(out) < size:
        flags = data[at]
        at += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                v = data[at] << 8 | data[at + 1]
                at += 2
                back = (v & 0xFFF) + 1
                for _ in range((v >> 12) + 3):
                    out.append(out[-back])
            else:
                out.append(data[at])
                at += 1
    return bytes(out[:size])


MODULE = 'module'

# Where the harness puts what a game allocates at run time: the init's block, the state block and the heap.
BLOCK = 0x03007F00
STATE = 0x02030000
HEAP = 0x03000000


class Game:
    """A game's calls of the driver and where the driver keeps its variables. A step of `setup` is
    ('call', address, arguments, stack words), ('copy', destination, ROM address, size) for the game's initialised
    data that the driver reads, or ('words', destination, words) for a block that the game builds at run time. `play`
    holds the calls that play a module, with MODULE for its address and LOOP for the song's loop flag."""
    layout = 'E'
    records = 12
    setup = ()
    play = ()
    frame = ()
    song_table = 0  # the game's pointers to its modules, in the order of its songs, or 0 for the modules in order
    song_count = 0
    no_loop = ()  # the songs that the game plays with the loop flag at 0
    aligned_stores = False  # the driver patches its code in IWRAM with a word store to an address that isn't aligned

    def globals(self, emu):
        """Returns the address of each variable of GLOBALS."""
        raise NotImplementedError

    def records_base(self, emu):
        raise NotImplementedError


LOOP = 'loop'


class ModPlay(Game):
    """A GBAModPlay build (groups A, B and C) with its variables at fixed addresses: G is the state block, and the
    other variables are at the addresses of `fixed`."""
    state = 0
    fixed = {}
    offsets = {'speed': 0x54, 'hz': 0x60, 'row_length': 0x78, 'row_left': 0x74, 'tick_length': 0x88, 'tick_left': 0x84,
               'tick': 0x90, 'arpeggio_tick': 0x92}

    def globals(self, emu):
        where = {name: self.state + offset for name, offset in self.offsets.items()}
        where.update(self.fixed)
        return where

    def records_base(self, emu):
        return self.state + 0x8BC


class Stuntman(ModPlay):
    """AUXE (Stuntman, USA). GBAModPlay 2002, group B. The game's code is ARM and calls ARM veneers of these Thumb
    routines: the init at 0x0832A6A8, the play wrapper 0x0832A6F4 and the frame wrapper 0x0832A6DC."""
    layout = 'B'
    records = 8
    state = 0x0203BEE0
    fixed = {'order': 0x0203BED0, 'row': 0x0203D0D4, 'tempo': 0x0203BC00, 'broke': state + 0x11BC,
             'playing': state + 0x11D4, 'fade': 0x0203BED4}
    setup = (('call', 0x083F10DC, (0x03006D00, 0x083D2668, 0x0832A7E8, 0), (3,)),  # init: heap, bank, sfx bank, rate
             ('call', 0x083F0C58, (0x40,), ()),  # master volume
             ('call', 0x083F0C64, (0x40,), ()),  # music volume
             ('call', 0x083F0C70, (0x50,), ()),  # sound effects' volume
             ('call', 0x083F1740, (1,), ()),     # loop flag
             ('call', 0x083F0C4C, (), ()))       # sound on
    play = (('call', 0x083F0BDC, (MODULE,), ()),)
    frame = (0x083F0C88, 0x083F0D94, 0x083F0DF4)
    song_table = 0x0832A650
    song_count = 5


class AZ8E(ModPlay):
    """AZ8E (USA). GBAModPlay 2002, group A, with 10 records of 0x80 bytes. The init wrapper 0x0802EC28 calls the init
    with the heap, the bank, the sound effects' bank and rate index 4. The volumes come from the options' defaults
    (bytes at 0x0835C3F8): music and sound effects 5 x 18. The build has no loop flag. The play wrapper 0x0802ECB4
    plays the songs of the table at 0x087E5D5C. The start-up copies the step tables' pointers to 0x03007994 from
    0x087E7270."""
    layout = 'A'
    records = 10
    state = 0x02001518
    fixed = {'order': 0x030073B0, 'row': 0x030073B4, 'tempo': 0x030073A8, 'broke': state + 0x12BC,
             'playing': state + 0x12D4, 'fade': 0x030073AC}
    setup = (('copy', 0x03007994, 0x087E7270, 9 * 4),
             ('call', 0x08348AFC, (0x03000000, 0x080E4B00, 0x08132C00, 4), ()),
             ('call', 0x08348710, (0x5A,), ()),
             ('call', 0x0834871C, (0x5A,), ()),
             ('call', 0x08348704, (0x40,), ()),
             ('call', 0x083486F8, (), ()))
    play = (('call', 0x08348688, (MODULE,), ()),)
    frame = (0x08348734, 0x0834880C, 0x0834886C)
    song_table = 0x087E5D5C
    song_count = 24


class AZ8ERev1(AZ8E):
    """AZ8E revision 1: the code of group A moved, the state 4 bytes later and the IWRAM variables 0x10 later. The init
    wrapper 0x0802F2F0, the play wrapper 0x0802F46C and the table at 0x087E6654. The step tables' pointers are at
    0x030079A4, from 0x087E7B68."""
    state = 0x0200151C
    fixed = {'order': 0x030073C0, 'row': 0x030073C4, 'tempo': 0x030073B8, 'broke': state + 0x12BC,
             'playing': state + 0x12D4, 'fade': 0x030073BC}
    setup = (('copy', 0x030079A4, 0x087E7B68, 9 * 4),
             ('call', 0x0834937C, (0x03000000, 0x080E52B0, 0x081333B0, 4), ()),
             ('call', 0x08348F90, (0x5A,), ()),
             ('call', 0x08348F9C, (0x5A,), ()),
             ('call', 0x08348F84, (0x40,), ()),
             ('call', 0x08348F78, (), ()))
    play = (('call', 0x08348F08, (MODULE,), ()),)
    frame = (0x08348FB4, 0x0834908C, 0x083490EC)
    song_table = 0x087E6654


class ModPlay2003(ModPlay):
    """A GBAModPlay 2003 build with layout C (groups C and C'): 8 records of 0x94 bytes."""
    layout = 'C'
    records = 8

    def globals(self, emu):
        g = self.state
        where = {name: g + offset for name, offset in self.offsets.items()}
        where.update({'order': g + 0x17D0, 'row': g + 0x17CC, 'tempo': g + 0x17C0, 'broke': g + 0x11FC,
                      'playing': g + 0x1214, 'fade': g + 0x17C4})
        return where


def mod_play_2003(init, volumes, loop, sound_on, heap, state, bank, sfx_bank, rate, sfx_channels, levels):
    """The setup of group C: init(heap, heap size, state, state size, [sp] bank, sfx bank, rate index, sfx
    channels)."""
    return (('call', init, (heap, 0x200, state, 0x1800), (bank, sfx_bank, rate, sfx_channels)),
            ('call', volumes, (levels[0],), ()), ('call', volumes + 0x10, (levels[1],), ()),
            ('call', volumes + 0x20, (levels[2],), ()), ('call', loop, (1,), ()), ('call', sound_on, (), ()))


class BMHE(ModPlay2003):
    """BMHE (USA). Group C. The routine 0x081E1AAC calls the init with the values below, sets the volumes 0x40, 0x46
    and 0x60 and the loop flag 1. Its play routine 0x081E1E74 plays 12 modules from a jump table, in their order in
    the ROM. Each song sets the music volume to 23 times an option plus 1: 0x45 at the game's start."""
    state = 0x020061C0
    setup = mod_play_2003(0x081F1A38, 0x081F1500, 0x081F2190, 0x081F14F4, 0x03000D00, state, 0x081690D8,
                          0x08000274, 0, 4, (0x40, 0x45, 0x60))
    play = (('call', 0x081F1478, (MODULE,), ()),)
    frame = (0x081F154C, 0x081F1674, 0x081F1710)


class AZFE(ModPlay2003):
    """AZFE (USA). Group C: BMHE's driver 0x13FB68 bytes lower. The routine 0x0809CC44 allocates the state at run time
    and calls the init. The volumes are 0x40, the levels 8 << 3. The play routine 0x0809CE34 plays the songs of the
    table at 0x083F253C."""
    state = STATE
    setup = mod_play_2003(0x080B1ED0, 0x080B1998, 0x080B2628, 0x080B198C, 0x03006220, state, 0x08031F08,
                          0x08000210, 0, 3, (0x40, 0x40, 0x40))
    play = (('call', 0x080B1910, (MODULE,), ()),)
    frame = (0x080B19E4, 0x080B1B0C, 0x080B1BA8)
    song_table = 0x083F253C
    song_count = 7


class BLXP(ModPlay2003):
    """BLXP (Europe). Group C'. The game's code is ARM. The init wrapper 0x0867DC48 calls the init with the block in
    ROM at 0x0867DBE8 (mixer mode 1), rate index 0 and 3 sound effect channels. Then the volumes 0x50, 0x40 and 0x40,
    the loop flag 1 and the sound on. The play routine 0x0867DC88 runs the init wrapper again and requests the song
    from the table at 0x0867DBB0. The interrupt calls the DMA switch, the mix and the flip."""
    layout = 'C'
    state = 0x0203E800
    setup = (('call', 0x087ED2D0, (0x0867DBE8, 0, 3), ()),
             ('call', 0x087ECD60, (0x50,), ()), ('call', 0x087ECD70, (0x40,), ()), ('call', 0x087ECD80, (0x40,), ()),
             ('call', 0x087EDA5C, (1,), ()), ('call', 0x087ECD54, (), ()))
    play = (('call', 0x087ECCB0, (MODULE,), ()),)
    frame = (0x087ECDAC, 0x087ECED4, 0x087ECFB8)
    song_table = 0x0867DBB0
    song_count = 11
    aligned_stores = True


class LsPlay(Game):
    """An LS_Play 2003 build (groups D, E1, E2 and E3). G, the state block, and P, the song record, are behind two
    pointers. P's fields are at the same offsets in every build."""
    state_pointer = 0
    song_pointer = 0
    playing = 0x1BBC  # G's playing flag
    fade = 0x50

    def globals(self, emu):
        g = emu.u32(self.state_pointer)
        p = emu.u32(self.song_pointer)
        return {'order': p + 0x44, 'row': p + 0x40, 'speed': p + 0x14, 'tempo': p + 0x3C, 'hz': p + 0x1C,
                'row_length': p + 0x24, 'row_left': p + 0x20, 'tick_length': p + 0x30, 'tick_left': p + 0x2C,
                'tick': p + 0x38, 'arpeggio_tick': p + 0x3A, 'broke': p + 0x4C, 'playing': g + self.playing,
                'fade': g + self.fade}

    def records_base(self, emu):
        return emu.u32(self.song_pointer) + 0x54


class ArcticTale(LsPlay):
    """BB5E (Arctic Tale, USA). LS_Play 2003, group E2. The game's init routine 0x08000B64 calls the init with the
    block at 0x03006EDC. The game's start-up copies the block from ROM with the rest of its IWRAM data (0x03006EB4 from
    0x08271FF0, the step tables' pointers at 0x0300712C included). The play wrapper 0x08000C18 sets the loop flag and
    requests the module. The vertical blank calls 0x08000BEC (DMA switch), and the main loop 0x08000C08 (mix, flip)."""
    state_pointer = 0x03004F90
    song_pointer = 0x03004F94
    setup = (('copy', 0x03006EB4, 0x08271FF0, 0x400),
             ('call', 0x080DA010, (0x03006EDC, 4, 4, 2), ()),  # init: block, rate index, sfx channels, output mode
             ('call', 0x080D9968, (0x50,), ()),
             ('call', 0x080D997C, (0x40,), ()),
             ('call', 0x080D9990, (0x40,), ()),
             ('call', 0x080D995C, (), ()))
    play = (('call', 0x080DA6EC, (LOOP,), ()), ('call', 0x080D97F0, (MODULE,), ()))
    frame = (0x080D99F0, 0x080D9BA8, 0x080D9CE0)
    song_table = 0x08271FF0
    song_count = 10


def ls_play(code, init, request, frame, volumes, loop, sound_on, init_args, block, levels, pointers, copies=(),
            songs=(0, 0), no_loop=(), playing=0x1BBC, fade=0x50, layout='E', records=12):
    """Returns the class of an LS_Play game. `init_args` are the init's arguments after the block: the rate index, the
    sound effect channels and, in groups E1 and E2, the output mode. `block` is the init's block, (address, words), or
    (address, None) for a block that the game holds in ROM or copies. `levels` are the master, music and sound effect
    volumes after the init, with None for one that the game doesn't set there. `pointers` are G's and P's pointers.
    `copies` are (address, ROM image, size) of the data and code that the game's start-up copies and the driver uses.
    `songs` is
    the game's table of modules (address, count), or (0, 0) for the modules in their order in the ROM."""
    setup = [('copy',) + tuple(c) for c in copies]
    if block[1] is not None:
        setup.append(('words', block[0], block[1]))
    setup.append(('call', init, (block[0],) + tuple(init_args), ()))
    for i, level in enumerate(levels):
        if level is not None:
            setup.append(('call', volumes + 0x14 * i, (level,), ()))
    setup.append(('call', sound_on, (), ()))
    attributes = {'__doc__': '%s. LS_Play.' % code, 'state_pointer': pointers[0], 'song_pointer': pointers[1],
                  'setup': tuple(setup), 'play': (('call', loop, (LOOP,), ()), ('call', request, (MODULE,), ())),
                  'frame': tuple(frame), 'song_table': songs[0], 'song_count': songs[1], 'no_loop': tuple(no_loop),
                  'playing': playing, 'fade': fade, 'layout': layout, 'records': records}
    return type(code, (LsPlay,), attributes)


def block(heap, heap_size, state, bank, sfx_bank, mode, state_size=0x2000):
    """An init block: the heap, the state block, the bank, the sound effects' bank and the mixer mode."""
    return (heap, heap_size, state, state_size, bank, sfx_bank, mode)


# The LS_Play games: the routines of the driver that each game calls, the init's arguments and the game's volumes,
# from the games' code (the init callers, the volume routines and the play wrappers). A block, heap or state that the
# game allocates at run time is at BLOCK, HEAP and STATE here.
LS_PLAY = [
    # Group D. The init wrapper 0x080A6DA4 fills the block at 0x02005094. The volume routine 0x080A6D6C sets 0x64,
    # 0x3F and 0x50. The song routine 0x080A7244 plays 11 modules in their order in the ROM, songs 6 and 7 with the
    # loop flag at 0.
    ls_play('BKNE', 0x080AE35C, 0x080ADC5C, (0x080ADE2C, 0x080ADF24, 0x080AE038), 0x080ADDD4, 0x080AE960, 0x080ADDC8,
            (4, 4), (0x02005094, block(0x03000D00, 0x6AC, 0x02003094, 0x08058814, 0x080001A4, 1)), (0x40, 0x3F, 0x50),
            (0x02007C64, 0x02007C70), no_loop=(6, 7), playing=0x1B48, fade=0x3C, layout='C'),
    # Group E1.
    ls_play('BNFE', 0x081366E8, 0x08135F60, (0x08136130, 0x0813629C, 0x081363C4), 0x081360D8, 0x08136D28, 0x081360CC,
            (0, 4, 2), (BLOCK, block(0x03005B10, 0x54C, STATE, 0x080431FC, 0x08000210, 0)), (0x40, 0x24, 0x40),
            (0x030065D0, 0x030065DC), songs=(0x0877B834, 9)),
    ls_play('BD6E', 0x081EF68C, 0x081EEEE8, (0x081EF0B8, 0x081EF228, 0x081EF360), 0x081EF060, 0x081EFD68, 0x081EF054,
            (4, 2, 1), (BLOCK, block(HEAP, 0x6AC, STATE, 0x080BDB90, 0x080003B4, 0)), (0x40, 0x50, 0x80),
            (0x03005844, 0x03005850), songs=(0x08201A00, 18), no_loop=(3, 4, 5)),
    # BHEE's driver clears memory with the game's routine 0x080EC674. That routine calls the game's code in IWRAM at
    # 0x03000208. The start-up copies that code from 0x0879AC68 (DMA 3 at 0x08000104). The game allocates the heap from
    # IWRAM at 0x03001544.
    ls_play('BHEE', 0x08146880, 0x081460DC, (0x081462AC, 0x0814641C, 0x08146554), 0x08146254, 0x08146F5C, 0x08146248,
            (0, 2, 2), (BLOCK, block(0x03001544, 0x360, STATE, 0x080573D8, 0x08000198, 0)), (0x3F, 0x3F, 0x3F),
            (0x0300242C, 0x03002438), copies=((0x030000E0, 0x0879AC68, 0x126C),), songs=(0x0879B9FC, 9)),
    ls_play('BP4P', 0x0831DAAC, 0x0831D308, (0x0831D4D8, 0x0831D648, 0x0831D780), 0x0831D480, 0x0831E188, 0x0831D474,
            (4, 4, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x082FB134, 0x082D6988, 1)), (0x3F, 0x20, 0x3F),
            (0x03004A00, 0x03004A0C), songs=(0x083C5890, 2)),
    ls_play('BILE', 0x0804AF84, 0x0804A7E0, (0x0804A9B0, 0x0804AB20, 0x0804AC58), 0x0804A958, 0x0804B660, 0x0804A94C,
            (4, 4, 2), (BLOCK, block(0x03000E18, 0x6AC, 0x0200497C, 0x080C9280, 0x080565B0, 1)), (0x64, 0x32, 0x50),
            (0x0200900C, 0x02009018)),
    # Group E2.
    ls_play('BP5P', 0x082FE25C, 0x082FDA3C, (0x082FDC3C, 0x082FDDF4, 0x082FDF2C), 0x082FDBB4, 0x082FE938, 0x082FDBA8,
            (4, 4, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x082D7530, 0x082B1E30, 1)), (0x3F, 0x20, 0x3F),
            (0x03004A00, 0x03004A0C), songs=(0x083EF3C4, 2)),
    ls_play('BDUE', 0x08335768, 0x08334F48, (0x08335148, 0x08335300, 0x08335438), 0x083350C0, 0x08335E44, 0x083350B4,
            (4, 2, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x0810E3F8, 0x0800042C, 0)), (0x40, 0x50, 0x80),
            (0x03005F30, 0x03005F3C), songs=(0x08B28BC0, 22), no_loop=(3, 4, 5)),
    ls_play('BTUE', 0x08255F94, 0x08255774, (0x08255974, 0x08255B2C, 0x08255C64), 0x082558EC, 0x08256670, 0x082558E0,
            (4, 4, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x0817A890, 0x08033928, 0)), (0x50, 0x40, 0x78),
            (0x03004B24, 0x03004B30), songs=(0x087ED4A4, 14)),
    ls_play('BAHP', 0x087CBE94, 0x087CB674, (0x087CB874, 0x087CBA2C, 0x087CBB64), 0x087CB7EC, 0x087CC570, 0x087CB7E0,
            (4, 4, 2), (BLOCK, block(0x03001B38, 0x6AC, 0x02000000, 0x08693E68, 0x086FF3BC, 1)), (0x3F, 0x50, 0x64),
            (0x030019D0, 0x030019DC), songs=(0x087D8C74, 7)),
    ls_play('BG9E', 0x0811AD90, 0x0811A570, (0x0811A770, 0x0811A928, 0x0811AA60), 0x0811A6E8, 0x0811B46C, 0x0811A6DC,
            (4, 4, 2), (BLOCK, block(0x03000C4C, 0x6AC, STATE, 0x080D42C8, 0x080474E4, 1)), (0x40, 0x30, 0x40),
            (0x0203FE4C, 0x0203FE58), songs=(0x083F8AE0, 15)),
    ls_play('BAWE', 0x08028DB8, 0x08028598, (0x08028798, 0x08028950, 0x08028A88), 0x08028710, 0x08029494, 0x08028704,
            (4, 4, 2), (BLOCK, block(0x03000E18, 0x6AC, 0x0200266C, 0x08097A0C, 0x08034554, 1)), (0x64, 0x32, 0x50),
            (0x02008244, 0x02008250), no_loop=(6, 7)),
    ls_play('B2FE', 0x080B1880, 0x080B1060, (0x080B1260, 0x080B1418, 0x080B1550), 0x080B11D8, 0x080B1F5C, 0x080B11CC,
            (4, 4, 2), (BLOCK, block(0x03001E00, 0x6AC, 0x02000000, 0x08047FF0, 0x08017000, 0)), (0x50, 0x40, 0x50),
            (0x0300504C, 0x03005050), songs=(0x0826C4E4, 5)),
    ls_play('BNKE', 0x080E3FDC, 0x080E37BC, (0x080E39BC, 0x080E3B74, 0x080E3CAC), 0x080E3934, 0x080E46B8, 0x080E3928,
            (4, 4, 2), (BLOCK, block(0x03001B20, 0x360, STATE, 0x0809B2CC, 0x0802001C, 0)), (0x3F, 0x3F, 0x3F),
            (0x030024DC, 0x030024E8), songs=(0x080F01AC, 11)),
    ls_play('B2LE', 0x082B0FCC, 0x082B07AC, (0x082B09AC, 0x082B0B64, 0x082B0C9C), 0x082B0924, 0x082B16A8, 0x082B0918,
            (4, 4, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x081B3598, 0x080349E0, 0)), (0x50, 0x20, 0x40),
            (0x03005244, 0x03005250), songs=(0x087A8A10, 11)),
    ls_play('B2NE', 0x0828DD5C, 0x0828D53C, (0x0828D73C, 0x0828D8F4, 0x0828DA2C), 0x0828D6B4, 0x0828E438, 0x0828D6A8,
            (4, 2, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x080925E0, 0x08000FB4, 0)), (0x40, 0x20, 0x30),
            (0x03004754, 0x03004760), songs=(0x08DB1174, 17)),
    ls_play('BYLP', 0x0823391C, 0x082330FC, (0x082332FC, 0x082334B4, 0x082335EC), 0x08233274, 0x08233FF8, 0x08233268,
            (4, 4, 2), (BLOCK, block(HEAP, 0x6AC, STATE, 0x081213CC, 0x0803F488, 0)), (0x50, 0x40, 0x78),
            (0x03004F60, 0x03004F6C), songs=(0x085E2378, 16)),
    # Group E3: 8 records, and an init without the output mode.
    ls_play('B3RE', 0x087D9484, 0x087D8C64, (0x087D8E34, 0x087D901C, 0x087D9154), 0x087D8DDC, 0x087D9B58, 0x087D8DD0,
            (0, 2), (0x0877A104, None), (0x40, 0x40, 0x40), (0x0203D2E0, 0x0203D2EC), songs=(0x0877A0D0, 10),
            playing=0x143C, records=8),
    ls_play('BNWE', 0x08140018, 0x0813F7F8, (0x0813F9C8, 0x0813FBB0, 0x0813FCE8), 0x0813F970, 0x081406EC, 0x0813F964,
            (0, 4), (BLOCK, block(0x03005B20, 0x54C, STATE, 0x08043E30, 0x08000210, 1)), (0x40, 0x3F, 0x3F),
            (0x030063A0, 0x030063AC), songs=(0x087F4368, 5), playing=0x143C, records=8),
    ls_play('BN7E', 0x08151F78, 0x08151758, (0x08151928, 0x08151B10, 0x08151C48), 0x081518D0, 0x0815264C, 0x081518C4,
            (0, 4), (BLOCK, block(0x03005A00, 0x54C, STATE, 0x08050FD4, 0x08000210, 1)), (0x40, 0x3F, 0x3F),
            (0x03006370, 0x0300637C), songs=(0x087EE238, 5), playing=0x143C, records=8),
]

GAMES = {b'AUXE': Stuntman, b'AZ8E': AZ8E, b'BMHE': BMHE, b'AZFE': AZFE, b'BLXP': BLXP,
         b'BB5E': ArcticTale}
GAMES.update({game.__name__.encode(): game for game in LS_PLAY})


def game_for(rom):
    code = bytes(rom[0xAC:0xB0])
    if code not in GAMES:
        raise SystemExit('no driver addresses known for game code %s' % code.decode('latin-1'))
    game = GAMES[code]
    # AZ8E's revision 1 has the version byte 1.
    if code == b'AZ8E' and rom[0xBC] == 1:
        game = AZ8ERev1
    return game()


def modules(rom):
    """Returns the addresses of the GBAMOD30 modules in the ROM: the signature at a word's start, with 1 to 16
    channels. The drivers' code holds the signature too."""
    data = bytes(rom)
    found = []
    at = data.find(SIGNATURE)
    while at >= 0:
        if at % 4 == 0 and 1 <= struct.unpack_from('<I', data, at + 0x28)[0] <= 16:
            found.append(ROM_BASE + at)
        at = data.find(SIGNATURE, at + 1)
    return found


def songs(rom, game):
    """Returns the module of each song: the game's table of pointers, then the modules that the table doesn't name, in
    their order in the ROM."""
    listed = list(struct.unpack_from('<%dI' % game.song_count, rom, game.song_table - ROM_BASE)) if game.song_table \
        else []
    return listed + [m for m in modules(rom) if m not in listed]


class DriverEmulator:
    def __init__(self, rom):
        self.game = game_for(rom)
        self.rom = rom
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        # The mixers of mixer mode 1 patch their code at each chunk. Unicorn translates the code again each time, and
        # keeps the old translations until its buffer fills. 1 GB by default.
        uc.ctl_set_tcg_buffer_size(TRANSLATIONS)
        # The drivers wait while the halfword at 0x04000000 holds 0x3E and then 0x3D. Zeros end the wait at once.
        uc.mem_map(0x00000000, 0x10000)
        uc.mem_map(0x02000000, 0x40000)
        # The mixers load a word 0x3C bytes before their copy and never use it. Before a copy at 0x03000000, the word
        # is in EWRAM's mirror.
        uc.mem_map(0x02FFF000, 0x1000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._bios)
        if self.game.aligned_stores:
            self.store = None
            uc.hook_add(UC_HOOK_MEM_WRITE, self._word_store, begin=0x03000000, end=0x03007FFF)
            uc.hook_add(UC_HOOK_BLOCK, self._align_store)
        self.uc = uc
        self.last = {}
        self.run(self.game.setup)

    def _unmapped(self, uc, access, address, size, value, user):
        # For an unmapped access, Unicorn's emu_start() raises UcError ahead of any exception from this hook.
        # call() raises SystemExit with this message instead.
        self.fault = 'the driver accessed unmapped memory at %08x (pc %08x)' % (address, uc.reg_read(UC_ARM_REG_PC))
        return False

    def _word_store(self, uc, access, address, size, value, user):
        # The ARM7TDMI stores a word at an address that isn't a multiple of 4 at the word's start. Unicorn stores it at
        # the address. The next block puts it where the ARM7TDMI does.
        if size == 4 and address & 3:
            self.store = (address, bytes(uc.mem_read(address, 4)), value)

    def _align_store(self, uc, address, size, user):
        if self.store:
            at, old, value = self.store
            self.store = None
            uc.mem_write(at, old)
            uc.mem_write(at & ~3, struct.pack('<I', value & 0xFFFFFFFF))

    def _bios(self, uc, number, user):
        """Runs the BIOS call that the SWI before the PC names: the signed division (6 and 7), the copies (0xB and
        0xC) and the LZ77 unpacking (0x11 and 0x12)."""
        pc = uc.reg_read(UC_ARM_REG_PC)
        if uc.reg_read(UC_ARM_REG_CPSR) & 0x20:
            call = struct.unpack('<H', uc.mem_read(pc - 2, 2))[0] & 0xFF
        else:
            call = (struct.unpack('<I', uc.mem_read(pc - 4, 4))[0] >> 16) & 0xFF
        r = [uc.reg_read(UC_ARM_REG_R0 + i) for i in range(3)]
        signed = [v - (1 << 32) if v & 0x80000000 else v for v in r]
        if call in (6, 7):
            a, b = (signed[0], signed[1]) if call == 6 else (signed[1], signed[0])
            q = 0 if b == 0 else abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
            m = a - q * b
            uc.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R1, m & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R3, abs(q) & 0xFFFFFFFF)
        elif call in (0xB, 0xC):
            source, destination, control = r
            count = control & 0x1FFFFF
            unit = 4 if call == 0xC or control & (1 << 26) else 2
            if call == 0xC:
                count = (count + 7) & ~7
            if control & (1 << 24):
                uc.mem_write(destination, bytes(uc.mem_read(source, unit)) * count)
            else:
                uc.mem_write(destination, bytes(uc.mem_read(source, unit * count)))
        elif call in (0x11, 0x12):
            uc.mem_write(r[1], lz77(self.read, r[0]))
        else:
            self.fault = 'the driver made the BIOS call %x at %08x' % (call, pc)
            uc.emu_stop()

    def call(self, address, args=(), stack=()):
        uc = self.uc
        sp = STACK_TOP - 4 * len(stack)
        for i, v in enumerate(stack):
            uc.mem_write(sp + 4 * i, struct.pack('<I', v & 0xFFFFFFFF))
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | 0x20)  # system mode, Thumb
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for r in range(13):
            uc.reg_write(UC_ARM_REG_R0 + r, args[r] & 0xFFFFFFFF if r < len(args) else 0)
        self.fault = None
        try:
            uc.emu_start(address | 1, RETURN_TRAP, count=80_000_000)
        except UcError:
            if self.fault:
                raise SystemExit(self.fault) from None
            raise
        if self.fault:
            raise SystemExit(self.fault)

    def run(self, steps, module=0, loop=1):
        for step in steps:
            if step[0] == 'copy':
                _, destination, source, size = step
                self.uc.mem_write(destination, bytes(self.rom[source - ROM_BASE:source - ROM_BASE + size]))
            elif step[0] == 'words':
                _, destination, words = step
                self.uc.mem_write(destination, struct.pack('<%dI' % len(words), *words))
            else:
                _, address, args, stack = step
                values = {MODULE: module, LOOP: loop}
                self.call(address, tuple(values.get(a, a) if isinstance(a, str) else a for a in args), stack)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]

    def start(self, song):
        found = songs(self.rom, self.game)
        if not 0 <= song < len(found):
            raise SystemExit('the game has %d songs, and %d is none of them' % (len(found), song))
        self.run(self.game.play, found[song], 0 if song in self.game.no_loop else 1)

    def frame(self):
        for address in self.game.frame:
            self.call(address)

    def globals_record(self):
        where = self.game.globals(self)
        return b''.join(self.read(where[name], size) for name, size in GLOBALS)

    def channel_record(self, c):
        layout = self.game.layout
        raw = self.read(self.game.records_base(self) + RECORD_SIZE[layout] * c, RECORD_SIZE[layout])
        record = bytearray(RECORD_SIZE[layout])
        for offset, size in RECORD_FIELDS[layout]:
            record[offset:offset + size] = raw[offset:offset + size]
        return bytes(record)

    def trace_lines(self, n):
        """Returns the trace's lines for frame n, after the frame has run."""
        records = {'g': self.globals_record()}
        for c in range(self.game.records):
            records['c%d' % c] = self.channel_record(c)
        lines = []
        for name, data in records.items():
            if self.last.get(name, bytes(len(data))) != data:
                self.last[name] = data
                lines.append('%d %s %s' % (n, name, data.hex()))
        return lines


def has_modules(rom):
    """Returns True if the ROM holds a GBAMOD30 module at a word's start."""
    at = bytes(rom).find(SIGNATURE)
    while at >= 0:
        if at % 4 == 0:
            return True
        at = bytes(rom).find(SIGNATURE, at + 1)
    return False


def trace(rom, song, frames):
    emu = DriverEmulator(rom)
    emu.start(song)
    for n in range(frames):
        emu.frame()
        for line in emu.trace_lines(n):
            print(line)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('mode', choices=('trace',))
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    a = p.parse_args()
    trace(load_rom(a.rom), a.song, a.frames)


if __name__ == '__main__':
    main()
