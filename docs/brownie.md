# Brownie Brown's GBA sound driver

This document covers the sound format, sequencer, mixer and hardware control
in Brownie Brown's driver for *Sword of Mana* on the Game Boy Advance. It's
based on the driver's code in the US release of *Sword of Mana* (game code
`AVSE`), and the addresses given as examples come from that game. *Magical
Vacation* (`AMVJ`) uses an older revision without the mixer; [The Magical
Vacation revision](#the-magical-vacation-revision) lists the differences.
`agbsf2` finds the driver and its tables from the code (see [Locating
the driver](#locating-the-driver)).

The driver plays 13 channels. Channels 0-3 play the music on the Game Boy's
square 1, square 2, wave and noise channels, and channels 4-7 play sound
effects on the same four channels, taking each one from the music while they
play. Channels 8-12 play samples on the five voices of a software mixer, which
mixes them into the two DirectSound FIFOs at 16384 Hz: FIFO A for the right
side and FIFO B for the left. A sample set holds a sample for each of the 12
semitones, and the mixer plays the lower octaves by repeating each point.

The driver counts time in frames and has no tempo variable. Each song
selects a set of note lengths from a table, with each set corresponding to a
tempo.

## Architecture

The driver is ARM code. The game calls the driver's init once, copies the mixer
to IWRAM, and calls the per-frame routine from its VBlank handler. The mixer
runs from the interrupts of DMA 1 and DMA 2, which feed the FIFOs.

In `AVSE` the routines are:

| Address | Routine |
|---|---|
| `0x080A1834` | init: resets the sound, the channels, the queue and the voices, and sets up the FIFOs' DMA and Timer 0 |
| `0x080A16EC` | queue a sound: `r0` = its number |
| `0x080A170C` | bring the sound back after a fade out: fades in at speed `r0` and queues sound `0x36` |
| `0x080A17C4` | fade out at speed `r0`, which stops everything at its end |
| `0x080A1900` | the per-frame routine |
| `0x080A1EB8` | the data reader, part of the per-frame routine; its command table, for commands `E0` to `FF`, is at `0x080A2174` |
| `0x03003150` | the mixer, copied from `0x080A0B74`: the routine of both DMA interrupts, which mixes voices 0 and 1 |
| `0x03003750` | the mixer's part for all five voices, copied from `0x080A1040` |

The game reaches them through Thumb routines at `0x080A153C` to `0x080A15E2`,
and veneers at `0x0836DF08` to `0x0836DF38`.

Its tables in ROM and its RAM hold:

| Address | Contents |
|---|---|
| `0x080A15E4` | the frequency table: a halfword for each key, the PSG's frequency setting |
| `0x080A168C` | the PSG registers that each channel's NRx1 and NRx2 go to: 8 words, the channels 4-7 the same as 0-3 |
| `0x080A16AC` | the PSG registers that each channel's frequency setting goes to: 8 words |
| `0x080A16CC` | the pans: each channel's bits of NR51, 12 bytes, then the bits it clears, 12 bytes |
| `0x080A16E4` | Timer 0's reload value: `0xFC00`, for 16384 points a second |
| `0x080A2800` | the length sets: 16 bytes each |
| `0x080A2980` | the envelopes: a word each, the address of its steps |
| `0x080A2FFC` | the song table: a word for each sound, the address of its list of channels |
| `0x080C2EE8` | the sample sets: a word each, the address of its 12 entries |
| `0x083678A0` | the waves: a word each, the address of its 16 bytes |
| `0x03002ACC` | the channel records: 14 of 0x38 bytes |
| `0x03002DDC` | the queue: the next request's offset, the next free offset, and 8 halfwords |
| `0x03002DEE` | the mixer's mode |
| `0x03002DF0` | the master level, the fade's step, and the fades' counts (see [Fades](#fades)) |
| `0x03002DFC` | the driver's copy of SOUNDCNT_L |
| `0x03002E0C` | the voices: 5 of 16 bytes |
| `0x03002E6C`, `0x03002E7C` | the 16 points that the mixer makes for FIFO A, and for FIFO B |

## Sounds

The song table lists every sound the game plays, music and sound effects
alike. A sound's list of channels has three words for each channel: its number,
the address of its data, and the address of its table of subroutines, which
`F6` and `F8` take places from. The word `0xFF` ends the list. The music
usually plays on channels 0-3 and 8-12, and the sound effects on channels 4-7,
but a sound can name any of the channels.

The game queues a sound by its number; the queue holds 8 requests. After the
channels, each frame starts the next request: for each channel the sound lists,
the driver sets the channel's in-use bit, its data and its subroutines, clears
the bytes from offset `0x24` to `0x2F`, sets its wait to 1, so that it reads its
data in the next frame, and sets its pan from the pan table. A sound effect's
channel sets bit 1 of the music channel with the same PSG channel, which then
stops writing the PSG's registers. The driver doesn't clear the rest of a
channel's record.

A request of 0 is no request, so sound 0 can't play. The driver drops a request
for sound `0x3B` or `0x3D` if NR42 holds a volume, or if channel 3 is in use
with fewer than 3 frames left to wait.

## Channel records

| Offset | Size | Contents |
|---|---|---|
| `0x00` | 1 | flags: bit 0 in use, bit 1 a sound effect has the PSG channel, bit 7 in use when a fade out stopped everything |
| `0x01` | 1 | the frames to wait until the next read of the data |
| `0x02` | 1 | the frames to the envelope's next step, or 0 for none |
| `0x03` | 1 | `F4`'s count |
| `0x04` | 4 | the next byte of the data |
| `0x08` | 4 | where `F5` goes back to |
| `0x0C` | 4 | where `F7` returns to |
| `0x10` | 4 | the subroutines |
| `0x14` | 4 | the sample set |
| `0x1C` | 4 | the wave |
| `0x20` | 2 | the frequency setting, or the noise channel's NR43 |
| `0x22` | 2 | NRx1 in the low byte and NRx2 in the high, as last set |
| `0x24` | 1 | NRx1, or on a sample channel the frames to the level's next step |
| `0x25` | 1 | NR10, or on a sample channel its volume with the envelope's level |
| `0x26` | 1 | the volume |
| `0x27` | 1 | the detune |
| `0x28` | 1 | the envelope's level |
| `0x29` | 1 | the frames each note or rest lasts |
| `0x2A`, `0x2B` | 1 each | a PSG channel's bits of NR51 and the bits it clears, or a sample channel's right and left levels |
| `0x2C` | 1 | the transpose, signed |
| `0x2D` | 1 | a sample channel's change in level, down with bit 7 set |
| `0x2E` | 1 | the frames between those changes |
| `0x2F` | 1 | the length set |
| `0x30` | 4 | the envelope that each note starts |
| `0x34` | 4 | the envelope's next step |

## The per-frame routine

1. For each channel in use, in order from 0 to 12, the driver counts down its
   wait. When the wait runs out, it reads the channel's data (see
   [Data](#data)). Otherwise it counts down the frames to the envelope's next
   step, and takes the step when they run out; and on a sample channel that
   doesn't take a step, it counts down the frames to the level's next change,
   and changes the level when they run out (see [Envelopes](#envelopes)).
2. It takes the next step of a fade in, or else of a fade out (see
   [Fades](#fades)).
3. It starts the next request in the queue (see [Sounds](#sounds)).

A wait of 0 counts down to 255.

## Data

A channel's data is a string of bytes:

* `00`-`7F` is a note of that key, after which the channel waits for its
  length.
* `80`-`DF` sets the length: byte `0x7F & b` of the channel's length set, 16
  bytes on for each set. The wait counts down in a byte, so a length of 0 waits
  256 frames.
* `E0`-`FF` is a command.

The driver reads the data up to a note, a rest, a hold, a release or the end,
which end its reading for the frame.

### Notes

A note's key is the byte plus the transpose, kept to 7 bits.

On channels 0-2 and 4-6, the frequency setting is the frequency table's entry
for the key, plus the detune. On the noise channels, 3 and 7, the byte itself,
without the transpose, gives NR43, with its nibbles swapped. The note sets the
channel's wait to its length, and starts its envelope: the envelope's first
step gives the frames to the next and the level, from which the channel's
volume is taken away. A level below the volume gives an NRx2 of 8, which is
silent. Unless a sound effect has the PSG channel, the driver writes NR10 from
the record's `0x25` on channels 0 and 4, then NRx1 and NRx2, and then the
frequency setting with bit 15, twice, which starts the channel.

On a sample channel, keys from 64 play key 47. The key's octave picks the rate:
keys 0-11 play at an eighth of the mixer's rate, 12-23 at a quarter, 24-35 at
half and the rest at the full rate. The key less 12 for each octave picks the
entry of the sample set, so keys 48-63 read past its 12 entries. An entry is
three words, the sample's start, its end and its loop, which the note copies to
the channel's voice with the mode for the rate. The note starts the envelope,
sets the voice's levels (see [Levels](#levels)), and starts the FIFOs' DMA if
it isn't running, and Timer 0.

### Commands

| Command | Arguments | Effect |
|---|---|---|
| `E0` | envelope | release: the envelope goes on from this envelope's first step at the next frame; ends the reading |
| `E1` | set | the sample set |
| `E2` | mode | the mixer's mode: 0 mixes voices 0 and 1 only, and clears the flags of channels 10-12 and of the 14th record; anything else mixes all five |
| `E3` | | brings the sound back after a fade out (see [Fades](#fades)); ends the per-frame routine |
| `E4`-`E7`, `EB`, `EF` | | nothing |
| `E8` | | a sound effect gives the music its PSG channel back, clearing bit 1 of the music channel 4 below |
| `E9` | | a sound effect takes the PSG channel from the music |
| `EA` | pan | a sample channel's pan: the right side's level from the low nibble and the left's from the high, each nibble *n* as `n × 16 + 15`, or 0 |
| `EC` | | a sample channel's rest: the voice of the channel's number & 7 falls silent; ends the reading |
| `ED` | | a PSG channel's rest: NRx1 and NRx2 are written as 0, and the frequency register as described in [Quirks](#quirks); ends the reading |
| `EE` | set | the length set |
| `F0` | envelope | the envelope that each note starts |
| `F1` | NRx1 | NRx1: the duty and the length |
| `F2` | volume | the volume, which a PSG channel takes from each level and a sample channel multiplies by |
| `F3` | transpose | the transpose, signed |
| `F4` | count | repeat from here |
| `F5` | | go back to `F4`'s place until its count runs out; a count of 0 gives 256 passes |
| `F6` | subroutine | call a subroutine; the calls don't nest |
| `F7` | | return |
| `F8` | subroutine | jump to a subroutine's place: a loop when the channel has played there before |
| `F9` | detune | the detune, added to each note's frequency setting |
| `FA` | NR10 | the sweep that each note on channel 0 or 4 writes |
| `FB` | | no sweep |
| `FC` | wave | load a wave into the wave RAM: the driver writes NR30 `0x40`, the 16 bytes, and NR30 `0x80` |
| `FD` | pan | a PSG channel's pan: the low nibble's bit 0 for the right side and the high nibble's for the left, which the driver shifts to the channel's bits and writes into NR51 |
| `FE` | | hold: the note goes on for another length; ends the reading |
| `FF` | | end (see below) |

`FF` turns the channel off. A music PSG channel writes NRx2 0 and its frequency
register `0x8000`, unless a sound effect has its PSG channel. A sound effect
gives its PSG channel back: if the music channel plays, the driver loads its
wave again on the wave channel, writes its pan into NR51, and writes its NRx1,
NRx2 and frequency setting with bit 15 flipped, which starts the channel again,
and NR10 on channel 4; otherwise it turns the PSG channel off. A sample
channel's voice falls silent, and if the low bytes of voices 0 and 1's next
points are both 0, the driver stops Timer 0 and the FIFOs' DMA.

## Envelopes

An envelope is a list of steps. A PSG step is two bytes: the frames until the
next step, and NRx2's value before the volume is taken away. Each step writes
NRx1, NRx2 and the frequency setting with bit 15, which starts the channel
again, so the hardware's envelope starts again from the step's volume. A step
of 0 frames is the last.

A sample step is three bytes: the frames until the next step, a change in level
and a level. The step sets the level, and the change, up or down by its low 7
bits each frame, down with bit 7 set, kept from 0 to 255, goes on until the
next step.

### Levels

A sample channel's volume with the envelope is `(volume + 1) × level / 256`,
rounded down, which the record keeps at `0x25`. A note sets the voice's sides
to

    right = (right pan + 1) × volume × (master + 1) / 65536
    left  = (left pan + 1) × volume × (master + 1) / 65536

and an envelope's step or a change in level to

    scaled = volume × (master + 1) / 256
    right  = (right pan + 1) × scaled / 256

each rounded down, so the two can differ by a step.

## Fades

The master level, 0-255, scales the sample channels' voices, and NR50 gets the
top 3 bits of it on each side. A fade changes it by its step each time its
timer, in 256ths of a frame, runs out, and sets NR50 and the voices of the
sample channels in use.

A fade out stops everything at its end: the master level drops to 0, the
driver turns the sound off and on (NR52), stops Timer 0, writes SOUNDCNT_H and
SOUNDCNT_L as the init does, sets bit 7 of each channel in use in place of bit
0, clears the queue's offsets and its first four requests with the last
record's new flags, and sets the master level to 255.

`E3`, or the game's routine at `0x080A170C`, brings the sound back: the master
level drops to 0 and fades in over 64 frames, 4 a frame, SOUNDCNT_L is cleared
until the fade's next step writes it from the driver's copy, the channels with
bit 0 or bit 7 set play again, Timer 0 starts, and sound `0x36` goes into the
queue. `E3` ends the per-frame routine there, so the later
channels, the fades and the queue wait until the next frame. The fade in's last
frame sets NR50 to `0x77`, and leaves the master level where the fade left it.

## The mixer

Each time the FIFOs have played 16 points, the DMA interrupts run the mixer,
which makes the next 16 points of each FIFO. In mode 0 it mixes voices 0 and 1;
otherwise it mixes all five through its part at `0x03003750`.

A voice is 16 bytes: the mode, a copy of it, the right and left levels, and the
next point, the sample's end and its loop. For each voice it mixes, the mixer
first checks whether the next point is the end: then it goes back to the loop,
and if the loop is the end too, the voice is silent and stays where it is. Then
it takes the voice's next points: with mode bit 0 set, 8 points, each played
twice; with bit 1, 4 points, each played four times; with bit 2, 2 points,
each played eight times; and otherwise 16 points. The part for five voices
interpolates between the points it plays twice. Each FIFO's point is the sum of
each voice's point times its level on that side, divided by 256 and rounded.

The end has to fall on a whole number of the voice's steps, since the mixer
only checks for it before each run.

## Quirks

The driver's tables of PSG registers have entries for channels 0-7 only. `ED`
on a sample channel reads past them: past the NRx1 table it finds the
frequency registers' table, so it writes 0 to the frequency register of PSG
channel *c* − 8, or of square 1 for channel 12 (NR23 and NR24 for channel 9,
NR43 and NR44 for channel 11), and past that table it finds the pans, so it
writes the routine's `r5` to an address outside the I/O registers. Writing 0
there doesn't start the PSG channel, but sets its frequency setting, or the
noise's clock, to 0 until its next note or envelope step writes it again. `ED`
doesn't touch the sample channel's voice, which goes on playing; only its
envelope's steps stop.

On a PSG channel, `ED` writes `r5` with bit 15 flipped to the frequency
register: what the routine last left there, such as the frequency setting of an
earlier channel's note in the same frame, or what the game's code left there
before the routine. The channel's DAC is off by then, so that does nothing that
can be heard.

`EC` on a PSG channel silences the voice of the channel's number & 7, and leaves
the channel playing.

The test that stops the FIFOs at a sample channel's end looks at the low byte of
voices 0 and 1's next points only, so it can stop them while voice 0 or 1 plays
from a point whose low byte is 0, or while voices 2-4 play. The next sample
note starts them again, and the voices play on from where they stopped.

`E3` queues sound `0x36` with the queue's offset kept to 8 bytes, where the
queue routine keeps it to 16. The fade in's end also writes `0xFF` to address
0, which does nothing.

## The Magical Vacation revision

*Magical Vacation* has an older revision of the driver. Its per-frame routine,
its data and most of its commands work as described above, apart from what
follows. It has no mixer: each of its two sample channels plays one sample at a
time straight through a FIFO, at a rate that a timer sets for each note.

In `AMVJ` the routines are ARM code that runs from the ROM. The game reaches
them through Thumb routines at `0x0805B564` to `0x0805B5B2`, and veneers at
`0x080C2044` to `0x080C2070`:

| Address | Routine |
|---|---|
| `0x0805BF3C` | init |
| `0x0805BEAC` | queue a sound: `r0` = its number |
| `0x0805BECC` | fade out at speed `r0`, which stops everything at its end |
| `0x0805C2C8` | the per-frame routine; its command table is at `0x0805C994` |
| `0x0805BFD8` | the DMA 1 interrupt, which feeds FIFO A |
| `0x0805C150` | the DMA 2 interrupt, which feeds FIFO B |

Its tables in ROM and its RAM hold:

| Address | Contents |
|---|---|
| `0x0805B5B4` | the frequency table |
| `0x0805B65C`, `0x0805B67C` | the PSG registers that each channel's NRx1 and NRx2 go to, and its frequency setting: 8 words each |
| `0x0805B69C` | the pans: each channel's bits of NR51, 12 bytes, then the bits it clears, 12 bytes |
| `0x0805B6B4` | the rate table: a halfword for each 12th of a semitone, a timer's reload value |
| `0x0805CE80` | the lengths |
| `0x0805CEE0` | the envelopes |
| `0x0805D2DC` | the song table |
| `0x0806C988` | the samples: a word each, the address of its start, end and loop |
| `0x08097174` | the waves |
| `0x02000EF0` | the channel records: 12 of 0x38 bytes |
| `0x02001190` | the queue: the next request's offset, the next free offset, and 4 halfwords |
| `0x0200119C`, `0x020011AC` | FIFO A's and FIFO B's samples: the next point, the end, the loop and the volume, a word each |
| `0x020011BC` | the driver's copy of SOUNDCNT_L |
| `0x020011D0` | the master level, the fade's step, and the fade out's frames, timer and period |

### Channels and commands

The driver plays 12 channels. Channels 0-7 are as above. Channels 8 and 9 play
the music's samples on FIFO A and FIFO B, and channels 10 and 11 play sound
effects' samples on FIFO A and FIFO B as well: a sound that starts channel 10
or 11 sets bit 1 of channel 8 or 9, which then leaves its FIFO alone, and the
sound effect's channel clears the bit again at its end. The record's `0x18`
and `0x1C` hold a sample channel's sample's end and loop.

There's one table of lengths, which `80`-`DF` read at byte `0x7F & b`. Each 16
bytes of it hold the lengths for a tempo, so a song changes tempo with the
high nibble of its length commands, and there's no `EE`.

The commands differ from those above:

| Command | Arguments | Effect |
|---|---|---|
| `E0`, `EA`, `EB`, `EE` | | nothing |
| `E1` | sample | the sample: its start, end and loop go to the record's `0x14`, `0x18` and `0x1C` |
| `E2`-`E9` | as `F2`-`F9` | the same as `F2`-`F9` |
| `EC` | | rest: stops the timer of FIFO A on an even channel, or FIFO B on an odd one, unless bit 1 of the channel's flags is set; ends the reading |
| `EF` | | the same as `FF` |

So there's no release, no sample pan, no mixer mode and nothing to bring the
sound back after a fade. The other commands work as above, but a PSG note
writes its frequency setting with bit 15 flipped, once, where the later
revision sets bit 15 and writes it twice, and a PSG envelope's step keeps the
new NRx1 and NRx2 in the record only while the channel has its PSG channel.

The queue holds 4 requests. After the channels and the fade, the per-frame
routine starts the next request whenever the queue's offsets differ, with no
check for sound 0 or for the noise channel.

### Samples

A sample note's key, with the transpose, gives an entry of the rate table: the
key times 12, plus the detune, which is signed here and moves the rate in 12ths
of a semitone. The entry is the reload value for the timer that plays the
FIFO: Timer 0 for FIFO A and Timer 1 for FIFO B. Key 0 plays the sample at
1024 Hz, and each 12 keys an octave higher, so key 48 plays it at 16384 Hz.

The note sets the wait, and the envelope's first step. A sample envelope's
step is three bytes: the frames until the next step, a byte whose high nibble
gives the frames between the level's changes and whose low nibble gives the
change, up or down by its low 3 bits, down with bit 3 set, and the level. With
0 frames between them, the level doesn't change. The channel's volume with the
envelope is `(volume + 1) × level / 256`, rounded down, as above.

Unless bit 1 of the channel's flags is set, the note then stops the FIFO's
timer, writes the sample's first 4 points into the FIFO as they are, sets the
FIFO's sample to the next point, the sample's end and loop and the channel's
volume with the envelope, sets the FIFO's interrupt in IE, sets the timer's
reload value, sets DMA 1's or DMA 2's control to `0xF600`, and starts the
timer. The driver gives the DMA no addresses: it's there for its interrupt,
which comes each time the FIFO asks for more points, when it holds 16 or fewer
after playing one.

The interrupt routine gives the FIFO the next 4 points of its sample, each
times `(volume + 1) / 256`. A positive point is rounded to the nearest step,
half up. A negative one comes out a step further from 0 unless the product is
a whole number of 256ths, and the second point of each pair after a negative
one comes out a step lower. When the sample reaches its end, the routine goes
on from its loop, or at the end of a sample that doesn't loop, it stops the
timer instead, without writing the last 4 points. The points left in the FIFO
play at the start of the next note on that FIFO.

An envelope's step sets the FIFO's volume to the channel's volume with the
envelope times the master level, divided by 256 and rounded down; a change in
level uses the master level plus 1. Neither touches the FIFO while bit 1 of the
channel's flags is set.

The end of a music sample channel stops its FIFO's timer, unless bit 1 of its
flags is set. The end of a sound effect's sample channel clears that bit on
channel 8 or 9 and stops the timer, so the music's sample stays stopped until
its next note.

### Fades and quirks

Only the game starts a fade out. It lasts 8 times the speed in frames, and
steps the master level down by 1 every `speed × 8` 256ths of a frame, or for a
speed below 32, by `32 / speed`, rounded down, every frame. Each step sets NR50,
and the volume of the FIFO of each sample channel in use that has its FIFO. At
its end, the driver empties the FIFOs, turns the sound off and on, clears the
PSG channels, stops the timers, clears the channels' flags and sets the master
level back to 255, but leaves the queue.

The init writes SOUNDCNT_L before it turns the sound off and on, which clears
it, so the PSG plays on neither side until a channel's `FD` writes it from the
driver's copy. The init also writes `0x8000` to `0x0400007A`, an unused
register, where NR44 is at `0x0400007C`.

`EC` on a PSG channel stops a FIFO's timer: FIFO A's on an even channel and
FIFO B's on an odd one. `ED` on a sample channel reads past the table of NRx1
and NRx2 registers into that of the frequency registers, so it writes 0 to the
frequency register of square 1, square 2, the wave or the noise for channels 8
to 11, and writes `r5` to an address outside the I/O registers.

## Locating the driver

`agbsf2` finds the driver from its code, without fixed addresses. The
per-frame routine starts by counting down the channels' waits:

```
push {r4-r7}              ; E92D00F0, or push {r4-r8}, E92D01F0, in the Magical Vacation revision
mov  r7, #0               ; E3A07000
ldr  r1, =channels        ; E59F1xxx
ldrb r2, [r1]             ; E5D12000
tst  r2, #1               ; E3120001
beq  next channel         ; 0Axxxxxx
ldrb r2, [r1, #1]         ; E5D12001
sub  r2, r2, #1           ; E2422001
cmp  r2, #0               ; E3520000
beq  read                 ; 0Axxxxxx
```

and its loop over the channels within 0x1000 bytes of it has to count 13 of
them, or 12 in the Magical Vacation revision: `add r1, r1, #0x38; add r7, r7,
#1; teq r7, #13` (`E2811038 E2877001 E337000D`). The tables come from the
instructions that load their addresses, found by the instructions around them
within the same 0x1000 bytes:

| Table | Code (words; `x` is any digit) | Load |
|---|---|---|
| command table | `E200001F E59F3xxx E793F100` | the 2nd |
| song table | `E59F2xxx E7920101` | the 1st |
| length sets | `E59F3xxx E7D30000` | the 1st |
| frequency table | `E59F2xxx E1A00080 E19250B0` | the 1st |
| pans | `E59F3xxx E7F34001` | the 1st |
| PSG registers | `E59F2xxx E7922107 E1C230B0 E59F2xxx E7922107` | the 1st and the 4th |
| rate table, in the Magical Vacation revision | `E59F2xxx E1A00080 E19200B0` | the 1st |

The routines of commands `F0`, `E1` and `FC` load the envelopes, the sample
sets or samples, and the waves: `E4D20001 E59F3xxx`, then `E0833100`,
`E7930100` or `E7934100`, within 0x20 bytes of the routine's start. In the
*Sword of Mana* revision, the init sets Timer 0 from `ldr r1, =reload; ldr r0,
[r1]; strh r0, [r3, #0xA0]` (`E59F1xxx E5910000 E1C30AB0`) within 0x1000 bytes
before the per-frame routine. That setting gives the mixer's rate. Without it,
`agbsf2` assumes 16384 Hz.

The song table has no count. `agbsf2` counts its entries up to the
first whose list of channels isn't in the ROM, names a channel that the
revision doesn't have, or doesn't end within its channels. `--song-table` and `--song-count` override the
song table and the number of sounds. Without the driver's code, `agbsf2`
can't find the driver's other tables or read the game.
