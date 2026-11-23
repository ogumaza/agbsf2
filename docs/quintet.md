# Quintet's GBA sound driver

This document covers the song format, sequencer and sound hardware control
in Quintet's driver for Banpresto's *Super Robot Taisen* games on the Game
Boy Advance. It's based on the code of the driver in *Super Robot Taisen A*
(game code `ASRJ`). The example addresses come from that game. Other
games put the driver and its data elsewhere. `agbsf2` finds them
from the code (see [Locating the driver](#locating-the-driver)).

The driver plays six channels, one for each of the GBA's sound channels: the
Game Boy's two square channels, its wave channel and its noise channel, and
the two DirectSound FIFOs. Each FIFO plays one sample at a time, at a rate
that a timer sets. The driver doesn't mix samples in software.

## Games and revisions

| Game | Revision |
|---|---|
| *Super Robot Taisen A* (`ASRJ`) | A |
| *Super Robot Taisen D* (`A6SJ`) | A |
| *Super Robot Taisen R* (`AJ9J`) | A |
| *Super Robot Taisen J* (`B6JJ`) | J |
| *Bouken Yuuki Plaston GP* (`A2PJ`) | J, with music files |
| *Bouken Yuuki Densetsu Plaston Gate* (`APJJ`) | J, with music files |
| *Bouken Yuuki PlaStar World: Densetsu no Plust Gate EX* (`BPDJ`) | J, with music files |
| *Legend of Dynamic: Goushouden* (`AVDJ`) | J, with music files |
| *Blender Bros.* (`ABRE`) | J, with music files |
| *Doubutsu no Shima no Chobi Gurumi* (`A8DJ`) | J, with music files |
| *Gachasute! Dyna Device Red* and *Blue* (`AAIJ`, `ABIJ`) | J, with music files |
| *Robopon 2: Ring Version* and *Cross Version* (`ARPE`, `ACVE`) | J, with music files |
| *Magical Fengshen* (`AJOJ`) | J, with music files |
| *Suta Komi: Star Communicator* (`ASKJ`) | S |

Most of this document applies to every revision.
[The J revision](#the-j-revision) and [the S revision](#the-s-revision) list
the differences.

## Architecture

The driver is Thumb code in ROM. The game calls the per-frame routine of the
driver once a frame. The routine runs these steps:

1. For each of the six music channels in turn, it counts down the frames to the
   next note of the channel. When the count runs out, it reads the commands of
   the channel up to its next note. It writes the settings of the channel into
   a copy of the sound registers called the register image of the music. Then
   it runs the effects of the channel. They also write into the image.
2. It does the same for the six sound effect channels. They have an image of
   their own.
3. It writes the images to the sound hardware (see [Output](#output)).
4. It moves the sample of each FIFO on by a frame, and stops it or starts it again
   from its loop when it reaches the end (see [PCM](#pcm)).

In `ASRJ` the routines are:

| Address | Routine |
|---|---|
| `0x080034F0` | the sound init of the game: calls the init of the driver and the FIFO setup, and passes the files of the game to the driver |
| `0x08003550` | the play-song routine of the game: looks up the music file, skips to song `r0`, and calls the play routine of the driver |
| `0x08003594` | the routine of the game for sound effects: looks up the file of sound effects, skips to sound effect `r0`, and calls the routine of the driver for sound effects |
| `0x08003544` | the per-frame sound routine of the game: calls the per-frame routine of the driver |
| `0x0805CF44` | init: resets the channels and the PSG |
| `0x0805D1FC` | play a song: `r0` = the address of its header |
| `0x0805D254` | play a sound effect: `r0` = the address of its header |
| `0x0805D184` | turn the outputs of sound effect channel `r0` on in the image of the sound effects |
| `0x0805D298` | set the wave table |
| `0x0805D2A4` | set the noise macros of the music |
| `0x0805D2B0` | set the noise macros of the sound effects |
| `0x0805D2BC`, `0x0805D2C8` | load the samples of the music and of the sound effects, through the loader at `0x0805D4BC` |
| `0x0805D30C` | set the master volume of the PSG: `r0` = left, `r1` = right, each 0-7 |
| `0x0805D788` | the note reader, with the command table for commands `0E` to `F1` at `0x0805DA60` |
| `0x0805D408`, `0x0805D47C` | the frequency setting and the PCM pitch of a note |
| `0x0805D524` | load a wave into the wave RAM |
| `0x0805D5D0`, `0x0805D638` | select a noise macro, select a sample |
| `0x0805D6CC` | start a PCM note |
| `0x0805EC84`, `0x0805ECF4` | the effects of a PSG channel, the effects of a PCM channel |
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

The tables in ROM and the RAM of the driver contain:

| Address | Contents |
|---|---|
| `0x087F0F60` | the music channels: 6 words, the address of the record of each channel |
| `0x087F0F78` | the sound effect channels |
| `0x087F0F90` | the length table: 13 bytes |
| `0x087F0F9D` | the sine of the LFO: 256 signed bytes |
| `0x087F109E` | the frequency table: 84 halfwords |
| `0x087F1146` | the PCM pitch table: 53 halfwords |
| `0x03000784` | the address of the channel being read |
| `0x03000788` | its number, 0-5 |
| `0x0300078C` | the address of the song playing |
| `0x03000790` | the address of the wave table |
| `0x03000794`, `0x03000798` | the addresses of the noise macros of the music and of the sound effects |
| `0x030007A8` | the sample tables: 128 words for the samples of the music, then 128 for those of the sound effects |
| `0x0300079E` | the sample loaded into each FIFO: 2 bytes |
| `0x030007A1`, `0x030007A2` | the master volume, left and right |
| `0x03000030` | the register image of the music |
| `0x03000048` | the register image of the sound effects |
| `0x030007A0` | 1 while the per-frame routine reads the music channels, 0 while it reads the sound effect channels |
| `0x03000BB0` | the FIFO records: 2 of 0x24 bytes |
| `0x03000BA8` | the seed of the random number generator |

With no sound effect playing, the output stage writes the image of the music.
[Sound effects](#sound-effects) covers how a sound effect plays.

## Songs

The game keeps its songs in a music file, one song after another. A song
starts with a header of 7 halfwords: its length in bytes, and then the offset
of the data of each channel from the start of the song. Bit 0 of the length is
ignored. The next song starts that many bytes on. The play-song routine of the
game finds song *n* by skipping *n* songs this way. It passes the address of
the song to the driver. Nothing in the driver gives the number of songs. `agbsf2`
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
  48, 64 and 128. The note lasts 384 ticks divided by that value. A quarter
  note is therefore 96 ticks.

The pitch of the note is octave × 12 + pitch + the two transposes, kept within
the frequency table (0-83) or the PCM pitch table (0-52).

After a note, `D1` makes it half as long again. `D4` followed by a note of the
same pitch ties the length of that note to it, with the `D1` of that note if it
has one. `D4` before a note of another pitch makes that note slide from this
one (see [Pitch](#pitch)).

A note sets the level of the channel to its volume. On a PCM channel it also
starts the sample (see [PCM](#pcm)). Playing again does the same without a new
pitch. On a PCM channel it plays the sample at its sample rate plus the
detune. A rest sets the level to 0 and stops the FIFO of a PCM channel.

A note or a play again starts the effects of the channel over: the noise macro, the
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
| `3E` | step | the step time of the hardware envelope |
| `3F`, `4E` | | the hardware envelope goes up, down |
| `4F` | octave | the octave |
| `5E` | volume | the volume |
| `5F`, `6E` | | volume up, down; up stops at 15, or 3 on the wave channel and 1 on a PCM channel |
| `6F`, `7E` | | octave up, to at most 6; down, to at least 0 |
| `7F` | tempo | the tempo (see [Timing](#timing)) |
| `8E` | shift | the clock shift of the noise channel (NR43 bits 4-7) |
| `8F` | width | its 7-step mode (NR43 bit 3) |
| `9E` | ratio | its clock divider (NR43 bits 0-2) |
| `9F` | 0 or 1 | the 64-step mode of the wave channel (NR30 bit 5) |
| `AE` | bank | the wave bank it plays (NR30 bit 6) |
| `AF` | 0 or 1 | the wave channel on (NR30 bit 7) |
| `BE` | 0 or 1 | the wave channel at 75% (NR32 bit 15) |
| `BF` | | the loop point |
| `CE`, `CF` | bits | the master volume of the PSG: NR50 bits 4-6 for the left side, bits 0-2 for the right side |
| `D0` | keep, set | the channel outputs on each side: NR51 for the PSG channels, the high byte of SOUNDCNT_H for the FIFOs. The bits of `keep` stay, and those of `set` are set. |
| `D2` | start, peak, end, attack, decay | the volume envelope (see [Volume](#volume)) |
| `D3` | | volume envelope off |
| `D5` | detune | a signed detune, × 10 on a PCM channel |
| `D6` | | repeat from here |
| `D7` | count | go back to the matching `D6` until the passes add up to `count`. A count of 0 makes 256 passes. Repeats nest up to 8 deep. |
| `DA` | wave | load wave `wave & 7F` from the wave table into the wave RAM: into bank 0, or bank 1 if bit 7 is set |
| `DB` | macro | the noise macro that each note starts, or 0 for none |
| `DC`, `DD` | sample | load sample `sample` into the FIFO of the channel, and clear the range set by `E7` |
| `DE` | frames, first, then | a switch: the duty (or on the wave channel, the wave bank) is `first` for `frames` frames of each note, and then `then` |
| `DF` | delay, period, depth | the LFO, on (see [Pitch](#pitch)) |
| `E0` | | LFO off |
| `E1`, `EB` | | no switch |
| `E2` | start, middle, end, first, second | the pitch envelope (see [Pitch](#pitch)) |
| `E3` | | pitch envelope off |
| `E4` | semitones | the transpose, signed |
| `E5` | frames | how long a slide takes |
| `E6` | 2 bytes | a 16-bit signed detune, little-endian |
| `E7` | start, end, loop | the part of a sample that a PCM note plays, in % of its length (see [PCM](#pcm)) |
| `E8` | resolution | SOUNDBIAS bits 14-15, written at once |
| `E9` | semitones | a second transpose |
| `EA` | frames | a wave bank switch: bank 0 for `frames` frames of each note, then bank 1 |
| `EC` | shape | the LFO's shape |
| `ED` | bits | the shape of the LFO becomes bits 0-6 of the shape ORed with `bits` |
| `EE`, `EF` | | transpose up, down a semitone |
| `F1` | amount | adds a signed amount to the volume, keeping it from 0 to 15, 4 on the wave channel and 1 on a PCM channel |
| `FF` | | the end of the channel, or back to the loop point |

`D1` outside a note, `D8`, `D9`, `F0` and `F2`-`FE` do nothing, apart from
`F0` in the J revision.

`FF` sends the channel back to its loop point if it has one. It resets the
counts of the channel (see [Timing](#timing)). The octave, the volume and
every other setting stay as the end of the loop leaves them. A loop that
relies on a setting from before it therefore plays its first pass with the
setting that the intro left. If the loop point is the `FF` itself, the channel
stops reading until the song is changed. A channel without a loop point ends:
its level goes to 0 and the FIFO of a PCM channel stops. Each channel loops on
its own. Channels with loops of different lengths therefore drift apart.

## Timing

Each channel has a separate tempo. `7F` sets it. A `7F` on channel 0 sets the
tempo of every channel. A tick lasts 37 / tempo frames. A quarter note of 96
ticks therefore lasts 96 × 37 / *T* frames at tempo *T*. The tempo is close to
the beats per minute: 90 gives 90.8.

The driver times each channel by counting the ticks that it has played since
the count was last reset, and the frames that those ticks take. It computes
the frames as ticks × 37 / tempo, rounded down. When a note is read, its
length goes into the tick count, and the channel waits for the difference
between the new frame count and the old one. Each note therefore plays at the
start of the frame that its tick falls in. A wait of 0 frames becomes 1, and
the frame count goes up by 1 with it. A note therefore always lasts at least a
frame.

A loop resets the counts of the channel, and so does a `7F`. A `7F` on channel
0 resets the counts of every channel. The driver then counts from the frame
that it's in, and drops the fraction of a frame that it had counted. A channel
whose loop isn't a whole number of frames long plays each pass slightly early,
compared with the ticks of the song.

Each frame the driver takes 1 from the countdown of frames of the channel, a
signed halfword. It reads the next note of the channel once the countdown is 0
or less.

## Pitch

The pitch of a square or wave note is its entry in the frequency table plus the
detune, kept within 0-0x7FF: the 11-bit frequency setting of NR13 and NR14, or
NR33 and NR34. The table starts at C2. The wave channel plays a 32-step
pattern an octave lower than the squares, and a 64-step one two octaves lower.

The pitch of a PCM note is its entry in the PCM pitch table plus the detune:
the change to the rate of the sample in thousandths. Entry 24 of the table is
0. That entry plays the sample at its sample rate. Entry 36 is 1000 and
doubles the rate. Entry 12 is -501, about half the rate.

Each frame the pitch starts from the pitch of the note, and the effects add to it:

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
  straight line from the pitch of the last note to the pitch of the new one,
  over the frames that `E5` sets (2 at the start). The rate of the PCM channel
  follows.

On a square or wave channel an offset moves the frequency setting directly,
within 0-0x7FF. On a PCM channel it's added to the PCM pitch twice over. A
channel whose pitch moves sets the rate of its FIFO again each frame.

The random number generator is an LCG: seed × 0x41C64E6D + 0x3039, with the
value (seed >> 16) & 0x7F. The init sets the seed to 1. No song resets it.

### Noise

The NR43 setting of the noise channel comes from `8E`, `8F` and `9E`, or from
its noise macro. The game passes a file of noise macros to the driver. The
file starts with a table of halfwords: the offset of macro *n* from the start
of the file is at 2*n* + 2. Each frame of a note, the macro moves on to its
next setting:

| Byte | Meaning |
|---|---|
| `FF` | the end of the macro: the channel restarts at volume 0, which silences it |
| `FD` *n* | each setting after this one lasts *n* frames |
| `FE` *v* | sets NR42 to *v* and restarts the channel, and the macro goes on to the next byte in the same frame |
| other | the NR43 setting of the frame |

## Volume

The level of a channel is its volume during a note and 0 during a rest. On a
square or noise channel it's the 4-bit volume in NRx2. That volume takes
effect when the channel restarts at a note. The hardware envelope (`3E`, `3F`, `4E`) works
as usual.

The volume envelope (`D2`) works in percent of the volume: from `start` to
`peak` over `attack` frames, and then to `end` over `decay` frames. Each frame
it computes the volume × level / 100. When that changes, it writes the new
volume into NRx2 and restarts the channel. Once the decay is over, the
volume stays where the envelope left it.

The level of the wave channel selects the volume code of NR32: 0 for silence,
then 25%, 50% and 100% for 1-3. The volume envelope does the same with its
volume, with 100% for 3 and above. A level above 3 at the start of a note leaves the
code that came before. `BE` sets the 75% bit. That bit overrides the code.

The level of a PCM channel is 0 or 1: the volume bit of SOUNDCNT_H for its
FIFO, 50% or 100%. The master volume that the game sets also affects the
FIFOs. At 4 or less it halves them, and at 1 or less it turns them off. The play routine sets it
to 7.

## Output

The image contains the 12 halfwords from SOUND1CNT_L (NR10) to SOUNDCNT_H. A
note sets the restart bit of NRx4 of the channel in the image. The output
stage compares the image with a copy of the values that it last wrote:

* For a PSG channel whose restart bit is set, it writes all the registers of
  the channel, from NR10, NR21, NR30 or NR41 up, and clears the restart bit.
  Square 1 writes NR13 and NR14 three times.
* For the others, it writes each register that has changed.
* It writes NR50 and NR51, with the outputs of each PSG channel from the
  image, and SOUNDCNT_H, if they've changed.

Before the wave channel, it loads the waves of the channel into the wave RAM
again if a sound effect has loaded others.

### Waves

The game passes a wave table to the driver: a 2-byte header and then 16-byte
waves, each 32 steps of 4 bits. The CPU can write only the wave RAM bank that
isn't playing. `DA` therefore selects the other bank in NR30, writes the 16
bytes and writes NR30 back as it read it, with bit 7 clear. That turns the wave channel
off until its next restart.

In the 64-step mode the channel plays both banks, starting with the one NR30
selects. The switch commands change the bank that it starts with during a
note.

## PCM

The game passes a sample file to the driver: samples one after another. Each
sample is a word with its size, the 6-byte header included, a halfword with
its rate in Hz, and then its 8-bit signed data. The file ends where a size of
0 follows a sample, or after 128 samples. The driver keeps two such files: the
file of the music in bank 0 and the file of the sound effects in bank 1.

`DC` loads a sample into the FIFO record of the channel: its data, its rate,
and a length of (size − 64) × 994 / 1000 bytes. That length leaves out the end
of the sample. The record contains:

| Offset | Contents |
|---|---|
| `+0x00` | where the next start plays from |
| `+0x04` | the data of the sample |
| `+0x08` | where it loops back to, or 0 |
| `+0x0C` | the bytes it plays before it stops or loops, from the data |
| `+0x10` | the length of the sample |
| `+0x14` | the rate of the sample |
| `+0x18` | the rate it plays at |
| `+0x1C` | how far it has played, in 60ths of a byte, or -1 when it's stopped |
| `+0x20` | that count at the loop point |

A note plays the sample:

1. It stops the FIFO.
2. It sets the rate: the rate of the sample + the rate of the sample × pitch /
   1000, rounded towards 0.
3. It sets the range from `E7`. With a range of 0, 0 and 0, the FIFO plays
   the length of the sample once. The FIFO starts where the last range that it
   played started, or at the start of the sample if no range has played since
   `DC` or `DD` loaded the sample. After a range that started past the start of
   the sample, the FIFO therefore plays on past the end of the sample. Otherwise it
   plays from `start`% of the length to `end`%, and then loops back to
   `loop`%. With `loop` equal to `end`, the range plays once. The position
   starts at the start × 60.
4. It starts the FIFO: it resets the FIFO in SOUNDCNT_H, writes the first 32
   bytes into it, points the DMA at the 32 bytes after them, and starts the
   DMA (control `0xB640`) and the timer, at 0x10000 − 16780000 / rate.

The DMA feeds the FIFO on the requests of the timer. The driver gets no
notification when a sample ends. Each frame, it adds the rate to the
position and checks whether another frame would pass the end. If so, it
stops the FIFO. For a looping sample, it restarts the FIFO from the loop
point and sets the position to the count of the loop point plus the rate. Each
pass therefore lasts a whole number of frames and ends a frame or two before
the endpoint set by `E7`.

## Sound effects

The game keeps its sound effects in a file of their own, in the format of the
songs. The routine of the game for sound effects finds sound effect *n* as the
play-song routine finds a song. It passes the address of the sound effect to
the routine of the driver for sound effects. That routine leaves the music
alone. For each channel whose data doesn't start with `FF`, it resets the
sound effect channel and turns its outputs on in the image of the sound
effects (the bits of NR51 on a PSG channel, and SOUNDCNT_H = `0x730E` on a PCM
channel). It then points the channel at its data. A channel
whose data starts with `FF` is left to the music.

The per-frame routine reads the sound effect channels after the music
channels, with these differences:

* `DB` takes the noise macros of the sound effects, and `DC` and `DD` their
  samples. In the J revision, the reset sets the bank flags of a sound effect
  channel to 1. `F0` switches them as it switches those of a music channel.
* When `FF` ends a PSG channel, the routine writes the halfword of the image
  with NR12, NR22, NR32 or NR42 of the channel straight to the hardware. The
  level in it is then 0.
* The notes of a PCM channel play the sample in its FIFO. A music channel
  doesn't start or stop a FIFO while a sound effect channel plays on it.

While a sound effect channel plays, the output stage writes the image of the
sound effects for its sound channel in place of the image of the music, the
bits of NR51 included. NR50 comes from the image of the music. The output
stage loads the waves of the music into the wave RAM again only while the
image of the music plays the wave channel.
When the sound effect's channel ends, the music's registers go back to the
hardware where they differ from the values last written.

`agbsf2` converts each sound effect as a song. The music file's songs come
first, then the sound effects. After the game's sound init, a sound effect
plays with no music. Its conversion lasts until the last of its channels
ends.

## The J revision

The driver of *Super Robot Taisen J* keeps the same commands and data formats,
with these differences:

| | A revision | J revision |
|---|---|---|
| Channel records | | a sample bank flag at `0x0C` and a noise macro bank flag at `0x0D`, and the fields after them moved on, in records of the same size |
| Banks | the music always uses bank 0 | `F0` switches the noise channel to the other bank of noise macros, and the other channels to the other bank of samples |
| Starting values | tempo 90, hardware envelope up | tempo 120, hardware envelope down |
| Random numbers | no song resets the seed | the play routine and each loop set the seed back to 1 |
| Wave volume | 0-3: silence, 25%, 50%, 100% | 0-4: silence, 25%, 50%, 75%, 100%; `5F` stops at 4, and the volume envelope plays silence above 4 |
| Volume envelope | stops after the decay | goes on writing the end level, and after a rest does nothing |
| Pitch envelope | after a rest, adds its end offset once and then starts over | after a rest, does nothing |
| PCM pitch effects | count double | count once |
| FIFO starts | clear the count of the DMA with a 16-bit write | clear its count and control with a 32-bit write |
| Timers | written when a FIFO starts or bends | written at the end of the frame, by `0x0807FE30` in `B6JJ`; a stop drops a setting that's waiting |
| Noise macros | `DB` without a macro table leaves the macro of the channel as it was | `DB` without a macro table turns the macro of the channel off |

In `B6JJ` the play routine is at `0x0807D250`, the routine for sound effects
at `0x0807D2B8`, the note reader at `0x0807D844`, the per-frame routine at
`0x0807EEE4`, and the routine that sets the seed at `0x0807D490`.

## The S revision

*Suta Komi: Star Communicator* (`ASKJ`) has a build of the A revision with
another song format. Its play routine is the play routine of the A revision. Its note reader
tells a note from a command by bit 7 of the first byte, and divides 384 by the
next byte: `2080 4030 2800 D0xx Exxx 46B0 7821 20C0 0040`. Its routine that
plays a sound effect starts `B570 4Axx 2100 7011` and then follows the routine
of the A revision from `49xx`. The S revision differs from the A revision in these
ways:

| | A revision | S revision |
|---|---|---|
| a note | one byte: the pitch in the low nibble, the length index in the high nibble | two bytes: the pitch, then the note value |
| the pitches | 0-11, 12 to play again, 13 to rest | 0-11, 14 to play again, 15 to rest. The others only wait for the length of the note |
| the length table | 13 bytes | none: a note lasts 384 ticks divided by its note value |
| the commands | `0E` to `F1` | `80` to `BA`, in the order of the A revision |
| the tie of `D4` | a note of the same pitch | a note with the same first byte |
| after a rest | the envelope step of the square channels is 0 | the square channels keep their envelope step |
| the starting tempo | 90 | 150 |
| SOUNDBIAS after the reset of the play routine | resolution 1 (65536 Hz) | resolution 3 (262144 Hz) |
| the length of a sample | its size less 64 bytes, less 0.6% | its size less 256 bytes, less 0.6% |
| `E7` with its loop at its end | plays the range once | loops from the end of the range |

The commands of the S revision, numbered as in the A revision:

| S | A | S | A | S | A | S | A |
|---|---|---|---|---|---|---|---|
| `80` | `0E` | `8F` | `7F` | `9E` | `D4` | `AD` | `E3` |
| `81` | `0F` | `90` | `8E` | `9F` | `D5` | `AE` | `E4` |
| `82` | `1E` | `91` | `8F` | `A0` | `D6` | `AF` | `E5` |
| `83` | `1F` | `92` | `9E` | `A1` | `D7` | `B0` | skips the byte after it |
| `84` | `2E` | `93` | `9F` | `A2` | `D8` | `B1` | `E6` |
| `85` | `2F` | `94` | `AE` | `A3` | `D9` | `B2` | `E7` |
| `86` | `3E` | `95` | `AF` | `A4` | `DA` | `B3` | `E8` |
| `87` | `3F` | `96` | `BE` | `A5` | `DB` | `B4` | `E9` |
| `88` | `4E` | `97` | `BF` | `A6` | `DC` | `B5` | `EA` |
| `89` | `4F` | `98` | `CE` | `A7` | `DD` | `B6` | `EB` |
| `8A` | `5E` | `99` | `CF` | `A8` | `DE` | `B7` | `EC` |
| `8B` | `5F` | `9A` | `D0` | `A9` | `DF` | `B8` | `ED` |
| `8C` | `6E` | `9B` | `D1` | `AA` | `E0` | `B9` | `EE` |
| `8D` | `6F` | `9C` | `D2` | `AB` | `E1` | `BA` | `EF` |
| `8E` | `7E` | `9D` | `D3` | `AC` | `E2` | | |

`BB` to `FE` do nothing, and `FF` ends the channel as in the A revision. The
S revision has no `F0` or `F1`.

The game passes its music file, its file of sound effects, its wave table, its
noise macros and its samples to the driver in literals: `ldr r1, =table; adds
r0, r1, #0; bl routine`. Its play-song routine loads the address of the music
file from a literal and skips to the song before it calls the play routine. `agbsf2`
looks back up to 96 bytes from that call for an `ldr` whose literal is a song.

In `ASKJ` the play routine is at `0x0805FC30`, the routine for sound effects
at `0x0805FC94` and the per-frame routine at `0x08061428`. The note reader is
at `0x080601A4` and its command table at `0x0806044C`. The play-song routine of
the game is at `0x08026040`, and its music file at `0x0879808C`.

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

In the J revision it passes the bank of each channel:

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

The tables of the driver come from the instructions that load their addresses,
found by the instructions around them:

| Table | Code (halfwords; `x` is any digit) | Load |
|---|---|---|
| length table | `2x0D DD00 E1xx 2x0C DD00 E1xx 48xx 18xx 7801 20C0 0040` | the 7th |
| frequency table | `2E53 DD00 2653 4Dxx 1C30 210C` | the 4th |
| PCM pitch table | `2A34 DD00 2234 48xx 0051` | the 4th |
| LFO table | `48xx 0411 1409 1809 2000 5608` | the 1st |

The game passes its files to the driver through routines that store the
address of each file in the variables of the driver, or pass it to the sample
loader with its bank in `r1`. `agbsf2` finds the variables from the routines
that read them, then the routines that store them (`ldr r1, =variable; str r0,
[r1]` or `[r1, #4]`; `bx lr`), and then the calls of the game to those
routines. Before each call, the game looks the file up: `movs r0, #archive;
movs r1, #file; bl lookup`. `agbsf2` runs the lookup routine of the game on a
small Thumb interpreter to get the address of the file. It finds the music
file the same way, from the calls of the game to the play routine, with the
lookup a few instructions before. The file of sound effects comes from the
calls of the game to the routine for sound effects. In the A revision, the
routine that passes the noise macros of the sound effects to the driver stores
them through the address of the word after the variable of the music.

| Variable or routine | Found from |
|---|---|
| the variable of the wave table | `B530 0600 0E02 48xx 6801 1C05 2900`, the load of the 4th halfword |
| the variable of the noise macros, A revision | `0600 0E03 1C19 48xx 7800 2800 D0xx 48xx 6802`, the load of the 8th halfword |
| the two noise macro variables, J revision | `B510 0600 0E03 48xx 6802 1C14 3490 2000 6020 2B00 D0xx 49xx 7B52 0090 1840`, the load of the 12th halfword |
| the sample loader, A revision | `B570 1C02 48xx 2500 024C 237F` |
| the sample loader, J revision | `B570 4Bxx 008A 18D2 1C03 6013 48xx 2200 024D 247F` |
| the callers of the loader for banks 0 and 1 | `B500 2100 Fxxx Fxxx BC01 4700` and `B500 2101 ...`, whose `bl` goes to the loader |

### Music files

The games other than the *Super Robot Taisen* ones keep their songs, their
sound effects and the tables that the driver plays them with in a music file.
The loader of the driver reads the file into the tables of the driver in RAM. Its code
starts `B5F0 1C05 CD10 1C2A 08A0 0080 182D 4Bxx 2000 7018 8811 4Exx 2900 D0xx
4Cxx`. The file has seven parts, in this order:

1. the songs, one after another, up to a length of 0
2. the sound effects, in the same form
3. the wave table
4. the noise macros of bank 0
5. the noise macros of bank 1
6. the samples of bank 0
7. the samples of bank 1

A word with the size of the part in bytes comes before each part. The loader
moves on by the size rounded down to a multiple of 4. It skips the size of the
last part. The game then plays song n or sound effect n of the file through
routines that read the tables of the loader.

The games pass the file to the loader in three ways: with a literal (`ldr r0,
=file`), with a lookup of the file a few instructions before the call, or from
a routine that returns the address of a numbered file (`bl routine; adds r0,
#4`). In the third way, the code of the game selects the number. `agbsf2`
reads the files of the first two ways from the code. It also scans the ROM, a
word at a time, for every file that reads in full. That finds the files of the
third way. It also finds the files of a program that the game sends to another
console. That program has a copy of the driver. The files that the code names
come first, then those that a word of the ROM points to, as a table of
numbered files does, and then the rest. `agbsf2` numbers the songs and sound
effects of each file after those of the file before.

`--song-table` and `--song-count` override the music file and the number of
songs. Without the code of the driver, `agbsf2` can't find the tables of the
driver or read the game.
