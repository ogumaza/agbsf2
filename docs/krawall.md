# Krawall

Krawall is Sebastian Kienzl's XM and S3M module player for the Game Boy
Advance. Its converter translates tracker files into Krawall modules, with
sample and instrument tables shared by all the modules of a game. The library
plays them through a software mixer. This document describes the data,
player and mixer in the build of 2003/09/01. The example addresses come from
the US release of *Digimon Racing* (game code `BDGE`). The sections after
[Quirks](#quirks) describe how the other builds that `agbsf2` reads differ
from it. For this build, `--info` prints the version string
`$Id: Krawall $Date: 2003/09/01 06:51:01 $`, stored in the ROM. It also
prints the build that detection finds. `agbsf2` finds the player and its
tables from the code (see [Locating the player](#locating-the-player)).

A module has up to 32 channels, each playing one note at a time on one of
the 32 mixer channels. A row names a channel with 5 bits. The player
record has room for 20 channels. The records of the channels past the 20th are
in the copy of the player record that a jingle keeps (see
[Architecture](#architecture)). A jingle that starts over such a module
overwrites them. The mixer runs at 16384 Hz and makes 276 samples
per frame. The player processes a tick whenever its sound timer expires, at
the exact sample where it falls rather than at a frame boundary.

## Architecture

The player is Thumb code in ROM. The mixer is ARM code. The start-up code of
the game copies the mixer to IWRAM with the rest of the IWRAM data of the
library. The game calls `kragInit()` with its choice of stereo,
`kramSetMasterVol()` and `kramQualityMode()` once, and `krapPlay()` to start a
module. Once a frame, it calls the worker of the mixer, `kramWorker()`. The
worker mixes the frame and runs the ticks of the player. In `BDGE` the
routines are:

| Address | Routine |
|---|---|
| `0x080130B4` | the game's sound init: `kragInit(1)`, `kramSetMasterVol(0x40080)`, `kramQualityMode(0)` |
| `0x08013194` | the game's routine that plays entry `n` of its table of modules in mode 1 if the music is on |
| `0x080132D0` | the game's routine that plays entry `n` of its table of modules once as a jingle (mode 4) if the music is on |
| `0x080372A8` | `kragInit(stereo)`: sets up the sound, the DMA and the timers, and calls `kramSetMasterVol(0x80)` |
| `0x08037490` | `kramQualityMode(mode)` |
| `0x08037518` | `kramSetMasterVol(volume)` |
| `0x08037648` | `kramSetMusicVol()`, the master volume of the music |
| `0x0803765C` | `kramSetSoundTimer(routine)`: starts and stops the ticks of the player |
| `0x08037684` | `kramSetSoundTimerBPM(bpm)`, the length of a tick |
| `0x0803774C` | sets the sides of each playing mixer channel again |
| `0x08037950` | finds the data of the row that the player is at |
| `0x080379D4` | moves to the next row after a row |
| `0x08037AF4` | plays a row |
| `0x08037F58` | plays a tick between rows |
| `0x08037FE0` | runs the envelopes and vibratos of the instruments |
| `0x080382E8` | the tick of the player, called by the worker |
| `0x08038398` | `krapPlay(module, mode, song)`: mode 1 loops, 2 plays one song of the module, 4 plays a jingle |
| `0x08038674` | ends a jingle and goes back to the module it interrupted |
| `0x080396E0` | `krapStop()` |
| `0x080373A8` | `getDmaAddress()`: returns the buffers and the samples for the worker to mix |
| `0x03000094` | `kramPlayExt(sample, sfx, handle, frequency, volume, pan)`, in ARM |
| `0x03000320` | `kramStop(handle)` |
| `0x030003E8` | `kramSetFreq(handle, frequency)` |
| `0x03000434` | `kramSetVol(handle, volume)` |
| `0x030004B4` | `kramSetPan(handle, pan)` |
| `0x03000578` | `kramSetPos(handle, position)` |
| `0x03000C48` | the mixer: mixes the playing channels for a number of samples |
| `0x03001068` | `kramWorker()` |

The effect handlers are Thumb routines listed in two tables. The
effect-column table at `0x087D4F40` has three words per effect: a row
handler, a handler for ticks between rows, and a byte indicating whether the
row handler also runs between rows. The volume-column table at `0x087D51A4`
has a routine and a byte per effect.

The tables and the RAM of the library contain:

| Address | Contents |
|---|---|
| `0x087BF3F4` | the game's samples: the address of each sample header |
| `0x08545474` | the game's instruments: a word for each instrument |
| `0x087D7CEC` | the game's table of modules: the address of each module header |
| `0x087D51F4` | the sine wave of the vibrato: 64 halfwords, then the ramp at `0x087D5274`, the square at `0x087D52F4` and the random wave at `0x087D5374` |
| `0x087D53F4` | the periods: 120 halfwords, from C-0 |
| `0x087D54E4` | the fine tunes: 64 halfwords, 32768 at entry 32 |
| `0x087D5564` | the linear frequencies: 768 words, the 64 steps of a semitone 12 times |
| `0x087D4F00` | the mixing routines: 16 words, for both sides, the left, both, the right, the centre, two unused, and the ramp at a stop, then again with interpolation |
| `0x02001644` | the player record (0x34 bytes), then its channels (20 of 0x60 bytes) |
| `0x02001DF8` | a copy of the player record and channels that a jingle keeps |
| `0x020008B4` | the mixer channels: 32 of 0x2C bytes |
| `0x02001638` | the routine of the sound timer, then its step (`0x0200163C`) and count (`0x0200163E`), halfwords |
| `0x02000E34` | the count that the sound timer keeps while it's stopped |
| `0x02001640` | the music volume, 128 at the start |
| `0x03000000` | the game's choice of stereo |
| `0x03000090` | the master volumes of the music and the sound effects, then the interpolation (`0x03000092`) and the ramping at a stop (`0x03000093`) |
| `0x03000600` | the count of plays that the handles of the mixer contain |

## Modules

A module starts with a header of `0x16C` bytes, followed by the addresses of
its patterns, a word each.

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 1 | the channels |
| `0x001` | 1 | the orders |
| `0x002` | 1 | the order that the module goes back to after the last |
| `0x003` | 256 | the orders: a pattern each, 254 for a marker between songs (`+++`) and 255 after the last (`---`) |
| `0x103` | 32 | the pan of each channel: -64 to 64 |
| `0x123` | 64 | the first order of each song, for a module with songs separated by markers |
| `0x163` | 1 | the global volume: 64 for full |
| `0x164` | 1 | the speed: the ticks of a row |
| `0x165` | 1 | the tempo, in BPM as in a tracker |
| `0x166` | 1 | 1 if the module has XM instruments, 0 for S3M samples |
| `0x167` | 1 | 1 for XM's linear frequencies, 0 for Amiga periods |
| `0x168` | 1 | 1 for the fast volume slides of S3M: they slide on the row tick too |
| `0x169` | 2 | flags that the player doesn't read |
| `0x16C` | 4 each | the patterns |

A pattern starts with an index of the data of every 4th row, 16 halfwords of
offsets from the start of the data. Then come the row count, a byte at `0x20`,
and the data from `0x21`. Each row is a list of the channels that it has data
for, ended by a 0. The data of each channel starts with a byte. Its low 5 bits
contain the channel, and its top 3 bits say what follows:

| Bit | Bytes | Contents |
|---|---|---|
| `0x20` | 2 | the note in the top 7 bits of the first byte (1 for C-0, 127 for a note off), and the instrument in the second byte and the low bit of the first (1 on, 0 for none) |
| `0x40` | 1 | the volume column: `0x10` to `0x60` for a volume of 0 to 64 (more is 64), `0x61` on for an effect |
| `0x80` | 2 | the effect and its operand |

For a module with S3M samples, the instrument is a sample, 1 for entry 0 of
the game's table of samples. For a module with XM instruments, it's an
instrument. An instrument names a sample for each note.

## Samples and instruments

A sample has a header of `0x12` bytes, followed by its points: 8-bit, unsigned,
with `0x80` as silence.

| Offset | Size | Contents |
|---|---|---|
| `0x00` | 4 | the loop's length, in points |
| `0x04` | 4 | the address just past the last point |
| `0x08` | 4 | the rate of C-4 (the player doesn't read it) |
| `0x0C` | 1 | the fine tune: signed, in 128ths of a semitone |
| `0x0D` | 1 | the relative note: signed, in semitones |
| `0x0E` | 1 | the default volume: 0 to 64 |
| `0x0F` | 1 | the default pan: -64 to 64 |
| `0x10` | 1 | the loop: 0 none, 1 forwards, 2 back and forth |
| `0x11` | 1 | 1 if the mixer interpolates the sample in its quality mode 1 |

An instrument starts with 96 halfwords: the sample of each note, as a number in
the game's table. Then come the volume envelope and the pan envelope, `0x34`
bytes each, a halfword of the fade after the release, and 4 bytes of vibrato:
its type and sweep (the player reads neither), its depth and its rate.

An envelope starts with 12 points of 4 bytes. Each point is a halfword with the
tick in its low 9 bits and the level (0 to 64) in its top 7, then the step of
the level to the next point, in 256ths of a level a tick. After the points come
the last point, the sustain point, the point that a loop goes back to, and
flags: 1 on, 2 a sustain, 4 a loop.

## The player

`krapPlay()` stops the module that is playing. A jingle pauses it instead. A
jingle that starts while nothing is playing plays once, as a module of its
own. `krapPlay()` sets the speed and the tick to the speed of the module. The
first tick therefore plays the first row. It also sets the global volume to 64,
the master volume of the music to the global volume of the module, and the
tempo. The module starts from the first order of song `song`. After its last
order, it goes back to the order that the header gives, or in mode 2 to the
first order of the song. Each channel starts with the pan of the module and a
channel volume of 64. The vibrato, the tremolo and the panbrello of a channel
start with the sine wave, and a new note starts the first two again. In a
module with instruments, the vibrato of each channel starts 32 points into the
wave.

### Ticks

The step of the sound timer, in samples, is 4 × 15 × 16384 over 24 times the
tempo, rounded down. That is 240 at a tempo of 170, about 68 ticks a second. A
tick adds 1 to the tick count. When the count reaches the speed, it goes back
to 0, and the tick plays the next row. Otherwise the tick plays the tick
between rows. Then, in a module with instruments, the tick runs the envelopes
and the vibratos of the instruments.

### Rows

For each channel that the row has data for, the player reads the note,
instrument, volume and effect. An instrument sets the sample and its volume. In
a module with instruments, it does so only with a note, and then sets the pan
of the sample too. A volume sets the volume. A volume column effect runs its
part for the row before the player reads the effect operand of the row.

A note off releases the note. With a volume envelope, the fade starts and the
envelopes go on past their sustain points. Without one, the note stops. Any
other note adds the relative note of the sample. With the slide to a note
(effects 19 and 24, but not 50), the note then sets the goal of the slide.
Otherwise the note does the following. In a module with instruments, it takes
the sample of the instrument for the note and starts the envelopes. It sets the
period of the note and the count of the tremor, and clears the start point.
After the effect, it starts a note. The effect can change the start point or
keep the note from starting. The note starts at the frequency of the period, at
the volume times the channel volume over 64, and at the pan of the channel.
With a start point, the player moves the position of the mixer channel there.

After the data of the row, each channel that the row had no data for loses its
effect. Each channel whose note an effect changed goes back to the period,
volume and pan of the note, if the effect of the channel has changed.

Between rows, the effects of the volume column and of the effect column run
their parts for the ticks between rows.

### Periods and frequencies

In a module with Amiga periods, the period of a note is its entry in the table
of periods, times the entry of the fine tune, over 32768. The entry of the fine
tune is 32 plus the fine tune over 4. The frequency is 14317456 (8363 Hz times
1712, the period of C-4) over the period. In a module with linear frequencies,
the period is 1536 plus 64 times the note plus half the fine tune. The
frequency is the entry of the linear table for the step of the period within
its octave (`period mod 768`), shifted right by 23 less the octave
(`period / 768`). The mixer steps through the sample by the frequency over
16384 points a sample. The sample therefore plays at the frequency in Hz.

### Effects

An operand of 0 keeps the last operand of the effect, unless the table says
that the effect does something with 0. The slides of the volume and the pan stop at 0 and 64, and
-64 and 64.

| Effect | Name | Function |
|---|---|---|
| 1 | Speed | sets the speed; 0 does nothing |
| 2 | Tempo | sets the tempo, from `0x20` |
| 3 | Speed or tempo | an operand below `0x20` sets the speed, 0 too, and from `0x20` on, the tempo |
| 4 | Jump | goes to the order after the row |
| 5 | Break | goes to the next order's row, in decimal digits |
| 6 | Volume slide (S3M) | `x0` up, `0y` down, between rows, and in a module with fast slides, on the row too; `xF` and `Fy` once, on the row |
| 7 | Volume slide (XM) | `x0` up, `0y` down, between rows |
| 8, 9 | Fine volume down, up | once, on the row |
| 10, 14 | Portamento down, up (XM) | 10 lowers the period by 4 times the operand between rows, and stops the note at 0; 14 raises it. Linear periods rise with the pitch, and Amiga periods fall |
| 11, 15 | Portamento down, up (S3M) | 11 raises the period by 4 times the operand between rows, or with `Fy`, by 4 times `y` once, and with `Ey`, by `y` once; 15 lowers it the same way, and stops the note at 0 |
| 12, 16 | Fine portamento down, up | 12 lowers the period by 4 times the operand, once, and stops the note at 0; 16 raises it |
| 13, 17 | Extra fine portamento down, up | 13 lowers the period by the operand, once, and stops the note at 0; 17 raises it |
| 18 | Volume | sets the volume, without a limit |
| 19 | Slide to a note | slides the period to the period of the note by 4 times the operand between rows; with glissando, plays the nearest note of the table of periods |
| 20 | Vibrato | sets the speed (`x0`) and depth (`0y`); between rows, adds the wave's point times 4 times the depth over 128 to the period, and moves on by the speed |
| 21 | Tremor | `xy`: plays the note for `x` + 1 ticks and mutes it for `y` + 1 |
| 22 | Arpeggio | plays the note, the note plus `x` and the note plus `y`, a tick each |
| 23 | Volume slide (S3M) and vibrato | the vibrato's part between rows, with its last speed and depth |
| 24 | Volume slide (S3M) and slide to a note | the slide's part between rows, at its last step |
| 25 | Channel volume | sets the channel volume, up to 64 |
| 26 | Channel volume slide | `x0` up, `0y` down, between rows |
| 27 | Offset | starts the note 256 times the operand points into its sample, or moves the playing note there; an operand of 0 does nothing |
| 28 | Pan slide | `x0` left, `0y` right, by twice the digit, between rows; `xF` and `Fy` once, on the row |
| 29 | Retrig | `xy`: starts the note again every so many ticks, and changes the volume by `x`: -1, -2, -4, -8 or -16 for 1-5, times 2/3 for 6, a half for 7, +1, +2, +4, +8 or +16 for 9-13, times 3/2 for 14 and twice for 15 |
| 30 | Tremolo | sets the speed and depth; between rows, adds the wave's point times the depth over 64 to the volume |
| 31 | Fine vibrato | the vibrato, a quarter as deep |
| 32 | Global volume | sets the global volume, up to 64 |
| 33 | Global volume slide | `x0` up, `0y` down, between rows |
| 34 | Pan | sets the pan: the operand less `0x40`, or 0 above `0x80` |
| 35 | Panbrello | sets the speed and depth; between rows, adds the wave's point times the depth over 64 to the pan |
| 36 | Mark | calls the game's callback with the operand |
| 37 | Glissando | 1 turns it on, 0 off |
| 38, 39, 40 | Wave | of the vibrato, the tremolo or the panbrello: 0 sine, 1 ramp, 2 square, 3 random, and from 4 on, a new note doesn't start it again |
| 43 | Pattern loop | 0 marks the row; `x` goes back to it `x` times |
| 44 | Note cut | stops the note at tick `x`; 0 keeps the row's note from starting |
| 45 | Note delay | starts the note at tick `x`, without starting the envelopes |
| 49 | Volume slide (XM) and vibrato | as 23 |
| 50 | Volume slide (XM) and slide to a note | as 24 |

Effects 41, 42, 46, 47 and 48 do nothing in this build.

The effects of the volume column, from `0x61`, are the high digit of the column
less 6, with the low digit as the operand:

| Column | Function |
|---|---|
| `0x6y` | volume down by `y`, between rows |
| `0x7y` | volume up by `y`, between rows |
| `0x8y` | volume down by `y`, once |
| `0x9y` | volume up by `y`, once |
| `0xAy` | sets the speed of the vibrato |
| `0xBy` | sets the depth of the vibrato, 4 times `y`, and plays the vibrato between rows |
| `0xCy` | sets the pan, from -64 for 0 to 64 for 15 |
| `0xDy` | pan left by `y`, between rows |
| `0xEy` | pan right by `y`, between rows |
| `0xFy` | sets the step of the slide to a note to 16 times `y`, if the last operand of the effect column isn't 0 (see [Quirks](#quirks)) |

### Envelopes

In a module with instruments, a note on an instrument with a volume envelope
starts the envelope before its first point, at a fade of 32767. Each tick adds
the step of the point to the level. At the tick of the next point, the level
becomes the level of that point. A sustain point holds until the release. The
last point goes back to the loop point, or ends the envelope. An envelope that
ends at level 0 ends the note too. The volume of the envelope is the level
times the fade over 2^23, 0 to 64. It multiplies the volume and the channel
volume, over 4096. After the release, each tick takes the step of the fade from
the fade, and the note stops at 0. The pan envelope works the same way, from
-64 at level 0 to 64 at level 64, and is added to the pan of the channel. It
differs from the volume envelope in three ways: it reaches each point a tick
sooner, a loop takes it back to a tick past the loop point, and its end never
stops the note. The vibrato of the instrument adds the point of the wave times
its depth over 256 to the period that the effects leave. It moves on by its
rate, a quarter of a point at a time.

### The next row

After a row, a jump goes to the first row of its order, through the order
before it. A break, or the end of the pattern, goes to the next order, past any
markers between songs. Past the last order, and at a marker in mode 2, the
module goes back to its restart order. In mode 2 it goes back to the first
order of its song instead. A module that doesn't loop stops there. A jingle
ends there, and the module that it interrupted goes on. A break then goes to
its row, or to the first row if the pattern is shorter.

## The mixer

Each of the 32 mixer channels plays a sample. A handle names a channel in its
low byte. The bytes above contain the play that started the channel, from a
count of every play. A player channel whose mixer channel has gone to another
note therefore can't change that note. `kramPlayExt()` uses the channel that
its handle names if the handle still matches the channel, even after the
sample has ended. Otherwise it uses the first free channel.

The volume and pan of a channel set the level of each side: the volume times
the master volume times 64 less the pan, or 64 plus it, over 8192. The volume
is 0 to 64 from the player, but the mixer keeps the low byte of any value that
it gets. The master volume of the music is the music volume (128 at the start),
times the global volume of the module, times the global volume of the player
(64 at the start), over 8192, up to 255. In mono, every channel plays on the
left. The DirectSound hardware plays the left side on both sides.

The worker mixes up to each tick that falls in the frame and runs the tick. It
then mixes on to the next tick or to the end of the frame. It keeps the count
of samples left to the next tick. A channel moves through its sample by its
step, a 16.16 fraction of points a sample, 4 times its frequency. At the end
of a loop it goes back by the length of the loop. With a loop back and forth,
it turns round instead, and turns again at the start of the loop. A sample
without a loop stops at its end. The mixer computes how many samples a channel
can mix before the end, in 4s, with the `Div` call of the BIOS. A channel at
volume 0 moves on by a guess without mixing: the samples times the whole
points of its step plus 1. The guess can stop a sample without a loop before
its end.

The mix adds each point times the level of its side, shifted right by 3 bits.
The output shifts the sum by the shift of the master volume: the third byte of
the argument of `kramSetMasterVol()` (5 if it's 0). It then scales the sum by
the low byte of the argument over 128, into the 8-bit buffers. At `kramSetMasterVol(0x40080)`, a channel at
a level of 128 plays its sample at full scale.

In quality mode 2 the mixer interpolates every sample. In mode 1 it
interpolates the samples whose header sets the flag. With flag 16 of the mode,
a stop ramps a channel that plays louder than 5 down over 16 samples. A ramping
channel keeps the count of samples left to ramp in the byte that selects its
master volume. To set the sides of the channel, the mixer then reads a master
volume past the two: the interpolation, the ramp, or a byte of the code of
`kramPlayExt()`. That code follows them in IWRAM.

## Quirks

* **Tremolo.** This build shifts the volume of the tremolo (the volume times
  the channel volume times the volume of the envelope) by 6 bits, not 12. The
  mixer therefore gets the low byte of a volume 64 times too loud.
* **Pan slide.** The fine pan slide right (`Fy`) marks the volume slide as
  fine, not the pan slide. A later normal pan slide on the channel therefore
  still slides between rows.
* **The slide to a note in the volume column** runs the part of the effect for
  the row, between rows. That part sets the step from the operand of the effect
  column. It doesn't slide.
* **Pattern loop.** The jump back of the loop is a break. It therefore goes to
  the marked row of the next order.
* **Note cut** stops the note, rather than setting its volume to 0.
* **Retrig** starts the note again on the ticks that the whole operand, rather
  than its low digit, divides.
* **Tremor** counts the row tick too.
* **Arpeggio** plays linear frequencies in every module, and starts each row
  from the note, with the note plus `x` on the first tick between rows.
* **Note delay** doesn't start the envelopes of the instrument again. A new
  note has started them when the row was read.
* **A start point before a loop that plays backwards.** Take a sample with a
  loop back and forth that plays backwards. An offset or a retrig can move it
  to before its loop. The mixer then computes a negative count of samples and
  runs past its buffer. `agbsf2` moves the position as the mixer would, but
  doesn't model what happens to the memory of the game.

## The builds of player.c

Five builds before 2003/09/01 contain a version string that names the revision
of `player.c`, such as `$Id: player.c,v 1.4 2003/03/19 23:12:15 seb Exp $`:

| Revision | Date | A game with it |
|---|---|---|
| 1.2.2.10 | 2002/08/24 | a homebrew game |
| 1.2.2.14 | 2002/09/12 | *Racing Fever* (`BRWP`) |
| 1.2.2.15 | 2002/12/19 | a prototype |
| 1.2.2.17 | 2003/01/13 | a homebrew game |
| 1.4 | 2003/03/19 | *007: Everything or Nothing* (`BJBE`) |

Each build is the one before it with changes. The build of 2003/09/01 is 1.4
with a few more. The data of the modules, samples and instruments is the same
in all of them. So are the tables and the record of the player, the table of 51
effects, the mixer rate of 16384 Hz and the 276 samples a frame. The table of
effects is at `0x0805160C` in `BJBE` and `0x08012CD0` in `BRWP`. The table of
mixing routines is `0x78` bytes before it in the builds before 1.4, and `0x74`
before it in 1.4. In agblib, `src/krawall/player_v1.cpp` contains the parts of
the model in which these builds differ.

### Build 1.4

Build 1.4 differs from 2003/09/01 in five ways:

* **The guess of the points of a channel.** To skip a channel at volume 0, and to
  test whether a sample ends in the samples to mix, the mixer guesses the
  points that the channel moves. It takes the step in 256ths of a point, times
  the samples, over 256, plus 1 point. The build of 2003/09/01 takes the whole
  points of the step, times the samples, plus 1 a sample. The test for the
  start of the loop, in a channel that plays backwards, adds nothing.
* **The next row.** A jump goes straight to the first row of its order, or to
  the first order if it names one past the last. It doesn't pass through the
  order before it, the markers or the end of the module. A break then goes to
  its row.
* **The pan of an instrument.** In a module with instruments, the note takes
  the pan of its sample when it starts, before the envelopes start. The build of
  2003/09/01 takes it when the row gives the instrument.
* **A note off** releases a pan envelope that's off too. That envelope then
  runs.
* **`kramSetMasterVol()`** doesn't store the last entry of its table.

The player record and channels, and the 32 mixer channels of `0x2C` bytes, are
laid out as in 2003/09/01. In `BJBE` they're at `0x02001644` and
`0x020008B4`.

### The builds before 1.4

Builds 1.2.2.10 to 1.2.2.17 have a mixer that 1.4 rewrote:

* **24 mixer channels** of `0x28` bytes, in IWRAM. A record has its fields
  up to the interpolation, at `0x20`, at the offsets of the later builds. Then
  come the interpolation flag of the sample (`0x21`), the flag for sound
  effects (`0x22`), the mixing routine (`0x23`) and the start of the loop (a word
  at `0x24`).
* **No ramping.** `kramQualityMode()` takes 0 to 2 as the mode, and any other
  value as 0. *Racing Fever* calls it with `0x12`. The mixer of the game then
  doesn't interpolate.
* **`kramSetMasterVol()`** takes its whole argument as the output level
  over 128, and the shift is always 5. It stores the last entry of its table
  through an address with a bit set in its top four bits. The hardware
  ignores those bits. The store therefore reaches the table.
* **The sound timer.** The worker starts the count of the timer again at each
  tick. It runs the tick only while the timer runs. `kramSetSoundTimer()`
  keeps the count as it is.
* **The volume column** contains a volume from `0x10` to `0x50`. An effect
  starts at `0x51`. A column from `0x51` to `0x5F` names an entry past the
  table of column effects. `agbsf2` leaves such a column out, with a warning.
* **A module that doesn't loop** stops at its end, and the player still finds
  the row that it would go back to. Nothing plays that row.

### The builds before 1.2.2.17

Builds 1.2.2.10 to 1.2.2.15 have no channel volume. The table of effects has
no routines for effects 25 and 26. Those effects do nothing. A note starts at
its volume, and the volume of the envelope multiplies the volume over 64. The
shift of 6 bits for the tremolo is right in these builds. The record of a
channel has neither the channel volume nor its slide. The pan, the start point and the effect move
down a byte, to `0x05` to `0x07`. The note, the period and the played period
move down two, to `0x08` to `0x0D`. The bytes from the flag of a volume slide
that runs once to the volume of the envelope move down one, to `0x3E` to `0x4C`.
The rest stays.

### The builds before 1.2.2.15

Builds 1.2.2.10 and 1.2.2.14 handle jumps and breaks another way:

* The operand of a break is binary, not decimal, and a row past 63 counts as
  row 0.
* A jump also breaks to row 0, unless a break has given a row. A break also
  jumps to the next order, unless a jump has given an order.
* After the row, a jump goes to the first row of its order, or to the first
  order if it names one past the last. A break then goes to its row, whatever
  the length of the pattern. Neither looks for a marker or the end of the
  module. A break in the last order therefore goes to the first. Otherwise the
  next row works as in the later builds.
* Some of their calls of `Div` in the BIOS have no test for a divisor of 0.
  The later builds have the test. On the hardware, `Div` doesn't return after
  a divisor of 0. `agbsf2` goes on as the later builds do.

### Build 1.2.2.10

Build 1.2.2.10 runs the envelopes another way. An envelope starts at tick 0
rather than 255. A tick compares the count of ticks with the tick of the next
point before it moves the count on. It takes the level before it adds the
step. The volume envelope reaches the same levels on the same ticks as in the
later builds. The pan envelope reaches each point on the same tick as the
volume envelope, and a loop takes it back to the tick of the loop point. A
volume envelope that runs sets the volume of the channel on every tick, after
a sustain point and after the end of the envelope too.

A volume in the volume column leaves the last effect of the column in place.
That effect goes on between the rows.

### The games' code

The sound init of *Racing Fever* calls `kragInit(1)`, `kramQualityMode(0x12)`
and `krapSetMusicVol(0, 0)`. The routine at `0x080042A4` sets the music volume
to half of the value that the game passes to it, 127, and plays a module in
loop mode.

*007: Everything or Nothing* plays each piece through a table of what it
plays, at `0x0804F498`. Each entry contains a module and the mode and song for
`krapPlay()`. The routine at `0x08022260` plays an entry in that mode. The
game plays most of its music in loop mode, some of it from the first order of
a song, and a few pieces once, in song mode or as a jingle.

## Locating the player

`agbsf2` looks for the code of this build:

* `krapPlay()`, from its start:
  `B5F0 4657 464E 4645 B4E0 1C0F 4680 4692 1C38 2204 4010 2800`.
* The note on in the routine that plays a row:
  `0407 49xx 0BBA 588F 0071 616F 3902 5A78 4Fxx 0082 58B8 6128`. The first
  `ldr` loads the address of the table of instruments, and the second the
  address of the table of samples.
* The timers of `kragInit()`:
  `48xx 49xx 4Bxx 8008 3102 48xx 800B 3906 8008`. The fourth `ldr` loads the
  reload value of Timer 0. The mixer rate is 2^24 over `0x10000` less the
  reload value.
* The step of `getDmaAddress()`, half the samples of a frame:
  `49xx 23xx 0058 680B 181A 600A`.
* `kragInit()`, `kramQualityMode()` and `kramSetMasterVol()`. The calls in the
  game set their arguments:
  `B510 1C04 2080 F000 F9xx 1C20 F000 F8xx F000 F8xx`,
  `B530 2210 4002 2A00 D0xx 49xx 2301 700B` and
  `B5F0 1C04 0C20 2800 D0xx 49xx`. `kragInit()` calls `kramSetMasterVol()`
  itself, before the game can.
* A call of `krapPlay()` in the game for an entry of a table of modules:
  `49xx 00xx 1840 6800 21xx 22xx Fxxx Fxxx`. That's `ldr r1, =table`,
  `lsls r0, rN, #2`, `adds r0, r0, r1`, `ldr r0, [r0]`, `movs r1, #mode`,
  `movs r2, #song` and the `bl`. Where the mode has bit 2 set and bit 0
  clear, the call plays a jingle. With bit 0 set too, `krapPlay()` loops the
  module. The routine that contains the call must start within 64 bytes before
  it, with `push {..., lr}`, `lsls r0, r0, #24` and `lsrs rN, r0, #24`. The
  first argument of the routine then selects the entry. Each call of the
  routine in the game sets the entry with `movs r0, #entry`. `agbsf2` converts the
  module of each of those entries once, as a jingle. In `BDGE`, the routine at
  `0x080132D0` takes that form. The three calls of the routine in the game
  pass entry 16. A module whose orders separate songs with `+++` still converts
  song by song.
* A call of `krapPlay()` in the game for a module:
  `48xx 21xx 22xx Fxxx Fxxx`, or `48xx 21xx Fxxx Fxxx` without the song.
  That's `ldr r0, =module`, `movs r1, #mode`, `movs r2, #song` and the `bl`.
  If no such call loops a module, the module plays once. It converts in the
  mode of the first such call in the ROM. *Cocoto Kart Racer* (`BC5P`) plays
  five modules once that way, in mode 0. Those are the three jingles at the end
  of a race, from `0x080053BA` to `0x080053E6`, and the ones that the calls at
  `0x08005426` and `0x0800BC9C` play. The game also plays the three jingles in
  mode 4 at the end of a cup, from `0x08009692` to `0x080096D6`. Mode 0 plays a module once and stops. It
  doesn't play the module as a jingle.

The builds of `player.c` are told apart by their version strings, from
`$Id: player.c,v ` up to the date. Their code matches the patterns above with
three changes:

* The note on in the routine that plays a row has another form in every build of
  `player.c`, with the same literals:
  `0407 49xx 0BBA 588F 00x0 616F 3802 5A39 4Fxx 008A 58B8 6128`.
* `kramQualityMode()` before 1.4:
  `B510 4Axx 2802 DCxx 7010 4Axx 4Cxx 2018 1C91 780A 2A00`.
* `kramSetMasterVol()` before 1.4:
  `B5F0 4Cxx 4Exx 1C05 2100 2000 11C3 1C1A 2B7F DD00`.

A game with one of these builds can call the routines of the library from ARM
code. `agbsf2` reads two forms of such a call. One is a `bl` to a veneer of
`ldr ip, [pc]`, `bx ip` and the address of the routine. The other is
`ldr rN, =routine`, `mov lr, pc` and `bx rN`. The start-up code of `BFKE` puts
a `mov r0, #value` between the `ldr` and the `mov lr, pc`, and `agbsf2` reads
that form too. The argument is the last value written to `r0` before the call.
It must come from a `mov` of a constant, an `ldr` of a literal or a `mov` from
a register that one of those set. That write must fall within the 16
instructions before the call. Each of them must be unconditional, and none of
them a branch, a call or a `push`.

The tables of the player are found by their first values. The first values of
the square wave, all 255, also turn up in other data. So can those of the
ramp. The ramp follows the sine, and the square wave follows the ramp. The
square wave is therefore taken from right after the ramp. The ramp is taken
from right after the sine where it's there. In *Franklin's Great Adventures*,
the first values of the ramp turn up in other data. Where no ramp follows the
sine, the ramp is the first copy of its values that the square wave follows.
*Cocoto Kart Racer* (`BC5P`) contains another copy of the first values of the
ramp before the tables of the player. The modules are found by
a scan of the ROM for headers that the player can play: up to 32 channels, a
restart before the last order, a speed, a tempo from `0x20` on, flags of 0 or 1,
pans within -64 and 64, and patterns whose index starts at 0 and doesn't go
back. The table of modules of the game is the longest run of words that each
point to a module that the scan found. The order of the table numbers the
modules. The modules that it doesn't list come after them, in ROM order.
`--song-table` sets a table of modules instead. `--song-count` sets the number
of entries in that table or in the table of the game. With `--song-count`, the modules that the table doesn't list
are left out. Without a table, `--song-count` keeps that many of the modules
that the scan finds.

A game can play its music through a table of what it plays, rather than
through its table of modules. An entry of such a table has 8 bytes: the
address of the module, the mode and the song for `krapPlay()`, and 2 bytes of
0. `agbsf2` finds the table through the routine of the game that plays an
entry, from its call of `krapPlay()`:
`49xx 00xx 1840 6803 6844 7901 0A22 0612 0E12 1C18 Fxxx Fxxx`. That's
`ldr r1, =table`, `lsls r0, rN, #3`, `adds r0, r0, r1`, `ldr r3, [r0]`,
`ldr r4, [r0, #4]`, `ldrb r1, [r0, #4]`, `lsrs r2, r4, #8`,
`lsls r2, r2, #24`, `lsrs r2, r2, #24`, `adds r0, r3, #0` and the `bl`. The
table ends before the first entry whose module isn't one that detection
found, whose mode isn't 0 to 7, or whose last 2 bytes aren't 0. The entries
convert in the order of the table, before the modules that no entry plays. An
entry that repeats an earlier one is left out. Each entry plays in its mode.
It starts from its song if the orders of the module separate songs with `+++`.
*Eragon* (`BENE`) has such a routine at `0x0803FD04`. Its table, at
`0x08F2C9D4`, has 29 entries.

A game can also set a mode and a music volume for each entry of its table of
modules, in two separate tables. *Franklin's Great Adventures* (`BFLE`) plays
entry n through the routine at `0x0800362C`:
`B510 1C04 48xx 00A1 1808 6800 4Bxx 18C9 6809 Fxxx Fxxx 48xx 1820 7800 2100 Fxxx Fxxx`.
That's `push {r4, lr}`, `adds r4, r0, #0`, `ldr r0, =modules`,
`lsls r1, r4, #2`, `adds r0, r1, r0`, `ldr r0, [r0]`, `ldr r3, =modes`,
`adds r1, r1, r3`, `ldr r1, [r1]`, `bl krapPlay`, `ldr r0, =volumes`,
`adds r0, r4, r0`, `ldrb r0, [r0]`, `movs r1, #0` and
`bl krapSetMusicVol`. The table of modes has a word for each entry, at
`0x08048000`. The table of volumes has a byte for each entry, at
`0x08048044`. The start-up code of the game copies the table of modules to RAM, at
`0x0300605C`, and the routine reads that copy. `agbsf2` takes the table of
modules that its scan found. Each entry converts in its mode, at its volume.
An entry that repeats an earlier one is left out. In `BFLE`, 3 of the 17
entries have mode 0. The game plays those once. The volumes go from 70 to
128.

## The builds from 2004/09/17 on

Three builds after 2003/09/01 have a version string with their date:

| Build | Version string | A game with it |
|---|---|---|
| 2004/09/17 | `$Id: Krawall $Date: 2004/09/17 09:01:35 $` | *Juka and the Monophonic Menace* (`BJKE`) |
| 2004/11/14 | `$Id: Krawall $Date: 2004/11/14 16:32:34 $` | *Babar to the Rescue* (`BBVE`) |
| 2005-04-21 | `$Id: Krawall $Id: version.h 8 2005-04-21 12:24:45Z seb $` | *Eragon* (`BENE`) |

The three builds have the same player. The code of their mixers differs in a
few routines. The build of 2005-04-21 is the one whose source is public.

### Data

The modules, samples and instruments are laid out as in 2003/09/01, but for
two changes to the patterns:

* The row count of a pattern is a halfword at `0x20`, and its data starts at
  `0x22`.
* The note of a channel takes 2 or 3 bytes. The first byte contains the note
  in its low 7 bits. The second contains the low 8 bits of the instrument. If
  the top bit of the first byte is set, a third byte contains the top 8 bits of
  the instrument.

### The player

The player record contains the row as a halfword at `0x0E`. The speed, the
tick, the order and the order that a jump goes to follow it, a byte each. The
row that a break goes to is a halfword at `0x18`. A break past row 127 goes to
its row. In 2003/09/01, such a break counts as none.

The record of a channel has `0x64` bytes. A played volume and a played pan follow
the start point, at `0x08` and `0x09`. They move the effect, the note and the
periods on by 2 bytes. With the alignment of the words, the rest of the
record moves on by 4.

The mixer plays the played volume and pan. The tremolo and the tremor set the
played volume, and the panbrello sets the played pan. Every other change to
the volume or the pan sets the played one too. The player then passes to the
mixer the channel volume times the played volume times the volume of the
envelope, over 4096. It also passes the played pan plus the pan of the
envelope, within -64 and 64. In 2003/09/01, the player passes the volume and
the pan. A tick of an envelope there undoes what the three effects add.

The player differs in six more ways:

* **Envelopes.** In a module with instruments, a note starts its envelopes
  when it starts. The build of 2003/09/01 starts them when it reads the row. A
  delayed note starts them on its tick.
* **The vibrato of an instrument** runs on an instrument without envelopes too. In
  2003/09/01, it runs only on an instrument with an envelope.
* **Tremolo.** The volume of the tremolo becomes the played volume. The quirk of
  2003/09/01, a volume 64 times too loud, is gone.
* **Tremor** mutes the note through its played volume. A tick of a volume
  envelope then keeps it muted until the tremor plays it again.
* **Note cut** at tick `x` sets the volume to 0 and leaves the note playing.
* **Retrig** starts the note again on the ticks that the low digit of the
  operand divides.

### The mixer

* **Ramping.** The ramp at a stop is on unless flag 32 of `kramQualityMode()`
  turns it off. Flag 16 does nothing.
* **`kramSetMusicVol()`** sets the sides of the music channels again, and
  leaves those of the sound effects as they are. In 2003/09/01, it sets the
  sides of every playing channel. From 2004/11/14 on, it also selects the
  mixing routine of each music channel again from its pan.
* **The centre.** In 2005-04-21, a channel in the centre mixes with the
  routine for the centre. In mono, every channel plays at the level of the
  centre, rather than of a pan to the left. A game in mono then plays at half
  the level of the earlier builds.
* **The samples of a call.** A call of the worker mixes 276 samples in most
  games. In *Cocoto Platform Jumper* (`BC8P`), *Let's Ride! Friends
  Forever* (`BEFE`) and *Let's Ride! Dreamer* (`BL9E`), a call mixes 552, at
  the same rate of 16384 Hz.

The mixer channels and the other variables of the mixer are laid out as in
2003/09/01.

### Locating the player

The three builds have the same code for the player, in other forms than in
2003/09/01:

* `krapPlay()`:
  `B5F0 4657 464E 4645 B4E0 4680 2002 1C0F 4692 Fxxx Fxxx 2003 Fxxx Fxxx 2204`.
* The note on: `0431 48xx 0B8A 5886 007C 61AE 3C02 5B31 48xx 008E 5984 616C`.
  The first `ldr` loads the address of the table of instruments, and the
  second the address of the table of samples.
* The timers of `kragInit()`: `4Bxx 49xx 48xx 800B 3102 4Bxx 8008 3906 800B`.
  The fourth `ldr` loads the reload value.
* The step of `getDmaAddress()`: `4Bxx 18D5 23xx 6035 00xD`. That's
  `movs r3, #n` and then `lsls r5, r3, #shift`. `n` shifted left by the
  shift is the number of samples that a call of the worker mixes. In most games, `n`
  is 138 and the shift is 1, for 276 samples. In the three games whose calls
  mix 552 samples, the shift is 2.
* `kragInit()` and `kramQualityMode()`:
  `B510 1C04 2080 F000 Fxxx 1C20 F000 F8xx` and
  `B530 2120 4001 2900 D0xx 49xx 2300 700B`. `kramSetMasterVol()` takes
  the same form as in 2003/09/01.

Detection tells the three builds apart by the code of their mixers, not by the
version string:

* 2005-04-21: `425A 0993 E7xx 2304 E7xx 2301 7023 7042`, in the routine that
  selects the mixing routine of a channel from its pan.
* 2004/11/14: `425A 0993 E7xx 2300 E7xx 2201 2340 7022 425A 1C13 7043`, in the
  same routine.
* 2004/09/17: `B570 4Bxx 28FF D9xx 20FF 7018 48xx 2420 1C1E 2540 7881 2900`,
  the start of `kramSetMusicVol()`.

## The unsigned build

*Puppy Luv: Spa and Resort* (`BYXE`) has a build of Krawall without a version
string. Another compiler built it, with other registers and another order of
routines. Its player and mixer work as in 2005-04-21, and its data and records
have the same layout.

The start-up code of the game copies the IWRAM code and data of the library to
`0x03000500` on. `kramPlayExt()` and the routines that it calls are ARM code
in EWRAM, from `0x02001B2C` on. The player record is at `0x02000010`.

Detection finds the build by its code:

* `krapPlay()`:
  `B5F0 465F 4656 464D 4644 B4F0 4681 2002 468A 4693 Fxxx Fxxx 2003 Fxxx Fxxx 2204`.
* The note on loads the table of samples at the start of the row, with the
  first `ldr` of `48xx 49xx 2300 7473 9004 9102 469B 6973 7818 3301 6173 2800`.
  It loads the table of instruments where it plays a note, with the first `ldr`
  of `4Axx 0083 589A 4640 0043 61AA 189B 3B02 881B 9A04 009B 5899`.
* The timers of `kragInit()`: `4Axx 4Bxx 801A 22C4 3302 801A 4Axx 3B06 801A`.
  The third `ldr` loads the reload value.
* The step of `getDmaAddress()`:
  `49xx 4Bxx 680A 18D3 602B 4Bxx 18D3 6033 23xx 005B`.
* `kragInit()`: `B510 1C04 2080 F000 Fxxx 1C20 Fxxx Fxxx 2001 F000 Fxxx`.
* `kramQualityMode()`: `B510 0683 D5xx 4Axx 2300 7013 230F 4018 2802 DCxx`.
* `kramSetMasterVol()`: `B5F0 4647 B480 1C04 0C00 2800 D1xx 2005`.

## The modified build

*Big Mutha Truckers* (`B63E`), *Street Racing Syndicate* (`BCZP`), *M&M's
Break 'Em* (`BEME`) and *Princess Natasha* (`BNPE`) have a build of Krawall
without a version string. Their developer changed the build of 2005-04-21 and
built it with another compiler. Its records have the layout of 2005-04-21. Its
mixer has 14 channels. `krapPlay()` takes a negative song
as an order to start from. The games keep each module packed in a music file.
Before a game plays a module, a routine of the game unpacks the file into RAM,
and then the game calls `krapPlay()` with the module there.

### Music files

A music file starts with a byte that counts the samples that it lists. A byte
follows for each of them: its number in the game's table of samples. The game unpacks
those samples into RAM before the module. No music file of the four games
lists any. The packed data follow, at the next multiple of 4.

The packed data start with the unpacked size, 3 bytes, low byte first. Six
bytes follow:

| Offset | Value |
|---|---|
| 3 | the first escape code, `E` |
| 4 | the bits of an escape code, `A` |
| 5 | the most bits of a gamma code, `M` |
| 6 | the shortest run that has a long length, `L` |
| 7 | the low bits of the distance of a long copy, `D` |
| 8 | the count of frequent bytes, `N`, from 1 to 32 |

The `N` frequent bytes follow, and then the bit stream. The stream is read a
bit at a time, the top bit of each byte first.

A gamma code is up to `M - 1` bits of 1, then a bit of 0 unless there were
`M - 1` of them. As many bits as there were 1s follow. The value of the code is a
1 with those bits below it, from 1 to `2^M - 1`.

The stream is a list of items. Each item starts with `A` bits:

* A value other than `E` is the top `A` bits of a byte. Its other `8 - A` bits
  follow.
* `E` starts a gamma code `g`. If `g` is 2 or more, a second gamma code `h`
  follows. An `h` of `2L - 1` ends the stream. Any other `h` starts a copy of
  `g + 1` bytes. The high byte of the distance is `h - 1`, with `D` bits read below
  it, and only its low 8 bits count. A byte `b` follows. The copy goes back
  `256 × high + 256 - b` bytes and takes one byte at a time. It can copy
  bytes that it has just made.
* A `g` of 1 starts a bit. A 0 starts a copy of 2 bytes, back `256 - b`
  bytes, with `b` the next byte.
* A 1 starts another bit. A 0 there makes a byte whose top bits are `E`. Its
  other `8 - A` bits follow. Before them come `A` bits that become the next
  `E`.
* A 1 there starts a run. A gamma code `r` is its length. If `r` is `L` or
  more, `9 - M` bits follow below it, and a gamma code is the high byte of the
  length, plus 1. The run has the low 8 bits of `r`, plus 1, plus 256 for each
  of that high byte. A gamma code `f` then selects the byte. Below 32, it's
  frequent byte `f`, from 1. From 32 on, 3 bits follow below it, and the low
  8 bits of the result are the byte.

The unpacking routine of the game writes halfwords. A last odd byte is written
with a 0 after it.

### Unpacked modules

The unpacked data end with a byte for the count of instruments, `I`, and one
for the count of patterns, `P`. From the start, they contain:

* the patterns, in the layout of the builds from 2004/09/17 on
* the header of the module, as in those builds, up to `0x16C`
* at `0x16C`, a word for the address of the instruments of the module
* from `0x170`, a word for each pattern: its offset in the unpacked data
* the `I` instruments, `0x130` bytes each, laid out as in those builds

The loader of the game turns the offset of each pattern into an address, and
stores the address of the instruments at `0x16C`. The instrument of a note is
entry `instrument - 1` of the instruments of the module. The sample of a note
comes from the game's table, as in the other builds. `kragInit()` copies that
table to RAM at the start.

The header of a sample contains the count of its points at `0x04`. The other
builds have the address after its last point there.

The byte at `0x12E` of an instrument is 1 if the mixer interpolates its notes
in quality mode 1. `kramPlayExt()` turns the interpolation off for every sound
that it starts. The note on of the player then sets the flag of the mixer
channel from that byte. The other builds set it from the header of the sample.

### Detection

Detection finds the build by its code:

* `krapPlay()`:
  `B5F0 4647 B480 1C07 1C0E 4690 2002 Fxxx Fxxx 2003 Fxxx Fxxx 2104`.
* The loader of music files reads the game's table of samples:
  `48xx 4641 0089 4688 4440 6805 686C 3446 2002 4240 4004`. The first `ldr`
  loads the address of the table.
* The timers of `kragInit()`: `49xx 4Axx 1C10 8008 3102 20C4 8008 3906 23xx
  021B 1C18 8008`. The `movs` and the `lsls` after it set the reload value of
  timer 0.
* Half the samples of a frame: `2002 7008 49xx 20xx 0080 6008`. The second
  `movs` sets the value.
* `kragInit()`:
  `B530 1C04 2080 Fxxx Fxxx 1C20 Fxxx Fxxx 2001 Fxxx Fxxx Fxxx Fxxx Fxxx Fxxx 48xx 8800`.
* `kramQualityMode()`: `B530 1C02 2020 4010 2800 D0xx 49xx 2000`.
* `kramSetMasterVol()`: `B5F0 1C07 0C38 2800 D1xx 2005`.

The code of the game points to its music files from literals and from tables.
Detection reads every word of the ROM that points to a music file. A file
counts if it unpacks to its size and contains a module that the player can
read. The files that tables of 2 or more words list come first, in the order of
the tables. The others follow in the order of the first word that points to each.
Only a game with no such words has its ROM scanned for music files.

Detection unpacks each module and lays it out as the loader of the game does.
The modules follow the end of the ROM, from the next multiple of `0x10000`. The
routines of the game that play a music file call the loader, directly or through a routine
that only calls it, and then `krapPlay()`. A call of such a routine with
`ldr r0, =file; movs r1, #0` plays the file once, in the jingle mode. The
others play the file in the loop mode.

## Set-up and play in other games

The [architecture](#architecture) section gives the set-up and play calls of
*Digimon Racing*. This section gives those of the other games that `agbsf2` supports,
by build.

*The Sims: Bustin' Out* (`ASIE`) has the build of 2003/09/01. Its sound init,
at `0x08041338`, calls `kragInit(1)`, `kramSetMasterVol(0x40080)` and
`kramQualityMode(2)`. The mixer then interpolates every sample. The sound init
then sets the music volume from the game's settings. Without a save, the
settings are the defaults at `0x0806A210`. Those give the music a volume of
255. `agbsf2` plays the music at 128. At 255, the loudest notes of a few
pieces would require more than the full loudness of a MIDI file. The conversion is
therefore 6 dB quieter than the game, with the same balance. The game plays
every piece through its table of what it plays, at `0x08D73EF8`. The table has
71 entries, one of them a repeat. The routine at `0x0804129C` plays an entry
in the entry's mode. The routine at `0x08041504` plays an entry as a jingle,
in mode 4. The game plays entry 27, of mode 2, that way. The song of that entry
is the last song of its module. Both modes play it the same way.

*Harry Potter and the Prisoner of Azkaban* (`BHTE`) has the build of
2003/09/01, with the same RAM as *Digimon Racing*. Its sound init, at
`0x0803FDFC`, calls `kragInit(1)`, `kramSetMasterVol(0x40080)` and
`kramQualityMode(2)`. The routine at `0x0803FF98` then sets the volumes from
the game's options at `0x030055A1` and `0x030055A2`. The effects get 25 times
their level, and the music gets 12 times its level through
`krapSetMusicVol()`. Both default levels are 10 of 10, at `0x0806B815`. The
music volume is then 120. `agbsf2` takes that volume from the table of games'
settings. The routine at `0x0803FEB4` plays an entry of its table of
modules, at `0x08FB0CC4`, in loop mode.

*Juka and the Monophonic Menace* (`BJKE`) has the build of 2004/09/17. Its
sound init, at `0x0802395C`, calls `kragInit(1)`. The mixer runs at quality 0,
with a master volume of `0x80`. The routine at `0x08023E9C` plays an entry of
its table of modules in loop mode. The routine at `0x08023A84` runs before the
worker of the mixer in each frame. That routine sets the music volume. After
each start, it raises the volume from 0 by 2 a frame, up to 128. `agbsf2` takes
that fade-in from a table of game settings. The first pass of a conversion then
fades in. The marked loop starts on the second pass.

*Babar to the Rescue* (`BBVE`) has the build of 2004/11/14. Its sound init,
ARM code at `0x0801B218`, calls `kragInit(0)`. The music is then mono. The
game plays its modules in mode 3, loop and song. Its modules have no markers
between songs, and go back to their first order at their end. Mode 3 therefore
plays them as mode 1 does.

*Eragon* (`BENE`) has the build of 2005-04-21. Its sound init, at
`0x0803FD90`, calls `kragInit(1)`, `kramSetMasterVol(0x40080)` and
`kramQualityMode(2)`. It then sets the music volume with `krapSetMusicVol()`,
from the game's options. The default music level of the options, 7 of 10, is at
`0x08063A41`. The table at `0x08063C8C` gives level 7 a volume of 161.
`agbsf2` takes that volume from the table of game settings. The game plays
every piece through its table of what it plays (see [Locating the
player](#locating-the-player)). Entries 4, 19 and 20 play in song mode. That
mode doesn't loop at the end of the module. The other entries play in loop mode.

*Cocoto Kart Racer* (`BC5P`) has the same build. The start-up code copies the
IWRAM data of the library in two parts. The init of the game calls `kragInit(1)` at
`0x0800CC00`. The mixer runs at quality 0, with a master volume of `0x80`.
The game plays each module through a direct call of `krapPlay()`. Five
modules play once, in mode 0 (see [Locating the player](#locating-the-player)).
The others play in loop mode. After some of those calls, the game sets the
music volume with `krapSetMusicVol()`, at `0x0800FBC8`. `krapPlay()` keeps the
volume. The module of a race plays at 100 on the first four tracks
(`0x0800C936`) and at 90 on the fifth (`0x0800C94E`). The jingle of the last
lap sets 90 (`0x0800542E`). The module of the last lap, the jingles at the end
of the race and the module of the results therefore play at 90. The menus set
128. `agbsf2` takes the volume of each module from the table of piece settings.
The jingle of the countdown stays at 128. The game plays it at 90 in the races
of a cup after the first.

*Cocoto Platform Jumper* (`BC8P`) has the same build. A call of its worker
mixes 552 samples. The routine at `0x08000298` calls `kragInit(1)`. The game
calls `kramQualityMode()` with 0 or 2 as its state changes. `agbsf2` mixes at
quality 0. The call at `0x08002B46` sets that quality. The routine at
`0x08008C1C` plays a module in the mode that its caller passes. It then sets the music volume from a level in the game's options. The
routine at `0x08008B88` starts a module later, in mode 1 or 0. Four modules
play once, in mode 0: from `0x0801357E`, `0x0801375A` and `0x08016B80` through
the first routine, and from `0x08013E4A` through the second. Detection can't
read those calls. `agbsf2` takes the four modules from the table of games'
settings.

Each caller of `0x08008C1C` passes a factor. It also sets whether the music
level in the options of the game scales the factor. The default level is 8 of
10 (`0x08000506`). With the level, the routine sets the factor times 8 over 10.
The module of a level has a factor of 110. The routine then sets 88. In the
first four worlds, a tip stops the music 51 frames after the start. The end of
a tip (`0x08016EB2`) and the end of a pause (`0x08016790`) set the factor times
8 over 12: 73. A restart after a lost life plays at 88 without a tip. `agbsf2`
takes the volume of each module from the table of piece settings:

| Module | Music volume | Where the game plays it |
|---|---|---|
| `0x0802DE90`, `0x081499C8`, `0x080322F0`, `0x08056334` | 73 | the levels of the first four worlds, after the tip |
| `0x08041C38` | 88 | the levels of the fifth world, without tips, and 120 at the end of the game (`0x08006118`) |
| `0x080439E4` | 80 | the flood when the clock runs out, a factor of 100 with the level (`0x080165A8`) |
| `0x0804EC30` | 88 | once, a factor of 110 with the level (`0x08013E4A`) |
| `0x08032C94`, `0x080565D4` | 110 | once, without the level (`0x0801375A`, `0x0801357E`) |
| `0x080349C0`, `0x081448FC`, `0x0813D328`, `0x0803520C` | 120 | without the level (`0x0800588C`, `0x08005D20`, `0x08008AAC`, `0x08016B80`) |

*Let's Ride! Friends Forever* (`BEFE`) has the same build, with calls of 552
samples. The start-up code copies the EWRAM data of the library in two parts.
The game calls `kragInit(0)`, for mono, at `0x08000370` and again at
`0x0801C0E2`. It then sets the music volume to 128 at `0x0801C114`. The game
plays each of its two modules in loop mode.

*Let's Ride! Dreamer* (`BL9E`) has the same build, with calls of 552 samples.
Its sound init, at `0x08004CE0`, calls `kragInit(0)`, for mono. It then sets
the music volume to 128 and calls `kramQualityMode(1)`. The routine at
`0x08004D88` plays each of its three modules in loop mode. *Horse and Pony:
Let's Ride 2* (`BYPP`) is the European release of the game. Its sound init is at
`0x08004BE0`. Its routine at `0x08004C88` plays the modules.

*Puppy Luv: Spa and Resort* (`BYXE`) has the unsigned build. Its boot, at
`0x08024BD6`, calls `kragInit(1)`, `krapSetMusicVol(60, 0)`,
`kramSetSFXVol(0x80)` and `kramQualityMode(0x20)`. The mixer then runs at
quality 0, without the ramp. The table at `0x0805B69C` names the module of each
piece of music, in entries of 8 bytes. The routine at `0x0802492C` plays
an entry. It first sets the music volume from the game's options with
`krapSetMusicVol()`, and then plays the module in loop mode. The default volume
of the options, from the routine at `0x0801A548`, is 64. `agbsf2` takes that
volume from the table of game settings. Five of the six modules that the
table lists have 21 to 29 channels. The game plays no jingles. A seventh
module, of one channel, is in no table.

The four games with the modified build set up the library in the same way.
The sound init of each game calls `kragInit()` with the stereo setting in the
options of the game, 1 by default. It then calls `kramQualityMode(0x21)` and
`kramSetMasterVol(0x40080)`. The mixer runs at quality 1, without the ramp.
The byte at `0x12E` of each instrument then sets whether its notes are
interpolated. The start-up code of each game sets the music volume of its options
with `krapSetMusicVol()`: *Big Mutha Truckers* 100 at `0x08000684`, *Street
Racing Syndicate* 64 at `0x080003DA`, *M&M's Break 'Em* 48 at `0x08000418`
and *Princess Natasha* 64 at `0x080008F4`. `agbsf2` takes the stereo, the
quality and the volume from the table of game settings. Each game plays a
music file through a routine that unpacks the file and calls `krapPlay()`.
*Princess Natasha* plays two of its files once, as jingles.
