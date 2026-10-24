#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Run Nintendo R&D2's GBA sound driver under the Unicorn ARM emulator.

This is the reference for checking agbsf2's model of the driver. It calls the game's sound init, the
driver's routine that asks for a sequence, and the driver's per-frame routines (no video and no interrupts; the BIOS
calls the driver makes are emulated), and captures what the driver does:

    trace       each voice's state after each frame, and the writes to the PSG's registers, in the format of
                agbsf2 --trace
    dump        the players and tracks in use after a number of frames
    render      the mixer's output, the samples the driver sends to the FIFOs, as a WAV file at the mixer's rate
                (the PSG isn't rendered)

    driver_emu.py ROM trace SONG FRAMES [--music N] [--yoshi]
    driver_emu.py ROM dump SONG FRAMES [--music N] [--yoshi]
    driver_emu.py ROM render SONG FRAMES OUT.wav [--music N] [--yoshi] [--tracks 1,3]

SONG is agbsf2's number for a song. agbsf2 numbers the sequences first, then the sound effects. A sound effect is a
track of one of the sound effects' sequences. The game's request for it names the sequence and the track. The script
makes that request on the first of the players that the game plays its sound effects on.

Where the game has a play-music routine that gives the driver settings with the sequence, the script starts a sequence
through it as the first music number that plays the sequence. --music N starts it as music number N instead. Super
Mario Advance 2's routine mutes track 8 of sequence 30 as music 17 and track 9 as music 18. agbsf2 writes the version
of music 18 to <name>_30_music18. That game also mutes track 6 of sequences 15 to 18 unless the player rides Yoshi.
The script starts those sequences without Yoshi, like agbsf2's first version. --yoshi starts them with the player
riding Yoshi, like agbsf2's <name>_NN_yoshi.

The routine and RAM addresses are chosen by the ROM's game code. Those of The Legend of Zelda: A Link to the Past
(AZLE), Super Mario Advance 2 (AA2E) and Super Mario Advance 3 (A3AE) are built in. For another game, add an entry to
GAMES (see docs/rd2.md).
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE,
                     UC_MEM_READ_UNMAPPED, UC_MODE_ARM, Uc)
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R6, UC_ARM_REG_R7,
                               UC_ARM_REG_SP)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom

RETURN_TRAP = 0x0F000000

# The PSG's registers, whose writes the trace lists: NR10 to NR44, the wave RAM, and NR50 to NR52.
PSG = (0x04000060, 0x040000A0)

VOICE_SIZE = 0x78
TRACK_SIZE = 0x54


class Addresses:
    """AZLE (The Legend of Zelda: A Link to the Past & Four Swords, US)."""
    sound_init = 0x08129E60  # the game's sound init, which calls the driver's init with the game's settings
    request = 0x0812C718     # the driver's play request: r0 = player, r1 = sequence
    effect_request = 0x0812C73C  # the driver's request for a sound effect: r0 = player, r1 = the sound effects'
                                 # sequence, r2 = the track
    settings = 0x030003CC    # where the driver keeps the address of the game's settings
    commit = 0x0812C704      # the driver's routine that hands the requests made since its last call to the next frame
    requests = 0x0812CC24    # the per-frame routine's first step: it carries out the requests handed over
    vblank = 0x0812A798      # the driver's VBlank routine: restarts timer 0 and the output DMAs
    frame = 0x0812A7A4       # the driver's per-frame routine: requests, sequencer, PSG voices and mixer
    voices = 0x03001760      # 7 DirectSound voices, then 4 PSG voices, of 0x78 bytes
    active = 0x03000168      # the first DirectSound voice that plays, in the order the mixer takes them
    tracks = 0x03000C20      # 24 tracks of 0x54 bytes
    players = 0x03001C88     # 20 players of 0x48 bytes
    fixed_region = 0x03000158  # the region that instruments with a sample for each key play
    outputs = 0x03000140     # the two output buffers, each 176 left samples then 176 right ones
    output_index = 0x03000152  # the buffer the mixer writes this frame
    note_on = 0x0812B7B4     # the driver's note on: r0 = track, r1 = note, r2 = velocity, r3 = length in 1/150 ticks
    note_key = 0x0812B7F2    # in the note on, after the instrument lookup: r7 = the note with the track's transpose
    note_voice = 0x0812B8A2  # in the note on, once it has a voice: r4 = the voice, r6 = the note's region, r7 = the
                             # note its pitch is for
    note_done = 0x0812B964   # in the note on, once it has set the voice's sample or PSG setting: r4 = the voice
    active_end = 0           # what the last voice in the mixer's list links to: 0, or a node that ends the list
    music_player = 0x12      # the player that the game plays its music on
    effect_player = 0x10     # the first of the players that the game plays its sound effects on
    player_size = 0x48       # a player's record, and the offset of its byte that is 1 while it plays
    player_playing = 0x42
    voice_note = 0x09        # the offsets of a voice's note, before the transpose, and its velocity
    voice_velocity = 0x0A
    play_music = None        # the game's play-music routine, where it gives the driver settings with the sequence
    yoshi_mute = None        # Super Mario Advance 2's routine that mutes tracks unless the player rides Yoshi


class AddressesAA2E(Addresses):
    """AA2E (Super Mario Advance 2: Super Mario World, US), with the older revision of the driver. Its players have
    no second volume, its voices keep the velocity where A Link to the Past's keep the note, and its lists of voices
    run between nodes at their ends."""
    sound_init = 0x0809B258
    request = 0x0809D840
    effect_request = 0x0809D864
    settings = 0x030004E4
    commit = 0x0809D82C
    requests = 0x0809DCA8
    vblank = 0x0809BA74
    frame = 0x0809BA80
    voices = 0x03001878
    active = 0x03000120      # the first voice of the mixer's list: the next voice of the node at its start
    active_end = 0x0300012C  # the node at its end
    tracks = 0x03000D38
    players = 0x03001DA0
    fixed_region = 0x030000A8
    outputs = 0x03000090
    output_index = 0x030000A2
    note_on = 0x0809C900
    note_key = 0x0809C93E
    note_voice = 0x0809C9F6
    note_done = 0x0809CAB8
    music_player = 0x13
    effect_player = 0x11
    player_size = 0x44
    player_playing = 0x41
    voice_note = None
    voice_velocity = 0x09
    play_music = 0x0809B348  # the game's play-music routine: r0 = the music number. For sequence 30, it also mutes
                             # track 8 as music 17 and track 9 as music 18.
    music_table = 0x08177064  # a halfword for each music number: the sequence in its low byte
    music_count = 0x30
    # The routine that mutes track 6 of the music whose entry in the table has bit 9 set: r0 = 0 mutes it and r0 = 1
    # lets it play. The game's level start code (0x08003D52) calls it after the play-music routine, with the flag that
    # the player rides Yoshi.
    yoshi_mute = 0x0809B3CC
    yoshi_bit = 0x200


class AddressesA3AE(Addresses):
    """A3AE (Super Mario Advance 3: Yoshi's Island, US). The game's driver is A Link to the Past's revision with
    another volume for sample voices. The driver's variables are 0x98 bytes lower than in A Link to the Past."""
    sound_init = 0x0812C214
    request = 0x0812EEBC
    effect_request = 0x0812EEE0
    settings = 0x03000334
    commit = 0x0812EEA8
    requests = 0x0812F3C8
    vblank = 0x0812CF50
    frame = 0x0812CF5C
    voices = 0x030016C8
    active = 0x030000D0
    tracks = 0x03000B88
    players = 0x03001BF0
    fixed_region = 0x030000C0
    outputs = 0x030000A8
    output_index = 0x030000BA
    note_on = 0x0812DF58
    note_key = 0x0812DF96
    note_voice = 0x0812E046
    note_done = 0x0812E108
    music_player = 0x13
    effect_player = 0x11
    play_music = 0x0812C3B4  # the game's play-music routine: r0 = the music number. It also requests the sample
                             # voices and the second volume that the music plays with.
    music_table = 0x083077E4  # a halfword for each music number: the sequence in its low byte
    music_count = 0x29


GAMES = {b'AZLE': Addresses, b'AA2E': AddressesAA2E, b'A3AE': AddressesA3AE}


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
        uc.mem_map(ROM_BASE, (len(rom) + 0xFFF) & ~0xFFF)
        uc.mem_write(ROM_BASE, rom)
        uc.mem_map(RETURN_TRAP, 0x1000)
        uc.mem_write(RETURN_TRAP, b'\xfe\xe7' * 0x800)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_INTR, self._svc)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io_write, begin=PSG[0], end=PSG[1] - 1)
        self.uc = uc
        self.writes = []
        self.player_number = self.addr.music_player
        self.call(self.addr.sound_init)

    def _unmapped(self, uc, access, address, size, value, user):
        # A region that plays an instrument the bank doesn't have reads its sample from past the sample set's table, and
        # the address it finds there can be outside the GBA's memory, where the hardware reads open bus. agbsf2
        # reads 0 there, and so does the emulator.
        if access == UC_MEM_READ_UNMAPPED:
            uc.mem_map(address & ~0xFFF, 0x1000 if (address & 0xFFF) + size <= 0x1000 else 0x2000)
            return True
        raise SystemExit('the driver accessed unmapped memory at %08x' % address)

    def _io_write(self, uc, access, address, size, value, user):
        self.writes.append((address, size, value & ((1 << (8 * size)) - 1)))

    def _svc(self, uc, intno, user):
        # The driver calls the BIOS's Div, CpuSet and CpuFastSet.
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        number = uc.mem_read(pc - 2, 1)[0] if thumb else (struct.unpack('<I', uc.mem_read(pc - 4, 4))[0] >> 16) & 0xFF
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
                # Writes from the host don't reach the hooks, so the ones to the PSG, such as a wave's, are kept here.
                if PSG[0] <= r1 + i * unit < PSG[1]:
                    self.writes.append((r1 + i * unit, unit, int.from_bytes(value, 'little')))
        else:
            raise SystemExit('the driver made BIOS call %02x' % number)

    def call(self, address, args=(), thumb=True):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_CPSR, 0x1F | (0x20 if thumb else 0))  # system mode first: SP/LR are banked
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.emu_start(address | (1 if thumb else 0), RETURN_TRAP, count=50_000_000)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]

    def play(self, song, music=None, yoshi=False):
        """Asks for a sequence on the music player, as the game's play routine does. The driver starts it in the next
        frame. Where the game's play-music routine gives the driver settings with the sequence, the routine starts it
        as music number `music`. By default that is the first music number that plays the sequence. In Super Mario
        Advance 2, the game's routine for the tracks that play only while the player rides Yoshi follows. `yoshi` lets
        them play.

        A sound effect is asked for on the first of the game's players for sound effects."""
        self.writes = []
        effect = self.effect_of(song)
        if music is None and not effect:
            music = self.music_number(song)
        elif music is not None and (effect or self.addr.play_music is None or self.music_sequence(music) != song):
            raise SystemExit('music %d doesn\'t play song %d' % (music, song))
        a = self.addr
        if yoshi and (music is None or a.yoshi_mute is None or not self.music_entry(music) & a.yoshi_bit):
            raise SystemExit('song %d has no tracks that play only while the player rides Yoshi' % song)
        if effect:
            self.player_number = self.addr.effect_player
            self.call(self.addr.effect_request, (self.player_number,) + effect)
        elif music is None:
            self.call(self.addr.request, (self.addr.music_player, song))
        else:
            self.call(self.addr.play_music, (music,))
            if self.addr.yoshi_mute is not None:
                self.call(self.addr.yoshi_mute, (1 if yoshi else 0,))
        self.call(self.addr.commit)
        return self.writes

    def effect_of(self, song):
        """Returns (the sound effects' sequence, the track) for the sound effect that agbsf2's song `song` is, or None
        for a sequence. A table of offsets ends where its first entry points. The table of sequences and the table of
        their bank lists give the count of sequences. A sound effects' sequence starts with a halfword for each of its
        tracks, up to where its first track starts."""
        settings = self.u32(self.addr.settings)

        def count(table):
            first = self.u32(table)
            return first // 4 if first % 4 == 0 else 0

        index = song - min(count(self.u32(settings + 8)), count(self.u32(settings + 20)))
        if index < 0:
            return None
        effects, lists = self.u32(settings + 12), self.u32(settings + 24)
        for sequence in range(min(count(effects), count(lists))):
            start = effects + self.u32(effects + 4 * sequence)
            end, track = 0x10000, 0
            while 2 * track < end:
                offset = struct.unpack('<H', self.read(start + 2 * track, 2))[0]
                end = min(end, offset) if offset else end
                track += 1
            tracks = (end + 1) // 2
            if index < tracks:
                return sequence, index
            index -= tracks
        raise SystemExit('the game has no song %d' % song)

    def music_number(self, sequence):
        """Returns the first music number that plays `sequence` through the game's play-music routine, or None."""
        a = self.addr
        if a.play_music is None:
            return None
        for music in range(a.music_count):
            if self.music_sequence(music) == sequence:
                return music
        return None

    def music_sequence(self, music):
        """Returns the sequence that music number `music` plays, or None for a music number past the table's end."""
        a = self.addr
        return self.read(a.music_table + 2 * music, 1)[0] if 0 <= music < a.music_count else None

    def music_entry(self, music):
        """Returns the entry of music number `music` in the game's table of music."""
        return struct.unpack('<H', self.read(self.addr.music_table + 2 * music, 2))[0]

    def frame(self):
        """Runs one frame as the game's VBlank handler does: the driver's VBlank routine, then its per-frame routine.
        Returns the PSG register writes."""
        self.writes = []
        self.call(self.addr.vblank)
        self.call(self.addr.frame)
        return self.writes

    def track_number(self, track):
        """Returns the player's number for the track at `track`, or -1."""
        for n in range(10):
            if self.u32(self.player() + 8 + 4 * n) == track:
                return n
        return -1

    def muted_tracks(self):
        """Returns the numbers of the player's tracks whose mute flag is set."""
        muted = set()
        for n in range(10):
            track = self.u32(self.player() + 8 + 4 * n)
            if track and self.read(track + 0x4A, 1)[0]:
                muted.add(n)
        return muted

    def mute_tracks_except(self, numbers, muted=()):
        """Mutes the player's tracks other than `numbers` with each track's mute flag. The tracks that the game muted,
        `muted`, stay muted. The flag stops a track starting notes."""
        for n in range(10):
            track = self.u32(self.player() + 8 + 4 * n)
            if track:
                self.uc.mem_write(track + 0x4A, bytes([0 if n in numbers and n not in muted else 1]))

    def player(self):
        """Returns the address of the record of the player that plays the song."""
        return self.addr.players + self.addr.player_size * self.player_number

    def playing(self):
        return self.read(self.player() + self.addr.player_playing, 1)[0] != 0

    def voice_lines(self):
        """Returns each voice's state as the trace prints it, for the voices in use, and the order of the DirectSound
        voices that play."""
        a = self.addr
        lines = []
        for i in range(11):
            base = a.voices + VOICE_SIZE * i
            raw = self.read(base, VOICE_SIZE)
            if raw[1] == 0:
                continue
            u8 = lambda o: raw[o]
            u16 = lambda o: struct.unpack_from('<H', raw, o)[0]
            s16 = lambda o: struct.unpack_from('<h', raw, o)[0]
            u32 = lambda o: struct.unpack_from('<I', raw, o)[0]
            s32 = lambda o: struct.unpack_from('<i', raw, o)[0]
            track = u32(0x04)
            track = (track - a.tracks) // TRACK_SIZE if track else -1
            region = u32(0x54)
            region = 'fixed' if region == a.fixed_region else '%08x' % region
            lines.append('v%d %d %d t%d p%d n%d v%d %x %x vol%d f%d e%d %d:%d lfo%d,%d sl%d,%d,%d,%d,%d '
                         'env%d,%d,%d,%d,%d %s r%d s%08x pos%x x%x' % (
                             i, u8(0x01), u8(0x00), track, u8(0x08), u8(a.voice_note) if a.voice_note else 0,
                             u8(a.voice_velocity), u32(0x0C), u32(0x10),
                             u32(0x14), u16(0x18), u8(0x1A), u8(0x1B), u8(0x1C), u32(0x20), u32(0x24), u32(0x2C),
                             u32(0x30), s32(0x34), s32(0x38), s32(0x3C), s32(0x40), s32(0x44), u16(0x48), s16(0x4A),
                             u8(0x50), region, u8(0x58), u32(0x5C), u32(0x60), u32(0x64)))
        order = []
        v = self.u32(a.active)
        while v and v != a.active_end and len(order) < 7:
            order.append((v - a.voices) // VOICE_SIZE)
            v = self.u32(v + 0x6C)
        lines.append('active' + ''.join(' v%d' % i for i in order))
        return lines


def render(rom, song, frames, out, tracks=None, music=None, yoshi=False):
    emu = DriverEmulator(rom)
    emu.play(song, music, yoshi)
    a = emu.addr
    muted = set()
    if tracks is not None:
        # Carry out the requests now. The tracks can then be muted before they play anything. The per-frame routine
        # then finds no requests. That is the only difference. The tracks that the game's requests muted stay muted.
        emu.call(a.requests)
        muted = emu.muted_tracks()
    pcm = bytearray()
    for f in range(frames):
        if tracks is not None:
            emu.mute_tracks_except(tracks, muted)
        emu.frame()
        buffer = emu.u32(a.outputs + 4 * emu.read(a.output_index, 1)[0])
        data = emu.read(buffer, 352)
        for i in range(176):
            for side in (data[i], data[176 + i]):
                pcm += struct.pack('<h', (side - 256 if side >= 128 else side) * 256)
    with wave.open(str(out), 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(10512)
        w.writeframes(bytes(pcm))


class NoteLog:
    """Records each note the driver starts, with hooks in its note on routine."""

    def __init__(self, emu):
        self.emu = emu
        self.notes = []
        self.pending = None
        a = emu.addr
        for at in (a.note_on, a.note_key, a.note_voice, a.note_done):
            emu.uc.hook_add(UC_HOOK_CODE, self._hook, begin=at, end=at)

    def _hook(self, uc, address, size, user):
        a = self.emu.addr
        if address == a.note_on:
            track = uc.reg_read(UC_ARM_REG_R0)
            self.pending = {'track': self.emu.track_number(track), 'velocity': uc.reg_read(UC_ARM_REG_R2) & 0xFF}
        elif address == a.note_key and self.pending is not None:
            self.pending['key'] = uc.reg_read(UC_ARM_REG_R7) & 0xFF
        elif address == a.note_voice and self.pending is not None:
            voice = uc.reg_read(UC_ARM_REG_R4)
            self.pending['voice'] = (voice - a.voices) // VOICE_SIZE
            self.pending['pitch_note'] = uc.reg_read(UC_ARM_REG_R7) & 0xFF
            self.pending['region'] = uc.reg_read(UC_ARM_REG_R6)
            self.notes.append(self.pending)
            self.pending = None
        elif address == a.note_done and self.notes:
            voice = uc.reg_read(UC_ARM_REG_R4)
            note = self.notes[-1]
            note['type'] = self.emu.read(voice, 1)[0]
            note['sample'] = self.emu.u32(voice + 0x5C)


def notes(rom, song, frames, writes=None, music=None, yoshi=False):
    """Returns each frame's new notes and the voices' state after it: for each frame, (notes, voices, newest), where a
    note is a dict, voices[i] is (state, pitch) or None for a free voice, and newest[n] is the newest voice that track
    n plays, or -1. With `writes`, a list, each write to the PSG's registers goes on its end as (frame, address, size,
    value). The request's writes, before the first frame, have frame -1. A sequence starts as music number `music`,
    with the player riding Yoshi with `yoshi` (see DriverEmulator.play())."""
    emu = DriverEmulator(rom)
    log = NoteLog(emu)
    request = emu.play(song, music, yoshi)
    if writes is not None:
        writes.extend((-1,) + w for w in request)
    a = emu.addr
    result = []
    for f in range(frames):
        log.notes = []
        frame_writes = emu.frame()
        if writes is not None:
            writes.extend((f,) + w for w in frame_writes)
        for n in log.notes:
            n['frame'] = f
        voices = []
        for i in range(11):
            raw = emu.read(a.voices + VOICE_SIZE * i, VOICE_SIZE)
            if raw[1] == 0:
                voices.append(None)
                continue
            voices.append((raw[1], struct.unpack_from('<I', raw, 0x10)[0]))
        newest = []
        for n in range(10):
            track = emu.u32(emu.player() + 8 + 4 * n)
            head = emu.u32(track + 0x0C) if track else 0
            newest.append((head - a.voices) // VOICE_SIZE if head else -1)
        result.append((log.notes, voices, newest))
        if f > 0 and not emu.playing():
            break
    return result


def format_write(frame, write):
    address, size, value = write
    return '%d %08x %d %0*x' % (frame, address, size, 2 * size, value)


def trace(rom, song, frames, music=None, yoshi=False):
    emu = DriverEmulator(rom)
    for w in emu.play(song, music, yoshi):
        print(format_write(-1, w))
    for f in range(frames):
        for w in emu.frame():
            print(format_write(f, w))
        for line in emu.voice_lines():
            print('%d %s' % (f, line))
        if f > 0 and not emu.playing():
            break


def dump(rom, song, frames, music=None, yoshi=False):
    emu = DriverEmulator(rom)
    emu.play(song, music, yoshi)
    for f in range(frames):
        emu.frame()
    a = emu.addr
    for p in range(20):
        raw = emu.read(a.players + a.player_size * p, a.player_size)
        if raw[a.player_playing]:
            print('player %2d: %s' % (p, raw.hex()))
    for t in range(24):
        raw = emu.read(a.tracks + TRACK_SIZE * t, TRACK_SIZE)
        if any(raw):
            print('track %2d: %s' % (t, raw.hex()))
    for line in emu.voice_lines():
        print(line)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom')
    p.add_argument('command', choices=['trace', 'dump', 'render'])
    p.add_argument('song', type=int)
    p.add_argument('frames', type=int)
    p.add_argument('out', nargs='?')
    p.add_argument('--tracks', help='render: only these of the player\'s tracks (0-9), such as 1,3')
    p.add_argument('--music', type=int, help='start the sequence as this music number through the game\'s play-music '
                   'routine (default: the first music number that plays it)')
    p.add_argument('--yoshi', action='store_true', help='start the sequence with the player riding Yoshi (AA2E)')
    a = p.parse_args()
    rom = load_rom(a.rom)
    if a.command == 'trace':
        trace(rom, a.song, a.frames, a.music, a.yoshi)
    elif a.command == 'render':
        if not a.out:
            p.error('render needs OUT.wav')
        render(rom, a.song, a.frames, a.out,
               [int(t) for t in a.tracks.split(',')] if a.tracks else None, a.music, a.yoshi)
    else:
        dump(rom, a.song, a.frames, a.music, a.yoshi)


if __name__ == '__main__':
    main()
