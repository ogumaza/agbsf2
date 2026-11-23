# Brownie Brown's GBA sound driver

This document covers the sound format, sequencer, mixer and hardware control
in the Brownie Brown driver for *Sword of Mana* on the Game Boy Advance. It's
based on the code of the driver in the US release of *Sword of Mana* (game code
`AVSE`), and the addresses given as examples come from that game. *Magical
Vacation* (`AMVJ`) uses an older revision without the mixer. [The Magical
Vacation revision](#the-magical-vacation-revision) lists the differences.
`agbsf2` finds the driver and its tables from the code (see [Locating
the driver](#locating-the-driver)).

The driver plays 13 channels. Channels 0-3 play the music on the Game Boy's
square 1, square 2, wave and noise channels. Channels 4-7 play sound effects
on the same four channels and take each one from the music while they play.
Channels 8-12 play samples on the five voices of a software mixer. The mixer
mixes them into the two DirectSound FIFOs at 16384 Hz: FIFO A for the right
side and FIFO B for the left. A sample set contains a sample for each of the
12 semitones. The mixer plays the lower octaves by repeating each point.

The driver counts time in frames and has no tempo variable. Each song
selects a set of note lengths from a table, with each set corresponding to a
tempo.

## Architecture

The driver is ARM code. The game calls the init of the driver once, copies the
mixer to IWRAM, and calls the per-frame routine from the VBlank handler. The
mixer runs from the interrupts of DMA 1 and DMA 2. These DMA channels feed the
FIFOs.

In `AVSE` the routines are:

| Address | Routine |
|---|---|
| `0x080A1834` | init: resets the sound, the channels, the queue and the voices, and sets up the DMA of the FIFOs and Timer 0 |
| `0x080A16EC` | queue a sound: `r0` = its number |
| `0x080A170C` | bring the sound back after a fade out: fades in at speed `r0` and queues sound `0x36` |
| `0x080A17C4` | fade out at speed `r0`: stops everything at the end of the fade |
| `0x080A1900` | the per-frame routine |
| `0x080A1EB8` | the data reader, part of the per-frame routine, with the command table for commands `E0` to `FF` at `0x080A2174` |
| `0x03003150` | the mixer, copied from `0x080A0B74`: the routine of both DMA interrupts, mixes voices 0 and 1 |
| `0x03003750` | the part of the mixer for all five voices, copied from `0x080A1040` |

The game reaches them through Thumb routines at `0x080A153C` to `0x080A15E2`,
and veneers at `0x0836DF08` to `0x0836DF38`.

The tables of the driver in ROM and in RAM contain:

| Address | Contents |
|---|---|
| `0x080A15E4` | the frequency table: a halfword for each key, the frequency setting of the PSG |
| `0x080A168C` | the PSG registers that NRx1 and NRx2 of each channel go to: 8 words, the channels 4-7 the same as 0-3 |
| `0x080A16AC` | the PSG registers that the frequency setting of each channel goes to: 8 words |
| `0x080A16CC` | the pans: the bits of NR51 for each channel, 12 bytes, then the bits that each one clears, 12 bytes |
| `0x080A16E4` | the reload value of Timer 0: `0xFC00`, for 16384 points a second |
| `0x080A2800` | the length sets: 16 bytes each |
| `0x080A2980` | the envelopes: a word each, the address of its steps |
| `0x080A2FFC` | the song table: a word for each sound, the address of its list of channels |
| `0x080C2EE8` | the sample sets: a word each, the address of its 12 entries |
| `0x083678A0` | the waves: a word each, the address of its 16 bytes |
| `0x03002ACC` | the channel records: 14 of 0x38 bytes |
| `0x03002DDC` | the queue: the offset of the next request, the next free offset, and 8 halfwords |
| `0x03002DEE` | the mode of the mixer |
| `0x03002DF0` | the master level, the step of the fade, and the counts of the fades (see [Fades](#fades)) |
| `0x03002DFC` | the copy of SOUNDCNT_L that the driver keeps |
| `0x03002E0C` | the voices: 5 of 16 bytes |
| `0x03002E6C`, `0x03002E7C` | the 16 points that the mixer makes for FIFO A, and for FIFO B |

## Sounds

The song table lists every sound the game plays, music and sound effects
alike. The list of channels of a sound has three words for each channel: the
number of the channel, the address of its data, and the address of its table
of subroutines. `F6` and `F8` take places from that table. The word `0xFF`
ends the list. The music usually plays on channels 0-3 and 8-12, and the sound
effects on channels 4-7. A sound can name any of the channels, though.

The game queues a sound by its number. The queue has room for 8 requests.
After the channels, each frame starts the next request. For each channel that
the sound lists, the driver sets the in-use bit of the channel, its data and
its subroutines, and clears the bytes from offset `0x24` to `0x2F`. It sets
the wait to 1, for a read of the data in the next frame, and sets the pan from
the pan table. A channel of a sound effect sets bit 1 of the music channel
with the same PSG channel. That music channel then stops writing the registers
of the PSG. The driver doesn't clear the rest of a channel record.

A request of 0 is no request. Sound 0 therefore can't play. The driver drops a
request for sound `0x3B` or `0x3D` if NR42 contains a volume, or if channel 3
is in use with fewer than 3 frames left to wait.

## Channel records

| Offset | Size | Contents |
|---|---|---|
| `0x00` | 1 | flags: bit 0 in use, bit 1 a sound effect has the PSG channel, bit 7 in use when a fade out stopped everything |
| `0x01` | 1 | the frames to wait until the next read of the data |
| `0x02` | 1 | the frames to the next step of the envelope, or 0 for none |
| `0x03` | 1 | the count of `F4` |
| `0x04` | 4 | the next byte of the data |
| `0x08` | 4 | where `F5` goes back to |
| `0x0C` | 4 | where `F7` returns to |
| `0x10` | 4 | the subroutines |
| `0x14` | 4 | the sample set |
| `0x1C` | 4 | the wave |
| `0x20` | 2 | the frequency setting, or NR43 on the noise channel |
| `0x22` | 2 | NRx1 in the low byte and NRx2 in the high, as last set |
| `0x24` | 1 | NRx1, or on a sample channel the frames to the next step of the level |
| `0x25` | 1 | NR10, or on a sample channel the volume with the level of the envelope |
| `0x26` | 1 | the volume |
| `0x27` | 1 | the detune |
| `0x28` | 1 | the level of the envelope |
| `0x29` | 1 | the frames each note or rest lasts |
| `0x2A`, `0x2B` | 1 each | on a PSG channel, the bits of NR51 and the bits that it clears; on a sample channel, the right and left levels |
| `0x2C` | 1 | the transpose, signed |
| `0x2D` | 1 | on a sample channel, the change in level, down with bit 7 set |
| `0x2E` | 1 | the frames between those changes |
| `0x2F` | 1 | the length set |
| `0x30` | 4 | the envelope that each note starts |
| `0x34` | 4 | the next step of the envelope |

## The per-frame routine

1. For each channel in use, in order from 0 to 12, the driver counts down the
   wait. When the wait runs out, it reads the data of the channel (see
   [Data](#data)). Otherwise it counts down the frames to the next step of the
   envelope and takes the step when they run out. On a sample channel that
   doesn't take a step, it counts down the frames to the next change of the
   level instead, and changes the level when they run out (see
   [Envelopes](#envelopes)).
2. It takes the next step of a fade in, or else of a fade out (see
   [Fades](#fades)).
3. It starts the next request in the queue (see [Sounds](#sounds)).

A wait of 0 counts down to 255.

## Data

The data of a channel is a string of bytes:

* `00`-`7F` is a note of that key. The channel then waits for the length.
* `80`-`DF` sets the length: byte `0x7F & b` of the length set of the channel,
  16 bytes on for each set. The wait counts down in a byte. A length of 0
  therefore waits 256 frames.
* `E0`-`FF` is a command.

The driver reads the data up to a note, a rest, a hold, a release or the end.
Each of these ends the reading for the frame.

### Notes

The key of a note is the byte plus the transpose, kept to 7 bits.

On channels 0-2 and 4-6, the frequency setting is the entry of the frequency
table for the key, plus the detune. On the noise channels, 3 and 7, the byte
itself, without the transpose, sets NR43, with the nibbles swapped. The note
sets the wait of the channel to the length and starts the envelope. The first
step of the envelope sets the frames to the next step and the level. The
volume of the channel is taken away from that level. A level below the volume
results in an NRx2 of 8. That setting is silent. Unless a sound effect has the PSG
channel, the driver writes NR10 from offset `0x25` of the record on channels 0
and 4, then NRx1 and NRx2. It then writes the frequency setting with bit 15
twice. Bit 15 starts the channel.

On a sample channel, keys from 64 play key 47. The octave of the key selects
the rate: keys 0-11 play at an eighth of the mixer rate, 12-23 at a quarter,
24-35 at half and the rest at the full rate. The key less 12 for each octave
selects the entry of the sample set. Keys 48-63 therefore read past the 12
entries. An entry is three words: the start of the sample, the end and the
loop. The note copies them to the voice of the channel with the mode for the
rate. The note starts the envelope and sets the levels of the voice (see
[Levels](#levels)). It also starts Timer 0, and the DMA of the FIFOs if that
isn't running.

### Commands

| Command | Arguments | Effect |
|---|---|---|
| `E0` | envelope | release: the envelope goes on from the first step of this envelope at the next frame; ends the reading |
| `E1` | set | the sample set |
| `E2` | mode | the mode of the mixer: 0 mixes voices 0 and 1 only, and clears the flags of channels 10-12 and of the 14th record; anything else mixes all five |
| `E3` | | brings the sound back after a fade out (see [Fades](#fades)); ends the per-frame routine |
| `E4`-`E7`, `EB`, `EF` | | nothing |
| `E8` | | a sound effect returns the PSG channel to the music: clears bit 1 of the music channel 4 below |
| `E9` | | a sound effect takes the PSG channel from the music |
| `EA` | pan | the pan of a sample channel: the level of the right side from the low nibble and of the left side from the high nibble, each nibble *n* as `n × 16 + 15`, or 0 |
| `EC` | | the rest of a sample channel: the voice with the number of the channel & 7 falls silent; ends the reading |
| `ED` | | the rest of a PSG channel: NRx1 and NRx2 are written as 0, and the frequency register as described in [Quirks](#quirks); ends the reading |
| `EE` | set | the length set |
| `F0` | envelope | the envelope that each note starts |
| `F1` | NRx1 | NRx1: the duty and the length |
| `F2` | volume | the volume: a PSG channel takes it from each level, and a sample channel multiplies by it |
| `F3` | transpose | the transpose, signed |
| `F4` | count | repeat from here |
| `F5` | | go back to the place of `F4` until the count runs out; a count of 0 makes 256 passes |
| `F6` | subroutine | call a subroutine; the calls don't nest |
| `F7` | | return |
| `F8` | subroutine | jump to the place of a subroutine: a loop when the channel has played there before |
| `F9` | detune | the detune, added to the frequency setting of each note |
| `FA` | NR10 | the sweep that each note on channel 0 or 4 writes |
| `FB` | | no sweep |
| `FC` | wave | load a wave into the wave RAM: the driver writes NR30 `0x40`, the 16 bytes, and NR30 `0x80` |
| `FD` | pan | the pan of a PSG channel: bit 0 of the low nibble for the right side and bit 0 of the high nibble for the left; the driver shifts them to the bits of the channel and writes them into NR51 |
| `FE` | | hold: the note goes on for another length; ends the reading |
| `FF` | | end (see below) |

`FF` turns the channel off. A music PSG channel writes 0 to NRx2 and `0x8000`
to the frequency register, unless a sound effect has the PSG channel. A sound
effect returns the PSG channel. If the music channel plays, the driver loads
the wave of the music channel again on the wave channel and writes the pan of
the music channel into NR51. It then writes NRx1, NRx2 and the frequency
setting of the music channel with bit 15 flipped, and NR10 on channel 4. Bit
15 starts the channel again. If the music channel doesn't play, the driver
turns the PSG channel off. On a sample channel, the voice falls silent. If the
low bytes of the next points of voices 0 and 1 are both 0, the driver also
stops Timer 0 and the DMA of the FIFOs.

## Envelopes

An envelope is a list of steps. A PSG step is two bytes: the frames until the
next step, and the value of NRx2 before the volume is taken away. Each step
writes NRx1, NRx2 and the frequency setting with bit 15. Bit 15 starts the
channel again. The hardware envelope therefore starts again from the volume of
the step. A step of 0 frames is the last.

A sample step is three bytes: the frames until the next step, a change in level
and a level. The step sets the level. The change then goes on each frame until
the next step: up or down by the low 7 bits, down with bit 7 set, kept from 0
to 255.

### Levels

The volume of a sample channel with the envelope is
`(volume + 1) × level / 256`, rounded down. The record keeps it at offset
`0x25`. A note sets the sides of the voice to

    right = (right pan + 1) × volume × (master + 1) / 65536
    left  = (left pan + 1) × volume × (master + 1) / 65536

and a step of the envelope or a change in level sets them to

    scaled = volume × (master + 1) / 256
    right  = (right pan + 1) × scaled / 256

Each value is rounded down. The two forms can therefore differ by a step.

## Fades

The master level, 0-255, scales the voices of the sample channels. NR50 gets
the top 3 bits of it on each side. A fade changes the level by the step of the
fade each time the timer of the fade runs out. The timer counts in 256ths of a
frame. Each change sets NR50 and the voices of the sample channels in use.

A fade out stops everything at the end of the fade. The master level drops to
0, and the driver turns the sound off and on (NR52) and stops Timer 0. It
writes SOUNDCNT_H and SOUNDCNT_L as the init does, and sets bit 7 of each
channel in use in place of bit 0. It clears the offsets of the queue and the
first four requests with the new flags of the last record. It then sets the
master level to 255.

`E3`, or the routine of the game at `0x080A170C`, brings the sound back:

* The master level drops to 0 and fades in over 64 frames, 4 a frame.
* SOUNDCNT_L is cleared until the next step of the fade writes it from the
  copy that the driver keeps.
* The channels with bit 0 or bit 7 set play again.
* Timer 0 starts.
* Sound `0x36` goes into the queue.

`E3` ends the per-frame routine there. The later channels, the fades and the
queue therefore wait until the next frame. The last frame of the fade in sets
NR50 to `0x77` and leaves the master level where the fade left it.

## The mixer

Each time the FIFOs have played 16 points, the DMA interrupts run the mixer.
The mixer makes the next 16 points of each FIFO. In mode 0 it mixes voices 0
and 1. In any other mode it mixes all five through the part at `0x03003750`.

A voice is 16 bytes: the mode, a copy of it, the right and left levels, and the
next point, the end of the sample and the loop. For each voice that it mixes,
the mixer first checks whether the next point is the end. If it is, the voice
goes back to the loop. If the loop is the end too, the voice is silent and
stays where it is. The mixer then takes the next points of the voice:

* with mode bit 0 set, 8 points, each played twice
* with bit 1, 4 points, each played four times
* with bit 2, 2 points, each played eight times
* otherwise 16 points

The part for five voices interpolates between the points that it plays twice.
The point of a FIFO is the sum of the point of each voice times the level of
the voice on that side, divided by 256 and rounded.

The end has to fall on a whole number of steps of the voice. The mixer checks
for the end only before each run.

## Quirks

The tables of PSG registers in the driver have entries for channels 0-7 only.
`ED` on a sample channel reads past them. Past the NRx1 table it finds the
table of frequency registers. It therefore writes 0 to the frequency register
of PSG channel *c* − 8, or of square 1 for channel 12 (NR23 and NR24 for
channel 9, NR43 and NR44 for channel 11). Past that table it finds the pans,
and it writes `r5` of the routine to an address outside the I/O registers.
Writing 0 to a frequency register doesn't start the PSG channel. It sets the
frequency setting, or the clock of the noise, to 0 until the next note or
envelope step of that channel writes it again. `ED` doesn't touch the voice of
the sample channel, and the voice goes on playing. Only the steps of the
envelope stop.

On a PSG channel, `ED` writes `r5` with bit 15 flipped to the frequency
register. `r5` contains what the routine last left there, such as the
frequency setting of a note of an earlier channel in the same frame, or what
the code of the game left there before the routine. The DAC of the channel is
off by then. The write therefore does nothing that can be heard.

`EC` on a PSG channel silences the voice with the number of the channel & 7,
and leaves the channel playing.

The test that stops the FIFOs at the end of a sample channel looks only at the
low byte of the next points of voices 0 and 1. It can therefore stop them
while voice 0 or 1 plays from a point whose low byte is 0, or while voices 2-4
play. The next sample note starts them again, and the voices play on from
where they stopped.

`E3` queues sound `0x36` with the offset of the queue kept to 8 bytes. The
queue routine keeps it to 16. The end of the fade in also writes `0xFF` to
address 0. That write does nothing.

## The Magical Vacation revision

*Magical Vacation* has an older revision of the driver. The per-frame routine,
the data and most of the commands work as described above, apart from what
follows. This revision has no mixer. Each of the two sample channels plays one
sample at a time straight through a FIFO, at a rate that a timer sets for each
note.

In `AMVJ` the routines are ARM code that runs from the ROM. The game reaches
them through Thumb routines at `0x0805B564` to `0x0805B5B2`, and veneers at
`0x080C2044` to `0x080C2070`:

| Address | Routine |
|---|---|
| `0x0805BF3C` | init |
| `0x0805BEAC` | queue a sound: `r0` = its number |
| `0x0805BECC` | fade out at speed `r0`: stops everything at the end of the fade |
| `0x0805C2C8` | the per-frame routine, with the command table at `0x0805C994` |
| `0x0805BFD8` | the DMA 1 interrupt: feeds FIFO A |
| `0x0805C150` | the DMA 2 interrupt: feeds FIFO B |

The tables of the driver in ROM and in RAM contain:

| Address | Contents |
|---|---|
| `0x0805B5B4` | the frequency table |
| `0x0805B65C`, `0x0805B67C` | the PSG registers that NRx1 and NRx2 of each channel go to, and those that the frequency setting goes to: 8 words each |
| `0x0805B69C` | the pans: the bits of NR51 for each channel, 12 bytes, then the bits that each one clears, 12 bytes |
| `0x0805B6B4` | the rate table: a halfword for each 12th of a semitone, the reload value of a timer |
| `0x0805CE80` | the lengths |
| `0x0805CEE0` | the envelopes |
| `0x0805D2DC` | the song table |
| `0x0806C988` | the samples: a word each, the address of the start, end and loop of the sample |
| `0x08097174` | the waves |
| `0x02000EF0` | the channel records: 12 of 0x38 bytes |
| `0x02001190` | the queue: the offset of the next request, the next free offset, and 4 halfwords |
| `0x0200119C`, `0x020011AC` | the samples of FIFO A and of FIFO B: the next point, the end, the loop and the volume, a word each |
| `0x020011BC` | the copy of SOUNDCNT_L that the driver keeps |
| `0x020011D0` | the master level, the step of the fade, and the frames, timer and period of the fade out |

### Channels and commands

The driver plays 12 channels. Channels 0-7 are as above. Channels 8 and 9 play
the samples of the music on FIFO A and FIFO B. Channels 10 and 11 play the
samples of sound effects on FIFO A and FIFO B as well. A sound that starts
channel 10 or 11 sets bit 1 of channel 8 or 9. That channel then leaves the
FIFO alone. The channel of the sound effect clears the bit again at the end.
On a sample channel, offsets `0x18` and `0x1C` of the record contain the end
and the loop of the sample.

There's one table of lengths. `80`-`DF` read it at byte `0x7F & b`. Each 16
bytes of the table contain the lengths for a tempo. A song therefore changes
tempo with the high nibble of the length commands. There's no `EE`.

The commands differ from those above:

| Command | Arguments | Effect |
|---|---|---|
| `E0`, `EA`, `EB`, `EE` | | nothing |
| `E1` | sample | the sample: the start, end and loop go to offsets `0x14`, `0x18` and `0x1C` of the record |
| `E2`-`E9` | as `F2`-`F9` | the same as `F2`-`F9` |
| `EC` | | rest: stops the timer of FIFO A on an even channel, or FIFO B on an odd one, unless bit 1 of the flags of the channel is set; ends the reading |
| `EF` | | the same as `FF` |

This revision therefore has no release, no sample pan, no mixer mode and
nothing to bring the sound back after a fade. The other commands work as
above, with two differences. A PSG note writes the frequency setting with bit
15 flipped, once. The later revision sets bit 15 and writes the setting twice.
A step of a PSG envelope keeps the new NRx1 and NRx2 in the record only while
the channel has the PSG channel.

The queue has room for 4 requests. After the channels and the fade, the
per-frame routine starts the next request whenever the offsets of the queue
differ. It doesn't check for sound 0 or for the noise channel.

### Samples

The key of a sample note, with the transpose, selects an entry of the rate
table: the key times 12, plus the detune. The detune is signed here and moves
the rate in 12ths of a semitone. The entry is the reload value for the timer
that plays the FIFO: Timer 0 for FIFO A and Timer 1 for FIFO B. Key 0 plays
the sample at 1024 Hz, and each 12 keys play an octave higher. Key 48
therefore plays it at 16384 Hz.

The note sets the wait and the first step of the envelope. A step of a sample
envelope is three bytes:

1. the frames until the next step
2. a byte with the frames between the changes of the level in the high nibble
   and the change in the low nibble: up or down by the low 3 bits, down with
   bit 3 set
3. the level

With 0 frames between the changes, the level doesn't change. The volume of the
channel with the envelope is `(volume + 1) × level / 256`, rounded down, as
above.

Unless bit 1 of the flags of the channel is set, the note then does the
following:

* stops the timer of the FIFO
* writes the first 4 points of the sample into the FIFO as they are
* sets the sample of the FIFO to the next point, the end and the loop of the
  sample and the volume of the channel with the envelope
* sets the interrupt of the FIFO in IE
* sets the reload value of the timer
* sets the control of DMA 1 or DMA 2 to `0xF600`
* starts the timer

The driver sets no addresses for the DMA. The DMA serves only for the
interrupt. The interrupt comes each time the FIFO asks for more points: when
the FIFO contains 16 or fewer after playing one.

The interrupt routine writes the next 4 points of the sample into the FIFO,
each times `(volume + 1) / 256`. A positive point is rounded to the nearest
step, half up. A negative one comes out a step further from 0 unless the
product is a whole number of 256ths. The second point of each pair after a
negative one comes out a step lower. When the sample reaches the end, the
routine goes on from the loop. At the end of a sample that doesn't loop, the
routine stops the timer instead, without writing the last 4 points. The points
left in the FIFO play at the start of the next note on that FIFO.

A step of an envelope sets the volume of the FIFO to the volume of the channel
with the envelope times the master level, divided by 256 and rounded down. A
change in level uses the master level plus 1. Neither touches the FIFO while
bit 1 of the flags of the channel is set.

The end of a music sample channel stops the timer of the FIFO, unless bit 1 of
the flags is set. The end of a sample channel of a sound effect clears that
bit on channel 8 or 9 and stops the timer. The sample of the music therefore
stays stopped until the next note.

### Fades and quirks

Only the game starts a fade out. The fade lasts 8 times the speed in frames.
It steps the master level down by 1 every `speed × 8` 256ths of a frame. For a
speed below 32, it steps down by `32 / speed`, rounded down, every frame. Each
step sets NR50, and the volume of the FIFO of each sample channel in use that
has the FIFO. At the end of the fade, the driver empties the FIFOs, turns the
sound off and on, clears the PSG channels and stops the timers. It clears the
flags of the channels and sets the master level back to 255, but leaves the
queue.

The init writes SOUNDCNT_L before it turns the sound off and on. Turning the
sound off clears SOUNDCNT_L. The PSG therefore plays on neither side until
`FD` on a channel writes the register from the copy that the driver keeps. The
init also writes `0x8000` to `0x0400007A`, an unused register. NR44 is at
`0x0400007C`.

`EC` on a PSG channel stops the timer of a FIFO: of FIFO A on an even channel
and of FIFO B on an odd one. `ED` on a sample channel reads past the table of
NRx1 and NRx2 registers into the table of frequency registers. It therefore
writes 0 to the frequency register of square 1, square 2, the wave or the
noise for channels 8 to 11. It also writes `r5` to an address outside the I/O
registers.

## Locating the driver

`agbsf2` finds the driver from its code, without fixed addresses. The
per-frame routine starts by counting down the waits of the channels:

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

The loop of the routine over the channels, within 0x1000 bytes of the start,
has to count 13 of them, or 12 in the Magical Vacation revision: `add r1, r1,
#0x38; add r7, r7, #1; teq r7, #13` (`E2811038 E2877001 E337000D`). The tables
come from the instructions that load their addresses. Those instructions are
found by the instructions around them within the same 0x1000 bytes:

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
`E7930100` or `E7934100`, within 0x20 bytes of the start of the routine. In
the *Sword of Mana* revision, the init sets Timer 0 from `ldr r1, =reload; ldr
r0, [r1]; strh r0, [r3, #0xA0]` (`E59F1xxx E5910000 E1C30AB0`) within 0x1000
bytes before the per-frame routine. That setting sets the rate of the mixer.
Without it, `agbsf2` assumes 16384 Hz.

The song table has no count. `agbsf2` counts the entries up to the first one
whose list of channels isn't in the ROM, names a channel that the revision
doesn't have, or doesn't end within the channels. `--song-table` and
`--song-count` override the song table and the number of sounds. Without the
code of the driver, `agbsf2` can't find the other tables of the driver or read
the game.
