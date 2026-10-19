#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Copy a ROM and replace song 0 with a test song for commands and outputs the original songs may not use.

    test_song.py ROM OUT.gba

compare_trace.py and compare_notes.py then check agbsf2 against the driver on the copy:

    python tools/konami/test_song.py rom.gba test.gba
    python tools/konami/compare_trace.py test.gba build/agbsf2 --songs 0 --frames 2000
    build/agbsf2 -q -s 0 -o test test.gba
    python tools/konami/compare_notes.py test.gba build/agbsf2 test --songs 0

Test songs for the WCT 2004 revision (BYWP), the Rave Master revision (BRME), the Eternal Duelist revision (AY5E and
AYWE) and the Dungeon Dice Monsters revision (AYDE) are built in.

In the WCT 2004 and Rave Master songs, square 1 has an attack and a decay, vibrato, a bend and pans that make it take
square 2 for its left side. The wave channel plays with vibrato, changes waves and bends a note, which reloads wave 0.
The noise channel plays two notes, voice 0 calls a fragment of its own data both ways and runs the commands that do
nothing, voice 1 loops the song from its loop point, and voice 2 pans itself. In WCT 2004, voice 2 also sets the echo up
and routes voice 1 to echo bus 0.

In the Eternal Duelist song, square 1 plays legato notes that take it more than 24 semitones from the note it started,
and sets NR51 bits for square 2 as well as its own. The wave channel's legato note and bend reload wave 0, and the noise
channel plays a legato note too. Voice 0 calls a fragment both ways, turns vibrato on and off with F4 and FC, and runs
commands that do nothing until the driver's leftover call count takes it back after the second call. Voices 1 and 3 set
the next voice's volume, and voice 1 loops the song, which resets square 1's pan. Vibrato and a bend wipe out the starts
of two of voice 2's notes.

In the Dungeon Dice Monsters song, square 1 sets NR50, bends, plays vibrato at depth 6, restarts, sets a volume without
a note, releases one note and rests with FB during vibrato, and stops the song. Square 2 calls a fragment with F4 and
F5 until the driver's leftover call count takes it back after the second call. The wave channel loads a wave while a
note plays, plays legato notes at the four volume codes, and loads another wave through F5. The noise channel plays a
note of the square table and a sample note on voice 0. Voices 0 and 2 play notes of their samples at other rates and
bend them, while the other voice of their FIFO is silent, and voice 2 plays voice 3's notes, the second at volume 0.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # for gbarom.py, in tools/
from gbarom import ROM_BASE, load_rom


def call_fragment(head, calls, tail, fragment, places):
    """Returns the track head + calls + tail + fragment. The fragment's offset in the track goes into `calls` as a
    little-endian halfword at each index in `places`."""
    offset = len(head) + len(calls) + len(tail)
    calls = list(calls)
    for i in places:
        calls[i], calls[i + 1] = offset & 0xFF, offset >> 8
    return head + calls + tail + fragment


def test_tracks(revision):
    """Returns 16 test tracks for the WCT 2004 revision or 12 for the Rave Master revision. They play samples 9, 10 and
    11. Rave Master uses 80-8F for duty and wave commands. Its F3 has no extra bytes, and echo opcodes and 00-7F do
    nothing."""
    rave_master = revision == 'Rave Master'
    duty = 0x80 if rave_master else 0x00
    tracks = [[0x00, 0xFD] for _ in range(12 if rave_master else 16)]
    tracks[0] = [0x00, 0xF4, 0x20, 0x00, 0xF5, 0x10, 0x00, 0xF1, 0x30, 0x00, 0xF0, 0x8F, 0x00,
                 0xDC, 0x30, 0x20, 0xF2, 0x50, 0x05, 0xF0, 0xFF, 0x05, 0xF1, 0x00, 0x05, 0xC8, 0x05, 0xE0, 0x05,
                 duty | 0x02, 0x00, 0xD7, 0x24, 0x10, 0xF0, 0x0F, 0x08, 0xD9, 0x26, 0x08, 0xF0, 0xF0, 0x08, 0xD9, 0x28,
                 0x08, 0xF0, 0x3C, 0x06, 0xDB, 0x2A, 0x0A, 0xFD]
    tracks[1] = [0x00, 0xD5, 0x20, 0x40, 0xD6, 0x22, 0x20, 0xFD]
    tracks[2] = [0x00, duty | 0x04, 0x00, 0xDA, 0x20, 0x20, 0xF1, 0x20, 0x10, duty | 0x05, 0x00, 0xD8, 0x22, 0x10,
                 0xF1, 0x00, 0x04, 0xF2, 0x48, 0x08, 0xE0, 0x05, 0xFD]

    # Noise notes 3 and 7, whose entries in the noise tables of BYWP and BRME restart the channel.
    tracks[3] = [0x00, 0xDA, 0x03, 0x10, 0xDC, 0x07, 0x10, 0xE0, 0x05, 0xFD]

    # Voice 0 calls a fragment after its end, first with a duty change and then without.
    head = [0x00, 0xAB, 0x0A, 0x10] if rave_master else [0x00, 0xF9, 0x01, 0x00, 0xAB, 0x0A, 0x10]
    calls = [0x93, 0, 0, 0x02, 0x10, 0x9F, 0, 0, 0x01, 0x08]
    nothing = [0xFA, 0x00, 0xF6, 0x00, 0xFB, 0x00, 0xFC, 0x00, 0x07, 0x00] if rave_master else \
        [0xFA, 0x01, 0x00, 0xF6, 0x05, 0x00, 0xFB, 0x00, 0xFC, 0x00]
    tail = nothing + [0xB9, 0x0B, 0xFE, 0x10, 0xFD]
    fragment = [0x03, 0xA6, 0x09, 0x04, 0xC4, 0x06, 0xF2, 0x60, 0x07]
    tracks[4] = call_fragment(head, calls, tail, fragment, (1, 6))

    loop_point = [0xF3] if rave_master else [0xF3, 0x00, 0x00]
    tracks[5] = [0x00] + loop_point + [0x00, 0xA8, 0x0A, 0x30, 0xC3, 0xE1, 0x00, 0xFE]
    echo = [] if rave_master else [0xF7, 0x40, 0x00, 0xF8, 0x85, 0x00, 0xF9, 0x11, 0x00]
    tracks[6] = [0x00] + echo + [0xA8, 0x0B, 0x20, 0xF0, 0x3A, 0x10, 0xFD]
    return tracks


def eternal_duelist_tracks():
    """Returns the 10 tracks of the Eternal Duelist test song. It plays samples 10, 11, 18 and 21. The revision reads
    Rave Master's commands, except that F4-FC are vibrato, as F1 is, and F0 sets a PSG track's NR51 bits, or a sample
    track's volume and the next one's. Its delays from F0 up take a second byte."""
    tracks = [[0x00, 0xFD] for _ in range(10)]
    tracks[0] = [0x00, 0xF0, 0x10, 0x00, 0xF3, 0x00, 0xD8, 0x30, 0x10, 0xD8, 0x24, 0x08, 0xD8, 0x4C, 0x08, 0xDA, 0x30,
                 0x08, 0xF0, 0x13, 0x00, 0xF2, 0x50, 0x05, 0xF1, 0x30, 0x10, 0xD8, 0x32, 0x10, 0xF1, 0x00, 0x05, 0x82,
                 0x00, 0xE0, 0x05, 0xD7, 0x24, 0xF0, 0x40, 0xF0, 0x11, 0x00, 0xD9, 0x26, 0x08, 0xFD]
    tracks[1] = [0x00, 0xF0, 0x20, 0x00, 0xD5, 0x20, 0x40, 0xD6, 0x22, 0x20, 0xE0, 0x05, 0xFD]
    tracks[2] = [0x00, 0x86, 0x00, 0xD8, 0x20, 0x10, 0xD8, 0x24, 0x10, 0xF2, 0x48, 0x08, 0xF1, 0x20, 0x10, 0xF1, 0x00,
                 0x05, 0xDA, 0x22, 0x10, 0xE0, 0x05, 0xFD]

    # Noise note 0's entry restarts the channel, and note 1's doesn't.
    tracks[3] = [0x00, 0xF0, 0x80, 0x00, 0xDA, 0x00, 0x10, 0xDA, 0x01, 0x10, 0xD8, 0x00, 0x10, 0xE0, 0x05, 0xFD]

    # Voice 0 calls a fragment after its end, first with a duty change and then without. The driver keeps the second
    # call's return position, so the 256th command after the call returns there.
    head = [0x00, 0xAB, 0x0A, 0x10]
    calls = [0x93, 0, 0, 0x02, 0x10, 0x9F, 0, 0, 0x01, 0x08]
    tail = [0xB9, 0x0B, 0xFE, 0x10, 0xF4, 0x10, 0x08, 0xFC, 0x00, 0x01] + [0x07, 0x01] * 253 + [0xFD]
    fragment = [0x03, 0xA6, 0x0A, 0x04, 0xC4, 0x06, 0xF2, 0x60, 0x07]
    tracks[4] = call_fragment(head, calls, tail, fragment, (1, 6))

    tracks[5] = [0x00, 0xF3, 0x00, 0xA8, 0x0A, 0x20, 0xF0, 0x5A, 0x20, 0xA8, 0x0B, 0xF2, 0x10, 0xFE]
    tracks[6] = [0x00, 0xAC, 0x0B, 0x40, 0xF1, 0x20, 0x10, 0xBA, 0x0A, 0x05, 0x10, 0xF1, 0x00, 0x08, 0xB9, 0x0A, 0x02,
                 0x00, 0xF2, 0x44, 0x10, 0xE0, 0x05, 0xFD]
    tracks[7] = [0x00, 0xA8, 0x12, 0x10, 0xC6, 0x10, 0xF0, 0x93, 0x10, 0xFD]
    tracks[8] = [0x00, 0xBA, 0x15, 0x03, 0x40, 0xE0, 0x05, 0xFD]
    tracks[9] = [0x00, 0xB8, 0x0A, 0xFB, 0x20, 0xFD]
    return tracks


def dungeon_dice_tracks():
    """Returns the 8 tracks of the Dungeon Dice Monsters test song. It plays samples 0, 3, 5 and 8, from sample map
    entries 0C, 10 and 17 and a duty byte of 5. Its notes are Ex nn on the PSG, Bx nn from the sample map, Ax nn on the
    next track, and 9x nn, the track's sample at note nn. Delays from F0 up take a second byte."""
    tracks = [[0x00, 0xFD] for _ in range(8)]

    # Square 1 sets NR50, plays a note with a duty byte of 40, vibrato at depth 6, bends with F3, restarts with C9, and
    # gives the channel volume 5 with D5, which writes pitch 0. It releases one note with FB, and FB during vibrato
    # rests. A 9x note plays 1/16 semitone above note nn. It stops the song at frame 400, after square 2's call has
    # returned to where it was kept.
    tracks[0] = [0x00, 0xF0, 0x53, 0x00, 0x40, 0x00, 0xEC, 0x20, 0x10, 0x86, 0x00, 0xEC, 0x22, 0x20, 0x80, 0x00, 0xF3,
                 0x30, 0x08, 0xF3, 0x10, 0x08, 0xC9, 0x08, 0xD5, 0x08, 0xFC, 0x04, 0xEA, 0x24, 0x08, 0xFB, 0x08, 0x84,
                 0x00, 0xEA, 0x26, 0x08, 0xFB, 0x08, 0x80, 0x00, 0x9A, 0x20, 0x08, 0xFC, 0x04, 0x7D, 0x00, 0xE8, 0x2A,
                 0x10, 0xFC, 0x00, 0xF0, 0x77, 0xF1, 0x00, 0xFF]

    # Square 2 calls a fragment of its own data with F4, and with F5, whose last byte, 10, is a duty byte and the delay
    # after the call. The driver keeps the return position, so the 256th command after the call returns there again.
    head = [0x00, 0xF7, 0x00, 0xE8, 0x20, 0x08]
    calls = [0xF4, 0, 0, 0x02, 0x04, 0xF6, 0x00, 0xF5, 0, 0, 0x01, 0x10]
    tail = [0xE9, 0x22, 0x04] + [0x07, 0x01] * 255 + [0xFD]
    fragment = [0x04, 0xEA, 0x24, 0x06, 0xFC, 0x02]
    tracks[1] = call_fragment(head, calls, tail, fragment, (1, 8))

    # The wave channel loads wave 1 while a note plays, which restarts it at frequency 0, and plays legato notes at the
    # four volume codes. A duty byte and 05 don't change the wave, and F5's last byte, 72, loads wave 2 and waits 114
    # frames after the call, which has no commands of its own.
    tracks[2] = [0x00, 0x70, 0x00, 0xE9, 0x20, 0x08, 0x71, 0x08, 0xE3, 0x22, 0x08, 0xE6, 0x24, 0x08, 0xE1, 0x26, 0x08,
                 0x7E, 0x00, 0xE9, 0x28, 0x08, 0x05, 0x00, 0xFC, 0x04, 0xF5, 0x00, 0x00, 0x00, 0x72, 0xE9, 0x20, 0x10,
                 0xFC, 0x00, 0xFD]

    # The noise channel plays notes 72 and 76, restarts with C6, and plays note 32, whose frequency table entry it takes
    # as a noise setting. At frame 110 it plays sample 8 on voice 0 with A9.
    tracks[3] = [0x00, 0xE8, 0x48, 0x04, 0xC6, 0x04, 0xE8, 0x4C, 0x04, 0xFC, 0x02, 0xE8, 0x20, 0x04, 0xFC, 0x5C, 0xA9,
                 0x17, 0x06, 0xFC, 0x02, 0xFD]

    # Voice 0 plays sample 5 from a duty byte at notes 24, the sample's rate, 28 and 26, and bends them with F2,
    # once in the frame of a note, which starts it at the bent pitch. D6 changes the volume, C9 restarts the note, and a
    # rest stops it. Later it releases a note with FB. Voice 1 is silent throughout, as the FIFO that plays voices 0 and
    # 1 takes the rate of the last note started on it.
    tracks[4] = [0x00, 0xB8, 0x0C, 0x10, 0x05, 0x00, 0x9A, 0x18, 0x10, 0x9A, 0x1C, 0x10, 0xF2, 0x30, 0x08, 0xF2, 0x10,
                 0x08, 0x9A, 0x1A, 0x00, 0xF2, 0x28, 0x08, 0xD6, 0x08, 0xC9, 0x08, 0xF8, 0x06, 0xFC, 0x28, 0xB9, 0x10,
                 0x08, 0xFB, 0x08, 0xFC, 0x00, 0xFD]

    # Track 5 bends voice 2's note with F1 while voice 3 is silent, and voice 2 then plays voice 3's notes with Ax. The
    # second is at volume 0, which stops the voice.
    tracks[5] = [0x18, 0xF1, 0x30, 0x08, 0xF1, 0x20, 0x08, 0xFD]
    tracks[6] = [0x00, 0xB8, 0x17, 0x10, 0x9B, 0x1E, 0x20, 0xFC, 0x04, 0xAA, 0x17, 0x10, 0xA0, 0x17, 0x04, 0xFD]
    return tracks


# Per-game settings: the song table (song 0 is replaced), free space for the test data, and the driver revision.
GAMES = {b'BYWP': dict(song_table=0x0893C7D0, base=0x08D00000, revision='WCT 2004'),
         b'BRME': dict(song_table=0x0809A7E0, base=0x08780000, revision='Rave Master'),
         b'AY5E': dict(song_table=0x080E09D0, base=0x081B0000, revision='Eternal Duelist'),
         b'AYWE': dict(song_table=0x081CEF50, base=0x08250000, revision='Eternal Duelist'),
         b'AYDE': dict(song_table=0x0805E034, base=0x087FC000, revision='Dungeon Dice Monsters')}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', help='the game (.gba)')
    p.add_argument('out', help='the copy to write')
    a = p.parse_args()
    rom = bytearray(load_rom(a.rom))
    game = GAMES.get(bytes(rom[0xAC:0xB0]))
    if game is None:
        raise SystemExit('no test song for game code %s' % rom[0xAC:0xB0].decode('latin-1'))

    data = bytearray()
    offsets = []
    revision = game['revision']
    if revision == 'Dungeon Dice Monsters':
        tracks = dungeon_dice_tracks()
    elif revision == 'Eternal Duelist':
        tracks = eternal_duelist_tracks()
    else:
        tracks = test_tracks(revision)
    for track in tracks:
        offsets.append(len(data))
        data += bytes(track)
    base = game['base'] - ROM_BASE
    if len(set(rom[base:base + len(data)])) > 1:
        raise SystemExit('the space for the test song at %08X is in use' % game['base'])
    rom[base:base + len(data)] = data
    struct.pack_into('<I%dH' % len(offsets), rom, game['song_table'] - ROM_BASE, game['base'], *offsets)
    Path(a.out).write_bytes(rom)


if __name__ == '__main__':
    main()
