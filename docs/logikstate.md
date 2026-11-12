# Logik State's sound driver

*Aero the Acro-Bat: Rascal Rival Revenge* plays its music with a driver that
its credits name as Logik State's. Later games name the driver in their ROMs as
GBAModPlay and LS_Play. It plays modules in the manner of ProTracker: patterns
of rows, a cell for each channel in a row, and samples of signed 8-bit points.
A software mixer plays up to 8 channels at 21024 Hz, the last two for the
game's sound effects. *Pinball Challenge Deluxe* has an earlier build with
another format of module. "The GBAMOD1 build" describes it. Later builds play
a third format, GBAMOD30, in 26 games. "The GBAMOD30 builds" describes them. This document
describes the driver as `agbsf2` models it. The addresses come from the US release (game code `AAOE`)
unless a table lists each release's.

## Routines

The driver is Thumb code in ROM. The mixer's inner loop is ARM code that the
init copies to RAM.

| Address | Routine |
|---|---|
| `0x081034E8` | the init: `r0` is the memory that the driver uses for the mixer's code (600 bytes) and two mix buffers of 352 bytes |
| `0x08103418` | plays the module at `r0` from order `r1` with the bank of samples at `r2`: clears the channels, then the load (`0x08103600`) and the start (`0x081036F8`) |
| `0x08103480` | the frame routine: points both DMA channels at the mix buffer that it filled a frame earlier (`0x08104620`, ARM), and mixes the other buffer (`0x08103D34`) |
| `0x08103B4C` | reads a row: a cell for each of the module's channels (`0x0810426C`) |
| `0x08104110` | runs a tick of the effects of channels 0 to 3 |
| `0x08104684` | the mixer's loop for a channel: ARM code. The init copies it to RAM with the routines after it, 600 bytes in all |
| `0x08103384` | starts the output: timers 0 and 1 at 21024 Hz, and DMA 1 and 2 from the same buffer to FIFO A and FIFO B |
| `0x0810345C`, `0x08103468`, `0x08103474` | set the master volume, the music's volume and the sound effects' volume, each up to 64 |
| `0x08103450`, `0x08103444` | switch the sound on and off (`SOUNDCNT_X`) |
| `0x08103A90`, `0x08103AB0` | pause and resume: set and clear a flag (`0x03006B58`) and switch the sound off and on; while the flag is set, the frame routine mixes nothing |
| `0x08103F44` | names the bank of the sound effects' samples in `r0` |
| `0x08103F68` | plays sound effect `r0` at pitch `r1` on channel `r2` after the module's, at volume `r3` |
| `0x08103FFC` | stops the sound effect on channel `r0` after the module's |

The driver also has routines that play a second piece of the module over the
first and go back, queue a change of order for the next beat, and play one of
the module's samples on a channel. Aero the Acro-Bat calls none of them.

Each release has the same routines at other addresses. The variables keep
their addresses in the three released games, and are 0x260 bytes lower in the
beta.

| | USA (`AAOE`) | Japan (`AAOJ`) | Europe (`AAOP`) | European beta (no game code) |
|---|---|---|---|---|
| init | `0x081034E8` | `0x081039C0` | `0x08103840` | `0x080F8C50` |
| init's memory (`r0`) | `0x03005D50` | `0x03005D50` | `0x03005D50` | `0x03005BE0` |
| play | `0x08103418` | `0x081038F0` | `0x08103770` | `0x080F8B80` |
| frame routine | `0x08103480` | `0x08103958` | `0x081037D8` | `0x080F8BE8` |
| its caller | `0x0802944E` | `0x080298FA` | `0x08029702` | `0x08021672` |
| names the sound effects' bank | `0x08103F44` | `0x0810441C` | `0x0810429C` | `0x080F96AC` |
| sound on | `0x08103450` | `0x08103928` | `0x081037A8` | `0x080F8BB8` |
| master volume | `0x0810345C` | `0x08103934` | `0x081037B4` | `0x080F8BC4` |
| music's volume | `0x08103468` | `0x08103940` | `0x081037C0` | `0x080F8BD0` |
| bank of the music's samples | `0x0809B0C8` | `0x0809B5A0` | `0x0809B410` | `0x08090834` |
| table of the levels' modules | `0x08350068` | `0x083A069C` | `0x083576C8` | `0x08328618` |
| first module | `0x08035F6C` | `0x08036444` | `0x08036220` | `0x0802B6D8` |

## The game's code

The game sets the driver up once (`0x0802C35C`): the init with its memory, the
play routine with the title's module, a master volume of 0, the sound effects'
bank at `0x080A5CF4` and the sound's switch. That bank follows the music's
bank.
The handler that the game installs for the vertical blank (`0x08029444`) calls
the frame routine before anything else, once a frame.

Eight places call the play routine. Each passes order 0 and the one bank of
samples at `0x0809B0C8`. Seven name their module. The eighth reads it from a
table of 25 pointers at `0x08350068`, by the level's number. The table names
18 of the ROM's 22 modules, some of them more than once. `agbsf2` numbers the
modules in the order of their addresses, from 0. The table's entries are then
songs 5, 13, 17, 3, 6, 1, 9, 7, 8, 14, 10, 18, 8, 1, 11, 12, 7, 0, 15, 17, 14,
16, 16, 6 and 21. The other calls play songs 0, 2, 5, 19 and 20.

After a call of the play routine, the game sets the three volumes: the master
volume, usually 64, and the music's and the sound effects' volumes from its
options. It fades the music with the master volume. At its start, the game
sets the music's volume in its options to 48 of 64 (`0x08026CE6` stores 0x30
at `0x030024C0` and passes it to the routine at `0x08103468`) and the sound
effects' volume to 64.

The game pauses the driver twice without any input: for 28 frames from the
fifth frame (the call at `0x08033786`), and for a frame when the title screen
starts, at frame 1036 (the routine at `0x080338B8`, called from `0x0801C7C8`).
The title screen's routine (`0x0801C790`) then plays the title's module again.

## Modules

A module starts with the text `GBAMOD2.` at a word's start. Its header is
0x1AF8 bytes long, and its patterns follow.

| Offset | Size | Field |
|---|---|---|
| `0x0000` | 8 | `GBAMOD2.` |
| `0x0008` | 4 | the channels: 6 in Aero the Acro-Bat |
| `0x000C` | 4 | the size of the patterns in bytes |
| `0x0014` | 4 | the mixer's rate in hertz: 21024 |
| `0x0018` | 4 | the count of orders |
| `0x001C` | 4 × 976 | each pitch's step: what a sample adds to a channel's place, in 4096ths of a point |
| `0x0F5C` | 2 × 976 | each pitch's Amiga period |
| `0x16FC` | 12 × 31 | the samples' headers |
| `0x1870` | 4 | the count of patterns, up to 64 |
| `0x1874` | 128 | each order's pattern |
| `0x18F4` | 4 × 64 | each pattern's count of rows |
| `0x1AF4` | 4 | below 0 if the module's points follow its patterns |

The two tables have 8 pitches to a semitone, the unit of ProTracker's fine
tune. Pitch 8 has the period 1712, ProTracker's C-1 an octave down, and pitch
200 the period 428. A period p plays a sample at 3546894.6 / p points a
second. A pitch's step is that rate times 4096 over the mixer's rate, rounded
down.

A sample's header contains five halfwords: the length in points, the fine tune
in pitches, the volume up to 64, the loop's first point and the loop's length.
A loop of up to 2 points is no loop.

A pattern is its rows, and a row is a cell of 4 bytes for each channel. A
cell's first halfword contains the pitch in its low 9 bits, with 0x1FF for no
note, and the sample's number from 1 in the 5 bits above. Its second halfword
contains the effect in the high byte and the operand in the low byte. A pattern
can have fewer than 64 rows. The player plays the missing rows as empty ones,
unless a jump ends the pattern first.

## Banks of samples

A bank starts with 31 headers of the same form as a module's. The points of
each sample follow, in the headers' order. The load routine reads only the
lengths from a bank's headers, to find where each sample's points start. The
length, the fine tune, the volume and the loop of a sample come from the
module's header. Aero the Acro-Bat's modules set other lengths and loops for
some samples than the bank does. A note of such a sample can play a few points of
the next one. With no bank, or with the header's last field below 0, the
points follow the module's patterns.

## Rows and ticks

The player counts in the mixer's samples. A module starts at 6 ticks a row and
tempo 125. A row lasts the mixer's rate times the speed over the ticks a
second, rounded down, and the tempo t makes t × 50 / 125 ticks a second,
rounded down: 2522 samples at the start. The effects between rows run every
420 samples, the rate over 50, whatever the tempo. Their ticks run on through
the rows. A row that starts as a tick falls due plays without that tick. At a
tempo above 125, a row has fewer ticks than its speed.

The frame routine mixes 352 samples a frame. Its loop runs the row and the
tick that fall due, then mixes up to the next of them. Each pass of the loop
counts as one sample more than it mixes. A frame with a row or a tick near its
end therefore mixes a sample or two fewer than 352. Those samples stay silent,
and the song loses that time.

After a pattern's 64th row, the player moves to the next order, and from the
last order to the first. A module never stops.

## Effects

A cell's effect acts at its row. The player then runs the effects of channels
0 to 3 at each tick up to the next row. The other channels get their rows'
settings and no ticks.

| Effect | At the row | At each tick |
|---|---|---|
| 0 xy | sets an arpeggio of x and y semitones | from the second tick: x, y, y, x, y, y |
| 1 xx | sets a slide up | the period falls by xx |
| 2 xx | sets a slide down | the period rises by xx |
| 3 xx | keeps the cell's note as the target of a slide, at speed xx | the note reaches the target's pitch at the first tick |
| 4 xy | sets a vibrato of speed x and depth y | the period moves by ProTracker's sine table times y / 128 |
| 5 xy | as 3, with a volume slide | 3, then A |
| 6 xy | as 4, with a volume slide | 4, then A |
| A xy | sets a volume slide | from the second tick: the volume rises by x, or falls by y if x is 0 |
| B xx | the next row is the first of order xx | |
| C xx | sets the volume to xx | |
| D xx | the next row starts at cell xx × 4 of the next order's pattern | |
| E 6x | the pattern loop. It never goes back to its row | |
| F xx | up to 0x1F: the ticks of a row. Above: the tempo | |

A note with a slide to it (effect 3) doesn't start. It uses its sample's
volume. The slide compares the pitches' steps where it means their periods,
and finds its target passed at the first tick. A slide's offset to the period
stays until the next note. That note's first step still counts it. A vibrato
starts from the wave's start at each row. Its offset is cleared at the next row.

A note without a sample number plays the channel's last sample. A sample
number without a note sets the channel to that sample's points and volume. The
channel keeps its place, its length and its loop.

Aero the Acro-Bat's modules use effects 1, 2, B, C and F.

## The mixer

For each channel with a volume above 0, the mixer adds each point times the
volume over 256 to the mix buffer's byte. The bytes wrap. Nothing limits
them. Both FIFOs play the same buffer on both sides. The music is therefore mono,
and a channel at volume 64 plays its points at half of full scale. The game's
three volumes subtract their shortfall from 64 from each channel's volume: the
larger shortfall of the master volume and the music's volume for the first six
channels, and of the master volume and the sound effects' volume for the last
two.

The driver sets `SOUNDCNT_H` once, when it first starts its output: both
FIFOs at full volume. The game switches the sound off when it pauses the
driver, 5 frames after its start. In mGBA 0.11.0, the switch clears the low
byte of `SOUNDCNT_H`, and both FIFOs play at half volume from then on. A
channel at volume 64 then plays its points at a quarter of full scale.

The mixer leaves a channel at volume 0 alone. The channel's sample then waits
at its point, and plays on from there when the volume rises.

At a sample's end, the channel's points move on by the loop's start, and the
loop's length becomes the length. The mixer's code keeps the fraction of the
channel's place there, with the mask 0xFFF from a word before its loop. The
copy in RAM leaves that word behind and reads the word 8 bytes before the
copy. The game keeps 0 there. A loop therefore starts from its first point
exactly. A loop that ends before the sample does plays only after the sample's
last point.

Before the output starts, the driver waits while a halfword reads 0x3E and
then 0x3D. It reads the halfword at `0x0000FCE2`, where a GBA has no memory.
The wait therefore ends at once.

## The build with long records

*Zapper: One Wicked Cricket!* (game code `AZPE`) plays the same modules with
another build of the driver. Its variables are in one structure at
`0x03007130`, and a mixer channel's record is 0x54 bytes long where Aero's is
0x4C: the length, the loop's start and the loop's length are words, and each
later field is 4 bytes further on. The rows, the ticks, the effects and the
mixer's loop work as in Aero's build, with these differences:

- The mixer plays a channel at volume 0 too. The channel's sample moves on in
  silence.
- The music's volume acts on the module's channels. Zapper's modules have 4,
  5 or 6. Aero's build applies it to the first six channels of the
  mixer whatever the module.
- Effect E6 is ProTracker's pattern loop, with a row and a count in each
  channel's record (at 0x50 and 0x52). `E60` marks the row. `E6x` goes back to
  the marked row x times. No module of Zapper has the effect.
- The start routine leaves the three volumes as the game last set them.

| Address | Routine |
|---|---|
| `0x080E4780` | the init; the game passes it the memory at `0x03000180` |
| `0x080E466C` | plays the module at `r0` from order `r1` with the bank at `r2` |
| `0x080E46DC` | the frame routine. The game's main loop calls it (`0x08000994`) |
| `0x080E46B8`, `0x080E46C4`, `0x080E46D0` | set the master volume, the music's volume and the sound effects' volume |
| `0x080E4698`, `0x080E46A8` | switch the sound off and on |
| `0x080E45BC` | stops the output; the next play starts it again and sets `SOUNDCNT_H` |
| `0x080E5134` | plays a sound effect |

The game's 30 calls of the play routine pass order 0 and the bank at
`0x0805BFEC`. Four of them read the module from a table of the levels at
`0x0832939C`. The game's options start with a music volume of 32 of 64
(`0x080C760A`), and the routine at `0x080C7748` passes the options' volumes to
the driver. In mGBA 0.11.0, that call comes at frame 17 of a boot without input.
The game's code never reaches the routine that switches the sound off, and
both FIFOs stay at full volume.

`agbsf2` tells the builds apart by the size of a record: the driver's code
after the play routine moves from a record to the next with `movs` and `adds`
instructions of 0x4C or of 0x54.

## The GBAMOD1 build

*Pinball Challenge Deluxe* (game code `APLP`) has an earlier build of the
driver. Its modules start with `GBAMOD1.`. It has 4 channels of music and 2
of sound effects, and one FIFO plays its mix. The addresses in this section
are the game's.

### Modules

The header is 0x730 bytes long. The patterns follow it, then the points of the
samples, in the headers' order. There is no bank.

| Offset | Size | Field |
|---|---|---|
| `0x000` | 8 | `GBAMOD1.` |
| `0x008` | 4 | the size of the patterns in bytes: 16 times the sum of their rows |
| `0x00C` | 4 | the size of the points in bytes |
| `0x010` | 4 | the mixer's rate in hertz: 21024 |
| `0x014` | 4 | the count of orders |
| `0x018` | 4 × 296 | each pitch's step, in 4096ths of a point |
| `0x4B8` | 12 × 31 | the samples' headers |
| `0x62C` | 4 | the count of patterns, up to 64 |
| `0x630` | 128 | each order's pattern |
| `0x6B0` | 64 | each pattern's count of rows, a byte each |
| `0x6F0` | 64 | zeros. No code reads them |

A module has no count of channels: a row is 4 cells of 4 bytes. There is no
table of periods. The table of steps has 8 pitches to a semitone. Pitch 8 has
the step 807, ProTracker's C-1 with the period 856, and pitch 104 the step
1614 of the period 428. A sample's header and a cell have the same form as in
GBAMOD2. The fine tune runs from 0 to 15 and is added to the pitch as it is.
A sample with the fine tune 13 therefore plays 13 eighths of a semitone above
its note, where ProTracker reads 13 as a fine tune below.

### Routines

| Address | Routine |
|---|---|
| `0x08004234` | the init. It gets the memory of its mixer's code from the game's allocator (`0x0800D8D8`) |
| `0x08004380` | the load: the state at `r0`, the module at `r1` |
| `0x08004428` | the start: order `r0`, and the range of orders from `r1` to `r2`, or -1 for none |
| `0x0800568C` | plays the module at `r0` from order `r1` with the range from `r2` to `r3`: clears the music's channels, then the load, the start and, once, the output's start |
| `0x080088D8` | the frame routine. The handler of the vertical blank calls it. It points DMA 1 at the current buffer and mixes the other one (`0x080048A0`) |
| `0x08004910`, `0x08004CE0` | read a row, and run a tick of the effects of channels 0 to 3 |
| `0x080CE184` | the mixer's loop for a channel: ARM code. The init copies it to RAM |
| `0x0800DD60` | starts the output: `SOUNDCNT_H` = 0x8B0E, DMA 1 to FIFO A, timer 0 at 21024 Hz |
| `0x0800DDC4`, `0x0800DDD0` | switch the sound off and on (`SOUNDCNT_X`) |
| `0x08004830`, `0x08004850` | pause and resume: set and clear a flag and switch the sound off and on |
| `0x08004650` | plays a jingle: keeps the current order and range, and goes back to them at the jingle's next jump |
| `0x08004C58` | plays a sample on a channel, for the game's sound effects |

The driver has no routines that set the volumes. The game writes the master
volume at `0x0200070C`, and the options' volumes for the music and the sound
effects at `0x02000434` and `0x02000438`. The start routine copies the
options' volumes to `0x02000710` and `0x02000714`.

### Differences from the GBAMOD2 builds

- **Steps.** A channel's step is the table's at the fine tune plus the pitch
  plus half the vibrato, plus the slide. The slide is a signed halfword of
  steps. Slides are therefore linear in frequency. Nothing limits the sum. A step
  that a slide brings to 0 or below makes the sample stand still or play
  backwards. A place that falls below 0 wraps, and counts as the sample's
  end.
- **Slides 1 and 2** add 4 times the operand to the slide, or subtract it from the slide, at each
  tick.
- **Slide to a note (3).** The speed is the operand times the scale at
  `0x020006F0`, and stays when the product is 0. Each tick adds the speed
  towards the target. The note reaches the target's pitch where the step at its
  pitch plus the slide reaches the target's step. The game sets the scale for
  each module: 8 for the title's module and the modules of tables 0 to 3
  (`0x080037B6`, `0x080072E2`), and 0x28, the initial value, for the others.
- **Vibrato (4)** moves the pitch by half of the wave times the depth over
  256. Its place in the wave starts again at a row's first tick.
- **Arpeggio (0)** keeps its intervals from row to row, and plays
  nothing: the tick works out a step and then replaces it.
- **Effects 5, 6, 9 and E do nothing.**
- **Volume slide (A).** An operand whose two digits share a set bit makes the
  tick skip that channel without moving on to the next. The tick's other
  passes come back to it. Neither that channel nor the ones after it get the
  tick.
- **A sample without a note** sets the channel to the sample's points and
  volume, and starts it from its first point. The channel keeps its length
  and its loop.
- **Jump (B)** sets the order to the operand less 1 and ends the pattern.
  **Break (D)** moves to the next order with no wrap to 0. **Speed (F)** sets
  a row's length to the rate times the operand over 50. An operand above
  0x1F sets it to the operand times 2 over 5 samples.
- **The end of a pattern.** With no range, the order moves on. With a range,
  an order above the range's last goes to its first, and another order
  stays. A pattern then plays again until a jump or a break moves the order.
  A jump to order n plays order n - 1, or the range's first order where n - 1
  is above the last.
- **The end of the module.** Nothing wraps the order to 0. While the order is
  past the last, the frame routine clears the music's channels at each frame.
- **The mixer** skips a channel at volume 0, or with a length of exactly 2. A
  sample whose loop's length is 0 therefore stays in the mixer after its end.
  It then adds the point at the loop's start at each sample, a constant. The
  RAM copy of the mixer reads its mask at `0x02FFFFFC`, a mirror of EWRAM's
  last word. The entry code clears that word. A loop therefore starts at its
  first point exactly.

### The game's side

A module contains many short pieces. Each is a run of orders that ends with a
jump: back into the run for a piece that loops, and to itself for a jingle.
The game plays a jingle through the jingle routine, whose first jump goes
back to the piece before. The game then waits for the jingle to end and
starts the next music itself. Module 1 is the title's music, module 0 contains
the samples of the sound effects, and no code names module 2. Table t plays
module t + 3.

Each table has a record in ROM. The table's setup copies it to RAM with the
table's data (`0x08009A20`). The record's word +0x60 contains the module's
address, and its words +0x10 and +0x14 name the start and the end of the
data. The end is 0x274 bytes after the record. Bytes of the record name
orders that the game plays:

| Offset | Read at | Plays |
|---|---|---|
| +0x224 | `0x08007D98`, `0x0800802A` | music after a lost ball and after the game's end |
| +0x228 | `0x080082E6` | music before the ball's launch |
| +0x22C | `0x0800558A`, `0x080055BE` | the main music, from the launch |
| +0x230 | `0x080083EC` | music before a game |
| +0x240 | `0x08007FB2` | music of the name entry |
| +0x234, +0x238, +0x23C, +0x244, +0x248 | `0x08009454`, `0x0800C194`, `0x08007FDE`, `0x08007D0C`, `0x08007452` | jingles |
| +0x258 | `0x0800855E` | a jingle of tables 4 to 7, whose records start with `0x23232323` (`0x08007F10`) |

The data starts with a zero word, and an array of event entries of 4 bytes
follows. An entry with bit 7 of its first byte set is a jingle of the order in
the low 7 bits, at the priority in its third byte (`0x0800B208`,
`0x0800B17C`). Another entry is a sound effect's sample. The game's objects
point at the entries that they play. A jingle entry that no word of the ROM
points at names a piece that nothing starts. Every start of a piece in this
game has the range (-1, -1), or a range that the piece's jump makes no
difference to.

The options keep a music level from 0 to 10, 10 by default. The game converts
a level with × 0x6666 >> 12 (`0x08003960`). Level 10 makes 63 of 64. Each
channel therefore plays at its volume less 1.

The output's start sets `SOUNDCNT_H` to 0x8B0E: FIFO A on both sides at full
volume, and FIFO B on neither. In mGBA 0.11.0, the register reads 0x030E at
the title. The game's code switches the sound off when it starts a table, and
at other changes of state (the calls at `0x08003874`, `0x0800399A`,
`0x08006F90` and `0x08006FE8`, and of the pause routine at `0x08005364`,
`0x08006F1A` and `0x080090CE`). From then on the register reads 0x0300: FIFO
A at half volume.

### Locating and converting the build

`agbsf2` tells the format by the signature in the load routine's literal
pool. `GBAMOD1.` is there as two words side by side. The play routine is
the caller of the load routine that calls the start routine after it. The
game's main code loads module 0 too, and starts nothing. Each module has a
song for its order 0, and one for each order after a pattern with a jump.
`--info` lists them in the order of the modules' addresses, with the order
that each starts at.

A table by game code lists what only the game's code says: the record's
layout and which of its bytes name music and which name jingles, and the
title's two starts of module 1, at orders 0 and 3. `agbsf2` finds each
table's record as the one word of the ROM that contains the module's address
and passes the checks of the words +0x10 and +0x14. It reads the record's
orders and the jingle entries of its data. A piece that only jingles start,
or that only a jingle entry without a pointer names, plays once. Every other
piece plays as the driver plays it, and loops. In *Pinball Challenge Deluxe*,
41 pieces loop as music, 210 play once as jingles, 27 play once as jingles
that nothing starts, and 37 play as the driver plays them. A game without an
entry in the table has every piece played as the driver plays it.

`agbsf2` counts the FIFOs from the value that the output's start writes to
`SOUNDCNT_H`, in the code after its load of `DMA1CNT_H`'s address. One FIFO
plays the mix at half the level of two. The routine that switches the sound
off is after the output's start in this build. The conversion of
*Pinball Challenge Deluxe* therefore plays one FIFO at half volume: a quarter
of the level of Aero the Acro-Bat's FIFOs at full volume.

A conversion plays a song from its order with no range, at the music volume
63 and the module's scale of the slide to a note. A note's key comes from its
step, with pitch 104 on key 60. A song ends at the first row past the
module's last order. A song that plays once ends where its first row with a
jump starts, with no loop. A song whose loop sounds nothing ends where the
loop would start. A note whose step falls to 0 or below plays at level 0
until the step is above 0 again or another sample starts: a SoundFont can't
play a sample backwards.

## The GBAMOD30 builds

Later games have builds of the driver that play modules with the signature
`GBAMOD30`. Their strings name them as GBAModPlay 2002, GBAModPlay 2003 or
LS_Play 2003. A module has up to 12 channels and packed patterns. Its samples
are in a bank that all of the game's modules share. The pitches are linear, as
in Fast Tracker 2's modules. Every module seen has linear pitches. Each build
can also play ProTracker's periods.

### Modules

A module starts with `GBAMOD30`. The word at `0x28` gives its channels. Its
header has one of three layouts:

| Field | Layout A | Layout B | Layout C |
|---|---|---|---|
| the rate of rate index 0, 10512 | `0x34` | none | `0x34` |
| the count of orders | `0x38` | `0x34` | `0x38` |
| the count of patterns | `0x1B1C` | `0x138` | `0x13C` |
| the rows of each pattern | `0x1B20` | `0x13C` | `0x140` |
| linear (0) or Amiga (1) pitches | `0x1B24` | `0x140` | `0x144` |
| the tempo at the start | `0x1B28` | `0x144` | `0x148` |
| the speed at the start | `0x1B2C` | `0x148` | `0x14C` |
| each order's pattern, a byte each | `0x1B30` | `0x14C` | `0x150` |
| each pattern's size, or its offset in layout B | `0x1C30` | `0x24C` | `0x250` |
| the patterns | `0x2034` | `0x650` | `0x654` |

Layout A is GBAMOD2's header moved down by 0x20 bytes. The tables of steps and
periods are still in it. In layouts B and C, the byte 0xFF ends the list
of orders. One game has layout C with offsets in place of the sizes. Its
table of patterns starts with 0. Every pattern has the same count of rows.

A pattern has a block for each channel. A block starts with five words: the
second and third give the places of its second and third streams from the
block's data, and the fifth gives the size of the data. Each stream is a list
of runs: a count of rows, then the value of each of them. The first stream
gives a halfword a row, high byte first: the note in the top 9 bits, and the
sample below them, from 1. A note of 0x1FF is none. The second stream gives
the volume column, the volume plus 1, or 0 for none. The third gives the
effect and its operand. Row 0 of an order starts the streams. A break or a
pattern loop to another row doesn't move them: the rows go on reading from
where the streams are.

### Banks of samples

A bank has 256 headers of 12 or 16 bytes, then the points of each sample in
turn, signed 8-bit. A header of 12 bytes has the length as a halfword. One of
16 bytes has it as a word, and ends with a halfword of 0. The fine tune, the
volume, the loop's start and length and the relative note follow it, a
halfword each. In every game, the bank starts a word or two after the last
module.

### The builds

Eight groups of builds play GBAMOD30 modules. Each group's details are data in
the model:

| Group | Games | Layout | Bank headers | Channel records |
|---|---|---|---|---|
| A | *Super Puzzle Fighter II* | A | 12 bytes | 10 of 0x80 bytes |
| B | *Stuntman* | A | 12 bytes | 8 of 0x90 bytes |
| C | 2 games | C | 12 bytes | 8 of 0x94 bytes |
| C' | 1 game | C with offsets | 12 bytes | 8 of 0x94 bytes |
| D | 1 game | B | 12 bytes | 12 of 0x94 bytes |
| E1 | 5 games | B | 12 bytes | 12 of 0x98 bytes |
| E2 | 12 games | B | 16 bytes | 12 of 0x98 bytes |
| E3 | 3 games | B | 16 bytes | 8 of 0x98 bytes |

What changes between them:

- **The frame's loop.** Groups A, B, C and C' count each pass of the mixer as
  one sample more than it mixes. A frame can therefore end a few samples
  short. In groups A and B, a tick that falls due with a row doesn't run.
- **The lengths.** Groups A, B, C and C' give a note's length and loop as
  4096ths of a point less one. The later groups leave out the "less one".
- **The tick's effects.** In groups A, B, C and C', only the first four
  channels get slides, vibratos and volume slides at the ticks. The volume
  column applies at a tick to the fifth record alone, once. Group D applies
  it once to the record after the module's last channel. Group E applies it to
  every channel at every tick.
- **The volume slide.** Group B adds twice the operand at a tick. The others
  add it once. Groups C and C' clamp the volume only at those ticks, group E
  at every tick.
- **The linear slides and vibratos.** The plain linear slide moves the period
  by 4 times the operand, and by once the operand in groups C and C'. In
  group A, the vibrato and the slide to a note act on the Amiga form alone.
  They move nothing that a linear note uses.
- **The loop flag.** Group A has none: a module stops after its last order.
  The others loop to order 0 when the game sets the flag.
- **The mixers.** Groups A and B have an ARM mixer in the ROM. The others
  unpack a mixer of mode 0 or mode 1 by the game's choice. In mode 0, group E
  leaves a channel at volume 0 alone, and its sample waits. In mode 1, a pass
  ends where a channel's sample ends.

A note's linear period is `7616 - ((relative note - 1 + note - 1) << 6) -
fine tune / 2`. Its step comes from a table of 768 frequencies an octave:
with `y = 7680 - period`, the frequency is the entry at `y % 768`, shifted
left by 2 and right by `7 - y / 768`. The step is that frequency times 4096
over the mixer's rate. The table isn't exactly any formula, and the model
reads it from the ROM.

### The games' calls

The game sets the rate, the sound effects' channels, the mixer's mode, the
three volumes and the loop flag through the driver's calls. The mixer
multiplies a channel's volume by the music's, the master and the fade, each
over 64. A value above 64 makes a channel louder. A table by game code in
`mod30_games.cpp` (ntdlib) holds each game's values, with the addresses in the
game's code that they come from. It also holds the order of the songs: a
table of pointers in most games, and a jump table of literal modules in four.
The modules that no song names follow the songs, in their order in the ROM.

The output's start sets `SOUNDCNT_H`. One game starts its output with FIFO A
alone. *Medal of Honor: Infiltrator* pauses the driver from its menus with
no new init after it. The pause writes `SOUNDCNT_X = 0`, and both FIFOs play
at half volume from then on. The conversion plays that game at half the
level, as it does Aero the Acro-Bat. Four more games pause the driver that
way from their pause menus alone. Their conversions play at the full level.

### Locating and converting the builds

`agbsf2` looks for the GBAMOD30 signature at a word's start, with a header
that the reader reads. It finds the bank after the last module and checks
its headers: 16 bytes if every header fits, else 12. The game's code then
has to be in the table of games. A game with GBAMOD30 modules that isn't there
gets an error.

The conversion runs the model of the game's build, with the game's settings
and each song's loop flag, and turns its channels into notes as for GBAMOD2.
Note 49 is key 60. A note's key comes from its step against the sample's rate
at note 49. The SoundFont sample has that rate. A module without the loop flag
ends after its last order's last row.

A module with the loop flag loops where a row's place comes back. Orders that
play the same patterns in the same sequence count as one place. Many modules
of layout B give a count of 255 orders and fill the orders after the real
ones with 0xFF. Most of them jump back before those orders. Where the table of
patterns gives 0 for pattern 255, an order of 0xFF plays pattern 0. A module
whose one real order plays pattern 0 then plays it 255 times before the driver
comes back to order 0. The conversion loops after one order. A loop that comes
back only after an hour doesn't count, and the conversion ends there with a
warning.

## Locating the driver

`agbsf2` looks for modules: the signature at a word's start, with a header
whose counts fit the player's tables and whose patterns fill the size that the
header names. The driver's load routine contains the signature too: two
words of its literal pool, side by side in Aero's build and apart in Zapper's. The
routine that calls the load routine is the play routine. Each call of the play routine that loads a bank's address into `r2`
names a bank. A module uses the bank of a call that names it, or else the
bank that most calls pass. A call that names a module with an order above 0
adds a song that starts there.

Three routines of 12 bytes follow the play routine. Each stores `r0` in a
variable, and the three variables follow one another: the master volume, the
music's volume and the sound effects' volume. Where the game's code passes the
second routine one constant above 0, that constant is the music's volume of
the conversion. `--info` shows it with the address of the call.

The routine that switches the sound off stores 0 in `SOUNDCNT_X`. It is
near the play routine too. Where the game's code, outside the driver, calls
it or a routine of the driver that calls it, the conversion plays at half the
level. The FIFOs play at that level from the call on. `--info` shows the
address of the call. A call that only the driver's code reaches doesn't count.

Where a game passes its music volume to the driver from a variable, detection
can't read it. A table in the driver's code then names the game's volume, by
game code, with the addresses that it comes from. Zapper has an entry.

## The conversion

Each module becomes a MIDI file, with a track for each channel that plays
notes. A note starts at its row. It ends where the next note starts, where
its sample ends, or where its volume falls to 0. When the volume rises again,
a new note starts from the point where the sample waited. In the build with
long records, the sample moves on in silence. A note there lasts through its
rows at volume 0, at level 0. The SoundFont gets
an instrument for each sample and start point. An instrument's sample contains
the points that the notes play: the module's length of them, and the loop
again after them where it ends before the sample does.

A note's key comes from its pitch, with pitch 200 on key 60. The SoundFont
sets the fine tune of each sample. A step that misses its key by 3 cents or more
bends the note. The lowest octaves' steps do. A step is a whole number.
Slides and vibratos bend their notes at each tick. Every note has the velocity
127, and controller 11 sets the channel's volume in the mix: the volume less
the shortfall of the music's volume that the game sets at its start. A note
whose volume that leaves at 0 or below isn't played. The level is half of that
for a game that leaves its FIFOs at half volume.

A row lasts 120 MIDI ticks, and the tempo sets its length in samples. A
tick of effects falls on the MIDI tick nearest its place in its row. The samples
that a frame leaves unmixed aren't counted: they come to about one in 100,000.
`--frame-timing` changes nothing: rows and ticks fall on samples.

A module loops from the row whose order, row, speed and tempo come back. The
MIDI file marks that loop. Where the second pass starts its notes unlike the
first, with another sample, level or pitch, the marked loop starts at the
second pass. A jingle ends in a loop of empty rows, where the driver waits
for the game's next call. No channel sounds in such a loop. The MIDI file
then has no loop and ends where the loop would start.

## What the conversion leaves out

- The game's changes of its volumes. A conversion plays each module at the
  full master volume and at the music's volume that the game sets at its
  start. The options' other volumes and the fades aren't followed.
- The pauses. The game pauses the driver while it starts and at its title
  screen. A module plays on afterwards from where it stopped.
- The wrap of the mix buffer's bytes. Six channels at full volume and full
  scale can pass a byte's range.
- The sound effects. They are single samples that the game's code starts.
- In the GBAMOD30 builds, the faults of the mixers. The mixer of mode 1 lets
  a sum borrow from the next byte of a group, and reads one point too many in
  each tail. One game's mixer of mode 1 writes past a short chunk's end. The
  model keeps each channel's place as the drivers do. The faults change only
  the sums.
