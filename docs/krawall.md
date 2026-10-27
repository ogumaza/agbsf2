# Krawall

Krawall is Sebastian Kienzl's XM and S3M module player for the Game Boy
Advance. Its converter translates tracker files into Krawall modules, with
sample and instrument tables shared by all the game's modules. The library
plays them through a software mixer. This document describes the data,
player and mixer in the build of 2003/09/01. The example addresses come from
the US release of *Digimon Racing* (game code `BDGE`). The sections after
[Quirks](#quirks) describe how the other builds that `agbsf2` reads differ
from it. For this build, `--info` prints the version string
`$Id: Krawall $Date: 2003/09/01 06:51:01 $`, stored in the ROM. It also
prints the build that detection finds. `agbsf2` finds the player and its
tables from the code (see [Locating the player](#locating-the-player)).

A module has up to 32 channels, each playing one note at a time on one of
the mixer's 32 channels. A row names a channel with 5 bits. The player's
record holds 20 channels. The records of the channels past the 20th lie in
the copy of the player's record that a jingle keeps (see
[Architecture](#architecture)). A jingle that starts over such a module
overwrites them. The mixer runs at 16384 Hz, producing 276 samples
per frame. The player processes a tick whenever its sound timer expires, at
the exact sample where it falls rather than at a frame boundary.

## Architecture

The player is Thumb code in ROM, and the mixer is ARM code, which the game's
start-up code copies to IWRAM with the rest of the library's IWRAM data. The
game calls `kragInit()` with its choice of stereo, `kramSetMasterVol()` and
`kramQualityMode()` once, `krapPlay()` to start a module, and the mixer's
worker, `kramWorker()`, once a frame, which mixes the frame and runs the
player's ticks. In `BDGE` the routines are:

| Address | Routine |
|---|---|
| `0x080130B4` | the game's sound init: `kragInit(1)`, `kramSetMasterVol(0x40080)`, `kramQualityMode(0)` |
| `0x08013194` | the game's routine that plays entry `n` of its table of modules in mode 1 if the music is on |
| `0x080132D0` | the game's routine that plays entry `n` of its table of modules once as a jingle (mode 4) if the music is on |
| `0x080372A8` | `kragInit(stereo)`: sets up the sound, the DMA and the timers, and calls `kramSetMasterVol(0x80)` |
| `0x08037490` | `kramQualityMode(mode)` |
| `0x08037518` | `kramSetMasterVol(volume)` |
| `0x08037648` | `kramSetMusicVol()`, the master volume of the music |
| `0x0803765C` | `kramSetSoundTimer(routine)`, which starts and stops the player's ticks |
| `0x08037684` | `kramSetSoundTimerBPM(bpm)`, the length of a tick |
| `0x0803774C` | sets each playing mixer channel's sides again |
| `0x08037950` | finds the data of the player's row |
| `0x080379D4` | moves to the next row after a row |
| `0x08037AF4` | plays a row |
| `0x08037F58` | plays a tick between rows |
| `0x08037FE0` | runs the instruments' envelopes and vibratos |
| `0x080382E8` | the player's tick, which the worker calls |
| `0x08038398` | `krapPlay(module, mode, song)`: mode 1 loops, 2 plays one song of the module, 4 plays a jingle |
| `0x08038674` | ends a jingle and goes back to the module it interrupted |
| `0x080396E0` | `krapStop()` |
| `0x080373A8` | `getDmaAddress()`, which gives the worker the buffers and the samples to mix |
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

Its tables in ROM and its RAM hold:

| Address | Contents |
|---|---|
| `0x087BF3F4` | the game's samples: a word for each sample's header |
| `0x08545474` | the game's instruments: a word for each instrument |
| `0x087D7CEC` | the game's table of modules: a word for each module's header |
| `0x087D51F4` | the vibrato's sine wave: 64 halfwords, then the ramp at `0x087D5274`, the square at `0x087D52F4` and the random wave at `0x087D5374` |
| `0x087D53F4` | the periods: 120 halfwords, from C-0 |
| `0x087D54E4` | the fine tunes: 64 halfwords, 32768 at entry 32 |
| `0x087D5564` | the linear frequencies: 768 words, a semitone's 64 steps 12 times |
| `0x087D4F00` | the mixing routines: 16 words, for both sides, the left, both, the right, the centre, two unused, and the ramp at a stop, then again with interpolation |
| `0x02001644` | the player's record (0x34 bytes), then its channels (20 of 0x60 bytes) |
| `0x02001DF8` | a copy of the player's record and channels, which a jingle keeps |
| `0x020008B4` | the mixer's channels: 32 of 0x2C bytes |
| `0x02001638` | the sound timer's routine, then its step (`0x0200163C`) and count (`0x0200163E`), halfwords |
| `0x02000E34` | the count that the sound timer keeps while it's stopped |
| `0x02001640` | the music's volume, 128 at the start |
| `0x03000000` | the game's choice of stereo |
| `0x03000090` | the master volumes of the music and the sound effects, then the interpolation (`0x03000092`) and the ramping at a stop (`0x03000093`) |
| `0x03000600` | the count of plays, which the mixer's handles hold |

## Modules

A module starts with a header of `0x16C` bytes, followed by the addresses of
its patterns, a word each.

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 1 | the channels |
| `0x001` | 1 | the orders |
| `0x002` | 1 | the order that the module goes back to after the last |
| `0x003` | 256 | the orders: a pattern each, 254 for a marker between songs (`+++`) and 255 after the last (`---`) |
| `0x103` | 32 | each channel's pan: -64 to 64 |
| `0x123` | 64 | each song's first order, for a module whose songs the markers separate |
| `0x163` | 1 | the global volume: 64 for full |
| `0x164` | 1 | the speed: the ticks of a row |
| `0x165` | 1 | the tempo, as a tracker's BPM |
| `0x166` | 1 | 1 if the module has XM instruments, 0 for S3M samples |
| `0x167` | 1 | 1 for XM's linear frequencies, 0 for Amiga periods |
| `0x168` | 1 | 1 for S3M's fast volume slides, which slide on the row's tick too |
| `0x169` | 2 | flags that the player doesn't read |
| `0x16C` | 4 each | the patterns |

A pattern starts with an index of the data of every 4th row, 16 halfwords of
offsets from the data's start, then its row count, a byte at `0x20`, and its
data from `0x21`. Each row is a list of the channels that it has data for,
ended by a 0. Each channel's data is a byte that gives the channel in its low 5
bits and what follows in its top 3:

| Bit | Bytes | Contents |
|---|---|---|
| `0x20` | 2 | the note in the top 7 bits of the first byte (1 for C-0, 127 for a note off), and the instrument in the second byte and the first's low bit (1 on, 0 for none) |
| `0x40` | 1 | the volume column: `0x10` to `0x60` for a volume of 0 to 64 (more is 64), `0x61` on for an effect |
| `0x80` | 2 | the effect and its operand |

For a module with S3M samples, the instrument is a sample, 1 for entry 0 of
the game's table of samples. For one with XM instruments, it's an instrument,
which gives a sample for each note.

## Samples and instruments

A sample has a header of `0x12` bytes, followed by its points: 8-bit, unsigned,
with `0x80` as silence.

| Offset | Size | Contents |
|---|---|---|
| `0x00` | 4 | the loop's length, in points |
| `0x04` | 4 | the address just past the last point |
| `0x08` | 4 | the rate of C-4, which the player doesn't read |
| `0x0C` | 1 | the fine tune: signed, in 128ths of a semitone |
| `0x0D` | 1 | the relative note: signed, in semitones |
| `0x0E` | 1 | the default volume: 0 to 64 |
| `0x0F` | 1 | the default pan: -64 to 64 |
| `0x10` | 1 | the loop: 0 none, 1 forwards, 2 back and forth |
| `0x11` | 1 | 1 if the mixer interpolates the sample in its quality mode 1 |

An instrument has 96 halfwords, each note's sample as a number in the game's
table, then the volume envelope and the pan envelope, `0x34` bytes each, a
halfword of the fade after the release, and 4 bytes of vibrato: its type and
sweep, which the player doesn't read, its depth and its rate. An envelope has 12
points of 4 bytes, each a halfword with the tick in its low 9 bits and the level
(0 to 64) in its top 7, then the step of the level to the next point, in 256ths
of a level a tick, and then the last point, the sustain point, the point that a
loop goes back to and flags: 1 on, 2 a sustain, 4 a loop.

## The player

`krapPlay()` stops the module that plays, unless the new one is a jingle, which
pauses it. A jingle that starts while nothing plays plays as a module of its
own, once. It sets the speed and the tick to the module's speed, so that the
first tick plays the first row, the global volume to 64, the master volume of
the music to the module's global volume, and the tempo. The module starts from
song `song`'s first order, and goes back to the order after its last that the
header gives, or in mode 2 to the song's first. Each channel starts with the
module's pan, a channel volume of 64, the sine wave for the vibrato, the
tremolo and the panbrello, the first two of which a new note starts again, and
in a module with instruments, its vibrato 32 points into the wave.

### Ticks

The sound timer's step is 4 times 15 × 16384 over 24 times the tempo, rounded
down, in samples: 240 at a tempo of 170, about 68 ticks a second. A tick adds 1
to the tick count, and at the speed, goes back to 0 and plays the next row.
Otherwise it plays the tick between rows. Then, in a module with instruments,
it runs the envelopes and the instruments' vibratos.

### Rows

For each channel the row has data for, the player reads its note, instrument,
volume and effect. An instrument sets the sample, and its volume: in a module
with instruments, only with a note, and then the sample's pan too. A volume sets
the volume, and a volume column effect runs its part for the row, before the
row's effect operand is read.

A note off releases the note: with a volume envelope, the fade starts and the
envelopes go on past their sustain points; without one, the note stops. Another
note adds the sample's relative note, and with the slide to a note (effects 19
and 24, but not 50), sets the slide's goal. Otherwise, in a module with instruments, it
takes the instrument's sample for the note and starts the envelopes, and it
sets the note's period and the tremor's count, clears the start point and
starts a note after the effect, which can change the start point or keep the
note from starting. The note starts at the period's frequency, at the volume
times the channel volume over 64 and at the channel's pan. With a start point,
it moves the mixer channel's position there.

After the row's data, each channel that it had none for loses its effect, and
each channel whose note an effect changed, and whose effect has changed, goes
back to its note's period, volume and pan.

Between rows, the volume column's effect and the effect column's run their
parts for the ticks between rows.

### Periods and frequencies

In a module with Amiga periods, a note's period is its entry in the table of
periods times the fine tune's entry, 32 plus the fine tune over 4, over 32768.
Its frequency is 14317456 (8363 Hz times 1712, C-4's period) over the period.
In a module with linear frequencies, the period is 1536 plus 64 times the note
plus half the fine tune, and the frequency is the linear table's entry for the
period's step within its octave, `period mod 768`, shifted right by 23 less the
octave, `period / 768`. The mixer steps through the sample by the frequency
over 16384 points a sample, so the sample plays at the frequency in Hz.

### Effects

Each effect's operand of 0 keeps its last one, unless the table says it does
something with 0. The slides of the volume and the pan stop at 0 and 64, and
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
| 19 | Slide to a note | slides the period to the note's by 4 times the operand between rows; with glissando, plays the nearest note of the table of periods |
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
| 38, 39, 40 | Wave | the vibrato's, the tremolo's or the panbrello's: 0 sine, 1 ramp, 2 square, 3 random, and from 4 on, a new note doesn't start it again |
| 43 | Pattern loop | 0 marks the row; `x` goes back to it `x` times |
| 44 | Note cut | stops the note at tick `x`; 0 keeps the row's note from starting |
| 45 | Note delay | starts the note at tick `x`, without starting the envelopes |
| 49 | Volume slide (XM) and vibrato | as 23 |
| 50 | Volume slide (XM) and slide to a note | as 24 |

Effects 41, 42, 46, 47 and 48 do nothing in this build.

The volume column's effects, from `0x61`, are the high digit of the column
less 6, with the low digit as the operand:

| Column | Function |
|---|---|
| `0x6y` | volume down by `y`, between rows |
| `0x7y` | volume up by `y`, between rows |
| `0x8y` | volume down by `y`, once |
| `0x9y` | volume up by `y`, once |
| `0xAy` | sets the vibrato's speed |
| `0xBy` | sets the vibrato's depth, 4 times `y`, and plays the vibrato between rows |
| `0xCy` | sets the pan, from -64 for 0 to 64 for 15 |
| `0xDy` | pan left by `y`, between rows |
| `0xEy` | pan right by `y`, between rows |
| `0xFy` | sets the step of the slide to a note to 16 times `y`, if the effect column's last operand isn't 0 (see [Quirks](#quirks)) |

### Envelopes

In a module with instruments, a note on an instrument with a volume envelope
starts the envelope before its first point, at a fade of 32767. Each tick adds
the point's step to the level, and at the next point's tick, the level becomes
that point's: a sustain point holds until the release, and the last point goes
back to the loop's point, or ends the envelope, and the note too if its level
is 0. The envelope's volume is the level times the fade over 2^23, 0 to 64,
which multiplies the volume and the channel volume, over 4096. After the
release, each tick takes the fade's step from the fade, and the note stops at
0. The pan envelope works the same way, from -64 at level 0 to 64 at level
64, added to the channel's pan. It differs from the volume envelope in three
ways: it reaches each point a tick sooner, a loop takes it back to a tick past
the loop's point, and its end never stops the note. The instrument's vibrato
adds the wave's point times its depth over 256 to the period that the effects
leave. It moves on by its rate, a quarter of a point at a time.

### The next row

After a row, a jump goes to its order's first row, through the order before
it. A break, or the pattern's end, goes to the next order, past any markers
between songs. Past the last order, and at a marker in mode 2, the module goes
back to its restart order, or in mode 2 to its song's first order, unless it
doesn't loop, when it stops, or a jingle ends and the module it interrupted
goes on. A break then goes to its row, or the first if the pattern is shorter.

## The mixer

Each of the 32 mixer channels plays a sample. A handle names a channel in its
low byte and the play that started it, a count of every play, above, so that a
player channel whose mixer channel has gone to another note can't change that
note. `kramPlayExt()` uses the channel that its handle names if the handle is
still the channel's, even after its sample ended, or the first free channel.

A channel's volume (0 to 64 from the player, though the mixer keeps the low
byte of what it's given) and pan give each side's level: the volume times the
master volume times 64 less the pan, or 64 plus it, over 8192. The master
volume of the music is the music's volume (128 at the start), times the
module's global volume, times the player's global volume (64 at the start),
over 8192, up to 255. In mono, every channel plays on the left, which the
DirectSound hardware plays on both sides.

The worker mixes up to each tick that falls in the frame, runs the tick, mixes
on to the next or the frame's end, and keeps the samples left to the next
tick. A channel moves through its sample by its step, a 16.16 fraction of
points a sample, 4 times its frequency. At a loop's end it goes back by the
loop's length, or with a loop back and forth, turns round, and at its start
turns again. A sample without a loop stops at its end. The mixer works out how
many samples a channel can mix before its end with the BIOS's `Div`, in 4s,
and a channel at volume 0 moves on by a guess, the samples times the whole
points of its step plus 1, without mixing, which can stop a sample without a
loop before its end.

The mix adds each point times its side's level, shifted right by 3 bits, and
the output shifts the sum by the master volume's shift, the third byte of
`kramSetMasterVol()`'s argument (5 if it's 0), and scales it by its low byte
over 128, into the 8-bit buffers. At `kramSetMasterVol(0x40080)`, a channel at
a level of 128 plays its sample at full scale.

In quality mode 2 the mixer interpolates every sample, and in mode 1 the ones
whose header asks for it. With the mode's flag 16, a stop ramps a channel that
plays louder than 5 down over 16 samples. A ramping channel holds the count of
samples left to ramp in the byte that picks its master volume. To set its
sides, the mixer then reads a master volume past the two: the interpolation,
the ramp, or a byte of `kramPlayExt()`'s code. That code follows them in
IWRAM.

## Quirks

* **Tremolo.** This build shifts the tremolo's volume, the volume times the
  channel volume times the envelope's volume, by 6 bits rather than 12, so the
  mixer gets the low byte of a volume 64 times too loud.
* **Pan slide.** The fine pan slide right (`Fy`) marks the volume slide as
  fine, rather than the pan slide, so a later normal pan slide on the channel
  still slides between rows.
* **The volume column's slide to a note** runs the effect's part for the row
  between rows, which sets the step from the effect column's operand, rather
  than sliding.
* **Pattern loop.** The loop's jump back is a break, so it goes to the marked
  row of the next order.
* **Note cut** stops the note, rather than setting its volume to 0.
* **Retrig** starts the note again on the ticks that the whole operand, rather
  than its low digit, divides.
* **Tremor** counts the row's tick too.
* **Arpeggio** plays linear frequencies in every module, and starts each row
  from the note, with the note plus `x` on the first tick between rows.
* **Note delay** doesn't start the instrument's envelopes again, since a new
  note starts them when the row is read.
* **A start point before a loop that plays backwards.** An offset or a retrig
  that moves a sample with a loop back and forth to before its loop, while it
  plays backwards, has the mixer work out a negative count of samples, which
  runs it past its buffer. `agbsf2` moves the position as the mixer
  would, but doesn't model what happens to the game's memory.

## The builds of player.c

Five builds before 2003/09/01 hold a version string that names the revision
of `player.c`, such as `$Id: player.c,v 1.4 2003/03/19 23:12:15 seb Exp $`:

| Revision | Date | A game with it |
|---|---|---|
| 1.2.2.10 | 2002/08/24 | a homebrew game |
| 1.2.2.14 | 2002/09/12 | *Racing Fever* (`BRWP`) |
| 1.2.2.15 | 2002/12/19 | a prototype |
| 1.2.2.17 | 2003/01/13 | a homebrew game |
| 1.4 | 2003/03/19 | *007: Everything or Nothing* (`BJBE`) |

Each build is the one before it with changes, and the build of 2003/09/01 is
1.4 with a few more. The data of the modules, samples and instruments is the
same in all of them. So are the player's tables, the player's record, the
table of 51 effects, the mixer's rate of 16384 Hz and its 276 samples a frame.
The effects' table is at `0x0805160C` in `BJBE` and `0x08012CD0` in `BRWP`.
The mixing routines' table is `0x78` bytes before it in the builds before 1.4,
and `0x74` before it in 1.4. `src/krawall/player_v1.cpp` holds the parts of
the model in which these builds differ.

### Build 1.4

Build 1.4 differs from 2003/09/01 in five ways:

* **The guess of a channel's points.** To skip a channel at volume 0, and to
  test whether a sample ends in the samples to mix, the mixer guesses the
  points that the channel moves. It takes the step in 256ths of a point, times
  the samples, over 256, plus 1 point. The build of 2003/09/01 takes the whole
  points of the step, times the samples, plus 1 a sample. The test of a loop's
  start that a channel playing backwards reaches adds nothing.
* **The next row.** A jump goes straight to its order's first row, or to the
  first order if it names one past the last. It doesn't pass through the
  order before it, the markers or the module's end. A break then goes to its
  row.
* **An instrument's pan.** In a module with instruments, the note takes its
  sample's pan when it starts, before the envelopes start. The build of
  2003/09/01 takes it when the row gives the instrument.
* **A note off** releases a pan envelope that's off too. That envelope then
  runs.
* **`kramSetMasterVol()`** doesn't store the last entry of its table.

The player's record and channels, and the mixer's 32 channels of `0x2C`
bytes, are laid out as in 2003/09/01. In `BJBE` they're at `0x02001644` and
`0x020008B4`.

### The builds before 1.4

Builds 1.2.2.10 to 1.2.2.17 have a mixer that 1.4 rewrote:

* **24 mixer channels** of `0x28` bytes, in IWRAM. A record holds its fields
  up to the interpolation, at `0x20`, at the offsets of the later builds. Then
  come the sample's flag for the interpolation (`0x21`), the sound effects'
  flag (`0x22`), the mixing routine (`0x23`) and the start of the loop (a word
  at `0x24`).
* **No ramping.** `kramQualityMode()` takes 0 to 2 as the mode, and any other
  value as 0. *Racing Fever* calls it with `0x12`. Its mixer then doesn't
  interpolate.
* **`kramSetMasterVol()`** takes its whole argument as the output's level
  over 128, and the shift is always 5. It stores its table's last entry
  through an address with a bit set in its top four bits. The hardware
  ignores those bits. The store therefore reaches the table.
* **The sound timer.** The worker starts the timer's count again at each
  tick, and runs the tick only while the timer runs. `kramSetSoundTimer()`
  keeps the count as it is.
* **The volume column** holds a volume from `0x10` to `0x50`. An effect starts
  at `0x51`. A column from `0x51` to `0x5F` names an entry past the table of
  the column's effects. `agbsf2` leaves such a column out, with a warning.
* **A module that doesn't loop** stops at its end, and the player still finds
  the row that it would go back to. Nothing plays that row.

### The builds before 1.2.2.17

Builds 1.2.2.10 to 1.2.2.15 have no channel volume. The effects' table has no
routines for effects 25 and 26. Those effects do nothing. A note starts at its
volume, and the envelope's volume multiplies the volume over 64. The tremolo's
shift of 6 bits is right in these builds. A channel's record has neither the
channel volume nor its slide. The pan, the start point and the effect move
down a byte, to `0x05` to `0x07`. The note, the period and the played period
move down two, to `0x08` to `0x0D`. The bytes from the flag of a volume slide
that runs once to the envelope's volume move down one, to `0x3E` to `0x4C`.
The rest stays.

### The builds before 1.2.2.15

Builds 1.2.2.10 and 1.2.2.14 handle jumps and breaks another way:

* A break's operand is binary rather than decimal, and a row past 63 counts as
  row 0.
* A jump also breaks to row 0, unless a break has given a row. A break also
  jumps to the next order, unless a jump has given an order.
* After the row, a jump goes to its order's first row, or to the first order
  if it names one past the last. A break then goes to its row, whatever the
  pattern's length. Neither looks for a marker or the module's end. A break
  in the last order therefore goes to the first. Otherwise the next row works
  as in the later builds.
* Some of their calls of the BIOS's `Div` lack the later builds' test for a
  divisor of 0, and the hardware's `Div` doesn't return from one. `agbsf2`
  goes on as the later builds do.

### Build 1.2.2.10

Build 1.2.2.10 runs the envelopes another way. An envelope starts at tick 0
rather than 255. A tick compares the count of ticks with the next point's
before it moves the count on, and takes the level before it adds the step.
The volume envelope reaches the same levels on the same ticks as in the later
builds. The pan envelope reaches each point on the same tick as the volume
envelope, and a loop takes it back to the loop point's tick. A volume
envelope that runs sets the channel's volume on every tick, after a sustain
point and the envelope's end too.

A volume in the volume column leaves the column's last effect in place, and
that effect goes on between the rows.

### The games' code

*Racing Fever*'s sound init calls `kragInit(1)`, `kramQualityMode(0x12)` and
`krapSetMusicVol(0, 0)`. Its routine at `0x080042A4` sets the music volume to
half of what the game gives it, 127, and plays a module in loop mode.

*007: Everything or Nothing* plays each piece through its table of what it
plays at `0x0804F498`. Each entry holds a module and the mode and song for
`krapPlay()`. Its routine at `0x08022260` plays an entry in that mode. The
game plays most of its music in loop mode, some of it from a song's first
order, and a few pieces once, in song mode or as a jingle.

## Locating the player

`agbsf2` looks for this build's code:

* `krapPlay()`, from its start:
  `B5F0 4657 464E 4645 B4E0 1C0F 4680 4692 1C38 2204 4010 2800`.
* The note on of the row's routine, whose literals give the tables of
  instruments and samples:
  `0407 49xx 0BBA 588F 0071 616F 3902 5A78 4Fxx 0082 58B8 6128`, the first
  `ldr` loading the instruments' and the second the samples'.
* `kragInit()`'s timers, whose literal gives Timer 0's reload value and so the
  mixer's rate, 2^24 over `0x10000` less the reload value:
  `48xx 49xx 4Bxx 8008 3102 48xx 800B 3906 8008`, from the fourth `ldr`.
* `getDmaAddress()`'s step, half the samples of a frame:
  `49xx 23xx 0058 680B 181A 600A`.
* `kragInit()`, `kramQualityMode()` and `kramSetMasterVol()`, whose arguments
  the game's calls give:
  `B510 1C04 2080 F000 F9xx 1C20 F000 F8xx F000 F8xx`,
  `B530 2210 4002 2A00 D0xx 49xx 2301 700B` and
  `B5F0 1C04 0C20 2800 D0xx 49xx`. `kragInit()` calls `kramSetMasterVol()`
  itself, before the game can.
* A game's call of `krapPlay()` for an entry of a table of modules:
  `49xx 00xx 1840 6800 21xx 22xx Fxxx Fxxx`. That's `ldr r1, =table`,
  `lsls r0, rN, #2`, `adds r0, r0, r1`, `ldr r0, [r0]`, `movs r1, #mode`,
  `movs r2, #song` and the `bl`. Where the mode has bit 2 set and bit 0
  clear, the call plays a jingle. With bit 0 set too, `krapPlay()` loops the
  module. The routine that holds the call has to start within 64 bytes before
  it, with `push {..., lr}`, `lsls r0, r0, #24` and `lsrs rN, r0, #24`. The
  routine's first argument then picks the entry. Each of the game's calls of
  the routine gives the entry with `movs r0, #entry`. `agbsf2` converts the
  module of each of those entries once, as a jingle. In `BDGE`, the routine at
  `0x080132D0` takes that form. The game's three calls of the routine give
  entry 16. A module whose orders separate songs with `+++` still converts
  song by song.

The builds of `player.c` are told apart by their version strings, from
`$Id: player.c,v ` up to the date. Their code takes the patterns above with
three changes:

* The note on of the row's routine takes another form in every build of
  `player.c`, with the same literals:
  `0407 49xx 0BBA 588F 00x0 616F 3802 5A39 4Fxx 008A 58B8 6128`.
* `kramQualityMode()` before 1.4:
  `B510 4Axx 2802 DCxx 7010 4Axx 4Cxx 2018 1C91 780A 2A00`.
* `kramSetMasterVol()` before 1.4:
  `B5F0 4Cxx 4Exx 1C05 2100 2000 11C3 1C1A 2B7F DD00`.

A game with one of these builds can call the library's routines from ARM
code. `agbsf2` reads two forms of such a call. One is a `bl` to a veneer of
`ldr ip, [pc]`, `bx ip` and the routine's address. The other is
`ldr rN, =routine`, `mov lr, pc` and `bx rN`. The argument is the last value
written to `r0` before the call. It has to come from a `mov` of a constant,
an `ldr` of a literal or a `mov` from a register that one of those set. That
write has to fall within the 16 instructions before the call. Each of them has
to be unconditional, and none of them a branch, a call or a `push`.

The player's tables are found by their first values. The square wave's first
values, all 255, also turn up in other data. The square wave is therefore
taken from right after the ramp. The modules are found by
a scan of the ROM for headers that the player can play: up to 32 channels, a
restart before the last order, a speed, a tempo from `0x20` on, flags of 0 or 1,
pans within -64 and 64, and patterns whose index starts at 0 and doesn't go
back. The game's table of modules is the longest run of words that each give a
module that the scan found, and its order numbers the modules, with the ones it
doesn't list after it, in the ROM's order. `--song-table` gives a table of
modules instead, and `--song-count` the number of entries in the given table or
the game's table. With `--song-count`, the modules that the table doesn't list
are left out. Without a table, `--song-count` keeps that many of the modules
that the scan finds.

A game can play its music through a table of what it plays, rather than
through its table of modules. An entry of such a table has 8 bytes: the
module's address, the mode and the song for `krapPlay()`, and 2 bytes of 0.
`agbsf2` finds the table through the game's routine that plays an entry, from
its call of `krapPlay()`:
`49xx 00xx 1840 6803 6844 7901 0A22 0612 0E12 1C18 Fxxx Fxxx`. That's
`ldr r1, =table`, `lsls r0, rN, #3`, `adds r0, r0, r1`, `ldr r3, [r0]`,
`ldr r4, [r0, #4]`, `ldrb r1, [r0, #4]`, `lsrs r2, r4, #8`,
`lsls r2, r2, #24`, `lsrs r2, r2, #24`, `adds r0, r3, #0` and the `bl`. The
table ends before the first entry whose module isn't one that detection
found, whose mode isn't 0 to 7, or whose last 2 bytes aren't 0. The entries
convert in the table's order, before the modules that no entry plays. An
entry that repeats an earlier one is left out. Each entry plays in its mode.
It starts from its song if the module's orders separate songs with `+++`.
*Eragon* (`BENE`) has such a routine at `0x0803FD04`. Its table, at
`0x08F2C9D4`, has 29 entries.

## The builds from 2004/09/17 on

Three builds after 2003/09/01 have a version string with their date:

| Build | Version string | A game with it |
|---|---|---|
| 2004/09/17 | `$Id: Krawall $Date: 2004/09/17 09:01:35 $` | *Juka and the Monophonic Menace* (`BJKE`) |
| 2004/11/14 | `$Id: Krawall $Date: 2004/11/14 16:32:34 $` | *Babar to the Rescue* (`BBVE`) |
| 2005-04-21 | `$Id: Krawall $Id: version.h 8 2005-04-21 12:24:45Z seb $` | *Eragon* (`BENE`) |

The three builds have the same player. Their mixers' code differs in a few
routines. The build of 2005-04-21 is the one whose source is public.

### Data

The modules, samples and instruments are laid out as in 2003/09/01, but for
two changes to the patterns:

* A pattern's row count is a halfword at `0x20`, and its data starts at
  `0x22`.
* A channel's note takes 2 or 3 bytes. The first byte holds the note in its
  low 7 bits. The second holds the instrument's low 8 bits. If the first
  byte's top bit is set, a third byte holds the instrument's top 8 bits.

### The player

The player's record holds the row as a halfword at `0x0E`. The speed, the
tick, the order and the order that a jump goes to follow it, a byte each. The
row that a break goes to is a halfword at `0x18`. A break past row 127 goes to
its row. In 2003/09/01, such a break counts as none.

A channel's record has `0x64` bytes. A played volume and a played pan follow
the start point, at `0x08` and `0x09`. They move the effect, the note and the
periods on by 2 bytes. With the alignment of the words, the rest of the
record moves on by 4.

The mixer plays the played volume and pan. The tremolo and the tremor set the
played volume, and the panbrello sets the played pan. Every other change to
the volume or the pan sets the played one too. The channel volume, the
envelopes and those three effects then give the mixer the channel volume
times the played volume times the envelope's volume, over 4096, and the
played pan plus the envelope's pan, within -64 and 64. In 2003/09/01, they
give it the volume and the pan. An envelope's tick there undoes what the
three effects add.

The player differs in six more ways:

* **Envelopes.** In a module with instruments, a note starts its envelopes
  when it starts. The build of 2003/09/01 starts them when it reads the row. A
  delayed note starts them on its tick.
* **An instrument's vibrato** runs on an instrument without envelopes too. In
  2003/09/01, it runs only on an instrument with an envelope.
* **Tremolo.** The tremolo's volume becomes the played volume. The quirk of
  2003/09/01, a volume 64 times too loud, is gone.
* **Tremor** mutes the note through its played volume. A volume envelope's
  tick then keeps it muted until the tremor plays it again.
* **Note cut** at tick `x` sets the volume to 0 and leaves the note playing.
* **Retrig** starts the note again on the ticks that the operand's low digit
  divides.

### The mixer

* **Ramping.** The ramp at a stop is on unless `kramQualityMode()`'s flag 32
  turns it off. Flag 16 does nothing.
* **`kramSetMusicVol()`** sets the sides of the music's channels again, and
  leaves the sound effects' as they are. In 2003/09/01, it sets the sides of
  every playing channel. From 2004/11/14 on, it also picks each music
  channel's mixing routine again from its pan.
* **The centre.** In 2005-04-21, a channel in the centre mixes with the
  routine for the centre. In mono, every channel plays at the level of the
  centre, rather than of a pan to the left. A game in mono then plays at half
  the level of the earlier builds.

The mixer's channels and the mixer's other variables are laid out as in
2003/09/01.

### Locating the player

The three builds have the same code for the player, in other forms than
2003/09/01's:

* `krapPlay()`:
  `B5F0 4657 464E 4645 B4E0 4680 2002 1C0F 4692 Fxxx Fxxx 2003 Fxxx Fxxx 2204`.
* The note on: `0431 48xx 0B8A 5886 007C 61AE 3C02 5B31 48xx 008E 5984 616C`,
  the first `ldr` loading the instruments' and the second the samples'.
* `kragInit()`'s timers: `4Bxx 49xx 48xx 800B 3102 4Bxx 8008 3906 800B`, from
  the fourth `ldr`.
* `getDmaAddress()`'s step: `4Bxx 18D5 23xx 6035 005D`.
* `kragInit()` and `kramQualityMode()`:
  `B510 1C04 2080 F000 Fxxx 1C20 F000 F8xx` and
  `B530 2120 4001 2900 D0xx 49xx 2300 700B`. `kramSetMasterVol()` takes
  the same form as in 2003/09/01.

Detection tells the three builds apart by their mixers' code, rather than by
the version string:

* 2005-04-21: `425A 0993 E7xx 2304 E7xx 2301 7023 7042`, in the routine that
  picks a channel's mixing routine from its pan.
* 2004/11/14: `425A 0993 E7xx 2300 E7xx 2201 2340 7022 425A 1C13 7043`, in the
  same routine.
* 2004/09/17: `B570 4Bxx 28FF D9xx 20FF 7018 48xx 2420 1C1E 2540 7881 2900`,
  the start of `kramSetMusicVol()`.

## The unsigned build

*Puppy Luv: Spa and Resort* (`BYXE`) has a build of Krawall without a version
string. Another compiler built it, with other registers and another order of
routines. Its player and mixer work as those of 2005-04-21 do, and its data
and records are laid out as theirs.

The game's start-up code copies the library's code and data in IWRAM to
`0x03000500` on. `kramPlayExt()` and the routines that it calls are ARM code
in EWRAM, from `0x02001B2C` on. The player's record is at `0x02000010`.

Detection finds the build by its code:

* `krapPlay()`:
  `B5F0 465F 4656 464D 4644 B4F0 4681 2002 468A 4693 Fxxx Fxxx 2003 Fxxx Fxxx 2204`.
* The note on loads the samples' table at the start of the row:
  `48xx 49xx 2300 7473 9004 9102 469B 6973 7818 3301 6173 2800`, from the
  first `ldr`. It loads the instruments' table where it plays a note:
  `4Axx 0083 589A 4640 0043 61AA 189B 3B02 881B 9A04 009B 5899`, from the
  first `ldr`.
* `kragInit()`'s timers: `4Axx 4Bxx 801A 22C4 3302 801A 4Axx 3B06 801A`, from
  the third `ldr`.
* `getDmaAddress()`'s step:
  `49xx 4Bxx 680A 18D3 602B 4Bxx 18D3 6033 23xx 005B`.
* `kragInit()`: `B510 1C04 2080 F000 Fxxx 1C20 Fxxx Fxxx 2001 F000 Fxxx`.
* `kramQualityMode()`: `B510 0683 D5xx 4Axx 2300 7013 230F 4018 2802 DCxx`.
* `kramSetMasterVol()`: `B5F0 4647 B480 1C04 0C00 2800 D1xx 2005`.

## The modified build

*Big Mutha Truckers* (`B63E`), *Street Racing Syndicate* (`BCZP`), *M&M's
Break 'Em* (`BEME`) and *Princess Natasha* (`BNPE`) have a build of Krawall
without a version string. Their developer changed the build of 2005-04-21 and
built it with another compiler. Its records are laid out as those of
2005-04-21, and its mixer has 14 channels. `krapPlay()` takes a negative song
as an order to start from. The games keep each module packed in a music file.
Before a game plays a module, a routine of the game unpacks the file into RAM,
and then the game calls `krapPlay()` with the module there.

### Music files

A music file starts with a byte that counts the samples it lists, and a byte
for each of them: its number in the game's table of samples. The game unpacks
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
| 7 | the low bits of a long copy's distance, `D` |
| 8 | the count of frequent bytes, `N`, from 1 to 32 |

The `N` frequent bytes follow, and then the bit stream. The stream is read a
bit at a time, the top bit of each byte first.

A gamma code is up to `M - 1` bits of 1, then a bit of 0 unless there were
`M - 1` of them. As many bits as there were 1s follow. The code's value is a
1 with those bits below it, from 1 to `2^M - 1`.

The stream is a list of items. Each item starts with `A` bits:

* A value other than `E` is a byte's top `A` bits. Its other `8 - A` bits
  follow.
* `E` starts a gamma code `g`. If `g` is 2 or more, a second gamma code `h`
  follows. An `h` of `2L - 1` ends the stream. Any other `h` starts a copy of
  `g + 1` bytes. The distance's high byte is `h - 1`, with `D` bits read below
  it, and only its low 8 bits count. A byte `b` follows. The copy goes back
  `256 × high + 256 - b` bytes and takes one byte at a time. It can copy
  bytes that it has just made.
* A `g` of 1 starts a bit. A 0 starts a copy of 2 bytes, back `256 - b`
  bytes, with `b` the next byte.
* A 1 starts another bit. A 0 there gives a byte whose top bits are `E`. Its
  other `8 - A` bits follow. Before them come `A` bits that become the next
  `E`.
* A 1 there starts a run. A gamma code `r` gives its length. If `r` is `L` or
  more, `9 - M` bits follow below it, and a gamma code gives the length's high
  byte, plus 1. The run has the low 8 bits of `r`, plus 1, plus 256 for each
  of that high byte. A gamma code `f` then gives the byte. Below 32, it's
  frequent byte `f`, from 1. From 32 on, 3 bits follow below it, and the low
  8 bits of the result are the byte.

The game's routine writes halfwords. A last odd byte is written with a 0
after it.

### Unpacked modules

The unpacked data end with a byte of the instruments' count `I` and one of the
patterns' count `P`. From the start, they hold:

* the patterns, in the layout of the builds from 2004/09/17 on
* the module's header, as in those builds, up to `0x16C`
* at `0x16C`, a word for the address of the module's instruments
* from `0x170`, a word for each pattern: its offset in the unpacked data
* the `I` instruments, `0x130` bytes each, laid out as in those builds

The game's loader turns each pattern's offset into an address, and stores the
instruments' address at `0x16C`. A note's instrument is entry `instrument - 1`
of the module's instruments. A note's sample comes from the game's table, as
in the other builds. `kragInit()` copies that table to RAM at the start.

A sample's header gives the count of its points at `0x04`. The other builds
give the address after its last point there.

An instrument's byte at `0x12E` is 1 if the mixer interpolates its notes in
quality mode 1. `kramPlayExt()` turns the interpolation off for every sound
that it starts. The player's note on then sets the mixer channel's flag from
the instrument's byte, in the way that the other builds set it from the
sample's header.

### Detection

Detection finds the build by its code:

* `krapPlay()`:
  `B5F0 4647 B480 1C07 1C0E 4690 2002 Fxxx Fxxx 2003 Fxxx Fxxx 2104`.
* The loader of music files reads the game's table of samples:
  `48xx 4641 0089 4688 4440 6805 686C 3446 2002 4240 4004`, from the first
  `ldr`.
* `kragInit()`'s timers: `49xx 4Axx 1C10 8008 3102 20C4 8008 3906 23xx 021B
  1C18 8008`. The `movs` and the `lsls` after it give timer 0's reload.
* Half the samples of a frame: `2002 7008 49xx 20xx 0080 6008`, from the
  second `movs`.
* `kragInit()`:
  `B530 1C04 2080 Fxxx Fxxx 1C20 Fxxx Fxxx 2001 Fxxx Fxxx Fxxx Fxxx Fxxx Fxxx 48xx 8800`.
* `kramQualityMode()`: `B530 1C02 2020 4010 2800 D0xx 49xx 2000`.
* `kramSetMasterVol()`: `B5F0 1C07 0C38 2800 D1xx 2005`.

The game's code points to its music files from literals and from tables.
Detection reads every word of the ROM that points to a music file. A file
counts if it unpacks to its size and holds a module that the player can read.
The files that tables of 2 or more words list come first, in the tables'
order. The others follow in the order of the first word that points to each.
Only a game with no such words has its ROM scanned for music files.

Detection unpacks each module and lays it out as the game's loader does. The
modules follow the ROM's end, from the next multiple of `0x10000`. The game's
routines that play a music file call the loader, directly or through a routine
that only calls it, and then `krapPlay()`. A call of such a routine with
`ldr r0, =file; movs r1, #0` plays the file once, in the jingle mode. The
others play the file in the loop mode.

## Set-up and play in other games

The [architecture](#architecture) gives *Digimon Racing*'s set-up and play
calls. This section gives those of the other games that `agbsf2` supports,
by build.

*The Sims: Bustin' Out* (`ASIE`) has the build of 2003/09/01. Its sound init,
at `0x08041338`, calls `kragInit(1)`, `kramSetMasterVol(0x40080)` and
`kramQualityMode(2)`. The mixer then interpolates every sample. The sound init
then sets the music volume from the game's settings. Without a save, the
settings are the defaults at `0x0806A210`. Those give the music a volume of
255. `agbsf2` plays the music at 128. At 255, the loudest notes of a few
pieces would need more than a MIDI file's full loudness. The conversion is
therefore 6 dB quieter than the game, with the same balance. The game plays
every piece through its table of what it plays, at `0x08D73EF8`. The table has
71 entries, one of them a repeat. The routine at `0x0804129C` plays an entry
in the entry's mode. The routine at `0x08041504` plays an entry as a jingle,
in mode 4. The game plays entry 27, of mode 2, that way. That entry's song is
its module's last, and both modes play it the same.

*Juka and the Monophonic Menace* (`BJKE`) has the build of 2004/09/17. Its
sound init, at `0x0802395C`, calls `kragInit(1)`. The mixer runs at quality 0,
with a master volume of `0x80`. The game's routine at `0x08023E9C` plays an
entry of its table of modules in loop mode. Its routine at `0x08023A84` runs
before the mixer's worker in each frame. That routine sets the music volume.
After each start, it raises the volume from 0 by 2 a frame, up to 128.
`agbsf2` takes that fade-in from a table of games' settings. A conversion's
first pass then fades in. Its marked loop starts on the second pass.

*Babar to the Rescue* (`BBVE`) has the build of 2004/11/14. Its sound init,
ARM code at `0x0801B218`, calls `kragInit(0)`. The music is then mono. The
game plays its modules in mode 3, loop and song. Its modules have no markers
between songs, and go back to their first order at their end. Mode 3 therefore
plays them as mode 1 does.

*Eragon* (`BENE`) has the build of 2005-04-21. Its sound init, at
`0x0803FD90`, calls `kragInit(1)`, `kramSetMasterVol(0x40080)` and
`kramQualityMode(2)`. It then sets the music volume with `krapSetMusicVol()`,
from the game's options. The options' default music level, 7 of 10, is at
`0x08063A41`. The table at `0x08063C8C` gives level 7 a volume of 161.
`agbsf2` takes that volume from the table of games' settings. The game plays
every piece through its table of what it plays (see [Locating the
player](#locating-the-player)). Entries 4, 19 and 20 play in song mode. That
mode doesn't loop at the module's end. The other entries play in loop mode.

*Puppy Luv: Spa and Resort* (`BYXE`) has the unsigned build. Its boot, at
`0x08024BD6`, calls `kragInit(1)`, `krapSetMusicVol(60, 0)`,
`kramSetSFXVol(0x80)` and `kramQualityMode(0x20)`. The mixer then runs at
quality 0, without the ramp. The game's table at `0x0805B69C` gives each piece
of music its module, in entries of 8 bytes. The routine at `0x0802492C` plays
an entry. It first sets the music volume from the game's options with
`krapSetMusicVol()`, and then plays the module in loop mode. The options'
default volume, from the routine at `0x0801A548`, is 64. `agbsf2` takes that
volume from the table of games' settings. Five of the six modules that the
table lists have 21 to 29 channels. The game plays no jingles. A seventh
module, of one channel, is in no table.

The four games with the modified build set up the library in the same way.
The game's sound init calls `kragInit()` with the stereo setting of the
game's options, 1 by default. It then calls `kramQualityMode(0x21)` and
`kramSetMasterVol(0x40080)`. The mixer runs at quality 1, without the ramp.
Each instrument's byte at `0x12E` then sets whether its notes are
interpolated. The game's start-up code sets the music volume of its options
with `krapSetMusicVol()`: *Big Mutha Truckers* 100 at `0x08000684`, *Street
Racing Syndicate* 64 at `0x080003DA`, *M&M's Break 'Em* 48 at `0x08000418`
and *Princess Natasha* 64 at `0x080008F4`. `agbsf2` takes the stereo, the
quality and the volume from the table of games' settings. Each game plays a
music file through its routine that unpacks the file and calls `krapPlay()`.
*Princess Natasha* plays two of its files once, as jingles.
