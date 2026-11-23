# Frontline Studios' XM-like player

*Chicken Shoot* and *Chicken Shoot 2* play their music with a player of
four-channel modules in the manner of FastTracker 2. It's the newer of the two
players of the F U H Sound System. The player is ARM code in the cartridge,
and the mixer is ARM code that the game copies to IWRAM. The mixer plays the
four channels of the module and writes 8-bit output for FIFO A. FIFO A plays
on both sides. This document describes the player as `agbsf2` models it. The addresses
are those of *Chicken Shoot* (game code `B6FP`). *Chicken Shoot 2* (`B6GE`) has
the same code 0x1309C bytes later.

## The games

| Game | Code | Mix | Load | Play | Modules | Songs |
|---|---|---|---|---|---|---|
| *Chicken Shoot* | `B6FP` | `0x080E5974` | `0x080DFB7C` | `0x080E040C` | `0x0801A208`, `0x0803227C` | 5 |
| *Chicken Shoot 2* | `B6GE` | `0x080F8A10` | `0x080F2C18` | `0x080F34A8` | `0x0801A2C0`, `0x08032334` | 5 |

Each game has two modules: one for the title, and one for all the levels. The
game plays each module with an order list that the game contains. The list of
a level selects the part of the second module for that level.

## The frame

The game calls the frame routine of the player once a frame. The routine
mixes the 336 samples of the frame in segments, at 20,068 samples a second.
The player counts the samples to the next tick. When the count reaches 0
inside a frame, the player runs a tick and the mixer goes on with the rest of
the frame. A tick lasts 20,068 × 5 / (2 × BPM) samples. Ticks therefore fall
at a sample, not at a frame. At the vertical blank the game restarts the sound
DMA on the buffer that the mixer filled, and swaps the buffers.

The player counts the ticks of a row. At the last tick of the row it reads
the next row. On the other ticks it runs the routines of each channel: the
routine of the effect and the routine of the volume column. The play of a
list leaves the first row after the ticks of the speed.

## Modules

The header of a module is 20 bytes: the count of channels, the count of patterns,
the master volume, the note handler, the last order, the order to restart at,
the speed (ticks a row) and the BPM, then the addresses of the order list, the
pattern table and the instrument table. A pattern starts with the count of
rows (a halfword) and a byte, then the cells.

The first byte of a cell contains the flags:

| Flag | Then |
|---|---|
| `0x01` | a note and an instrument (a byte each) |
| `0x10` | a byte of the volume column |
| `0x20` | an effect and its parameter (a byte each) |

An instrument has the length, the start and the end of the loop (in 1/8192 of
a point), the volume (0 to 64), the finetune (signed), the loop type (0 for
none) and the points, signed 8-bit.

The period of a note is 7680 − 64 × the note − half the finetune of the
instrument. The player looks up the step of the period in its tables. Period
4608 plays a sample at 8,363 points a second.

## Effects

| Number | Effect |
|---|---|
| 1 | sets the BPM |
| 2, 3 | slides the period down or up by 4 × the parameter a tick |
| 4 | slides toward the note of the cell by 4 × the parameter a tick |
| 5 | sets the volume |
| 6 | the volume slide: x up a tick, or with y set, the whole parameter down |
| 7 | the vibrato: speed x, depth y |
| 8 | runs the volume byte of the cell again |
| 9 | sets the speed |
| 10 | the arpeggio |
| 13 | the tremolo: changes the volume for good |
| 14 | jumps to an order |
| 15 | starts the sample 256 × the parameter points in |
| 16 | breaks to the next order, at a row |
| 17, 18 | the fine slides |
| 20, 23 | the wave of the vibrato and of the tremolo |
| 25, 26 | the fine volume slides |
| 27 | cuts the volume to 0 after the number of ticks in the parameter |
| 28 | delays the note by the number of ticks in the parameter |

Effect 24, the retrigger, counts ticks and plays nothing. Effects 11 and 12
make the tick run itself again without end. Neither game uses them, and
`agbsf2` doesn't follow them. A delayed note sets the period that sounds, but
not the period of the channel. The next row without a note therefore goes back
to the pitch of the note before.

The volume column sets the volume (0x10 to 0x50), slides it (0x60 to 0x7F a
tick, 0x80 to 0x9F once), sets the speed and depth of the vibrato (0xA0 to
0xBF), or slides toward the note of the cell (0xC0 to 0xFF). When that slide
arrives, it stops the effect of the channel.

## Levels

The mixer scales each point by the volume of the channel × the master volume.
It adds the four channels, keeps the bits from 14 up and clamps the sum to a
byte. At the master volume of 255 and a volume of 64, a point plays at almost
its full scale.

The game sets the master volume when it starts a song: the music volume of
the options × the volume of the level / 9. A new save has the volume in the
options at 9. The titles play at 224, the levels of *Chicken Shoot* at 240,
and the levels of *Chicken Shoot 2* at 255 or 224.

## Locating the player

Detection looks for the code of the mix routine in the ROM and checks the
other routines at their distances from it. Their literal pools contain the
addresses of the state of the player, the records of the channels and the
tables. The game code in the ROM header then selects the entry of the table
with the set-up and songs of the game. A game with the player that isn't in
the table gets an error.

## The songs

A song is a module and an order list, up to the last order of the list. The
list of the title comes first, then each distinct list of the level table of
the game, in the order of the table. The modules also contain patterns that no
list plays. They contain no sound that the songs don't already play. Most of
their instruments have no sample in the cartridge. Every note in them that
does sound is a note that one of the songs of *Chicken Shoot* plays, in both
games. The conversion leaves them out.

## The conversion

The conversion plays the model a tick at a time and reads each channel after
the tick. A note starts where a channel starts its sample, or where a sample
offset moves it, and ends where the channel stops or starts the sample again.
The key is the one nearest the step of the channel, and a bend covers the
rest. The volume of the channel × the master volume sets the expression on
both sides.

## What the conversion leaves out

- The mixer takes points without interpolation, and keeps 8 bits of the sum.
- The sound effects of the game are samples that the game mixes with the
  music. They aren't converted.
