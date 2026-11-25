# Engine Software's driver

*The Santa Clause 3: The Escape Clause* plays its music with Engine Software's
sound driver, a player of modules in the manner of FastTracker 2. The driver
is Thumb code in the cartridge, from 0x08000218 to about 0x080025F0. Its mixer
is ARM code that the driver copies to IWRAM with DMA 3. The mixer writes
8-bit samples for FIFO A, mono or stereo, at one of three rates. This document
describes the driver as `agbsf2` models it. The addresses are those of *The
Santa Clause 3* (game code `B33E`).

## The games

| Game | Code | Mixer handler | Bank | Instruments | Songs |
|---|---|---|---|---|---|
| *The Santa Clause 3: The Escape Clause* | `B33E` | `0x08001890` | `0x08763448` | 20 | 12 |

*Dragon Ball Z: Taiketsu* (`BDBE`) and *Ty the Tasmanian Tiger 2: Bush
Rescue* (`BTYE`) contain the same mixer and the same layout of bank. Their
builds of the driver have other limits and effects. `agbsf2` finds the driver
in them and reports that their build isn't supported.

## Detection

Detection finds the mixer's channel handler by its code. It names the build
from the game's code and checks the build's routines by their first
instructions. It reads the bank's address from the game's call of the
routine that loads the bank. A table of builds gives the facts of each game:
the routines, the mixer's rate, the most music channels and the game's
set-up.

## The game's set-up and the frame

The game's sound manager is a C++ object. Its init sets the music volume to
210 of 256 and the effect volume to 256. It calls the driver's reset with the
game's allocator, the init in mono at 13,378 Hz, the load of the bank and the
set-up of two effect channels with the table of effects.

The init caps the rate at 13,378 Hz. Timer 0 then runs at 13,379 samples a
second, and the mixer mixes 224 samples a frame. The game turns on the
vertical count at line 27 and the vertical blank. At the vertical count, the
manager steps its ramps of the music and effect volumes, then calls the
driver's frame routine (0x08000C14). At the vertical blank, the manager calls
the swap (0x08001174). The swap restarts DMA 1 on the buffer that the mixer
filled and points the mixer at the other.

The frame routine counts the samples to the next tick. At a tick it steps the
volume envelopes and the fade-outs, then reads the next row on tick 0 or runs
the effects on the other ticks. It mixes the frame's samples in pieces split
at the ticks. Ticks therefore fall inside a frame, at a sample. A tick lasts
13378 / ((tempo × 0x333 × 0x1001) >> 23) samples, in integer divisions.

## The mixer

Each channel has a record of 0x4C bytes at 0x03000778, the music channels
first. The mixer adds the signed 8-bit point of each active channel times its
volume, then steps the channel by its 16.16 step. It takes each point without
interpolation. A looping sample past its end moves back by the length of its
loop and keeps the fraction of the step. A sample that plays once stops at its
end. The sum goes through a table that clips it to 8 bits.

The volume of a music channel is the channel volume (0 to 64) × the music
volume >> 8, then × the envelope >> 6. After the note's release, the fade-out
multiplies it as well. The pitch of a note is an index: (note + relative
note) × 128 + fine tune + slide. A table of rates in hertz gives each index
its rate, and the step is the rate × the step factor >> 8. Note 49 with a
relative note of 0 plays a sample at 8,363 Hz.

## The bank

The bank starts with a 4-byte header: a byte that the driver doesn't read
(0x21), the version (1), the count of instruments and the count of songs. A
word follows for each song: its offset from the bank. The instruments come
next, each of `(length + 0x7F) & ~3` bytes:

| Offset | Contents |
|---|---|
| `0x00` | the length of the sample, in bytes |
| `0x04` | the start of the loop |
| `0x08` | the length of the loop (0 for a sample that plays once) |
| `0x0C` | the volume (0 to 64) |
| `0x0D` | the pan (0 to 255) |
| `0x0E` | the fine tune (signed) |
| `0x0F` | the relative note (signed) |
| `0x10` | the fade-out (a word) |
| `0x14` | the volume envelope: the count of points, the sustain point, the start and end of its loop (0xFF for none), then 12 points of tick and value as halfwords |
| `0x48` | the pan envelope, laid out the same way |
| `0x7C` | the points, signed 8-bit |

In *The Santa Clause 3*, only the sixth instrument has a volume envelope: 64
at tick 0, 4 at tick 5, 31 at tick 12 (the sustain point), then 10, 4 and 0
after the release. No instrument has a pan envelope. Every fade-out is 0x80.

A pool of packed rows follows the instruments. A row starts with a bit for
each of the 5 fields of each channel, the first channel in the high bit of the
first byte. A byte follows for each set bit: the note (1 to 0x60, or 0x61 for
a key off), the instrument (from 1), the volume column, the effect and its
operand.

A song starts with its count of channels, its count of orders, the order to
restart at, its count of patterns, its speed and its tempo, then two zero
bytes and the order list. Zeros pad the list to a multiple of 4 bytes. Each
pattern is a halfword count of rows, a zero halfword, then a word for each
row: its offset from the bank, or 0 for an empty row.

## Effects

The effects are FastTracker 2's: 0 arpeggio, 1 and 2 slides up and down, 3
slide to the note, 4 vibrato, 5 and 6 their mixes with a volume slide, 7
tremolo, 8 pan, 9 sample offset, A volume slide, B order jump, C volume, D
pattern break, E5 fine tune, EA and EB fine volume slides, ED note delay, EE
pattern delay and F speed or tempo. The driver adds 0x10 music volume, 0x11
music volume slide, 0x14 key off, 0x15 envelope place and 0x1B retrigger. The
other effects call a routine that the game can set. *The Santa Clause 3* sets
none. Its songs use effects 2, 3, 4, 8 and 9, and the volume column's volumes.

The mixer plays one channel of sound for both sides in this game. The pans
change nothing that is heard.

## The songs

The manager starts a song with its start routine (0x0802CBE8). The routine
sets the music volume to 0 and starts the song, and the vertical count then
raises the volume by `ceil(210 / frames)` a frame. The game passes 64 frames,
so the volume rises by 4 a frame and reaches 210 after 53 frames. Song 6 gets
1 frame and starts at 210. The jingle routine (0x0802CC7C) starts song 5 with
flag 0x100: the driver saves the place of the song that plays, plays the
jingle once, then resumes the saved song where it was. A jingle that starts
with no song before it stops at its end. A song that isn't a jingle loops to
its restart order after its last order, without end.

`agbsf2` converts each song as the manager starts it. A song fades in over 53
frames, song 6 starts at the full music volume, and the jingle plays once and
ends.

The game starts song 6 from three states of the player (0x0800D908,
0x080223EA and 0x08025F3E). Each state moves to a level or a screen 120 frames
after its animation ends, and the move fades the music out (0x0802CDD0). In
runs in mGBA 0.11.0, the fade started 175 or 200 frames after the song's
start, and the driver first returns to the restart order 245 frames after it.
The game never plays a second pass, so `agbsf2` converts song 6 as one pass,
with no loop markers.
