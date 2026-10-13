# Ubisoft Milan's driver

Ubisoft Milan wrote a sound engine of its own for *Tomb Raider: The Prophecy*.
Its music is a set of short sequences in a MIDI-like format, whose notes mostly
play samples through a kit with a sample for each key. A software mixer plays
the samples on four voices, and the sequences can also play the Game Boy's PSG.
This document describes the engine as `agbsf2` models it. The example
addresses come from the European release of the game (game code `AL9P`). The
game's sound effects are samples that it plays straight from the same sound
bank. They aren't part of the music. `agbsf2` doesn't convert them.

## Architecture

The engine is Thumb code in ROM. Only the mixer is ARM code, and the game
copies it to IWRAM. Its state is in EWRAM at `0x02003B00` (the sound
bank's tables) and `0x02003B80` (the music, with its track at `0x02003BB0`),
and the mixer's voices are in IWRAM at `0x03000014`.

| Address | Routine |
|---|---|
| `0x08002AA8` | returns file r0 of the game's archive, whose table is at `0x08061D48` |
| `0x080031A0` | the engine's init: takes the sound bank, then starts the sequencer if it hasn't started |
| `0x08003E90` | takes the sound bank: its counts and the addresses of its tables |
| `0x08003130` | the sound update that the game's main loop calls once a frame |
| `0x08003280` | the sequencer's step that the game's VBlank runs as a task |
| `0x080044B4` | starts a piece of music: r0 = its resource, r1 = 1 to start it now |
| `0x08004878` | the track's step: the countdown, then its commands |
| `0x08004950` | a channel's command: note on or off, program |
| `0x08004BB0` | a PSG note: channels 0-2, and the noise for channel 9's low keys |
| `0x080057D0` | a note of the kit, on channel 9 |
| `0x08000F14` | starts a sample on a voice |
| `0x0800097C` | the mixer (ARM, copied to IWRAM) that the FIFOs' DMA interrupts run |

The game's start hands the engine its sound bank, the archive's file 2, with
`movs r0, #2` and a call of the archive's lookup, then a call of the engine's
init.

## The sound bank

The bank starts with two halfword counts, of which the second is the number of
resources, and three words: the offsets from the bank's start of a table of
the game's sound events, of the resource table and of the kit table. The
resource table has a word for each resource, its offset from the bank. The kit
table has a halfword for each of channel 9's kits, a resource number, or
`0x8000` for none. The engine's init gives channel 9 the first.

Each resource starts with a byte for its kind:

| Kind | Resource |
|---|---|
| 0 | a piece of music: its list of sequences |
| 3 | a sample |
| 4 | a list of resources, one of which plays |
| 6 | a sequence |
| 7 | a kit: a halfword for each key, a sample's resource number or `0x8000` |

A sample has 16 bytes of header: its kind, a byte that is 1 if it loops, two
bytes that the engine doesn't read, its length in points, and its loop's start
and end in points from the first. A sample without a loop has `0x42424242` in
those two words. Its points follow, 8-bit and signed.

## Pieces of music

A piece of music has its kind and three bytes that the engine doesn't read,
then a halfword count of sequences, a halfword of flags and the sequences'
resource numbers. Flag 1 makes the list start again after its last sequence;
flag 8 gives channel 9 a voice for the whole piece. A piece of one sequence
with flag 1 plays it again and again.

Starting a piece silences the PSG's channels, stops channel 9's voice, takes a
voice for channel 9 if the piece has flag 8, and starts the track on the first
sequence, with the next one queued. When a sequence ends, the track takes the
queued one at once, in the same frame. The engine's callback then gives the
track the resource of the sequence after that, and the next sound update puts
the address of that sequence's commands in its place and moves the piece on. A
piece without flag 1 stops when its last sequence ends.

## Sequences

A sequence has its kind and three bytes that the engine doesn't read, then a
halfword countdown, then commands. Each command is a status byte, with the
channel in its low 4 bits, its arguments, then the next command's countdown:

| Status | Arguments | Command |
|---|---|---|
| `8c` | key, velocity | note off on channel c |
| `9c` | key, velocity | note on on channel c |
| `Cc` | program | program change on channel c |
| `Ec` | none | nothing |
| `FC` | none | the sequence's end |

Any other status makes the game reset the console.

The track's step runs once a frame. If its countdown isn't 0, it counts down;
otherwise it runs commands until a countdown isn't 0. So a countdown of n puts
the next command n + 1 frames later, and a countdown of 0 runs it in the same
frame. The first sequence's countdown is set before the first frame's step, so
its first command comes n frames in.

Channels 0, 1 and 2 play the PSG's square 1, square 2 and wave channel.
Channel 9's keys up to 11 play the noise channel, and its keys above that the
kit. A note on another channel plays nothing. A command's first argument
chooses between the PSG and the kit on channel 9. A program change to 11 or
below therefore goes to the PSG. The PSG stores it past the end of its three
channels' records, and it changes nothing that the music plays.

## The kit

A note of the kit plays the sample that the kit's entry for key + program
gives, where the program is channel 9's last program change above 11, 0 at
first. The engine doesn't check the entry. `0x8000` makes it read resource
32768, past the end of the resource table. In *Tomb Raider* that gives an
address in a mirror of the ROM, and the note plays whatever bytes are there.

The mixer plays a sample at a fixed rate, and a key chooses a sample. The
velocity is the voice's volume. Each note takes a voice: the piece's, if it has
flag 8, or else the last free one of the four. A note-off on channel 9,
whatever its key, stops the voice of channel 9's last note and frees it.
Another note that still plays goes on until its sample ends. A voice is also
free again at the sound update after the mixer reaches the end of its sample.
If no voice is free, the engine writes the note before the voices' records.

## The mixer

The game sets SOUNDCNT_H to `0xA90E`: the PSG and both FIFOs at full volume,
FIFO A on the right and FIFO B on the left, both clocked by Timer 0. The sound
init works out Timer 0's reload in floating point: `0xFFFF` less half of
`1024.17`. That gives `0xFDFF`, 513 cycles a point, or 32704 Hz. Each FIFO's
DMA interrupts the CPU at every 16 points. Whichever interrupt comes second
runs the mixer, and the mixer makes the next 16 points. Both FIFOs play them,
and the music is mono.

Each run, the mixer copies the next 8 points of each voice that plays with
DMA 3, and moves the voice on 8 points. A voice that loops goes back to its
loop's start once it reaches its loop's end; one that doesn't stops once it
reaches its end, after those 8 points. A loop therefore lasts a whole number
of 8 points from its start, and the points after the loop's end, up to the
next 8, come from beyond it. A sample without a loop leaves the voice's loop
points as the last sample that looped set them. The mixer multiplies each
point by its voice's volume and adds the voices. That gives 8 points at
16352 Hz. It puts a point halfway between each pair, averages each point with
the one before, shifts the sums right by 6, and clips them to 8 bits. A
full-scale sample at volume 64 fills the scale.

## The PSG

Each PSG channel has a table of 16-byte instruments: square 1's at
`0x0803D10C`, square 2's at `0x0803D2EC` and the wave channel's at
`0x0803D4CC`. A note on square 1 writes NR10 from the instrument's first byte,
NR11 and NR12 from its second and third, with the velocity ORed into NR12, and
NR13 and NR14 from the key's frequency setting and its fourth byte. Square 2's
instruments have no sweep byte, and the wave channel's note sets NR30 to `0xA0`
and writes the velocity to NR32 as its volume. The key's place in the frequency
table at `0x08038084` is 36 keys down on the squares and 12 down on the wave
channel. Every key therefore plays at its MIDI pitch. The wave channel's
program change loads 32 bytes of the wave table at `0x08038154` into both banks
of the wave RAM. The two banks play as 64 points.

An instrument's halfword at offset 12 has flags: 1 for a modulation, 2 for a
pitch mode, and 4 for a note that doesn't start the channel again while one
plays. The step's PSG update moves the frequency of a note with modulation each
frame. A note-off silences its channel, if its key is the last one that the
channel's note set.

Channel 9's noise notes take 4 bytes for the key from the table at
`0x080385CC`: NR41, NR42, with the velocity ORed into it, NR43 and NR44. A
noise note-off writes nothing. The noise's envelope and length run on.

The engine's init sets SOUNDCNT_L to `0xFF33`: every PSG channel on both
sides, at a master volume of 3 of 7.

## Quirks

* A note on a key outside the kit plays bytes from elsewhere in the
  cartridge (see [The kit](#the-kit)). `agbsf2` leaves the note out,
  with a warning.
* A note-off on channel 9 stops the voice of its last note, whatever its key.
* A countdown of n waits n + 1 frames, but the first sequence's first one n.

## Locating the driver

`agbsf2` looks for the engine's code:

* The track's step:
  `B530 1C04 68E0 2800 D0xx 8820 1C25 3514 2800 D1xx 6920 2800 D0xx 2000 6120
  68A2 2A00 D0xx 6062 7811 7850 0200 4301 1C90 6060 8021`.
* The routine that takes the sound bank:
  `4Axx 6110 8801 8011 8841 8051 6841 1841 6051 6881 1841 6091 68C1 1840 60D0
  4770`. The game calls it from a routine that starts with `push {lr}`, which
  the game's start calls after `movs r0, #N` and a call of the archive's
  lookup. `agbsf2` runs the lookup with N to find the bank.
* The PSG's instrument lookup, whose three literals give the channels' tables:
  `B500 0600 0E00 2901 D0xx 2901 DCxx 2900 D0xx Exxx 2902 D0xx Exxx 0100 49xx
  1840`.
* The noise note, the frequency lookup of square 1's note and the wave
  channel's program change, for the noise, frequency and wave tables:
  `00B1 48xx 1809 780A 3101 7808 3101 4338 0200 4310 4Axx 8010`,
  `49xx 4642 0050 1840 8807 6BE0 0040 1840 8800` and `0168 49xx 1840`.
* The sound init, whose last `ldr` loads the float that Timer 0's reload comes
  from.

The pieces of music are the bank's resources of kind 0, in order. `--song-table`
gives the sound bank's address instead.
