# Quintet's GBA sound driver

This document covers the song format, sequencer and sound hardware control
in Quintet's driver for Banpresto's *Super Robot Taisen* games on the Game
Boy Advance. It's based on the driver's code in *Super Robot Taisen A* (game
code `ASRJ`), and the addresses given as examples come from that game. Other
games put the driver and its data elsewhere. `agbsf2` finds them
from the code (see [Locating the driver](#locating-the-driver)).

The driver plays six channels, one for each of the GBA's sound channels: the
Game Boy's two square channels, its wave channel and its noise channel, and
the two DirectSound FIFOs, each of which plays one sample at a time at a rate
set by a timer. It doesn't mix samples in software.

## Games and revisions

| Game | Revision |
|---|---|
| *Super Robot Taisen A* (`ASRJ`) | A |
| *Super Robot Taisen D* (`A6SJ`) | A |
| *Super Robot Taisen R* (`AJ9J`) | A |
| *Super Robot Taisen J* (`B6JJ`) | J |

Most of this document holds for both revisions.
[The J revision](#the-j-revision) lists the differences.

## Architecture

The driver is Thumb code in ROM. The game calls its per-frame routine once a
frame, and the routine runs these steps:

1. For each of the six music channels in turn, it counts down the frames to the
   channel's next note. When the count runs out, it reads the channel's commands
   up to its next note, and writes the channel's settings into a copy of the
   sound registers called the music's register image. Then it runs the
   channel's effects, which also write into the image.
2. It does the same for the six sound effect channels, which have an image of
   their own.
3. It writes the images to the sound hardware (see [Output](#output)).
4. It moves each FIFO's sample on by a frame, and stops it or starts it again
   from its loop when it reaches the end (see [PCM](#pcm)).

In `ASRJ` the routines are:

| Address | Routine |
|---|---|
| `0x080034F0` | the game's sound init: calls the driver's init and the FIFO setup, and gives the driver the game's files |
| `0x08003550` | the game's play-song routine: looks up the music file, skips to song `r0`, and calls the driver's play routine |
| `0x08003594` | the game's routine for sound effects: looks up the sound effects' file, skips to sound effect `r0`, and calls the driver's routine for sound effects |
| `0x08003544` | the game's per-frame sound routine, which calls the driver's |
| `0x0805CF44` | init: resets the channels and the PSG |
| `0x0805D1FC` | play a song: `r0` = the address of its header |
| `0x0805D254` | play a sound effect: `r0` = the address of its header |
| `0x0805D184` | turn sound effect channel `r0`'s outputs on in the sound effects' image |
| `0x0805D298` | set the wave table |
| `0x0805D2A4` | set the music's noise macros |
| `0x0805D2B0` | set the sound effects' noise macros |
| `0x0805D2BC`, `0x0805D2C8` | load the music's and the sound effects' samples, through the loader at `0x0805D4BC` |
| `0x0805D30C` | set the PSG's master volume: `r0` = left, `r1` = right, each 0-7 |
| `0x0805D788` | the note reader; its command table, for commands `0E` to `F1`, is at `0x0805DA60` |
| `0x0805D408`, `0x0805D47C` | the frequency setting and the PCM pitch of a note |
| `0x0805D524` | load a wave into the wave RAM |
| `0x0805D5D0`, `0x0805D638` | select a noise macro, select a sample |
| `0x0805D6CC` | start a PCM note |
| `0x0805EC84`, `0x0805ECF4` | a PSG channel's effects, a PCM channel's effects |
| `0x0805E588`, `0x0805E700`, `0x0805E820`, `0x0805EA24` | the volume envelope, the pitch envelope, the LFO and the slide |
| `0x0805EAE4`, `0x0805EBE4` | the noise macro, the duty or wave bank switch |
| `0x0805ED98` | the per-frame routine |
| `0x0805EDB0`, `0x0805F13C` | its steps for the music channels and for the sound effect channels |
| `0x0805F4FC` | the output stage |
| `0x0805F890` | the FIFO setup |
| `0x0805FC5C` | play a sample: stops the FIFO, sets its rate and range, and starts it |
| `0x0805FA28`, `0x0805FCB0`, `0x0805FB54` | start a FIFO, stop it, set a new rate |
| `0x0805FD0C` | the FIFO update |
| `0x0805D3D8` | the random number generator |

Its tables in ROM and its RAM hold:

| Address | Contents |
|---|---|
| `0x087F0F60` | the music channels: 6 words, the address of each channel's record |
| `0x087F0F78` | the sound effect channels |
| `0x087F0F90` | the length table: 13 bytes |
| `0x087F0F9D` | the LFO's sine: 256 signed bytes |
| `0x087F109E` | the frequency table: 84 halfwords |
| `0x087F1146` | the PCM pitch table: 53 halfwords |
| `0x03000784` | the address of the channel being read |
| `0x03000788` | its number, 0-5 |
| `0x0300078C` | the address of the song playing |
| `0x03000790` | the address of the wave table |
| `0x03000794`, `0x03000798` | the addresses of the music's and the sound effects' noise macros |
| `0x030007A8` | the sample tables: 128 words for the music's samples, then 128 for the sound effects' |
| `0x0300079E` | the sample loaded into each FIFO: 2 bytes |
| `0x030007A1`, `0x030007A2` | the master volume, left and right |
| `0x03000030` | the music's register image |
| `0x03000048` | the sound effects' register image |
| `0x030007A0` | 1 while the per-frame routine reads the music channels, 0 while it reads the sound effect channels |
| `0x03000BB0` | the FIFO records: 2 of 0x24 bytes |
| `0x03000BA8` | the random number generator's seed |

With no sound effect playing, the output stage writes the music's image.
[Sound effects](#sound-effects) covers how a sound effect plays.

## Songs

The game keeps its songs in a music file, one song after another. A song
starts with a header of 7 halfwords: its length in bytes, and then the offset
of each channel's data from the start of the song. The length's bit 0 is
ignored, and the next song starts that many bytes on. The game's play-song
routine finds song *n* by skipping *n* songs this way, and passes its address
to the driver. Nothing in the driver gives the number of songs. `agbsf2`
counts them up to the first whose header is invalid: a length of less than
14, or a channel that starts outside the song.

The play routine resets the music and sound effect channels and the PSG, sets
the master volume to 7 on each side, and points each music channel at its data.
Each channel starts with octave 2, tempo 90, volume 0, duty 2 (50%) and a
downward sweep.

## Channels

| Channel | Plays on |
|---|---|
| 0 | square 1, the only one with a frequency sweep |
| 1 | square 2 |
| 2 | the wave channel |
| 3 | the noise channel |
| 4 | DirectSound FIFO A, through DMA 1 and timer 0 |
| 5 | DirectSound FIFO B, through DMA 2 and timer 1 |

Each channel has a record of 0x98 bytes. The fields that the commands set are
given below with the commands.

## Notes

A byte whose low nibble is 0-13 and whose high nibble is 0-12 is a note:

* The low nibble is the pitch, C to B as 0-11, or 12 to play again or 13 for a
  rest.
* The high nibble indexes the length table: 1, 2, 3, 4, 6, 8, 12, 16, 24, 32,
  48, 64 and 128. The note lasts 384 ticks divided by that value, so a quarter
  note is 96 ticks.

The note's pitch is octave × 12 + pitch + the two transposes, kept within the
frequency table (0-83) or the PCM pitch table (0-52).

After a note, `D1` makes it half as long again, and `D4` followed by a note of
the same pitch ties that note's length to it, with its own `D1` if it has
one. `D4` before a note of another pitch makes that note slide from this one
(see [Pitch](#pitch)).

A note sets the channel's level to its volume, and on a PCM channel starts the
sample (see [PCM](#pcm)). Playing again does the same without a new pitch, and
on a PCM channel plays the sample at its own rate plus the detune. A rest sets
the level to 0 and stops a PCM channel's FIFO.

A note or a play again starts the channel's effects over: the noise macro, the
volume and pitch envelopes, the LFO and the duty switch. A rest stops them.

## Commands

The other bytes are commands, some with arguments. Values are bytes unless the
table says otherwise.

| Command | Arguments | Effect |
|---|---|---|
| `0E` | shift | the sweep's shift (NR10 bits 0-2) |
| `0F`, `1E` | | the sweep goes up, down |
| `1F` | time | the sweep's time (NR10 bits 4-6) |
| `2E` | length | the sound length, with the length counter on; `63` turns it off |
| `2F` | duty | the square duty (0-3), and cancels a duty switch |
| `3E` | step | the hardware envelope's step time |
| `3F`, `4E` | | the hardware envelope goes up, down |
| `4F` | octave | the octave |
| `5E` | volume | the volume |
| `5F`, `6E` | | volume up, down; up stops at 15, or 3 on the wave channel and 1 on a PCM channel |
| `6F`, `7E` | | octave up, to at most 6; down, to at least 0 |
| `7F` | tempo | the tempo (see [Timing](#timing)) |
| `8E` | shift | the noise channel's clock shift (NR43 bits 4-7) |
| `8F` | width | its 7-step mode (NR43 bit 3) |
| `9E` | ratio | its clock divider (NR43 bits 0-2) |
| `9F` | 0 or 1 | the wave channel's 64-step mode (NR30 bit 5) |
| `AE` | bank | the wave bank it plays (NR30 bit 6) |
| `AF` | 0 or 1 | the wave channel on (NR30 bit 7) |
| `BE` | 0 or 1 | the wave channel at 75% (NR32 bit 15) |
| `BF` | | the loop point |
| `CE`, `CF` | bits | the PSG's master volume: the left side's NR50 bits 4-6, the right side's bits 0-2 |
| `D0` | keep, set | the channel outputs on each side: NR51 for the PSG channels, SOUNDCNT_H's high byte for the FIFOs. The bits of `keep` stay, and those of `set` are set. |
| `D2` | start, peak, end, attack, decay | the volume envelope (see [Volume](#volume)) |
| `D3` | | volume envelope off |
| `D5` | detune | a signed detune, × 10 on a PCM channel |
| `D6` | | repeat from here |
| `D7` | count | go back to the matching `D6` until the passes add up to `count`. A count of 0 makes 256 passes. Repeats nest up to 8 deep. |
| `DA` | wave | load wave `wave & 7F` from the wave table into the wave RAM: into bank 0, or bank 1 if bit 7 is set |
| `DB` | macro | the noise macro that each note starts, or 0 for none |
| `DC`, `DD` | sample | load sample `sample` into the channel's FIFO, and clear the range set by `E7` |
| `DE` | frames, first, then | a switch: the duty (or on the wave channel, the wave bank) is `first` for `frames` frames of each note, and then `then` |
| `DF` | delay, period, depth | the LFO, on (see [Pitch](#pitch)) |
| `E0` | | LFO off |
| `E1`, `EB` | | no switch |
| `E2` | start, middle, end, first, second | the pitch envelope (see [Pitch](#pitch)) |
| `E3` | | pitch envelope off |
| `E4` | semitones | the transpose, signed |
| `E5` | frames | how long a slide takes |
| `E6` | 2 bytes | a 16-bit signed detune, little-endian |
| `E7` | start, end, loop | the part of a sample a PCM note plays, in % of its length (see [PCM](#pcm)) |
| `E8` | resolution | SOUNDBIAS bits 14-15, written at once |
| `E9` | semitones | a second transpose |
| `EA` | frames | a wave bank switch: bank 0 for `frames` frames of each note, then bank 1 |
| `EC` | shape | the LFO's shape |
| `ED` | bits | the LFO's shape becomes its own bits 0-6 ORed with `bits` |
| `EE`, `EF` | | transpose up, down a semitone |
| `F1` | amount | adds a signed amount to the volume, keeping it from 0 to 15, 4 on the wave channel and 1 on a PCM channel |
| `FF` | | the end of the channel, or back to the loop point |

`D1` outside a note, `D8`, `D9`, `F0` and `F2`-`FE` do nothing, apart from
`F0` in the J revision.

`FF` sends the channel back to its loop point if it has one. It resets the
channel's counts (see [Timing](#timing)), but the octave, the volume and every
other setting stay as the end of the loop leaves them, so a loop that relies
on a setting from before it plays its first pass with the one the intro left.
If the loop point is the `FF` itself, the channel stops reading until the song
is changed. A channel without a loop point ends: its level goes to 0 and a PCM
channel's FIFO stops. Each channel loops on its own, so channels with loops of
different lengths drift apart.

## Timing

Each channel has its own tempo, which `7F` sets. A `7F` on channel 0 sets
every channel's. A tick lasts 37 / tempo frames, so a quarter note of 96 ticks
lasts 96 × 37 / *T* frames at tempo *T*, and the tempo is close to the beats per
minute: 90 gives 90.8.

The driver times each channel by counting the ticks it has played since the
count was last reset, and the frames those ticks take, which it works out as
ticks × 37 / tempo, rounded down. When a note is read, its length goes into the
tick count, and the channel waits for the difference between the new frame
count and the old one. So each note plays at the start of the frame that its
tick falls in. A wait of 0 frames becomes 1, and the frame count goes up by 1
with it, so a note always lasts at least a frame.

A loop resets the channel's counts, and so does a `7F`: on channel 0, every
channel's. The driver then counts from the frame it's in, and drops the
fraction of a frame that it had counted. A channel whose loop isn't a whole
number of frames long plays each pass slightly early, compared with the song's
ticks.

Each frame the driver takes 1 from the channel's countdown of frames, a signed
halfword, and reads the channel's next note once the countdown is 0 or less.

## Pitch

A square or wave note's pitch is its entry in the frequency table plus the
detune, kept within 0-0x7FF: the 11-bit frequency setting of NR13 and NR14, or
NR33 and NR34. The table starts at C2. The wave channel plays a 32-step
pattern an octave lower than the squares, and a 64-step one two octaves lower.

A PCM note's pitch is its entry in the PCM pitch table plus the detune: the
change to the sample's rate in thousandths. The table's entry 24 is 0, which
plays the sample at its own rate. Entry 36 is 1000, which doubles it, and
entry 12 is -501, about half.

Each frame the pitch starts from the note's and the effects add to it:

* **Pitch envelope** (`E2`): an offset that goes in a straight line from
  `start` to `middle` over `first` frames, and then to `end` over `second`
  frames, where it stays. The values are signed.
* **LFO** (`DF`): after `delay` frames, a wave with a period of `period`
  frames, scaled by `depth` / 128. The shapes are 0 for a sine from the LFO
  table, 1 for a triangle, 2 and 3 for rising and falling saws, 4 and 5 for
  square waves, 6 for a random value in the second half of each period, and 7
  for a new random value each half period. Shapes above 7 are sines. Bit 7
  turns the wave upside down.
* **Slide** (`D4` before a note of another pitch): the pitch moves in a
  straight line from the last note's to the new one's over the frames that
  `E5` sets (2 at the start), and the PCM channel's rate follows.

On a square or wave channel an offset moves the frequency setting directly,
within 0-0x7FF. On a PCM channel it's added to the PCM pitch twice over, and a
channel whose pitch moves sets its FIFO's rate again each frame.

The random number generator is an LCG: seed × 0x41C64E6D + 0x3039, with the
value (seed >> 16) & 0x7F. The init sets the seed to 1, and it carries on from
song to song.

### Noise

The noise channel's NR43 setting comes from `8E`, `8F` and `9E`, or from its
noise macro. The game gives the driver a file of noise macros, which starts
with a table of halfwords: macro *n*'s offset from the start of the file is at
2*n* + 2. Each frame of a note, the macro gives its next setting:

| Byte | Meaning |
|---|---|
| `FF` | the end of the macro: the channel restarts at volume 0, which silences it |
| `FD` *n* | each setting after this one lasts *n* frames |
| `FE` *v* | sets NR42 to *v* and restarts the channel, and the macro goes on to the next byte in the same frame |
| other | the frame's NR43 setting |

## Volume

The channel's level is its volume during a note and 0 during a rest. On a
square or noise channel it's the 4-bit volume in NRx2, which takes effect when
the channel restarts at a note. The hardware envelope (`3E`, `3F`, `4E`) works
as usual.

The volume envelope (`D2`) works in percent of the volume: from `start` to
`peak` over `attack` frames, and then to `end` over `decay` frames. Each frame
it works out the volume × level / 100, and when that changes, it writes the
new volume into NRx2 and restarts the channel. Once the decay is over, the
volume stays where the envelope left it.

The wave channel's level picks NR32's volume code: 0 for silence, then 25%,
50% and 100% for 1-3. The volume envelope does the same with its volume,
giving 100% for 3 and above. A level above 3 at the start of a note leaves the
code that came before. `BE` sets the 75% bit, which overrides the code.

A PCM channel's level is 0 or 1, SOUNDCNT_H's volume bit for its FIFO: 50% or
100%. The master volume set by the game also affects the FIFOs: at 4 or less
it halves them, and at 1 or less it turns them off. The play routine sets it
to 7.

## Output

The image holds the 12 halfwords from SOUND1CNT_L (NR10) to SOUNDCNT_H. A note
sets the restart bit of the channel's NRx4 in the image. The output stage
compares the image with a copy of the values it last wrote:

* For a PSG channel whose restart bit is set, it writes all of the channel's
  registers, from NR10, NR21, NR30 or NR41 up, and clears the restart bit.
  Square 1 writes NR13 and NR14 three times.
* For the others, it writes each register that has changed.
* It writes NR50 and NR51, with each PSG channel's outputs from the image, and
  SOUNDCNT_H, if they've changed.

Before the wave channel, it loads the channel's waves into the wave RAM again
if a sound effect has loaded others.

### Waves

The game gives the driver a wave table: a 2-byte header and then 16-byte
waves, each 32 steps of 4 bits. The CPU can only write the wave RAM bank that
isn't playing, so `DA` selects the other bank in NR30, writes the 16 bytes and
writes NR30 back as it read it, with bit 7 clear. That turns the wave channel
off until its next restart.

In the 64-step mode the channel plays both banks, starting with the one NR30
selects. The switch commands change the bank it starts with during a note.

## PCM

The game gives the driver a sample file: samples one after another, each a
word with its size, the 6-byte header included, a halfword with its rate in
Hz, and then its 8-bit signed data. The file ends where a size of 0 follows a
sample, or after 128 samples. The driver keeps two such files, the music's in
bank 0 and the sound effects', in bank 1.

`DC` loads a sample into the channel's FIFO record: its data, its rate, and a
length of (size − 64) × 994 / 1000 bytes, which leaves out the end of the
sample. The record holds:

| Offset | Contents |
|---|---|
| `+0x00` | where the next start plays from |
| `+0x04` | the sample's data |
| `+0x08` | where it loops back to, or 0 |
| `+0x0C` | the bytes it plays before it stops or loops, from the data |
| `+0x10` | the sample's length |
| `+0x14` | the sample's rate |
| `+0x18` | the rate it plays at |
| `+0x1C` | how far it has played, in 60ths of a byte, or -1 when it's stopped |
| `+0x20` | that count at the loop point |

A note plays the sample:

1. It stops the FIFO.
2. It sets the rate: the sample's rate + the sample's rate × pitch / 1000,
   rounded towards 0.
3. It sets the range from `E7`. With a range of 0, 0 and 0, the FIFO plays
   the sample's length once. The FIFO starts where the last range that it
   played started, or at the sample's start if no range has played since `DC`
   or `DD` loaded the sample. After a range that started past the sample's
   start, the FIFO therefore plays on past the sample's end. Otherwise it
   plays from `start`% of the length to `end`%, and then loops back to
   `loop`%. With `loop` equal to `end`, the range plays once. The position
   starts at the start × 60.
4. It starts the FIFO: it resets the FIFO in SOUNDCNT_H, writes the first 32
   bytes into it, points the DMA at the 32 bytes after them, and starts the
   DMA (control `0xB640`) and the timer, at 0x10000 − 16780000 / rate.

The DMA feeds the FIFO on the timer's requests. The driver gets no
notification when a sample ends. Each frame, it adds the rate to the
position and checks whether another frame would pass the end. If so, it
stops the FIFO. For a looping sample, it restarts the FIFO from the loop
point and sets the position to the loop point's count plus the rate. Each
pass therefore lasts a whole number of frames and ends a frame or two before
the endpoint set by `E7`.

## Sound effects

The game keeps its sound effects in a file of their own, in the songs' format.
Its routine for sound effects finds sound effect *n* as the play-song routine
finds a song, and passes its address to the driver's routine for sound
effects. That routine leaves the music alone. For each channel whose data
doesn't start with `FF`, it resets the sound effect channel, turns its
outputs on in the sound effects' image (NR51's bits on a PSG channel, and
SOUNDCNT_H = `0x730E` on a PCM channel) and points it at its data. A channel
whose data starts with `FF` is left to the music.

The per-frame routine reads the sound effect channels after the music
channels, with these differences:

* `DB` takes the sound effects' noise macros, and `DC` and `DD` their
  samples. In the J revision, the reset puts a sound effect channel's bank
  flags at 1, and `F0` switches them as it does a music channel's.
* When `FF` ends a PSG channel, the routine writes the image's halfword with
  the channel's NR12, NR22, NR32 or NR42 straight to the hardware. The level
  in it is then 0.
* A PCM channel's notes play the sample that its FIFO holds. A music channel
  doesn't start or stop a FIFO while a sound effect channel plays on it.

While a sound effect channel plays, the output stage writes the sound effects'
image for its sound channel in place of the music's, NR51's bits included.
NR50 comes from the music's image. The output stage loads the music's waves
into the wave RAM again only while the music's image plays the wave channel.
When the sound effect's channel ends, the music's registers go back to the
hardware where they differ from the values last written.

`agbsf2` converts each sound effect as a song. The music file's songs come
first, then the sound effects. After the game's sound init, a sound effect
plays with no music. Its conversion lasts until the last of its channels
ends.

## The J revision

*Super Robot Taisen J*'s driver keeps the same commands and data formats,
with these differences:

| | A revision | J revision |
|---|---|---|
| Channel records | | a sample bank flag at `0x0C` and a noise macro bank flag at `0x0D`, and the fields after them moved on, in records of the same size |
| Banks | the music always uses bank 0 | `F0` switches the noise channel to the other bank of noise macros, and the other channels to the other bank of samples |
| Starting values | tempo 90, hardware envelope up | tempo 120, hardware envelope down |
| Random numbers | the seed carries on | the play routine and each loop set the seed back to 1 |
| Wave volume | 0-3: silence, 25%, 50%, 100% | 0-4: silence, 25%, 50%, 75%, 100%; `5F` stops at 4, and the volume envelope gives silence above 4 |
| Volume envelope | stops after the decay | goes on writing the end level, and after a rest does nothing |
| Pitch envelope | after a rest, adds its end offset once and then starts over | after a rest, does nothing |
| PCM pitch effects | count double | count once |
| FIFO starts | clear the DMA's count with a 16-bit write | clear its count and control with a 32-bit write |
| Timers | written when a FIFO starts or bends | written at the end of the frame, by `0x0807FE30` in `B6JJ`; a stop drops a setting that's waiting |
| Noise macros | `DB` without a macro table leaves the channel's macro as it was | `DB` without a macro table turns the channel's macro off |

In `B6JJ` the play routine is at `0x0807D250`, the routine for sound effects
at `0x0807D2B8`, the note reader at `0x0807D844`, the per-frame routine at
`0x0807EEE4`, and the routine that sets the seed at `0x0807D490`.

## Locating the driver

`agbsf2` finds the driver from its code, without fixed addresses. The play
routine resets the channels from the table of channel records:

```
push {r4-r7, lr}           ; B5F0
adds r7, r0, #0            ; 1C07
movs r4, #0                ; 2400
ldr  r6, =music channels   ; 4Exx
ldr  r5, =effect channels  ; 4Dxx
ldmia r6!, {r0}            ; CE01
bl   reset a channel
ldmia r5!, {r0}            ; CD01
```

In the J revision it passes each channel's bank:

```
push {r4-r7, lr}           ; B5F0
adds r7, r0, #0            ; 1C07
movs r5, #0                ; 2500
ldr  r6, =music channels   ; 4Exx
lsls r4, r5, #2            ; 00AC
ldmia r6!, {r0}            ; CE01
movs r1, #0                ; 2100
bl   reset a channel
ldr  r0, =effect channels  ; 48xx
adds r4, r4, r0            ; 1824
ldr  r0, [r4]              ; 6820
```

The routine for sound effects skips each channel whose data starts with
`FF`:

```
push {r4-r6, lr}           ; B570
ldr  r1, =song playing     ; 49xx
str  r0, [r1]              ; 6008
movs r5, #0                ; 2500
ldr  r6, =effect channels  ; 4Exx, in the A revision only
ldr  r0, =song playing     ; 48xx
ldr  r1, [r0]              ; 6801
lsls r0, r5, #1            ; 0068
adds r0, r0, r1            ; 1840
ldrh r0, [r0, #2]          ; 8840
adds r4, r0, r1            ; 1844, or adds r6, r0, r1 (1846) in the J revision
ldrb r0, [r4]              ; 7820, or ldrb r0, [r6] (7830)
cmp  r0, #0xFF             ; 28FF
```

The driver's tables come from the instructions that load their addresses,
found by the instructions around them:

| Table | Code (halfwords; `x` is any digit) | Load |
|---|---|---|
| length table | `2x0D DD00 E1xx 2x0C DD00 E1xx 48xx 18xx 7801 20C0 0040` | the 7th |
| frequency table | `2E53 DD00 2653 4Dxx 1C30 210C` | the 4th |
| PCM pitch table | `2A34 DD00 2234 48xx 0051` | the 4th |
| LFO table | `48xx 0411 1409 1809 2000 5608` | the 1st |

The game gives the driver its files through routines that store each file's
address in the driver's variables, or pass it to the sample loader with its
bank in `r1`. `agbsf2` finds the variables from the routines that read
them, then the routines that store them (`ldr r1, =variable; str r0, [r1]` or
`[r1, #4]`; `bx lr`), and then the game's calls to those routines. Before each
call, the game looks the file up: `movs r0, #archive; movs r1, #file; bl
lookup`. `agbsf2` runs the game's lookup routine on a small Thumb
interpreter to get the file's address. It finds the music file the same way,
from the game's calls to the play routine, with the lookup a few instructions
before. The sound effects' file comes from the game's calls to the routine for
sound effects. In the A revision, the routine that gives the driver the sound
effects' noise macros stores them through the address of the word after the
music's variable.

| Variable or routine | Found from |
|---|---|
| the wave table's variable | `B530 0600 0E02 48xx 6801 1C05 2900`, the 4th halfword's load |
| the noise macros' variable, A revision | `0600 0E03 1C19 48xx 7800 2800 D0xx 48xx 6802`, the 8th halfword's load |
| the two noise macro variables, J revision | `B510 0600 0E03 48xx 6802 1C14 3490 2000 6020 2B00 D0xx 49xx 7B52 0090 1840`, the 12th halfword's load |
| the sample loader, A revision | `B570 1C02 48xx 2500 024C 237F` |
| the sample loader, J revision | `B570 4Bxx 008A 18D2 1C03 6013 48xx 2200 024D 247F` |
| the loader's callers for banks 0 and 1 | `B500 2100 Fxxx Fxxx BC01 4700` and `B500 2101 ...`, whose `bl` goes to the loader |

`--song-table` and `--song-count` override the music file and the number of
songs. Without the driver's code, `agbsf2` can't find the driver's
tables or read the game.
