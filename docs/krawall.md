# Krawall

Krawall is Sebastian Kienzl's XM and S3M module player for the Game Boy
Advance. Its converter translates tracker files into Krawall modules, with
sample and instrument tables shared by all the game's modules. The library
plays them through a software mixer. This document describes the data,
player and mixer in the 2003/09/01 build that `agbsf2` supports. The example
addresses come from the US release of *Digimon Racing* (game code `BDGE`).
Later builds, dated 2004/09/17, 2004/11/14 and 2005-04-21 in their version
strings, changed the code and data formats and aren't supported. `--info`
prints the version string stored in the ROM, which is
`$Id: Krawall $Date: 2003/09/01 06:51:01 $` for this build. `agbsf2`
finds the player and its tables from the code (see [Locating the
player](#locating-the-player)).

A module has up to 20 channels, each playing one note at a time on one of
the mixer's 32 channels. The mixer runs at 16384 Hz, producing 276 samples
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
| 27 | Offset | starts the note 256 times the operand points into its sample, or moves the playing note there |
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
64, added to the channel's pan, and the instrument's vibrato adds the wave's
point times its depth over 256 to the period that the effects leave, and moves
on by its rate, a quarter of a point at a time.

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
plays louder than 5 down over 16 samples.

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

The player's tables are found by their first values. The modules are found by
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
