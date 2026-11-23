# The Konami GBA sound driver

This document describes the sound driver used by some Konami Game Boy
Advance games. It is based on the driver code in the Japanese release of
*Yu-Gi-Oh! Ultimate Masters Edition: World Championship Tournament 2006*
(game code `BY6J`).

Addresses given as examples are from `BY6J`. The US release, `BY6E`, has the
same driver and data 0x90 bytes lower in ROM. Its RAM addresses are the same.
Other games place the driver and its data elsewhere. `agbsf2` finds them
from the code (see [Locating the driver](#locating-the-driver)).

*Yu-Gi-Oh! World Championship Tournament 2004* has an older revision of the
driver, with a different command set. [The WCT 2004
revision](#the-wct-2004-revision) describes how it differs. [The Kitaro
revision](#the-kitaro-revision) and [The Minna no Ouji-sama
revision](#the-minna-no-ouji-sama-revision) describe two variants of it. [The Rave Master
revision](#the-rave-master-revision), [The Hunter X Hunter
revision](#the-hunter-x-hunter-revision), [The Captain Tsubasa
revision](#the-captain-tsubasa-revision), [The Eternal Duelist
revision](#the-eternal-duelist-revision) and [The Dungeon Dice Monsters
revision](#the-dungeon-dice-monsters-revision) describe five still older ones.

## Architecture

The driver has two parts:

| Part | Runs | Job |
|---|---|---|
| Sequencer | once per frame from the VBlank interrupt (59.7275 Hz) | reads the song data, updates 16 tracks, and writes the Game Boy PSG registers and the DirectSound voice table |
| Mixer | from the DMA1 (FIFO A) interrupt, in ARM code copied to IWRAM | mixes 12 DirectSound voices, 16 stereo samples per call, into the FIFOs |

A song always has 16 tracks, each assigned to a sound channel:

| Track | Channel |
|---|---|
| 0 | PSG square 1 |
| 1 | PSG square 2 |
| 2 | PSG wave |
| 3 | PSG noise |
| 4-15 | DirectSound voices 0-11 (one voice per track: each track is monophonic) |

Timing is counted in frames. There's no tick resolution finer than a frame. A
song's tempo comes only from the lengths of its delays.

In `BY6J` the key routines are:

| Address | Routine |
|---|---|
| `0x0810B470` | sound init (sets up timers, DMA, copies the mixer to IWRAM) |
| `0x0810CFE4` | request a song: `r0` = song number |
| `0x0810CE30` | start the requested song (reads the song table) |
| `0x0810CAC8` | per-frame entry, called from the VBlank handler |
| `0x0810BCA0` | the sequencer proper, with the command reader at `0x0810C0AA` |
| `0x0810C736` | output stage (writes the PSG registers, then updates the 12 DirectSound voices) |
| `0x0810D720` | start a DirectSound note |
| `0x0810DFC8` | mixer (ARM), copied to and run from `0x03004C0C` |

The driver state is in RAM at `0x030050CC` (the track records start at
`+0x24`, 32 bytes each). The 12 voice records are at `0x0300546C` (28 bytes
each), and the echo buses at `0x03005608`.

### Builds of the driver

Other builds of this driver read the same song format, with a few differences:

| Games | Pitch bend | Voice records |
|---|---|---|
| *Ultimate Masters Edition* (`BY6J`, `BY6E`) | kept for later notes | 28 bytes |
| *Yu-Gi-Oh! GX: Duel Academy* (`BYGE`), *Animal Yokochou* (`BAYJ`) | kept for later notes | 24 bytes |
| *Shaman King: Master of Spirits* 1 and 2 (`BSOE`, `B2ME`), *Yu-Gi-Oh! Day of the Duelist* (`BY7E`), *Mermaid Melody Pichi Pichitto Live Start!* (`B3MJ`) | the playing note only | 24 bytes |
| *Tennis no Ouji-sama 2004* (`B4GJ`, `B4SJ`) | the playing note only | 24 bytes |

See [Pitch bend](#pitch-bend-f2) for the difference in bends. The 24-byte voice
records contain no ADPCM decoder state at `+0x18`. The mixer of these builds plays packed
samples instead (see [Packed samples](#packed-samples)). In these builds the driver's RAM follows the mixer that the
init copies to IWRAM: the state is at `+0x480`, the voice records at `+0x820`
and the echo buses at `+0x98C`. The mixer goes to `0x03000000` in `BSOE`,
`B2ME`, `BAYJ`, `B4GJ` and `B4SJ`, `0x03001EB0` in `B3MJ`, `0x03004788` in
`BY7E` and `0x030006C8` in `BYGE`.

The builds of *Mermaid Melody Pichi Pichitto Live Start!* and *Tennis no
Ouji-sama 2004* (The Prince of Tennis 2004) differ in more ways. `agbsf2`
reads them as two revisions, Live Start and Prince of Tennis 2004. Detection
finds both from their duty command's code, `2x0F 40xx 2x03 DD01 3x04 E000 01xx`
with any registers. Live Start keeps the Ultimate Masters revision's code that
reads a sample byte above `EF` with the next byte, `2xEF DDxx 3x01 3x01 2x0F
40xx 02xx`. Prince of Tennis 2004 doesn't.

Both builds read the Ultimate Masters revision's commands, with these
differences:

- The duty command reads only the opcode's low nibble, for every opcode from
  `00` to `8F`. A nibble of 0 to 3 sets the duty byte to the nibble shifted into
  bits 6-7. `00`-`03` and `80`-`83` then both select 12.5%, 25%, 50% and 75%.
  Any other nibble sets the duty byte to the nibble minus 4. The wave track
  then has waves 0 to 11.
- Delays read as in WCT 2004 (see [Delays in WCT 2004](#delays-in-wct-2004)).
- `90`-`9F` are calls. They work as in WCT 2004 (see [Calls](#calls)). Like
  `00`-`8F`, `90`-`9E` first set the duty byte from their low nibble.
- `F4` and `F5` set the attack and decay rates of PSG notes. They work as in
  WCT 2004 (see [Attack and decay](#attack-and-decay)).
- `F6 xx` with `xx` up to `63` selects entry `xx` of a table of samples that
  the game can pass to the driver with a song request. Init and every plain
  request clear the address of the table. Only the live stage of *Pichi Pichitto
  Live Start!* passes a table (see [Live stages](#live-stages)). `F6` with a
  higher value changes the playing note's sample from the table at once. No
  song with a table uses that, and `agbsf2` leaves it out.
- `FA xx` with any `xx` but 0 holds the track back if a setting that the game
  passes to the driver is 0 when `FA` runs. The output record of a held track has its
  retrigger byte and flags cleared every frame. The output stage then neither
  starts nor changes the track's notes. A song loop keeps the hold. `FA 00`
  releases the track, and so does the end of the track. A song request sets the
  setting to `FF`, and `FA` then holds nothing back. *Pichi Pichitto Live
  Start!* sets the setting to 0 after the requests of its live stage (see
  [Live stages](#live-stages)).
- A pitch bend applies to the playing note only.

Prince of Tennis 2004 differs from Live Start in three more ways:

- A sample byte is one byte. `F0`-`FF` are samples 240 to 255.
- The track records are 28 bytes, against 32 in the other builds. A record has no
  copy of the 16-bit sample number of the note. A bend or a vibrato step therefore puts the
  duty byte in bytes 6 and 7 of the output record. The duty byte contains the low
  byte of the sample number. Everything after the track records is `0x40`
  bytes lower: the voice records at `+0x7E0` from the mixer, and the echo buses
  at `+0x94C`.
- The output stage sets a playing voice's levels only when the track
  retriggers the voice. A pan change without a note waits for the next note or
  volume command.

The songs of *Pichi Pichitto Live Start!* use `9F`, `F6`, `FA` and samples
numbered from `100` up, and some of the samples are packed.

## Song table

The song table is an array of 36-byte entries:

| Offset | Size | Meaning |
|---|---|---|
| 0 | u32 | base address of the song's sequence data (often odd; read as two halfwords) |
| 4 | 16 × u16 | offset of each track's data from the base address |

Song data follows the table directly. The table therefore ends where the data
of the first song begins. In `BY6J` the table is at `0x08194210` and has room for 24 entries.

## Track data

Every track is a byte stream of the form

```
<delay> { <command> <delay> }*
```

A track begins with a delay. Each command is followed by the delay before the
next command. A delay of 0 runs the next command in the same frame.

### Delays

| First byte | Length | Value |
|---|---|---|
| `00`-`DF` | 1 | the byte |
| `Ex yy` | 2 | `x << 8 \| yy` (12 bits) |
| `Fx lo hi` | 3 | `hi << 8 \| lo` (the low nibble of the first byte is ignored) |

Delays are scaled by a tempo value: `frames = (delay × tempo + frac) >> 8`.
`frac` is the low byte of the previous result. All tracks share one `frac`.
The tempo starts at `0x100`. Delays therefore count frames at first.

### Pitch units

Pitches are counted in 1/32 of a semitone. On sample tracks, pitch 0 plays a
sample at its sample rate. On the square and wave tracks, pitch `n × 32` is note
`n`. The noise track uses raw note numbers instead (see [PSG](#psg-channels)).

### Commands

| Bytes | Command | Effect |
|---|---|---|
| `00`-`7F` | duty/wave | sets the track's *duty byte* to `op - 4` |
| `80`-`8F` | duty/wave | sets the duty byte to `(op & 0x3F) << 6` (low byte) |
| `90`-`9F` | - | handled by code that agbsf2 doesn't model |
| `A0`-`A7 ss` | note | sample note, volume `op & 7`, sample `ss` |
| `A8`-`AF vv ss` | note | sample note, volume `vv`, sample `ss` |
| `B0`-`B7 ss tt` | note | as `A0`, plus signed semitone offset `tt` |
| `B8`-`BF vv ss tt` | note | as `A8`, plus signed semitone offset `tt` |
| `C0`-`C7` | volume | volume `op & 7` |
| `C8`-`CF vv` | volume | volume `vv` |
| `D0`-`D7 nn` | PSG note | volume `op & 7`, note `nn` |
| `D8`-`DF vv nn` | PSG note | volume `vv`, note `nn` |
| `E0`-`EF` | rest | silences the track (the low nibble is ignored) |
| `F0 pp` | pan | `00` = left, `40` = centre, `7F` = right |
| `F1 dd` | vibrato | depth `dd >> 1`; 0 turns vibrato off |
| `F2 bb` | pitch bend | bend = `bb - 0x40` in 1/32 semitones (±2 semitones), for `bb` up to `7F` |
| `F2 xh ll` | pitch bend | a first byte above `7F` reads the next one too: bend = `0xhll - 0x400` (-32 to +96 semitones), and `x` is ignored |
| `F3` | loop point | sample tracks (4-15) skip the next 2 bytes |
| `F4`-`F6`, `FA`-`FC` | - | handled by code that agbsf2 doesn't model |
| `F7 ff` | echo feedback | echo bus 0 feedback = `ff`/256 |
| `F8 dd` | echo delay | echo bus 0 delay = `dd × 32` mixer samples |
| `F9 vb` | echo routing | DirectSound voice `v` (0-11) to echo bus `b - 1` (0-2); `b` = 0 takes it off buses 0 and 1 |
| `FD` | end track | the track stops for good |
| `FE` | end track | the track stops until the song loops |
| `FF 00` | stop song | everything is silenced on the next frame |
| `FF nn` | loop song | `nn` ≠ 0: all tracks jump back to their loop points |

A sample number above `EF` selects code that agbsf2 doesn't model.

#### Notes on sample tracks (`A0`-`BF`)

A note stops the voice's previous sound and starts sample `ss` with pitch
`tt × 32`, plus any retained pitch bend. The sample number indexes the sample
table. Each note selects a sample and a semitone offset from its original
pitch. The driver has no instruments or key splits. Sequences implement
multi-sampled instruments by choosing samples for each pitch range, and drum
tracks by choosing a sample for each hit.

#### Volume (`C0`-`CF`)

On a sample track this changes the volume of the sound already playing,
without restarting it. On a PSG track it retriggers the channel at the current
note with the new volume. The frequency written by a PSG retrigger leaves out
the pitch bend, unless vibrato is active.

#### PSG notes (`D0`-`DF`)

On the square and wave tracks the pitch becomes `nn × 32`, plus the bend in the
builds that keep it. On the noise track `nn` is used as-is to index the noise
table, and bend isn't applied. A song can play note `FF` as if it were note -1.
That note is past the end of the frequency table (see [PSG](#psg-channels)).

#### Rest (`E0`-`EF`)

On a sample track the voice is stopped immediately, with no release. On a PSG
track the channel's volume is set to 0.

#### Duty byte (`00`-`8F`)

On the square tracks bits 6-7 of the duty byte set the duty cycle: `80`,
`81`, `82` and `83` select 12.5%, 25%, 50% and 75%. On the wave track the duty
byte is the wave number: `04` selects wave 0 and `05` wave 1. The duty byte
takes effect at the next note.

#### Vibrato (`F1`)

Each frame while vibrato is on, the phase advances by `0x18` (of 256), and the
pitch offset is `(sin[phase] × depth) >> 12`, with `sin[i] = trunc(4096 ×
sin(2πi/256))`. That's a 5.6 Hz vibrato of ±`depth`/32 semitone. The phase
isn't reset by new notes.

#### Pitch bend (`F2`)

The bend applies to the note already playing. In the builds that keep it (see
[Builds of the driver](#builds-of-the-driver)), it also applies to every later
note and is added to vibrato, until the next `F2` or a song loop. The other
builds don't store it. The next note or vibrato step of the track then plays
without it.

#### Pan (`F0`)

Pan is stored as two levels, each 0-63: `pp ≤ 3F` sets right = `pp`, left =
63, and `pp ≥ 40` sets right = 63, left = `7F - pp`. Sample voices scale each
side through the volume table. PSG channels can only be on or off per side: a
channel plays on both sides when the two levels are equal, and on the louder
side otherwise.

### Track start, end and looping

* **Start.** On its first frame, each track silences its channel and reads its
  initial delay.
* **`F3` loop point.** This moves the track's start to just after the `F3`
  (and after the 2 extra bytes on sample tracks). `FF nn` later resumes the
  track from there.
* **`FF nn` loop.** When any track reaches `FF nn`, every track is reset in the
  same frame, and the whole frame is then run again from track 15 down. The
  reset works like this:
  * Each track returns to its loop point, or to its start if it has no `F3`.
  * Volume and bend return to 0, vibrato turns off, and the duty byte returns
    to 0. Tracks 0 and 1 are set to `80` (50% duty).
  * Tracks that ended with `FE` resume, and tracks that ended with `FD` stay
    silent.
  * Pan and echo settings stay as they are.
* **`FE` / `FD`.** These end a track. The channel is silenced unless something
  else already happened on the track in the same frame.

Tracks are processed from 15 down to 0 each frame.

## Samples

The sample table is an array of pointers, indexed by the sample number of a
note. Each sample is a 12-byte header followed by signed 8-bit PCM:

| Offset | Size | Meaning |
|---|---|---|
| 0 | u32 | step per mixer sample at pitch 0, 4.12 fixed point |
| 4 | s32 | length in samples |
| 8 | s32 | loop start, or -1 for a one-shot sample |
| 12 | s8[] | PCM data |

A looped sample plays `[0, length)` and then repeats `[loop start, length)`. A
negative length selects another mode. In the builds with 24-byte voice records,
that mode is a packed sample (see [Packed samples](#packed-samples)). `agbsf2`
reads packed samples only in the later builds of Live Start and Prince of Tennis
2004. None of the checked games of the other builds plays one.

The step is relative to the output rate of the mixer. The playback rate is
therefore `mixer rate × step / 4096`.

At pitch `p` the step becomes `(step × pitchtab[p]) >> 12`, where
`pitchtab[p] = round(4096 × 2^(p/384))` (the table is centred on index 0).

In `BY6J` the sample table is at `0x081D7F30`.

### Packed samples

The length of a packed sample has bit 31 set. The note start uses the low 24
bits of the length. A table of 32 pairs of samples follows the 12-byte header, 2 bytes
for each pair. The packed data starts at `+0x4C` of the header. The mixer reads
the data as a stream of codes:

| Byte | Plays |
|---|---|
| `C0`-`7F` | the byte, as one sample |
| `A0`-`BF` | pair `n - A0` of the table: its high byte, then its low byte |
| `90`-`9F` | `n` zero samples |
| `80`-`8F` | `n` samples of the value in the next byte |

In a run, `n` is the code's low nibble, or the next byte if the nibble is 0. A
run with a count of 0 plays one sample. The mixer then skips the next code if
that code is a single sample.

`agbsf2` unpacks a packed sample as the mixer plays it at one sample of the
data for each sample of the mix. At other steps the mixer departs from that in
three ways:

- When it moves on by more than one sample, it skips single samples one by one.
  A run or pair that it meets ends the skip, and plays from its first sample.
- After a run or pair that it moves past by more than one sample, it lands one
  sample short.
- At the start of each block of 16 samples, a pair that the mixer has just
  reached plays its low byte in place of its high byte.

These depend on the step and on where the blocks fall. A SoundFont can't
follow either. The mixer can't loop a packed sample. At the loop, the mixer
goes back to the byte of the loop start in the packed data. It goes back there
again on every step after that. `agbsf2` reads a packed sample only when it has no loop.

## Mixer

The mixer runs from the FIFO A DMA interrupt. Timer 0 sets the output rate:
`0xFCE2` in `BY6J`, for 16777216 / 798 = 21024 Hz. Each call makes 16
samples for each side, 22 calls per frame. FIFO A is the right channel and
FIFO B the left.

For each playing voice the mixer adds `(sample × level) / 128` for each side:

* Resampling is nearest-sample, with no interpolation.
* The per-side levels come from the volume table.
* The sums are written to the FIFOs as 8-bit values with no clipping: they wrap
  around.

The mixer also supports 4-bit ADPCM and reverse playback through voice flags.

### Voice records

The sequencer starts, changes and stops voices through the 12 voice records at
`0x0300546C`, 28 bytes each. These fields are known:

| Offset | Size | Meaning |
|---|---|---|
| 0 | u32 | address of the next PCM byte (a note start sets the address of the sample data) |
| 4 | u32 | samples left before the end of the sample |
| 8 | u16 | step per mixer sample, 4.12 fixed point, with the note's pitch applied |
| 10 | u16 | the mixer's fractional position, in 1/4096 of a sample |
| 12 | u8 | low byte of the sample number |
| 14 | u8 | flags: `80` playing, `40` looped |
| 15 | u8 | volume |
| 16 | u8 | level on the right |
| 17 | u8 | level on the left |

In the builds with 24-byte voice records, byte 18 contains the value that the
current run or pair of a packed sample plays next. Byte 19 contains the number
of samples left in the run or pair. Bytes 20-23 contain the address of the
sample data. A loop goes back to that address. The table of pairs of a packed
sample comes just before it.

### Volume table

`level = table[volume × 64 + pan level]`, where the pan level is 0-63 for each
side. The level is close to `volume × pan / 63` but isn't a simple rounding of
that product. `agbsf2` therefore reads the table itself. The
full-level column (`pan = 63`) equals the volume. In `BY6J` the table is at
`0x084BF1B8`.

### Echo

The driver has three echo buses, and `F9` routes a voice to one of them.
`F9 v0` takes a voice off buses 0 and 1, but not bus 2. The mixer treats a voice
routed to both buses 0 and 1 as routed to bus 1 alone. Each bus is a feedback
delay line: `y[n] = x[n] + feedback × y[n - delay]`. The output of the line is
added to the dry mix.

* **Delay line.** It stores 8-bit stereo samples in 16-sample blocks, 256 blocks
  in all. The init puts the line of bus 0 at the start of the work buffer that
  the game passes to the init. Buses 1 and 2 share the 8 KB after it. `F9` only
  routes a voice to a bus that has a line. On a bus without one, `F9` clears the
  routing of that bus.
* **Delay (`F8`).** `F8 dd` sets a delay of `2 × dd` blocks, or `dd × 32` samples:
  about 137 ms for `5A` and 193 ms for `7F`.
* **Feedback (`F7`).** `F7 ff` sets the target feedback to `ff`/256. The
  current feedback moves towards it by 8 per frame.

## PSG channels

**Squares.** On a note, the driver writes `SOUNDxCNT_H` as `volume << 12 |
duty byte` (no envelope) and `SOUNDxCNT_X` from the frequency table with the
restart bit. Pitch updates write only the frequency. The frequency table contains
one `u16` for each 1/32 semitone of notes 0 to 83, 2688 in all. Bit 15 is the
restart flag. Note 0 is C2 (65.4 Hz), and the table ends at B8. In `BY6J` the
table is at `0x084C59B8`.

**Wave.** The driver triggers the wave channel once at init and never
restarts it. For each note it loads a 16-byte wave RAM image into the idle
bank and switches banks. The images come from the wave table at
`wave table + ((wave << 4) + volume) × 16`. Each wave is stored pre-scaled to
16 volumes (peak ≈ volume), and quiet notes use the coarser rows. The 32 steps
of a row play at half the rate of a square channel. Wave note 0 therefore has a
32-step period at C1. The table is at `0x084BE7A0` in `BY6J`. The frequency
comes from the frequency table of the squares. The driver writes only its low
11 bits. A wave number from `80` up picks wave `& 7F` from a second table. The
sound effects of the game use that table. In the older revisions a panned
square 2 plays on the wave channel, with its duty byte `80` as the wave (see
[Output stage in WCT 2004](#output-stage-in-wct-2004)). It therefore plays wave
0 of that table.

**Pitches past the frequency table.** The driver doesn't check the pitch that
it looks up. Any pitch outside notes 0 to 83 reads outside the table. From
note 84 up the driver reads the words after the table. They contain other data,
not higher frequencies. A song can play note `FF` as if it were note -1. That
note reads the word 16 KB past the start of the table. Whatever data is there
sets the frequency. On a square channel, the channel restarts only if bit 15 of
the word is set. Otherwise a note that's playing goes on at the new frequency,
and a silent channel stays silent. `agbsf2` does the same.

**Noise.** Each note writes `SOUND4CNT_L = volume << 12` and
`SOUND4CNT_H = noise table[note]`. The table contains the `NR43` value with the
restart bit (`0x8010`, `0x8020`, ... in `BY6J`, table at `0x084BF1A0`). Every
output of the noise track, even a pan change, writes them again. A word without
the restart bit doesn't restart the channel: a silent channel stays silent,
and a playing one goes on with the new setting. The older revisions index the
table with the pitch of the noise track, the note × 32. Vibrato and bends
therefore move the entry that they read. A pitch below 0 reads the words before
the table.

**Pan changes in the Ultimate Masters revision.** The output that a pan change
writes has pitch 0 unless vibrato is on. A square or wave note without vibrato
drops to note 0 (C2) and stays there until the track's next note. On the noise
track, a pan change writes entry 0 of the noise table. Entry 0 has the restart
bit in each game checked. The channel therefore restarts with the setting of
entry 0. `agbsf2` does the same.

### Level of PSG against samples

The hardware mixes a PSG channel at volume *v* (0-15) at the same level as a
full-scale DirectSound voice at level 2*v* (under the driver's `SOUNDCNT`
settings: PSG at 100% and master volume 7).

## The WCT 2004 revision

*Yu-Gi-Oh! World Championship Tournament 2004* has an older revision of the
driver. Its song table, samples, pitch table, vibrato and PSG frequency table
work as described above. The sequencer also processes each frame the same way:
tracks run from 15 down to 0, a loop runs the frame again, and a track's first
frame silences its channel and reads its first delay. The delays and commands
differ. So do parts of the output stage and the mixer.

In `BYWP` the routines are:

| Address | Routine |
|---|---|
| `0x08096694` | sound init |
| `0x08098138` | request a song: `r0` = song number |
| `0x08097F2C` | start the requested song (the game calls it every frame) |
| `0x08097BE8` | per-frame entry |
| `0x08096E88` | the sequencer, with the command reader at `0x0809722C` and the output stage from `0x080977C6` |
| `0x08098938` | start a DirectSound note |

The driver's state is at `0x03005390` (the track records start at `+0x1C`, 28
bytes each), the voice records at `0x030056E0` (28 bytes each, laid out as in
the Ultimate Masters revision) and the echo buses at `0x0300587C`. The init
copies the mixer to `0x03004F18`. The song table is at `0x0893C7D0`, and the
sample table is at `0x0894E950`.

*Mermaid Melody Pichi Pichi Pitch* (`BMAJ`) has a build of this revision with
24-byte voice records. They have the layout of the 24-byte records of the
Ultimate Masters builds. The commands and outputs of the build are the same.

### Delays in WCT 2004

| First byte | Length | Value |
|---|---|---|
| `00`-`DF` | 1 | the byte |
| `bb yy`, `bb` from `E0` up | 2 | `(bb & 1F) << 8 \| yy` (13 bits) |

The tempo is `0x100`, as in the Ultimate Masters revision. A delay is therefore
a number of frames.

### Commands in WCT 2004

| Bytes | Command | Effect |
|---|---|---|
| `00`-`8F` | duty/wave | low nibble `n`: 0-3 set the duty byte to `n << 6`. 4-F set it to the wave `n - 4`, and load that wave into the wave RAM at once, at the track's volume |
| `90`-`9F lo hi nn` | call | plays `nn` commands from offset `0xhilo` of the track's data (see [Calls](#calls)) |
| `A0`-`AF ss` | note | sample note, volume `op & F`, sample `ss` |
| `B0`-`BF ss tt` | note | as `A0`, plus signed semitone offset `tt` |
| `C0`-`CF` | volume | volume `op & F` |
| `D0`-`DF nn` | PSG note | volume `op & F`, note `nn` |
| `E0`-`EF` | rest | silences the track, and stops its attack and decay |
| `F0 xy` | pan | left level `x`, right level `y` (0-F each) |
| `F1 dd` | vibrato | as in the Ultimate Masters revision |
| `F2 bb` | pitch bend | bends the playing note by `bb - 0x40` (1/32 semitones) for any `bb`, and isn't kept |
| `F3` | loop point | as in the Ultimate Masters revision |
| `F4 rr` | attack rate | see [Attack and decay](#attack-and-decay) |
| `F5 rr` | decay rate | see [Attack and decay](#attack-and-decay) |
| `F6 ii` | instrument | picks the samples of the notes from a table that the game can pass to the driver with a song request (see [Live stages](#live-stages)) |
| `F7 ff` | echo feedback | echo bus 0 feedback = `(ff & 7F)`/256 |
| `F8 dd` | echo delay | echo bus `dd >> 7` delay = `(dd & 7F) × 32` mixer samples |
| `F9 vb` | echo routing | as in the Ultimate Masters revision |
| `FA xx` | volume scale | `xx` other than 0 scales the later volumes of the track by a setting that the game passes to the driver, and `FA 00` ends that (see below) |
| `FB`, `FC` | - | nothing |
| `FD` | end track | the track stops until the song loops |
| `FE` | loop song | all tracks go back to their loop points |
| `FF` | stop song | everything is silenced on the next frame |

`FA` with any `xx` but 0 sets the track's scale to the setting, and `FA 00`
sets it to `FF`. A scale under `80` sets the volume of a note or a volume
command to `volume × scale / 16`, rounded down. A note or a volume command that the scale
takes to 0 does nothing, and the note that's playing goes on. A PSG note plays
at the scaled volume, even at 0. A song request sets the setting to `FF`, and
a song's start sets each track's scale to `FF`. A song loop keeps the scale.
*Mermaid Melody Pichi Pichi Pitch* sets the setting to 0 after the requests of
its live stage (see [Live stages](#live-stages)).

The notes store their volume as it is. The output record of a sample note
starts the voice without the retrigger byte. The output stage sets the levels of
the voice every frame anyway. On the noise track, the pitch of a PSG note is the
note × 32, as on the other PSG tracks.

A song starts with every track panned to the centre (`FF`) and without attack or
decay. A song loop resets the tracks as in the Ultimate Masters revision, but
they keep their pan and their attack and decay rates.

#### Calls

`9x lo hi nn` moves the track to offset `0xhilo` from the track start in the
song table. The track reads a delay there. It then plays `nn` commands, each
followed by its delay. After the last command, the track goes back and reads
the delay that follows the call, not the delay of the fragment. `90`-`9E` also
set the duty byte or the wave from their low nibble, as `00`-`8F` do, before
the first delay of the fragment. `9F` doesn't, and with `nn` = 0 it plays 256 commands. A call inside a
fragment replaces the return position of the first. A call runs `F3`, `FD`,
`FE` and `FF` in the fragment as if they were in the track.

#### Attack and decay

`F4` and `F5` set a track's attack and decay rates. A PSG note starts an attack
countdown from `FF` if the attack rate isn't 0, and a decay countdown from `FF`
if the decay rate isn't 0. Each frame, the attack countdown goes down by the
attack rate, and the track plays at `((0x10F - countdown) × volume) >> 8`,
from about 1/16 of the note's volume up to 1.06 times it. Once the attack
countdown reaches 0, the decay countdown goes down by the decay rate, and the track plays at
`((countdown + 0xF) × volume) >> 8`, back down to about 1/16. Each time the
level changes, the channel is triggered again at the new level.

### Output stage in WCT 2004

The output stage runs at the end of the sequencer. It differs from the output
stage of the Ultimate Masters revision in these ways:

* **Levels.** The volume table has 16 columns for each volume, one for each pan
  level, and a voice's level for a side is `table[volume × 16 + level]`. The
  mixer plays a level of *n* at *n*/16 of full scale. The mixer of the Ultimate
  Masters revision plays it at *n*/128. The output stage sets the levels of a
  playing voice every frame, from the volume and pan of its track. In `BYWP` the table is at
  `0x08C53CE0`.
* **PSG pan.** The output stage writes `NR51` every frame, from the pan of
  each PSG track. `FF` plays a channel on both sides, `0F` on the right and
  `F0` on the left. Any other pan uses the next PSG channel too. The channel of
  the track plays the right side, at volume `table[volume × 16 + y]`. The next
  channel plays a copy of the track output on the left side, at
  `table[volume × 16 + x]`, in place of the output of its track. When square 1
  goes back to the centre or one side, and the track of square 2 plays nothing
  in that frame, square 2 gets a copy of the square 1 output at volume 0. A
  retrigger silences square 2, and a pitch change moves it to the pitch of
  square 1. The wave channel is left
  out of `NR51` until it plays a note at a volume above 0.
* **Wave.** Every output of the wave track, a pitch change included, loads the
  wave RAM image for the wave and volume of the record into the idle bank and
  switches banks. It also sets the volume code of the channel to 100%, or to 0%
  for volume 0. The record of a bend contains wave 0. A bend therefore loads the
  image of wave 0, whatever wave the note had. The record of a vibrato step
  contains the wave of the track.
* **Noise.** The noise table is indexed with the pitch of the track, the note × 32.

### Mixer in WCT 2004

The mixer runs at 21024 Hz. Its echo buses work as in the Ultimate Masters
revision, except that the sequencer masks the routing that it passes to the
mixer with `0x1FFF`. The mask keeps only voices 5-11 of bus 0. Voices 0-4
routed to bus 0 therefore play dry.

## The Kitaro revision

*Gegege no Kitarou: Kikiippatsu! Youkai Rettou* (`BGKJ`) has a revision of
the driver that works like the WCT 2004 revision. Its songs, delays, output
records, output stage and voices are those of WCT 2004. These parts differ:

* **Commands.** `FA` does nothing and takes no argument, as `FB` and `FC` do.
  The other commands are those of WCT 2004.
* **Duties.** `00`-`8F` and the calls `90`-`9E` store a duty of 0-3 as it is.
  WCT 2004 shifts it into bits 6-7. The output stage writes the byte to the
  length and duty bits of a square, as in WCT 2004. The duty bits stay 0.
  Every square note therefore plays at a duty of 12.5 %.
* **Echo.** The init takes no work buffer. After the init, the game sets the
  delay lines of the echo buses through `0x0803853C`. `r0` is the line of bus
  0, and buses 1 and 2 share the 8 KB after it. `BGKJ` passes `0x02002304`. The
  mixer reads the routing of each bus from the record of the bus, and takes
  every voice from bus 0.

The reader compares `FC`, `FF`, `FE`, `EF`, `F9`, `F8`, `F7`, `F6`, `F5`, `F4`,
`F3`, `F2`, `F1` and `F0`.

In `BGKJ` the routines are:

| Address | Routine |
|---|---|
| `0x08035F70` | sound init: `r0` = the address where the init writes the mixer's address |
| `0x0803853C` | set the delay lines of the echo buses: `r0` = line of bus 0 |
| `0x080378AC` | request a song: `r0` = song number |
| `0x08037900` | request a song unless it's playing: `r0` = song number |
| `0x08037718` | start the requested song (the game calls it every frame) |
| `0x080373F0` | per-frame entry |
| `0x08036794` | the sequencer, with the command reader at `0x08036AD0` and the output stage from `0x08037064` |
| `0x08037FE0` | start a DirectSound note |

The driver's state is at `0x03000424` (the track records start at `+0x1C`, 28
bytes each), the voice records at `0x03000768` (28 bytes each) and the echo
buses at `0x03000904`. The init writes `0x03000000` as the address of the mixer. The
song table is at `0x080CD220`, and the sample table is at `0x080F5F00`.

## The Minna no Ouji-sama revision

*Minna no Ouji-sama* (`BMOJ`) has a revision of the driver that works like the
WCT 2004 revision. The songs, delays, output records, output stage and mixer
are those of WCT 2004. The voice records are the 24-byte records of the `BMAJ`
build. The commands are those of WCT 2004, with one more:

* **`FB xx`** sets the song volume to `xx`. A song starts at `10`, and a loop
  sets the volume back to `10`. At the end of the frame of each running track,
  after the attack or decay, the sequencer multiplies the track's volume by
  the song volume and divides it by 16. The tracks run from 15 down to 0. A
  track after the track of the `FB` plays at the new volume in that frame, and a
  track before it from the next frame.
* **Retriggers.** While the song volume isn't `10`, the output of each running
  track gets a retrigger every frame, unless the commands or the envelope of
  that frame gave it one. The retrigger has the track's duty byte and the
  note's pitch without vibrato. Vibrato therefore stops while the song volume
  isn't `10`.

The reader compares `FC`, `FF`, `FE`, `EF`, `FB`, `FA`, `F9`, `F8`, `F7`, `F6`,
`F5`, `F4`, `F3`, `F2`, `F1` and `F0`.

In `BMOJ` the routines are:

| Address | Routine |
|---|---|
| `0x0808BE34` | sound init |
| `0x0808D94C` | request a song: `r0` = song number |
| `0x0808D9A0` | request a song unless it's playing: `r0` = song number |
| `0x0808D744` | start the requested song (the game calls it every frame) |
| `0x0808D400` | per-frame entry |
| `0x0808C630` | the sequencer, with the command reader at `0x0808CA14` and the output stage from `0x0808D06A` |
| `0x0808E15C` | start a DirectSound note |

The driver's state is at `0x030004A0` (the track records start at `+0x1C`, 28
bytes each, and the song volume is at `+0x351`), the voice records at
`0x030007F4` (24 bytes each) and the echo buses at `0x03000960`. The init
copies the mixer to `0x03000000`. The song table is at `0x087E33F0`, and the
sample table is at `0x08806BF0`.

## The Rave Master revision

*Rave Master: Special Attack Force* (`BRME`) has a revision of the driver
older than that of WCT 2004. It works like the WCT 2004 revision, apart from these
differences:

* **Tracks.** A song has 12 tracks: the four PSG tracks, and 8 sample tracks
  for 8 voices. A song table entry is 28 bytes.
* **Commands.** There's no `F6`-`FA`: they do nothing, as `FB`, `FC` and
  `00`-`7F` do. `F3` has no extra bytes on sample tracks. `80`-`83` set
  the duty byte to their low nibble as it is. WCT 2004 shifts it into
  bits 6-7. A duty command therefore leaves a square at 12.5%.
* **Loops.** A song loop centres the pan of each track again.
* **Delays.** No tempo scales them. A delay is a number of frames.
* **Sample voices.** The output stage sets the levels of a voice only when the
  track output starts a note or retriggers the voice, not every frame. The
  output stage of the Ultimate Masters revision works the same way.
* **Mixer.** There's no echo. The mixer looks each sample up in a table of
  `round(sample × level / 15)` (at `0x08091F9C`). A level of 15 therefore plays
  at full scale.
* **Timer.** The init loads the setting of timer 0 (`0x0080FCE2`, 21024 Hz) from a
  literal rather than from a table.

In `BRME` the routines are:

| Address | Routine |
|---|---|
| `0x0802AF4C` | sound init |
| `0x0802C3EC` | request a song: `r0` = song number |
| `0x0802C29C` | start the requested song (the game calls it every frame) |
| `0x0802C018` | per-frame entry |
| `0x0802B5A4` | the sequencer, with the command reader at `0x0802B84E` and the output stage from `0x0802BB98` |
| `0x0802C8BC` | start a DirectSound note |

The driver's state is at `0x030040EC` (the track records start at `+0x0C`, 28
bytes each), and the voice records at `0x0300433C` (24 bytes each). The init
copies the mixer to `0x03004440`. The song table is at `0x0809A7E0`, and the
sample table is at `0x080E0EB0`.

*Groove Adventure Rave 2* (`ARIJ`) has the driver of `BRME`, 0x2B0 bytes lower
in ROM, with the same RAM. *Hikaru no Go 2* (`AKEJ`) has a build with 20-byte
voice records. The state of this build is at `0x03000770`, and the voice
records at `0x030009C0`. The second half of the init copies the mixer to
`0x03000AA0`. The wave RAM loader has no check for volume 0 (see [Locating the
driver](#locating-the-driver)). The commands and outputs are those of `BRME`.

## The Hunter X Hunter revision

*Hunter X Hunter: Minna Tomodachi Daisakusen!!* (`A8NJ`), *Tennis no
Ouji-sama 2003: Passion Red* and *Cool Blue* (`A8RJ`, `A9LJ`) and *Tennis no
Ouji-sama: Aim at the Victory!* (`AVAJ`) have a revision of the driver that
works like the Rave Master revision. Its songs, delays, output records and
mixer are those of Rave Master. These parts differ:

* **Commands.** `F6 ii` stores `ii` for the later notes of the track. If `ii`
  is above 0 and the game passed a table of samples with the request, each note
  plays entry `ii` of the table, unless the entry is 0. `F7`-`FC` do nothing.
* **Lists of samples.** The game can also pass a list of samples for one of the
  tracks with a request. Each note of that track then plays the next sample of the
  list, from the second sample table.
* **Loops.** A song loop keeps the pan of each track, as in WCT 2004.
* **Mixer rate.** The game picks one of five mixer rates, from 21024 Hz down to
  10512 Hz. The note start scales the step of each voice to keep its pitch.
  `A8RJ` and `A9LJ` mix at 15768 Hz. `AVAJ` changes the rate with its modes.
  `agbsf2` uses the first rate of the table, 21024 Hz.
* **Voices.** A voice record is 28 bytes. The last 4 bytes contain the address
  of the mixing loop of the voice.

The reader compares `FC`, `FF`, `FE`, `EF`, `F6`, `F5`, `F4`, `F3`, `F2`, `F1`
and `F0`. The note start loads the sample table with the same registers as WCT
2004. The square 1 and noise outputs load their tables with other registers
than in Rave Master.

In `A8RJ` the routines are:

| Address | Routine |
|---|---|
| `0x080983B0` | sound init: `r0` = the address where the init writes the mixer's address |
| `0x0809823C` | set the mixer rate: `r0` = 0-4 |
| `0x080999B4` | request a song: `r0` = song number, `r1` = fade-in rate (0 plays it at full volume at once) |
| `0x08099A6C` | request a song with a list of samples: `r1` = the list, `r2` = the track |
| `0x08099A90` | request a song with a table for `F6`: `r1` = the table |
| `0x08099AA8` | request a song with both: `r1` = the list, `r2` = the track, `r3` = the table |
| `0x08099854` | start the requested song |
| `0x080995A4` | per-frame entry |
| `0x08098B04` | the sequencer, with the command reader at `0x08098DBA` and the output stage from `0x0809919A` |
| `0x0809A0AC` | start a DirectSound note |

The driver's state is at `0x03000330` (the track records start at `+0x1C`, 28
bytes each), and the voice records at `0x030005E0`. The init copies the mixer
to `0x03000000`. The song table is at `0x089AC770`, the sample table at
`0x089C2920`, and the second sample table at `0x08A90D80`.

`A9LJ` has the driver of `A8RJ`, 0xD0 bytes higher in ROM, with the same RAM.
`A8NJ` and `AVAJ` have other builds, with their states at `0x03002054` and
`0x03005184`. Only `A8NJ` makes requests with lists of samples or tables. The
game builds them in RAM at run time. `agbsf2` converts each song as a plain request
plays it.

## The Captain Tsubasa revision

*Tennis no Ouji-sama* (`ATIJ`), *Kami no Kijutsu: Illusion of the Evil Eyes*
(`AG2J`), *Captain Tsubasa: Eikou no Kiseki* (`AKYJ`) and *Groove Adventure
Rave* (`ARVJ`) have a revision of the driver between the Eternal Duelist and
Rave Master revisions. Its songs, delays, `F0`, output records, voices and
mixer are those of the Eternal Duelist revision (see [The Eternal Duelist
revision](#the-eternal-duelist-revision)). These parts differ from Eternal
Duelist, and most of them follow Rave Master:

* **Commands.** `F4` and `F5` set the attack and decay rates of PSG notes, as
  in Rave Master (see [Attack and decay](#attack-and-decay)). `F1` and
  `F6`-`FC` are vibrato.
* **Notes.** Every PSG note retriggers its channel. There's no legato.
* **Bends and vibrato.** They add the pitch flag to the flags of the output record.
  Only that flag changes the pitch of a DirectSound voice. In Eternal Duelist
  any flag changes it, even the flag of a volume command.
* **Calls.** Returning from a call clears the return position.
* **Tracks.** A track record is 28 bytes. It has 4 bytes more than in Eternal
  Duelist, for the attack and decay rates and countdowns.

The reader compares `FC`, `FF`, `FE`, `EF`, `F5`, `F4`, `F3`, `F2`, `F0` and
`F0`. The reader of Rave Master compares `F1` in place of the first `F0`. The
square 1 output loads the frequency table with the same registers as Rave
Master.

In `ATIJ` the routines are:

| Address | Routine |
|---|---|
| `0x08075950` | sound init: `r0` = 0 |
| `0x08076B10` | request a song: `r0` = song number, `r1` = fade-in rate (0 plays it at full volume at once) |
| `0x080769D4` | start the requested song |
| `0x0807682C` | per-frame entry: runs the sequencer, then the mixer |
| `0x08075ED8` | the sequencer, with the command reader at `0x08076162` and the output stage from `0x080764DE` |
| `0x08076E9C` | start a DirectSound note |
| `0x080767A0` | the routine of the DMA 1 interrupt: moves the position of the mixer on |
| `0x08077054` | the mixer (ARM, in ROM) |

The driver's state is at `0x03000000` (the track records start at `+0x08`, 28
bytes each), and the voice records at `0x030001C4`. The song table is at
`0x083BAD10`, and the sample table is at `0x083C98B0`.

`AG2J` has the driver of `ATIJ`, with its state at `0x030008A0`. `AKYJ` and
`ARVJ` have another build, with their states at `0x03001AE4` and `0x03004074`.
The output stage of `ATIJ` puts square 2 on both sides while bit 6 of the setting at
`+0x1B0` of the state is set. The output stage of this build has no such mode.

## The Eternal Duelist revision

*Yu-Gi-Oh! The Eternal Duelist Soul* (`AY5E`) and *Yu-Gi-Oh! Worldwide
Edition* (`AYWE`) have a revision of the driver older than that of Rave Master.
The two games have the same driver. In `AYWE`, the code of the driver is
0x15A44 bytes higher in ROM and its RAM 0x100 bytes lower. The revision works
like the Rave Master revision, apart from these differences:

* **Tracks.** A song has 10 tracks: the four PSG tracks, and 6 sample tracks
  for 6 voices. A song table entry is 24 bytes.
* **Delays.** A delay byte up to `EF` is the delay itself, and `Fx yy` is
  `(x << 8) | yy` (12 bits).
* **Commands.** `F4`-`FC` are vibrato, as `F1` is, and take a byte. `F0`
  works differently on PSG and sample tracks (see below).
* **Legato.** A PSG note at the volume the track already has doesn't
  retrigger the channel. It only changes the frequency. A run of such notes
  therefore moves from note to note without restarting. On the wave track, the
  record of the note contains wave 0, as the record of a bend does. The output
  stage therefore loads the image of wave 0.
* **Bends and vibrato.** `F2` and vibrato set the flags of the output record to
  1, in place of the flags that the commands of the frame set. On a sample
  track, that cancels the start of a note in the same frame. The voice then
  plays on with its old sample, at the step that the output stage computes from
  the sample and pitch of the new note.
* **Calls.** Returning from a call leaves the return position intact. The
  counter continues to decrement. Every 256 commands, the track therefore jumps
  back to that position.
* **Output records.** They're 8 bytes, with no pan: the pitch, duty byte,
  volume, retrigger byte, flags and sample.
* **Sample voices.** A voice has one volume, 0-15, for both sides. The output
  stage sets it whenever the record of the track has the retrigger byte set. A
  record with any flag besides a note start or a stop makes the output stage
  compute the step of the voice again.
* **Mixer.** It works differently (see [Mixer in Eternal
  Duelist](#mixer-in-eternal-duelist)), and there's no echo.

`F0 xx` on a PSG track sets the `NR51` bits of the track. The output stage
writes `NR51` every frame as the OR of the bits of the four PSG tracks. One
track can therefore put the channel of another track on a side. A song starts
with the bits of each PSG track set for the channel of that track on both sides
(`0x11 << track`), and a loop sets them back.

`F0 xy` on a sample track sets the volume of the track to `y`. It also puts `x`
into the output record of the next track, with the retrigger byte set. That
track has had its turn in the frame already. The next voice then plays at
volume `x` until its track sets the volume again. On the last track, the next
record would be the variables of the driver. There `agbsf2` ignores the command
and prints a warning.

In `AY5E` the routines are:

| Address | Routine |
|---|---|
| `0x0807D578` | sound init: `r0` = 0 |
| `0x0807E690` | request a song: `r0` = song number, `r1` = fade-in rate (0 plays it at full volume at once) |
| `0x0807E554` | start the requested song |
| `0x0807E3B0` | per-frame entry: runs the sequencer, then the mixer |
| `0x0807DB58` | the sequencer, with the command reader at `0x0807DDC2` and the output stage from `0x0807E072` |
| `0x0807E918` | start a DirectSound note |
| `0x0807E324` | the routine of the DMA 1 interrupt: moves the position of the mixer on |

The driver's state is at `0x03005210` (the track records start at `+0x08`, 24
bytes each), and the voice records at `0x030053AC` (16 bytes each). The song
table is at `0x080E09D0`, and the sample table is at `0x0811B420`.

*Hikaru no Go* (`AHKJ`) and *Yu-Gi-Oh! Duel Monsters 6 Expert 2* (`AY6J`) have
the driver of `AY5E`. In `AHKJ` the driver is 0x1FC74 bytes lower in ROM and
0x4A4C bytes lower in RAM. In `AY6J` it is 0xC2C4 bytes higher in ROM and 0x100
bytes lower in RAM.

### Mixer in Eternal Duelist

The mixer is ARM code that runs from ROM at `0x0807EAD0`. The per-frame entry
calls it after the sequencer, through a veneer at `0x08080A18`. Timer 0 runs
the FIFOs at 19996.68 Hz (setting `0x0080FCB9`, loaded by the init from a
literal). The mixer fills a ring buffer of 704 samples for each FIFO, at
`0x03005414` for FIFO A and `0x03005734` for FIFO B, up to a position that the
DMA 1 interrupt moves on 16 samples at a time. Voices 0-2 go into FIFO A and
voices 3-5 into FIFO B, and both FIFOs play on both sides. A voice at volume
*v* plays at (*v* + 1)/16 of full scale, and not at all at volume 0. The mixer
adds the voices up without clipping.

## The Dungeon Dice Monsters revision

*Yu-Gi-Oh! Dungeon Dice Monsters* (`AYDE`) has the oldest revision of the
driver that `agbsf2` supports. The sequencer still sends the output of each
track to an output stage once per frame. Most of the details differ:

* **Tracks.** A song has 8 tracks: the four PSG tracks, and 4 sample tracks
  for 4 voices. A song table entry is 20 bytes.
* **Delays.** As in the Eternal Duelist revision, a delay byte up to `EF` is
  the delay itself, and `Fx yy` is `(x << 8) | yy`.
* **Pitches.** A pitch is an entry of a table, and a note is 16 entries. The
  entries of the PSG frequency table are the values that the output stage
  writes to the frequency register of a channel. The table in `AYDE` has 1152
  entries for notes 0-71, from C2 on. Then come 12 noise settings, and a part
  for vibrato with 4 entries for each note. The sample period table contains
  the timer period of each pitch of a sample note, from note 0 at 2500 Hz on.
  Its entry 0 stands for the sample rate of a sample.
* **Output records.** They're 12 bytes, on the stack: the pitch, duty byte,
  volume, retrigger byte and flags, and at bytes 6-7 an envelope for a square
  channel. There's no pan.
* **Sample voices.** A voice has one volume, 0-15, for both sides, and plays
  its sample once, at a rate that its FIFO sets. There's no mixer (see [FIFOs in
  Dungeon Dice Monsters](#fifos-in-dungeon-dice-monsters)).
* **Sound effects.** Six more tracks play sound effects. While they play, they
  take over square 2, the noise channel and the 4 voices.

These are the commands of this revision:

| Command | Description |
|---|---|
| `00`-`6F` | the duty byte, or on a sample track the sample that `9x` plays |
| `70`-`7B` | load wave `x` into the wave RAM at once |
| `7C`-`7F` | duty `x & 3` |
| `8x` | vibrato at depth `x`, or off at 0 |
| `9x nn` | note `nn` of the track's sample, at volume `x`: pitch `nn × 16 + 1` |
| `Ax nn` | `Bx nn`, played by the next track |
| `Bx nn` | a note of the sample that entry `nn` of the sample map names, at the sample's rate and volume `x` |
| `Cx` | play the track's pitch again, at volume `x` |
| `Dx` | volume `x`, without a note |
| `Ex nn` | PSG note `nn` at volume `x`: pitch `nn × 16`, or `nn + 1080` from note 72 on, where the noise settings are |
| `F0 xx` | the PSG volume on each side (`NR50`) |
| `F1 xx` | bend the sample note of the next track by `xx - 32` sixteenths of a semitone |
| `F2 xx` | bend the sample note of this track by `xx - 32` sixteenths |
| `F3 xx` | bend the PSG note by `xx - 32` sixteenths |
| `F4 lo hi nn` | call `nn` commands from offset `hilo` of the track's data |
| `F5 lo hi nn dd` | the same, starting with `dd` as a duty or wave command |
| `F6`, `F7` | clear and set a flag of the track that nothing reads |
| `F8` | fade the track out over 4 frames |
| `F9` | fade the next track out |
| `FA dd cc` | loop point, with a delay and a command for the loop |
| `FB` | release: a fade, with an envelope on a square channel, or a rest during vibrato |
| `FC` | rest |
| `FD` | end of the track |
| `FE` | loop the song |
| `FF` | stop the song: the driver silences it on the next frame |

`FA` makes the track data start after it. Its two bytes are a delay and a
command, usually a duty or a wave. The track skips them until the song loops.
From the loop on, the track starts from them.

A call reads commands from an offset relative to the track's start in the song
table. Each command has a preceding delay and counts towards the call's length.
`F5` runs its last byte as the first command. That byte also sets the delay
after the call. As in the Eternal Duelist revision, returning leaves the
return position intact. The counter keeps running. Every 256 commands, the
track therefore jumps back to that position.

`Ax`, `F1` and `F9` change the state of the next track. That track has already
run this frame. `Ax` and `F1` therefore also write to its output record. After
the last track, these writes would reach the state of the first sound effect
track and the variables of the driver. `agbsf2` ignores those commands and
prints a warning.

A fade reduces the starting volume to 3/4, 1/2 and 1/4 over three frames. On
the fourth frame it outputs duty byte 0 and volume 0. Only this last output
reaches a PSG channel. Ended tracks still run their per-frame updates. Fades
and volume output therefore continue after a track ends. `FB` also sets an
envelope on square channels that lowers the volume by one step every 1/64 second. `agbsf2`
omits this three-frame envelope.

Vibrato outputs a pitch every 4th frame. It alternates between the entry of the
note in the vibrato section of the frequency table and the entry `x` places
below it. Each update restarts the channel. For a note at pitch `16n`, entry
`4n` of the vibrato section contains note *n*. The three entries before it
contain the pitches 1, 2 and 3 sixteenths below it. Each 4 of depth therefore
lowers the pitch by another semitone. A pitch that `F3` has taken off a whole note is lowered by `x`
sixteenths in the main part of the table instead. A frame counter controls the
steps and resets only when the song loops.

`Cx` plays the pitch of the track again. `Dx` doesn't set the pitch of the
record. On a square channel, `Dx` therefore plays pitch 0, C2. A note, rest or
`Dx` in the same frame as `FD` or `FF` plays at volume 0. The track then skips
its per-frame work, and that work passes the volume on.

In `AYDE` the routines are:

| Address | Routine |
|---|---|
| `0x0802985C` | sound init: `r0` = the game's interrupt table, or 0 |
| `0x0802B264` | request a song: `r0` = song number, `r1` = fade-in rate (0 plays it at full volume at once) |
| `0x0802B04C` | start the requested song (the game calls it every frame) |
| `0x0802AE3C` | per-frame entry: runs the sequencer, then starts the notes on the voices |
| `0x08029EC4` | the sequencer, with the command reader at `0x0802A10C` and the output stage from `0x0802A5EE` |
| `0x080299C0` | a sound effect track |
| `0x0802AB58` | start the notes on the voices |
| `0x0802A9B4` | the routine of the DMA 1 interrupt: mixes voices 0 and 1 for FIFO A |
| `0x0802A7C0` | the routine of the DMA 2 interrupt: mixes voices 2 and 3 for FIFO B |

The driver's state is at `0x030009A8`: the voice records at `+0x04` (12 bytes
each), the track records at `+0x34` (24 bytes each), the sound effect tracks at
`+0xF4`, and the FIFO timer periods at `+0x1A4` and `+0x1A8`. The song table is
at `0x0805E034`. The sample table is at `0x08088534`, the sample map at
`0x0805DFE0`, the sample period table at `0x0805D918`, the PSG frequency table
at `0x0805CDC0`, the waves at `0x0805DF20`, and the wave volume table at
`0x0866CAE8`.

*GetBackers Dakkanya: Jigoku no Scaramouche* (`AGBJ`) and *Yu-Gi-Oh! Duel
Monsters 5 Expert 1* (`AY5J`) have a later build of this revision. Its command
reader, note starts and DMA interrupt routines are compiled differently. The
interrupt routines stop their DMA while they mix. The commands and outputs
are the same. In `AGBJ` the init is at `0x08024DA0`, and the driver's state at
`0x03000B24`. In `AY5J` the driver is 0x56994 bytes higher in ROM, with the
state at `0x030053C8`.

*Yu-Gi-Oh! Double Pack 2* (`BYVE`) contains *Dungeon Dice Monsters* with a
third build of the driver. Its init, request, per-frame entry and DMA
interrupt routines match those of `AYDE`. Its sequencer and output stage were
compiled with other registers. The patterns for its tables leave those
registers open (see [Locating the driver](#locating-the-driver)). In `BYVE` the
init is at `0x0802B2D0`, the request at `0x0802CCBC`, the start of the requested
song at `0x0802CA98`, the per-frame entry at `0x0802C888` and the output stage
at `0x0802C032`. The driver state is at the same address as in `AYDE`.

### Output stage in Dungeon Dice Monsters

The output stage writes a PSG channel only when the record of the track has
the retrigger byte or flags set:

* **Squares.** `NRx1` gets the duty byte, `NRx2` the volume and the envelope,
  and `NRx3`-`NRx4` the entry of the frequency table at the pitch. Every entry
  has bit 15 set. Every write therefore restarts the channel.
* **Wave.** `NR32` gets the duty byte and the volume code that the wave volume
  table sets for the volume of the track: 0, 25, 50, 75 or 100%. `NR33`-`NR34`
  get the entry of the frequency table without bit 15. A note therefore doesn't
  restart the channel. The wave RAM contains the last wave loaded, at full
  scale. Loading it turns the channel off and on again, and restarts it at
  frequency 0, until the next write sets the frequency of the note.
* **Noise.** `NR42` gets the volume, and `NR43`-`NR44` the entry at the pitch.
  The entry contains the noise setting and bit 15.

The output stage writes `NR50` every frame. `NR51` stays at `0xFF`, as the init
sets it.
A song starts, and loops, at `NR50` = `0x77` and with wave 0 in the wave RAM.

The record of a sample track sets the volume of the voice every frame. With
flag `0x40`, the output stage treats the pitch of the record as a timer period
and applies it to the FIFO of the voice. A note-start record stops the voice at
volume 0, or starts a note at any other volume. The duty byte selects the
sample, and the pitch indexes the sample period table. Entry 0 uses the sample
rate of the sample.

### FIFOs in Dungeon Dice Monsters

There's no mixer. Each entry of the sample table points at the 8-bit signed
data of a sample. The entry also contains the length of the sample in blocks
of 16 samples (bits 20-31) and the timer period of its sample rate (bits 0-15).

Voices 0 and 1 use FIFO A and timer 0. Voices 2 and 3 use FIFO B and timer 1.
The DMA interrupt routine of each FIFO mixes the next 16 samples from its two voices
into a buffer for DMA to send to the FIFO. Each voice plays at (*v* + 1)/32 of
full scale for volume *v*, or silence at volume 0. The sum isn't clipped. Both
FIFOs play on both sides, and each voice plays its sample once. When both
voices are silent, the routine stops their FIFO until the next note starts.

A FIFO plays at the rate of its timer. Both of its voices therefore play at the
rate of the last note started on either, or of the last bend. Starting a note
on voice 0 (or 2) also stops voice 1 (or 3) unless that voice plays at the
sample rate of its sample. A note on voice 1 (or 3) at any other rate doesn't
start while voice 0 (or 2) plays at the sample rate of its sample. `agbsf2` leaves these rules out. Each voice plays
at the rate of its notes and bends.

## Live stages

Two games change the state that a song request leaves before their live stages
play a song. Every song request sets the setting that `FA` reads to `FF`, and
passes no table of samples to `F6`. The live stages set the setting to 0 after
their requests. For some songs, they also pass a table to `F6`. The games play
these songs in both states. `agbsf2` therefore writes each of them twice:
`NAME_NN` as a request leaves it, and `NAME_NN_live` as the live stage plays
it. The songs and the tables come from tables in the code of the games. `agbsf2`
lists them by game code.

| Game | Request of the live stage | Songs | What `FA 01` does there | Songs with a table |
|---|---|---|---|---|
| *Mermaid Melody Pichi Pichi Pitch* (`BMAJ`) | `0x0800C254`: the song of entry `r0` of the table at `0x08EB7E08`, with the table of samples through `0x080158EC`, then `0x080166AC` with 0 | 0-8 | track 12 plays no more notes | 1, 4-7 |
| *Mermaid Melody Pichi Pichitto Live Start!* (`B3MJ`) | `0x0801E1C8` with `r1` = 1: the song of entry `r0` of the table at `0x09D37848`, with the table of samples through `0x0800B70C`, then `0x0800C514` with 0. `0x080106CC` does the same for songs 17-20. | 2, 4, 17-20 | track 12 or 14 is held | 0, 1, 4-8, 13 |

The request builds the table of samples in RAM. Entry 0 is 0. The entries
after it come from a list for each entry of the live stage. The pointers to the
lists are at `0x08EB10D4` in `BMAJ` and at `0x09D37800` in `B3MJ`. Each list
ends in `FFFF`. The request sets bits 14 and 15 of each sample number, and in
`B3MJ` bit 12 as well. The song start passes the table to `F6`. After `F6 xx`,
each note of the track plays the sample of entry `xx` in place of the sample of
the note, where the entry isn't 0:

| Bit of the entry | Effect |
|---|---|
| 15 | the sample comes from the second sample table: `0x0833D750` in `BMAJ` (note start `0x08015F48`), `0x08772980` in `B3MJ` (note start `0x0800BD94`), at the number's low 12 bits |
| 14 | the note plays at pitch 0 (the sample rate of the sample), whatever its semitone |
| 12 | `B3MJ`: the note plays at volume `7F` |

`F6 00` ends that. Most samples of the second tables are packed (see
[Packed samples](#packed-samples)). The mixer of `BMAJ` unpacks them with the
same code as the mixer of Live Start. `agbsf2` names them `Live sample N` in the SoundFont.

The first version is the song as a request leaves it. The song select of `BMAJ`
previews songs 0 to 7 that way (`0x0800E66E`), and `B3MJ` plays songs 17 to 20
that way at `0x0800FBA2`. With no input after boot, `BMAJ` stops at its
title. The title plays song 9. Songs 14 and 15 of `B3MJ` have
`FA 01` too, but the live stage of `B3MJ` plays neither of them.

## Revisions that agbsf2 doesn't read

Another game may have a revision whose command reader compares other opcodes
after `FF`, `FE` and `EF`. Detection names the reader and stops. None of the
games tested has one.

## Locating the driver

`agbsf2` first finds the command reader of the driver. The reader loads the
opcode of a command and compares it with `FC`: `ldrb rX,[rY]` / `cmp rX,#0xFC`
/ `ble`. The next opcode comparisons identify the revision:

| Next comparisons | Revision |
|---|---|
| `FF`, `FD`, `EF`, `FB` | Ultimate Masters |
| `FF`, `FE`, `EF`, `FA` | WCT 2004 |
| `FF`, `FE`, `EF`, `F9` | Kitaro |
| `FF`, `FE`, `EF`, `FB` | Minna no Ouji-sama |
| `FF`, `FE`, `EF`, `F5` | Rave Master, or Captain Tsubasa if the ninth comparison is `F0` |
| `FF`, `FE`, `EF`, `F6` | Hunter X Hunter |
| `FF`, `FE`, `EF`, `F3` | Eternal Duelist |
| `FF`, `FE`, `EF`, `FC` | Dungeon Dice Monsters |

For any other reader that compares `FF` and then `FE`, detection fails with an
error that says the game has an older revision. If no recognised command
reader is found, detection assumes the Ultimate Masters revision.

Detection then finds each table from the instruction sequence that loads it. It matches
Thumb code with the literal-pool offsets wildcarded. The song and sample tables
that it finds are checked against the data that they point to. The mixer timer
must set a rate from 4000 to 65536 Hz. If the timer setup code isn't found, or
its rate is out of that range, the tool assumes 21024 Hz and prints a warning.
The Dungeon Dice Monsters revision has no mixer. Its samples play at their
sample rates.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 36 + table` | `00E0 1900 0080 49xx 1842 8811 8850 0400 4301` |
| samples | note start | `4Bxx 403B 20A0 0200 4038 2800 D1xx 49xx` |
| volume | voice output | `78D2 0191 48xx 1809` |
| PSG frequency | square output | `48xx 2700 5FE9 0049 1809 880D` |
| noise | noise output | `49xx 2424 5F18 0040 1840 8800` |
| wave | wave RAM loader | `B510 1C04 4Bxx 2A00 D0xx 2080 4008` |
| mixer timer | timer setup | `49xx 0098 1840 6800 2180 0409 4308 6010`, or `49xx 0098 1840 6803 2080 0400 4318 6010` |

The code of the WCT 2004 revision is laid out differently. These patterns find
its tables instead. Its wave RAM loader and timer setup match the ones above.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 36 + table` | `00E8 1940 0080 49xx 1843 8819 8858 0400 4301` |
| samples | note start | `49xx 48xx 4030 0080 1840 6805` |
| volume | voice and PSG output | `78F8 0100 49xx 1841 200F 4020` |
| PSG frequency | square output | `48xx 2200 5EB9 0049 1809 880C` |
| noise | noise output | `49xx 2424 5F38 0040 1840 8800` |

The code of the Rave Master revision matches these, apart from a few registers
and the song table. The song start finds the entry as `song × 7 × 4`. The init
loads the timer setting from a literal. That literal then stands for the timer
table.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 28 + table` | `00E0 1B00 0080 49xx 1842 8811 8850 0400 4301` |
| samples | note start | `49xx 48xx 4028 0080 1840 6802` |
| volume | voice and PSG output | `78F8 0100 49xx 1841 200F 4028` |
| PSG frequency | square output | `48xx 2200 5EB9 0049 1809 880D` |
| mixer timer | timer setup | `6888 3138 48xx 6008` |

The code of the Eternal Duelist revision differs more. The revision has no
volume table, and its init loads the timer setting from a literal too.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 24 + table` | `0060 1900 00C0 49xx 1842 8811 8850 0400 4301` |
| samples | note start | `49xx 00A0 1840 6802 0058 49xx 1840 8801 6810 4348` |
| PSG frequency | square output | `48xx 2300 5EF9 0049 1809 880D` |
| noise | noise output | `49xx 2318 5EF8 0040 1840 8800` |
| wave | wave RAM loader | `B510 0109 1889 0109 4Axx 1889` |
| mixer timer | init | `49xx 48xx 6008`, with `0x04000100` as the first literal |

In the other revisions, the code of the wave RAM loader goes on to load the
second wave table, for waves from `80` up. `2800 D0xx 4Bxx` follows the wave
pattern of the first table. A GSF rip may have zeroed that code. The songs
don't run it. Some builds of the Rave Master revision, such as the build in
*Hikaru no Go 2*, have no check for volume 0 in the loader. Their loader is
`B510 1C04 4Bxx 2080 4008 2800 D0xx 4Bxx`, with the first table in the first
literal and the second table in the last. The loader of the Eternal Duelist
revision reads every wave from one table.

The Dungeon Dice Monsters revision has different tables. Detection requires both
the sample map and the sample period table. Noise settings start at note 72 in
the frequency table. The difference between two literals in the vibrato code
is the starting index of the vibrato section of the table.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 5 × 4 + table` | `00A0 1900 0080 49xx 1841 884C 0420 880D 4328` |
| samples | voice start | `00D1 48xx 1808 6805 6841` |
| PSG frequency | square output | `ldr r1,=frequencies` / `movs rA,#0` / `ldrsh r0,[r6,rA]` / `lsls r0,r0,#1` / `adds r0,r0,r1` / `ldrh r0,[r0]` |
| sample map | sample note | `ldr rA,=map` / `ldrb rB,[rC,#1]` / `adds rD,rE,rF` / `ldrb r0,[r0]` / `strb r0,[rG,#2]` |
| sample periods | sample bend | `ldr r0,=periods` / `lsls r1,rA,#1` / `adds r1,r1,r0` / `ldrh r0,[r1]` / `strh r0,[rB]` / `ldrh r0,[r1]` / `strh r0,[rC]` |
| waves | wave command | `lsls r0,rA,#4` / `ldr r1,=waves` / `adds r0,r0,r1` / `str r0,[r2]` |
| wave volumes | wave output | `ldr r1,=volumes` / `ldrb rA,[r6,#0x1b]` / `lsls r0,rA,#1` / `adds r0,r0,r1` |
| vibrato part | vibrato | `asrs rA,rB,#2` / `ldr r0,=part` / `ldr r1,=frequencies` / `subs r0,r0,r1` |

In the rows given as instructions, `rA` to `rG` match any register. The builds
of the revision number those registers differently.

In the Ultimate Masters revision, the code of the pitch bend command tells the
builds apart. The code passes on the bent pitch with `8BB0 4647 80F8 8830 1900
8038 7978 2120 4308 7178`. A build that keeps the bend follows that with `83F4`
(`strh r4,[r6,#0x1e]`). If that code isn't found, the tool assumes that the
bend is kept, and `--info` says so.

If the song start code isn't found, the tool looks for the same computation
with any registers: a shift of the song number, an `adds` of the song number,
another shift, and then the `ldr` of the table:

| Instructions | Computation |
|---|---|
| `lsls #3`, `adds`, `lsls #2` | `song × 36` |
| `lsls #3`, `subs`, `lsls #2` | `song × 28` (Rave Master) |
| `lsls #1`, `adds`, `lsls #3` | `song × 24` (Eternal Duelist) |
| `lsls #2`, `adds`, `lsls #2` | `song × 20` (Dungeon Dice Monsters) |

The tool uses a song table found that way only if the table is
laid out like the song data of the driver. The data scan below checks that too.
The code of another revision can compute an address the same way. Its commands
differ, but a few of its songs can parse anyway.

The song table runs up to the lowest song base address. Empty entries at its
end don't count as songs. If most tracks of an entry don't parse, the tool ends
the table before that entry and prints a warning. It also ends the table,
without a warning, at an entry that doesn't point into the ROM.

If the song-start or note-start code above loads a table within the ROM,
detection uses that table without scanning the data:

* If the first song of the song table can't be parsed, detection fails with an
  error that names the table.
* At least half of the samples used by the songs must have readable headers.
  Notes using the remaining samples are skipped with a warning. If fewer than
  half have readable headers, detection fails with an error naming the table.

If no table is found in the code, the tool scans the data of the ROM instead.
Almost any byte string parses as track data. A song table found this way must
therefore also be laid out like the song data of the driver:

* the data of the first song starts right after the table, and the entries of
  the table past the songs counted are empty or point into the ROM;
* the tracks of each song tile its data: the first starts at offset 0, and each
  ends exactly where the next begins (tracks may share data);
* each song ends exactly where the next begins;
* some track plays a note.

If the scan finds no song table either, detection fails. A game with a
recognised command reader has the driver. The error for such a game names the
reader and points to `--song-table`. Without the reader, the game is taken to
have another driver.

A sample table found by scanning must be followed directly by its sample
headers, in increasing order. GSF rips zero the entries of unused samples. That
breaks the pattern. For a GSF rip, the tool therefore falls back to the first
table that covers every sample the songs use, and says so.
