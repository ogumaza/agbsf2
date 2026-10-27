#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run a game's build of Krawall, the XM and S3M player, under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of Krawall. It copies the game's code and data to IWRAM and
EWRAM as its start-up code does, calls the game's sound init and krapPlay() (no BIOS, no video), and calls the mixer's
worker once a frame, as the game's VBlank handler does, with getDmaAddress() replaced by a routine that hands it the
same two buffers and a frame's samples each time. It captures what the player and the mixer do:

    trace       after each frame, the mixer's variables (g), the player's record (p), each of the module's channels (c0
                and on) and each mixer channel (m0 and on), in hex as Krawall keeps them in RAM, when they've changed
                since the line that last gave them. The player's record leaves out its pointers to the routines that
                work out periods and frequencies.
    render      the mix's 8-bit output, left and right

    driver_emu.py ROM trace MODULE FRAMES [--song N | --jingle]
    driver_emu.py ROM render MODULE FRAMES OUT.wav [--song N | --jingle]

MODULE is the address of the module's header in hex. `agbsf2 --info` lists the addresses. In a game whose modules
are packed in music files, MODULE is the address of a music file instead. The game's routine then plays the file's
module. `--info` lists each file with its module. krapPlay() plays the module in loop mode. With --song, krapPlay() plays song N of a module whose orders separate songs with +++, in song
mode. Song mode loops back to the song's start at the song's end. With --jingle, krapPlay() plays the module once, in
the jingle mode. With no module playing, the jingle mode plays the module to its end and stops. The routine and RAM
addresses are chosen by the ROM's game code. Those of the supported games are built in. Another game requires its
values in GAMES (see tools/README.md).
"""
import argparse
import ctypes
import re
import struct
import subprocess
import sys
import wave
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_MEM_READ_UNMAPPED, UC_MODE_ARM, UC_PROT_ALL, \
    Uc
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, \
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000
LEFT, RIGHT = 0x02030000, 0x02031000  # the buffers that the stand-in for getDmaAddress() hands the worker
MODE_LOOP, MODE_SONG, MODE_JINGLE = 1, 2, 4  # krapPlay()'s modes: loop, play one of the module's songs, play once
ORDER_SKIP = 254  # the order that separates songs (+++)


class Build2003:
    """The records of the 2003/09/01 build of Krawall, as the game keeps them in RAM. Build 1.4 keeps the same. The
    builds before it keep 24 mixer channels of 0x28 bytes, in IWRAM. The worker keeps the pointer to the left buffer at
    `left_slot` below its frame pointer, and to the right one below that or above it."""
    player_size = 0x34  # the player's record, which its channels follow
    channel_size = 0x60
    period_pointers = (0x24, 0x2C)  # the player's pointers to the routines that work out periods and frequencies
    mix_channels = 32
    mix_size = 0x2C
    left_slot = 0x1C
    left_at_sp = False  # True if the worker keeps the pointer to the left buffer at its stack pointer instead
    iwram_at = 0x03000000  # where the start-up code copies `iwram`
    before_init = ()  # calls (routine, arguments) that the game's start-up code makes before its sound init
    sound_init = 0   # the game's sound init, or 0 if `after_init` makes its calls into the library
    after_init = ()  # calls (routine, arguments) that the game makes after its sound init, before music plays
    settings = ()  # bytes that the game stores before its sound init, such as the options' defaults: (address, byte)
    sound_init_args = ()  # the arguments of the game's sound init
    sound_init_arm = False  # True if the sound init is ARM code
    after_play = ()  # bytes that the game's routine that starts a module stores: (address, byte)
    frame_routine = None  # a routine of the game's that runs before the worker in every frame, or None
    play_table = None  # the game's table of what it plays: 8 bytes an entry, (address, entries)
    dma_address = None  # getDmaAddress(), which the stand-in replaces, or None if the worker has none
    out_left, out_right = LEFT, RIGHT  # the buffers that the worker mixes a frame into
    left_reg = None  # the register that holds the left buffer's pointer after a tick, if no slot does
    vram = None  # the code and data that the start-up code copies to VRAM: (source, size, destination)
    heap = None  # the game's heap pointer, which its start-up code sets before the sound init: (address, value)
    worker_state = ()  # values that the worker reads to mix a frame, stored before each call: (address, value, size)
    play_file = None  # the game's routine that loads a music file and plays its module, (file, loop), or None
    unpacked = None  # the word that holds the address of a music file's unpacked data after a load
    jingle_files = ()  # the music files that the game plays once, in the jingle mode
    sample_length = False  # True if a sample's header gives its length where the others give its end
    player_pointers = (0x08, 0x14, 0x30)  # the player's pointers to the module, the row's data and the pattern
    channel_pointers = (0x18,)  # a channel's pointer to its instrument


class Build2004(Build2003):
    """The records of the builds from 2004/09/17 on: a channel has the volume and pan that the mixer plays too."""
    channel_size = 0x64


class BigMuthaTruckers(Build2004):
    """B63E (Big Mutha Truckers): the modified build of Krawall. The start-up code copies the library's code to IWRAM
    and VRAM. The game unpacks a music file's module to the top of EWRAM with its routine at 0x08031E0C, and its
    routine at 0x0802BB8C does that and then calls krapPlay(), in loop mode for a loop flag and in the jingle mode
    without. The worker mixes a frame into buffers in VRAM. Its state at 0x02001CE4 picks the buffers' quarter, and the
    timer's interrupt at 0x02001CE9 lets it mix."""
    iwram = (0x083F07B4, 0x374C)
    ewram = (0x083FC278, 0x3AA4)
    vram = (0x083EF5D4, 0x11E0, 0x06016E1C)
    heap = (0x02000EC8, 0x02010000)
    sound_init = 0x0802B934  # kragInit(1), kramQualityMode(0x21), kramSetMasterVol(0x40080) and the volumes
    sound_init_args = (1,)
    after_init = ((0x080345B4, (100, 0)),)  # krapSetMusicVol(100, 0), the options' volume, at 0x08000684
    play_file = 0x0802BB8C
    play = 0x0803303C
    worker = 0x03002F54
    worker_state = ((0x02001CE4, 0, 4), (0x02001CE8, 0, 1), (0x02001CE9, 1, 1))
    out_left, out_right = 0x0601775C, 0x06017BAC
    after_tick = 0x03003020
    left_reg = UC_ARM_REG_R4
    set_pos = 0x060176EC
    frame_samples = 276
    player = 0x02001F5C
    mixer = 0x02001CEC
    mix_channels = 14
    master = 0x0300308C
    timer = 0x03002B20
    music_volume = 0x02001F58
    plays = 0x03003088
    unpacked = 0x02000ECC  # the top of the free memory, which the loader moves down past the data
    sample_length = True
    module_table = None
    jingles = ()


class StreetRacingSyndicate(BigMuthaTruckers):
    """BCZP (Street Racing Syndicate): the modified build of Krawall, as in Big Mutha Truckers, at other addresses.
    The game's routine at 0x0802DC3C loads a music file with the routine at 0x080217BC and plays its module."""
    iwram = (0x083EE6C4, 0x3750)
    ewram = (0x083FA500, 0x3D34)
    vram = (0x083ED4E8, 0x11DC, 0x06016E1C)
    heap = (0x02002E90, 0x02010000)
    sound_init = 0x0802D9F4
    after_init = ((0x08020E5C, (64, 0)),)  # krapSetMusicVol(64, 0), the options' volume, at 0x080003DA
    play_file = 0x0802DC3C
    play = 0x0801F8E4
    worker = 0x03000508
    worker_state = ((0x02000C50, 0, 4), (0x03000645, 0, 1), (0x03000646, 1, 1))
    out_left, out_right = 0x06017758, 0x06017BA8
    after_tick = 0x030005D4
    set_pos = 0x060176E8
    player = 0x02000EC4
    mixer = 0x02000C54
    master = 0x03000640
    timer = 0x030000D4
    music_volume = 0x02000EC0
    plays = 0x0300063C
    unpacked = 0x02002E94


class MmsBreakEm(BigMuthaTruckers):
    """BEME (M&M's Break 'Em): the modified build of Krawall, another compile of it. The start-up code copies the
    library's code to IWRAM only, and the worker mixes into buffers there. The game's routine at 0x080131D8 loads a
    music file with the routine at 0x080132E8 and plays its module. The loader takes memory from the game's malloc()
    and keeps the address at 0x02000380."""
    iwram = (0x083F5254, 0x22BC)
    ewram = (0x083F7614, 0x4F6C)
    vram = None
    heap = None
    before_init = ((0x08019C34, ()),)  # sets up malloc()'s memory, 0x19000 bytes from 0x02027000
    sound_init = 0x08013050
    after_init = ((0x08015AA4, (48,)),)  # krapSetMusicVol(48), the options' volume, at 0x08000418
    play_file = 0x080131D8
    play = 0x08014588
    worker = 0x030005BC
    worker_state = ((0x0200038C, 0, 4), (0x030018D8, 0, 1), (0x030018D9, 1, 1))
    out_left, out_right = 0x03001038, 0x03001488
    after_tick = 0x03000688
    set_pos = 0x03000FC4
    player = 0x02000600
    mixer = 0x02000390
    master = 0x03000B44
    timer = 0x03000188
    music_volume = 0x020005FC
    plays = 0x03000B40
    unpacked = 0x02000380


class PrincessNatasha(MmsBreakEm):
    """BNPE (Princess Natasha): the modified build of Krawall, as in M&M's Break 'Em, with other addresses in EWRAM.
    The game's routine at 0x0801C9B8 loads a music file with the routine at 0x0801CAC8 and plays its module. It plays
    two files once, in the jingle mode."""
    iwram = (0x083EC938, 0x22BC)
    ewram = (0x083EECF8, 0x506C)
    before_init = ((0x0802335C, ()),)
    sound_init = 0x0801C830
    after_init = ((0x0801F284, (64,)),)  # krapSetMusicVol(64), the options' volume, at 0x080008F4
    play_file = 0x0801C9B8
    play = 0x0801DD68
    worker_state = ((0x0200048C, 0, 4), (0x030018D8, 0, 1), (0x030018D9, 1, 1))
    player = 0x02000700
    mixer = 0x02000490
    music_volume = 0x020006FC
    unpacked = 0x02000480
    jingle_files = (0x083E6830, 0x083E4D20)


class PuppyLuv(Build2004):
    """BYXE (Puppy Luv: Spa and Resort): the unsigned build of Krawall. Its start-up code copies the library's code and
    data in IWRAM to 0x03000500 on. kramPlayExt() and the routines that it calls are in EWRAM."""
    iwram = (0x08392608, 0x1BA4)
    iwram_at = 0x03000500
    ewram = (0x08394AD0, 0x20C8)
    # The game's boot at 0x08024BD6 calls kragInit(1), krapSetMusicVol(60, 0), kramSetSFXVol(0x80) and
    # kramQualityMode(0x20). The routine at 0x0802492C then sets the options' music volume, 64, before it plays music.
    after_init = ((0x08024F28, (1,)), (0x08025B64, (60, 0)), (0x080254D0, (0x80,)), (0x0802519C, (0x20,)),
                  (0x08025B64, (64, 0)))
    play = 0x08026F78
    worker = 0x03001D40
    after_tick = 0x03001DC4
    set_pos = 0x02001D88
    left_slot = 0
    left_at_sp = True
    dma_address = 0x08043CA8
    frame_samples = 276
    player = 0x02000010
    mixer = 0x030005BC
    master = 0x03001E5C
    timer = 0x02000008
    music_volume = 0x0200000C
    plays = 0x03001E60
    module_table = None  # the game's table at 0x0805B69C gives each module and its kind, 8 bytes each
    jingles = ()


class TheSims(Build2003):
    """ASIE (The Sims: Bustin' Out): the 2003/09/01 build of Krawall, with the RAM of Digimon Racing's."""
    iwram = (0x08FDA434, 0x1598)
    ewram = (0x08FDB9CC, 0x27F8)
    # The game's settings of the effects' and the music's volumes, which its sound init passes to the library. Without a
    # save, the defaults at 0x0806A210 give 128 and 255. The music here is at 128, the volume that agbsf2 plays.
    settings = ((0x03005D88, 0x80), (0x03005D89, 0x80))
    sound_init = 0x08041338  # kragInit(1), kramSetMasterVol(0x40080), kramQualityMode(2), then the settings' volumes
    play = 0x0804A8CC
    worker = 0x03001068
    after_tick = 0x0300111C
    set_pos = 0x03000578
    dma_address = 0x080498DC
    frame_samples = 276
    player = 0x02001644
    mixer = 0x020008B4
    master = 0x03000090
    timer = 0x0200163C
    music_volume = 0x02001640
    plays = 0x03000600
    play_table = (0x08D73EF8, 71)  # the module, mode and song of each piece that the routine at 0x0804129C plays
    module_table = None
    jingles = ()


class DigimonRacing(Build2003):
    """BDGE (Digimon Racing): the 2003/09/01 build of Krawall."""
    iwram = (0x087F2730, 0x1598)  # the code and data that the start-up code copies to IWRAM, and its size
    ewram = (0x087F3CC8, 0x27F8)  # the data it copies to EWRAM
    sound_init = 0x080130B4       # the game's sound init: kragInit(1), kramSetMasterVol(0x40080), kramQualityMode(0)
    play = 0x08038398             # krapPlay(module, mode, song)
    worker = 0x03001068           # kramWorker(), in ARM, which the game calls once a frame
    after_tick = 0x0300111C       # where the worker goes on after a tick of the player
    set_pos = 0x03000578          # kramSetPos(), in ARM, which moves a mixer channel's point
    dma_address = 0x080373A8      # getDmaAddress(), which the worker calls for the buffers and the samples to mix
    frame_samples = 276
    player = 0x02001644           # the player's record, then its channels
    mixer = 0x020008B4            # the mixer's channels
    master = 0x03000090           # the master volumes of the music and the sound effects, hqMode and hqRamp
    timer = 0x0200163C            # the sound timer's step and count
    music_volume = 0x02001640
    plays = 0x03000600            # the count of plays, which the mixer's handles hold
    module_table = 0x087D7CEC     # the game's table of modules, a word each
    jingles = (16,)               # the entries that the game's routine at 0x080132D0 plays once, in the jingle mode


class EverythingOrNothing(Build2003):
    """BJBE (007: Everything or Nothing): build 1.4 of 2003/03/19. The game's sound init sets the master volumes of the
    music and the sound effects from its settings, which hold 1 for on. The game's options set them to 128 when
    they're on, through kramSetSFXVol() at 0x0803D37C and the routine at 0x0803F638 that sets the music's volume."""
    iwram = (0x087AC4CC, 0x2ED4)
    ewram = (0x087AF3A0, 0x27F8)
    sound_init = 0x08021F68       # kragInit(1), kramSetMasterVol(0x40080), kramQualityMode(2), and the game's settings
    after_init = ((0x0803D37C, (0x80,)), (0x0803F638, (0x80, 0)))
    play = 0x0803E23C
    worker = 0x030029A4
    after_tick = 0x03002A58
    set_pos = 0x03001E9C
    dma_address = 0x0803D22C
    frame_samples = 276
    player = 0x02001644
    mixer = 0x020008B4
    master = 0x030019B4
    timer = 0x0200163C
    music_volume = 0x02001640
    plays = 0x03001F24
    module_table = 0
    jingles = ()
    play_table = (0x0804F498, 25)  # the module, the mode and the song of each piece that the routine at 0x08022260 plays


class Build1_2_2_17(Build2003):
    """A homebrew game with build 1.2.2.17 of 2003/01/13, and no game code. The game calls kragInit(1) and nothing else
    that sets up the mixer, then plays its first module in loop mode."""
    iwram = (0x0803D1C8, 0x2310)
    ewram = (0x0804042C, 0x2274)
    after_init = ((0x08004108, (1,)),)  # kragInit(1)
    play = 0x08004FDC
    worker = 0x030013DC
    after_tick = 0x0300148C
    set_pos = 0x03000910
    dma_address = 0x08004208
    frame_samples = 276
    player = 0x020010C0
    mixer = 0x030001A0
    master = 0x03000560
    timer = 0x020010B8
    music_volume = 0x020010BC
    plays = 0x030009A0
    module_table = 0
    jingles = ()
    mix_channels = 24
    mix_size = 0x28
    left_slot = 0x18


class Build1_2_2_15(Build2003):
    """A game with build 1.2.2.15 of 2002/12/19. Its sound init calls kragInit(1), kramQualityMode(0),
    kramSetMusicVol() with the game's setting and kramSetSFXVol(0x80). The setting starts at 70. The trace sets the
    library's default of 128 instead, as the conversion does. The game plays every module in loop mode."""
    iwram = (0x087BB2E4, 0x1E70)
    ewram = (0x087BDB74, 0x25B0C)
    after_init = ((0x08014240, (1,)), (0x08014428, (0,)), (0x080163FC, (128, 0)), (0x08014470, (0x80,)))
    play = 0x0801510C
    worker = 0x030013F4
    after_tick = 0x030014A4
    set_pos = 0x03000928
    dma_address = 0x08014340
    frame_samples = 276
    player = 0x02024958
    mixer = 0x030001B8
    master = 0x03000578
    timer = 0x02024950
    music_volume = 0x02024954
    plays = 0x030009B8
    module_table = 0
    jingles = ()
    mix_channels = 24
    mix_size = 0x28
    left_slot = 0x18


class Juka(Build2004):
    """BJKE (Juka and the Monophonic Menace): the 2004/09/17 build of Krawall."""
    iwram = (0x08F76FA0, 0x1C50)
    ewram = (0x08F78C18, 0x19C5C)
    sound_init = 0x0802395C  # kragInit(1), then the state of the game's music
    after_play = ((0x03006500, 1),)  # the routine at 0x08023E9C: the music fades in
    frame_routine = 0x08023A84  # sets the music volume, which it raises from 0 by 2 a frame after a start
    play = 0x0802C244
    worker = 0x03001720
    after_tick = 0x030017D4
    set_pos = 0x03000C30
    dma_address = 0x0802B178
    frame_samples = 276
    player = 0x020189F0
    mixer = 0x030001C8
    master = 0x03000748
    timer = 0x020189E8
    music_volume = 0x020189EC
    plays = 0x03000CB8
    module_table = 0x0827FBF0  # the routine at 0x08023E9C plays each entry in loop mode
    jingles = ()


class Babar(Build2004):
    """BBVE (Babar to the Rescue): the 2004/11/14 build of Krawall."""
    iwram = (0x083E9164, 0x7564)
    ewram = (0x083F06C8, 0x34DC)
    sound_init = 0x0801B218  # kragInit(0)
    sound_init_args = (0x02000000,)  # the game's record of its music
    sound_init_arm = True
    play = 0x0801D230
    worker = 0x03007034
    after_tick = 0x030070E8
    set_pos = 0x03006544
    dma_address = 0x0801C1A0
    frame_samples = 276
    player = 0x020016DC
    mixer = 0x0200094C
    master = 0x0300605C
    timer = 0x020016D4
    music_volume = 0x020016D8
    plays = 0x030065CC
    module_table = None
    jingles = ()


class Eragon(Build2004):
    """BENE (Eragon): the 2005-04-21 build of Krawall."""
    iwram = (0x08F2CAE0, 0x1F80)
    ewram = (0x08F2EA60, 0x232C)
    settings = ((0x03006838, 10), (0x03006839, 7))  # the options' defaults: the levels of the effects and the music
    sound_init = 0x0803FD90  # kragInit(1), kramSetMasterVol(0x40080), kramQualityMode(2), krapSetMusicVol(161, 0)
    play = 0x080438D0
    worker = 0x03001A50
    after_tick = 0x03001B04
    set_pos = 0x03000F60
    dma_address = 0x08042848
    frame_samples = 276
    player = 0x020010C0
    mixer = 0x030004F8
    master = 0x03000A78
    timer = 0x020010B8
    music_volume = 0x020010BC
    plays = 0x03000FE8
    play_table = (0x08F2C9D4, 29)  # the module, mode and song of each piece that the routine at 0x0803FD04 plays
    module_table = None
    jingles = ()


class RacingFever(Build2003):
    """BRWP (Racing Fever): build 1.2.2.14 of 2002/09/12. Its sound init calls kragInit(1), kramQualityMode(0x12) and
    krapSetMusicVol(0, 0). The build reads quality 0x12 as mode 0. The game's routine at 0x080042A4 sets the music
    volume to 127 and plays a module in loop mode."""
    iwram = (0x081FACEC, 0x1E70)
    ewram = (0x081FD744, 0x2274)
    after_init = ((0x08004604, (1,)), (0x080047EC, (0x12,)), (0x080066FC, (127, 0)))
    play = 0x0800540C
    worker = 0x030013F4
    after_tick = 0x030014A4
    set_pos = 0x03000928
    dma_address = 0x08004704
    frame_samples = 276
    player = 0x020010C0
    mixer = 0x030001B8
    master = 0x03000578
    timer = 0x020010B8
    music_volume = 0x020010BC
    plays = 0x030009B8
    module_table = 0
    jingles = ()
    mix_channels = 24
    mix_size = 0x28
    left_slot = 0x18


class Build1_2_2_10(Build2003):
    """A homebrew game with build 1.2.2.10 of 2002/08/24. Its ARM code calls kragInit(0), for mono, through a veneer
    at 0x0800DAD4. It plays each module in loop mode, and then sets the music volume to 128."""
    iwram = (0x0802E578, 0x1DD4)
    ewram = (0x08030D14, 0x2274)
    after_init = ((0x080091CC, (0,)),)  # kragInit(0)
    play = 0x0800A020
    worker = 0x03001358
    after_tick = 0x03001408
    set_pos = 0x0300088C
    dma_address = 0x080092CC
    frame_samples = 276
    player = 0x020010C0
    mixer = 0x0300011C
    master = 0x030004DC
    timer = 0x020010B8
    music_volume = 0x020010BC
    plays = 0x0300091C
    module_table = 0
    jingles = ()
    mix_channels = 24
    mix_size = 0x28
    left_slot = 0x18


GAMES = {b'ASIE': TheSims, b'B63E': BigMuthaTruckers, b'BBVE': Babar, b'BCZP': StreetRacingSyndicate,
         b'BDGE': DigimonRacing, b'BEME': MmsBreakEm, b'BENE': Eragon, b'BJKE': Juka, b'BJBE': EverythingOrNothing,
         b'BNPE': PrincessNatasha, b'BRWP': RacingFever, b'BYXE': PuppyLuv, b'MGBA': Build1_2_2_10,
         b'0000': Build1_2_2_15, bytes(4): Build1_2_2_17}


def addresses_for(rom):
    code = bytes(rom[0xAC:0xB0])
    if code not in GAMES:
        raise SystemExit('no Krawall addresses known for game code %s' % code.decode('latin-1'))
    return GAMES[code]


class KrawallEmulator:
    def __init__(self, rom):
        a = self.addr = addresses_for(rom)
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        # The hardware ignores an address's top four bits. Build 1.2.2.17's kramSetMasterVol() stores the last entry
        # of its table through such an address, 0x220008B4. EWRAM is mapped at 0x22000000 as well.
        self.ewram_memory = ctypes.create_string_buffer(0x40000)
        for base in (0x02000000, 0x22000000):
            uc.mem_map_ptr(base, 0x40000, UC_PROT_ALL, ctypes.addressof(self.ewram_memory))
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(0x06000000, 0x18000)
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, bytes(rom))
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        copies = [(a.iwram, a.iwram_at), (a.ewram, 0x02000000)]
        if a.vram:
            copies.append((a.vram[:2], a.vram[2]))
        for (source, size), destination in copies:
            uc.mem_write(destination, bytes(rom[source - ROM_BASE:source - ROM_BASE + size]))
        for address, value in a.settings:
            uc.mem_write(address, bytes((value,)))
        if a.heap:
            uc.mem_write(a.heap[0], struct.pack('<I', a.heap[1]))

        # getDmaAddress(&left, &right) stores the buffers' addresses and returns the frame's samples.
        if a.dma_address:
            assert a.frame_samples % 2 == 0 and a.frame_samples >> 1 < 256
            stub = struct.pack('<8H2I', 0x4A03, 0x6002, 0x4A03, 0x600A, 0x2000 | (a.frame_samples >> 1), 0x0040,
                               0x4770, 0x46C0, LEFT, RIGHT)
            uc.mem_write(a.dma_address, stub)
        self.translate = None
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        self.uc = uc
        for routine, args in a.before_init:
            self.call(routine, args)
        if a.sound_init:
            self.call(a.sound_init, a.sound_init_args, thumb=not a.sound_init_arm)
        for routine, args in a.after_init:
            self.call(routine, args)

    def _unmapped(self, uc, access, address, size, value, user):
        # A note on a channel without a sample has the mixer read its points from address 0 on, and a sample near the
        # ROM's end can have it read past the end, where the hardware reads open bus. agbsf2 reads 0 there, and
        # so does the emulator. The points don't change the mixer's positions, which the trace compares.
        if access == UC_MEM_READ_UNMAPPED:
            uc.mem_map(address & ~0xFFF, 0x1000 if (address & 0xFFF) + size <= 0x1000 else 0x2000)
            return True
        raise SystemExit('unmapped access at %08x (pc %08x)' % (address, uc.reg_read(UC_ARM_REG_PC)))

    def _svc(self, uc, intno, user):
        """The BIOS calls that Krawall makes: Div, and the CpuSet and CpuFastSet that clear and copy its records."""
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        if thumb:
            number = uc.mem_read(pc - 2, 1)[0]
        else:
            number = (struct.unpack('<I', uc.mem_read(pc - 4, 4))[0] >> 16) & 0xFF
        r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if number == 6:
            n = r0 - (1 << 32) if r0 & 0x80000000 else r0
            d = r1 - (1 << 32) if r1 & 0x80000000 else r1
            q = int(n / d) if d else 0
            uc.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R1, (n - q * d) & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R3, abs(q) & 0xFFFFFFFF)
        elif number in (0x0B, 0x0C):
            fill = r2 & (1 << 24)
            words = number == 0x0C or r2 & (1 << 26)
            count = r2 & 0x1FFFFF
            if number == 0x0C:
                count = (count + 7) & ~7
            unit = 4 if words else 2
            value = bytes(uc.mem_read(r0, unit))
            for i in range(count):
                if not fill:
                    value = bytes(uc.mem_read(r0 + i * unit, unit))
                uc.mem_write(r1 + i * unit, value)
        else:
            raise SystemExit('BIOS call %02x at %08x' % (number, pc))

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | (1 if thumb else 0))
        for i, v in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=100_000_000)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def play(self, address, mode=MODE_LOOP, song=0, model_module=None):
        """Plays the module at `address`, or the module of the music file there, and returns the module's address.
        With `model_module`, agbsf2's address of a music file's module, the records give the addresses in the unpacked
        data as agbsf2 does."""
        a = self.addr
        if not a.play_file:
            self.call(a.play, (address, mode, song))
            for at, value in a.after_play:
                self.uc.mem_write(at, bytes((value,)))
            return address

        self.call(a.play_file, (address, 0 if mode & MODE_JINGLE else 1))
        module = struct.unpack('<I', self.read(a.player + 8, 4))[0]
        if model_module is not None:
            # The file lists the samples to unpack, and the module's data follow at the next multiple of 4, with
            # their size.
            samples = self.read(address, 1)[0]
            packed = (address + 1 + samples + 3) & ~3
            size = int.from_bytes(self.read(packed, 3), 'little')
            low = struct.unpack('<I', self.read(a.unpacked, 4))[0]
            self.translate = (low, low + size, model_module - module)
        return module

    def frame(self):
        """Runs the game's routine for the frame, if it has one, and the worker. Returns the frame's output, left and
        right, as signed 8-bit points."""
        a = self.addr
        if a.frame_routine:
            self.call(a.frame_routine)
        for address, value, size in a.worker_state:
            self.uc.mem_write(address, value.to_bytes(size, 'little'))
        self.call(a.worker, thumb=False)
        n = a.frame_samples
        return self.read(a.out_left, n), self.read(a.out_right, n)

    def _translated(self, record, offsets):
        """Returns the record with each pointer at `offsets` into a music file's unpacked data moved to agbsf2's
        address."""
        if not self.translate:
            return record
        low, high, delta = self.translate
        out = bytearray(record)
        for i in offsets:
            word = struct.unpack_from('<I', out, i)[0]
            if low <= word < high:
                struct.pack_into('<I', out, i, (word + delta) & 0xFFFFFFFF)
        return bytes(out)

    def records(self, module):
        """Returns the mixer's variables, the player's record and each channel's, as the trace prints them."""
        a = self.addr
        timer = self.read(a.timer, 4)
        variables = self.read(a.master, 4) + timer + self.read(a.music_volume, 4) + self.read(a.plays, 4)
        player = bytearray(self.read(a.player, a.player_size))
        first, last = a.period_pointers
        player[first:last] = bytes(last - first)
        channels = self.read(module, 1)[0]
        lines = [('g', variables), ('p', self._translated(bytes(player), a.player_pointers))]
        for c in range(channels):
            record = self.read(a.player + a.player_size + a.channel_size * c, a.channel_size)
            lines.append(('c%d' % c, self._translated(record, a.channel_pointers)))
        for m in range(a.mix_channels):
            lines.append(('m%d' % m, self.read(a.mixer + a.mix_size * m, a.mix_size)))
        return lines

    def playing(self):
        return struct.unpack('<I', self.read(self.addr.player + 4, 4))[0] != 0


def song_count(rom, module):
    """Returns the number of songs that a module's orders separate with +++, or 0 if they have no marker. A song is a
    run of orders between markers. agbsf2 counts them the same way."""
    count = rom[module + 1 - ROM_BASE]
    orders = rom[module + 3 - ROM_BASE:module + 3 + count - ROM_BASE]
    songs, in_song = 0, False
    for o in orders:
        if o < ORDER_SKIP and not in_song:
            songs += 1
        in_song = o < ORDER_SKIP
    return min(songs, 64) if ORDER_SKIP in orders else 0


def jingle_modules(rom):
    """Returns the addresses of the modules that the game plays once, in the jingle mode."""
    a = addresses_for(rom)
    if not a.module_table:
        return set()
    return {struct.unpack_from('<I', rom, a.module_table + 4 * n - ROM_BASE)[0] for n in a.jingles}


def table_plays(rom):
    """Returns the entries of the game's table of what it plays, as (module, mode, song), without repeats."""
    a = addresses_for(rom)
    if not a.play_table:
        return []
    table, count = a.play_table
    plays = []
    for n in range(count):
        module, mode, song = struct.unpack_from('<IBB', rom, table + 8 * n - ROM_BASE)
        play = (module, mode, song if song_count(rom, module) else 0)
        if play not in plays:
            plays.append(play)
    return plays


def listed_files(tool, rom_path):
    """Returns the music file of each module that `agbsf2 --info` lists, as {module: file}, for a game whose modules
    are packed in music files."""
    text = subprocess.run([tool, '--driver', 'krawall', '--info', rom_path], capture_output=True, encoding='utf-8',
                          check=True).stdout
    return {int(m, 16): int(f, 16) for f, m in re.findall(r'^\s+0x([0-9A-F]{8}): 0x([0-9A-F]{8})$', text, re.MULTILINE)}


def song_plays(rom, modules, files=None):
    """Returns how krapPlay() plays each piece that agbsf2 numbers: a (module, mode, song) for each number. `modules`
    maps the numbers to the modules' addresses. The entries of the game's table of what it plays come first. A module
    that no entry plays follows. One without markers plays in loop mode, or in the jingle mode if the game plays it
    once. Each song of a module with markers plays in song mode, in the order of the numbers. For a game whose modules
    are packed in music files, `files` maps each module to its file, and the play is (file, mode, 0) instead."""
    a = addresses_for(rom)
    if a.play_file:
        return {n: (files[module], MODE_JINGLE if files[module] in a.jingle_files else MODE_LOOP, 0)
                for n, module in modules.items()}
    plays, seen = {}, {}
    jingles = jingle_modules(rom)
    table = table_plays(rom)
    for n in sorted(modules):
        module = modules[n]
        if n < len(table):
            plays[n] = table[n]
            continue
        song = seen.get(module, 0)
        seen[module] = song + 1
        if song_count(rom, module):
            plays[n] = (module, MODE_LOOP | MODE_SONG, song)
        else:
            plays[n] = (module, MODE_JINGLE if module in jingles else MODE_LOOP, 0)
    return plays


def trace(rom, address, frames, mode=MODE_LOOP, song=0, model_module=None):
    emu = KrawallEmulator(rom)
    module = emu.play(address, mode, song, model_module)
    last = {}
    for f in range(frames):
        emu.frame()
        for name, record in emu.records(module):
            if record != last.get(name, bytes(len(record))):
                print('%d %s %s' % (f, name, record.hex()))
                last[name] = record
        if not emu.playing():
            break


def render(rom, address, frames, path, mode=MODE_LOOP, song=0):
    emu = KrawallEmulator(rom)
    emu.play(address, mode, song)
    out = bytearray()
    for _ in range(frames):
        left, right = emu.frame()
        for l, r in zip(left, right):
            out += bytes(((l + 128) & 0xFF, (r + 128) & 0xFF))
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(1)
        w.setframerate(16384)
        w.writeframes(bytes(out))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'render'])
    p.add_argument('module', help="the module's address, in hex")
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--song', type=int, help='play this song of a module that separates songs with +++, in song mode')
    p.add_argument('--jingle', action='store_true', help='play the module once, in the jingle mode')
    a = p.parse_args()
    rom = load_rom(a.rom)
    module = int(a.module, 16)
    if a.song is not None:
        mode, song = MODE_LOOP | MODE_SONG, a.song
    else:
        mode, song = (MODE_JINGLE if a.jingle else MODE_LOOP), 0
    if a.command == 'trace':
        trace(rom, module, a.frames, mode, song)
        return
    if not a.out:
        p.error('render needs an output file')
    render(rom, module, a.frames, a.out, mode, song)


if __name__ == '__main__':
    main()
