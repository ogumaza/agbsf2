# Webfoot Technologies' sound driver

The Dragon Ball games that Webfoot Technologies made for the Game Boy Advance
play their music with a driver that the studio wrote. The driver plays Impulse
Tracker modules in a packed form. A song names a list of orders and a table of
patterns. Each pattern is a run of rows with an entry for each channel that
changes. A software mixer plays up to 15 music channels and the sound effects
of the game, and writes 8-bit output to both FIFOs. FIFO A plays the left side
and FIFO B the right side. This document describes the driver as `agbsf2`
models it. The addresses come from *Dragon Ball Z: The Legacy of Goku* (game
code `ALGE`) for the 2002 build and from *Dragon Ball Z: The Legacy of Goku
II* (`ALFE`) for the 2003 build, unless a table lists the addresses of each
game.

## The builds and the games

| Game | Code | Build | Mixer | Interrupt | Songs | Song table |
|---|---|---|---|---|---|---|
| *Dragon Ball Z: The Legacy of Goku* | `ALGE` | 2002 | 13,981.01 Hz | DMA 2, every 16 samples | 18 | `0x08591F3C` |
| *Dragon Ball Z: The Legacy of Goku II* | `ALFE` | 2003 | 15,978.30 Hz | DMA 2, every 16 samples | 44 | `0x081D4B2C` |
| *Dragon Ball Z: Buu's Fury* | `BG3E` | 2003 | 15,978.30 Hz | DMA 2, every 16 samples | 53 | `0x083BB78C` |
| *Dragon Ball Z: Buu's Fury* and *Dragon Ball GT: Transformation*, the menu | `BUFE` | 2003 | 15,978.30 Hz | timer 1, every 64 samples | 1 | `0x08013C48` |
| the same cartridge, *Buu's Fury* | `BUFE` | 2003 | 15,978.30 Hz | timer 1, every 64 samples | 53 | `0x083EC22C` |
| the same cartridge, *GT Transformation* | `BUFE` | 2003 | 15,978.30 Hz | timer 1, every 64 samples | 25 | `0x088F6B9C` |

The cartridge with two games contains three programs, each with a start-up
routine and a copy of the driver: the menu at `0x08000000`, *Buu's Fury* at
`0x08030000` and *GT Transformation* at `0x08800000`. The *Buu's Fury* on this
cartridge is the code of `BG3E`, 0x30000 bytes higher, with the same 53 songs.

The player of the 2003 build is the same code in every copy, apart from the
addresses. The builds differ in the header of a song, the mixer rate, the
width of the channel levels and the way the mixer adds the channels:

- The 2002 build has a header of 12 bytes. The game has one sample table and
  one table of masks for all its songs. The timer counts 1,200 cycles a
  sample. A tick lasts 35,000 samples over the tempo, rounded down. The levels
  of a channel are bytes, and the sum of each side wraps at 16 bits.
- The 2003 build has a header of 20 bytes. The header names the sample table
  and the table of masks of the song. The timer counts 1,050 cycles a sample.
  A tick lasts 40,000 samples over the tempo. The levels of a channel are
  halfwords, and the mixer adds both sides in one word, with a carry from the
  left side into the right.

Each build computes the step of a sample from a nominal rate of 14,000 or
16,000 Hz: 2^26 over that rate, rounded up. The timer runs a little off it. A
sample therefore plays about 2 cents off its stated rate.

## The games' calls

| Program | Init | `r0` of the init | Music volume | Play | Handler of the driver |
|---|---|---|---|---|---|
| `ALGE` | `0x0800A9F6` | 14000 | `0x0800A9CC` (`r0`) | `0x0800A5F6` (`r0` = header) | `0x08008AA6`, Thumb, in ROM |
| `ALFE` | `0x08020AF4` | `0x030026BC` | `0x08020AC8` | `0x08020A4C` | `0x03000798` |
| `BG3E` | `0x0803F1B8` | `0x03003898` | `0x0803F18C` | `0x0803F110` | `0x03000AE0` |
| `BUFE`, the menu | `0x08002060` | `0x03001300` | `0x08002034` | `0x08001FB8` | `0x03000668` |
| `BUFE`, *Buu's Fury* | `0x0806F7F4` | `0x0300389C` | `0x0806F7C8` | `0x0806F74C` | `0x03000AE0` |
| `BUFE`, *GT Transformation* | `0x08833FB0` | `0x03004874` | `0x08833F84` | `0x08833F08` | `0x030008D8` |

In the 2003 build, the init takes the record of the driver, and the music
player is the record plus 0xC. The routine of the music volume takes the
player and a volume from 0 to 8. The play routine takes the player and the
header of a song. The 2002 build takes the volume and the header in `r0`.

The init sets the music volume to 4 of 8. The games then pass the volume of
their options: 4 by default, and 3 in *GT Transformation*. The menu of the
cartridge with two games keeps the 4 of the init. A game starts a song with
its header from the song table, mostly after a check that the song isn't the
one that plays. A stop clears the flag of the player for a song that plays. No
game sets the mask of a channel, a mute, a tempo or a starting order. *GT
Transformation* halves its music volume on one screen and restores it
afterwards.

The handler of the driver counts the samples that the FIFOs have played. When
the lead of the mixer falls to its mark, the handler turns the interrupts on
again and runs the mix routine. Each run mixes 896 samples (`ALGE` and `BUFE`)
or 928 samples (`ALFE` and `BG3E`). The mix routine mixes the run in blocks of
256 samples in the 2002 build and 512 in the 2003 build. It starts a tick
where the samples of the last tick run out. The handlers don't return like
routines. They return through a register that the interrupt handler of the
game sets.

## Songs

The header of a song starts with two words: the address of a table of the
addresses of the patterns, and the address of the orders, a byte each. In the
2003 build, the header then names the sample table and the table of masks of
the song. The header ends with the tempo and speed of the song. The list of
orders ends with 0xFF, and the player then goes back to the first order.

A pattern starts with its count of rows. Each row lists the entries of the
channels that change, and ends with a 0 byte. The first byte of an entry
contains the channel in the low 4 bits and a mode in the high 4 bits. Mode 0
is followed by a mask byte, mode 1 takes the last mask of the channel, and
modes 2 to 15 take a mask from the table of 14 masks of the song. The low 4
bits of the mask select which of the note, instrument, volume and effect bytes
follow. The high 4 bits select which of them repeat the last ones of the
channel. A note from 0 to 119 starts the sample of the instrument, and note
255 stops it.

The entry of a sample has 12 bytes: the address of the signed 8-bit points,
the flags (a loop, and a ping-pong loop), the first point of the loop, the
length and the rate at C-5 of Impulse Tracker, note 60.

## Effects

The player reads these effects of Impulse Tracker: A (speed), B (jump to an
order), D (volume slide), E and F (slides down and up, with fine and
extra-fine steps), G (slide to a note), H (vibrato), K and L (vibrato or slide
to a note with a volume slide), O (sample offset), Q (retrigger), T (tempo), U
(fine vibrato), X (pan) and a note delay as effect 27. An effect with a
parameter of 0 takes the last parameter of the same kind on the channel. The
songs of the 2003 games use A, B, D, E, F, G, H, O, Q, T, X and the note
delay. *The Legacy of Goku* uses the same without A, and with U. Every T in
their songs sets a tempo.

## The mixer

Each voice plays its sample point by point, without interpolation, at a step
in 16384ths of a point. The level of a side is the music volume × the volume
of the channel × (64 ∓ the pan), shifted right by 10 bits. A channel at volume
64 and no pan plays at 16 of 64 at the music volume 4, and volumes 1 to 3 are
silent there. The mixer adds the points of each side times their levels,
shifts the sum right by 6 bits and clips it to a byte.

A ping-pong loop plays the loop forward and back. The 2002 build turns at the
start of the loop to a place below it, and can play several hundred points
from before the loop. A turn depends on where a block of the mix starts.

## Locating the driver

`agbsf2` finds the driver by its content. It looks for the table of steps: 120
words, with 0x4000 at note 60. The tables of slides and the sine of the
vibrato follow it. A song table is a run of headers with patterns that all
read to their last rows with entries for channels 1 to 15. A run of one header
counts only where a word in the image names it. Each song table uses the copy
of the driver in its program: the program with the start-up code before the
table. In the 2002 build, the code names the sample table and the table of
masks of the game. `--info` lists each song table with its songs, build,
program, mixer rate, samples a run of the mix and music volume. The song
numbers count through the song tables in address order.

The games pass the music volume of their options from a variable. Detection
can't read it there. A table by game code and program in agblib names the
volume of each game. A song table that the table doesn't name plays at the 4
of the init, with a warning.

## The conversion

Each song becomes a MIDI file, with a track for each channel that plays notes.
A note starts where a voice starts its sample, from the first point or from an
offset. It ends where the voice stops or starts the sample again. A retrigger
and a delayed note result in notes at their ticks. The key of a note is the
one nearest the rate of the voice, and a pitch bend covers the rest. Slides
and vibratos bend their notes at each tick. The levels of the two sides at
each tick set the pan (CC10) and expression (CC11) of the channel.

A row lasts 120 MIDI ticks, and the tempo map sets the length of each row in
samples. The beat and the bars are chosen as for Krawall modules.
`--frame-timing` changes nothing: rows and ticks fall on samples.

A song loops from the first row that comes back to a place in the song with
the same speed and tempo. The MIDI file marks that loop. A jingle ends in a
loop in which no voice sounds. Its MIDI file has no loop and ends where that
loop would start.

The SoundFont has a preset for each sample and start point. The rate of a
sample is the rate at which the driver plays it at note 60. That rate includes
the 2 cents between the nominal rate and the rate of the timer. A forward loop
stays as it is. A ping-pong loop becomes the points of the sample to its end,
then the points of the loop backwards, and the SoundFont loops over both. A
note plays at the pitch of the step of the voice in both builds. The zones keep the shortest release that
the format allows. The driver stops a voice at once.

`--music-volume N` plays every song table at the music volume N, from 1 to 8.

## What the conversion leaves out

- The character of the mixer: the points without interpolation, the cut of
  the sum to 8 bits and its clipping, and the wrap of the sum of each side in
  the 2002 build.
- The changes of the music volume in the game, such as the half volume of one
  screen of *GT Transformation*.
- The sound effects. They are single samples that the code of the game starts
  on the other voices of the mixer.
- A listing of the songs. `--dump` reports an error for this driver.
