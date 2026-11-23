# Frontline Studios' MOD player

*Hardcore Pool*, *Garfield: The Search for Pooky* and *Games Explosion!* play
their music with a player of four-channel modules in the manner of ProTracker.
It's the older of the two players of the F U H Sound System. The player and
the mixer are ARM code that the game copies to IWRAM. A software mixer of
eight channels plays the four channels of the module and four channels for
the sound effects of the game. It writes 8-bit output for FIFO A. FIFO A plays
on both sides. This document describes the player as `agbsf2` models it. The
addresses are those of *Hardcore Pool* (game code `BHOP`) unless a table lists
the addresses of each game.

## The games

| Game | Code | Player | Mixer | Master volume | Modules | Songs |
|---|---|---|---|---|---|---|
| *Hardcore Pool* | `BHOP` | `0x03002838` | `0x03000408` | `0x50` | `0x0832A954` | 11 |
| *Garfield: The Search for Pooky* | `BGOE` | `0x0300052C` | `0x03000390` | `0x30` | `0x08766D14`, `0x087BFDD4` | 18 |
| *Games Explosion!* | `BG7E` | `0x03002C10` | `0x03002A78` | `0x40` | `0x082BF4B0` | 15 |

The player and the mixer are the same code in the three games, apart from the
addresses. The code around them differs: each game sets up the mixer, starts
its modules and runs the player differently. agblib therefore contains a table
of the three games, with their set-ups and the orders at which the code of
each game starts each module.

## The frame

Once a frame, the vertical blank of the game restarts the sound DMA on the
buffer that the mixer filled in the last frame. It then runs the player and
the mixer in turn. The mixer writes 304 samples a frame. Timer 0 counts 924
cycles a sample. The mixer therefore runs at 2^24 / 924 = 18,157.2 Hz.

The player counts the frames of a row. At the last frame of the row it reads
the next row. On the other frames it runs the tick routine of each channel,
from the effect of the last cell of the channel.

## Modules

A module starts with a header of six words: the count of channels, the count of
orders, the count of patterns, the speed (frames a row), the address of the
instrument table and the address of the order table. Each order is the address
of the record of a pattern: the count of rows and the address of the cells. A
pattern with a count of 0 plays one row.

An instrument is four words: the start of the loop, the end of the loop (0 for
none), the volume (0 to 64) and the address of the sample. A sample is the
length in points and the address of the points, signed 8-bit.

A row contains a cell for each channel. The first byte of a cell contains the
flags, and 0 is an empty cell:

| Bits | Then |
|---|---|
| 0 | a period index (a halfword) and an instrument (a byte, 0 for the last one) |
| 0 and 4 (flags `0x11`) | the period index of the target of a slide, and a byte that the player skips |
| 7 | a volume (a byte) |
| 1 to 6 | an effect, as a byte offset into the table of effects of the player, and a parameter (a byte) |

A period index is a step in a table of 7,680 steps, 64 to a semitone, from the
highest pitch down. Index 4608 plays a sample at 8,363 points a second.

## Effects

| Number | Effect |
|---|---|
| 1 | keeps the parameter |
| 2 | slides the period index down by 4 × the parameter a frame |
| 3 | slides it up by 4 × the parameter a frame |
| 4 | slides toward the target by 4 × the parameter a frame |
| 5 | sets the volume |
| 6 | the volume slide: x up and y down a frame, from 0 to 64 |
| 7, 8 | the vibrato: speed x, depth y |
| 9 | sets the speed |
| 12 | the volume slide, with the vibrato of the parameter of the last vibrato |
| 14 | jumps to the first row of an order; the rest of the row plays |
| 15 | starts the sample 256 × the parameter points in, or at the last offset for 0 |

The other entries of the table do nothing. A slide toward a lower period
index, a higher pitch, reaches its target at once. Both paths of the handler
in that direction end on the target.

## Levels

At each row the player adds the fade step to the music volume, from 0 to 256,
and clears the step at either end. It then sets the level of each channel to
the volume of the channel × the music volume, shifted right by 8 bits. A
volume slide between rows changes the volume of the channel. The level follows
it at the next row.

The mixer scales each point by the level × the master volume, shifted right by
6 bits, then by 6 bits more. It adds the channels in 16-bit halves of a word,
on a bias, and maps each sum through a table to a byte. The init of the game
builds the table. At the scale of 64 in the table, the mapping halves the sum
and clips it to a byte.

## The games' starts

A game starts a module at an order with a fade step. The start sets the step
only while the music volume is 0, as after the init. *Hardcore Pool* and
*Garfield* start every song with a step of 256. The music therefore plays at
full volume from the first row. *Games Explosion!* starts most songs with a
step of 31, a fade-in over 8 rows, and orders 199, 219 and 230 with 255.

Each part of a module that a start reaches is a song. A part of a module that
no start reaches through the orders and the jumps becomes a song too. It
starts at the order after the jump that ends the part before it. *Games
Explosion!* has three such songs. `--info` marks them. Every song of
*Garfield* starts where the set-up of a level or another call of the game
starts it.

## Locating the player

Detection looks for the code of the player and the code of the mixer in the
ROM. Their literal pools contain the addresses of the state of the player, the
context of the mixer, the effect table, the table of periods, the vibrato
table, the clip table and the output buffers. The copy to IWRAM in the
start-up code shows the place of the code in the ROM. The game code and the
version in the ROM header then select the entry of the table with the set-up
and songs of the game. A release with an image that matches another release
outside the header uses the entry of that release. Revision 2 of the European
release of *Garfield* is such a release. A game or release with the player
that isn't in the table gets an error.

## The conversion

The conversion plays the model a frame at a time and reads each channel of
the module before the mix of the frame. A note starts where a channel starts
its sample, or where a sample offset moves it, and ends where the channel
stops or starts the sample again. The key is the one nearest the step of the
channel, and a bend covers the rest. The level of the channel sets the
expression on both sides.

## What the conversion leaves out

- The mixer takes points without interpolation, and maps each sum to 8 bits.
- The mixer checks the end of a sample once every two samples. It starts each
  pass of a loop again without the part of a point that the step had passed. A
  pass of a loop therefore lasts the first even count of samples in which the
  points pass the end of the loop, and runs up to two steps past it. A short
  loop plays flat of its step. From the first restart of the loop in a note,
  the conversion bends the note to the pitch at which the points of the loop
  take that count of samples. It does so where that pitch differs from the
  pitch of the step by 3 cents or more. The loop in the SoundFont includes the
  point at the end of the loop but not the points past it. The mixer always
  plays that point.
- The sound effects of the game play on the other four channels of the mixer.
  They aren't converted.
