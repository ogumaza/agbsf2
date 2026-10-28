# The Konami GBA sound driver

This document describes the sound driver used by some Konami Game Boy
Advance games. It is based on the driver code in the Japanese release of
*Yu-Gi-Oh! Ultimate Masters Edition: World Championship Tournament 2006*
(game code `BY6J`).

Addresses given as examples are from `BY6J`. The US release, `BY6E`, has the
same driver and data 0x90 bytes lower in ROM; its RAM addresses are the same.
Other games place the driver and its data elsewhere; `agbsf2` finds them
from the code (see [Locating the driver](#locating-the-driver)).

*Yu-Gi-Oh! World Championship Tournament 2004* has an older revision of the
driver, with a different command set. [The WCT 2004
revision](#the-wct-2004-revision) describes how it differs, and [The Rave
Master revision](#the-rave-master-revision), [The Eternal Duelist
revision](#the-eternal-duelist-revision) and [The Dungeon Dice Monsters
revision](#the-dungeon-dice-monsters-revision) describe three still older ones.

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
| 4-15 | DirectSound voices 0-11 (one voice per track, so each track is monophonic) |

Timing is counted in frames. There's no tick resolution finer than a frame, so
a song's tempo comes only from how long its delays are.

In `BY6J` the key routines are:

| Address | Routine |
|---|---|
| `0x0810B470` | sound init (sets up timers, DMA, copies the mixer to IWRAM) |
| `0x0810CFE4` | request a song: `r0` = song number |
| `0x0810CE30` | start the requested song (reads the song table) |
| `0x0810CAC8` | per-frame entry, called from the VBlank handler |
| `0x0810BCA0` | the sequencer proper; its command reader is at `0x0810C0AA` |
| `0x0810C736` | output stage (writes the PSG registers, then updates the 12 DirectSound voices) |
| `0x0810D720` | start a DirectSound note |
| `0x0810DFC8` | mixer (ARM), copied to and run from `0x03004C0C` |

The driver's RAM state lives at `0x030050CC` (the track records start at
`+0x24`, 32 bytes each), the 12 voice records at `0x0300546C` (28 bytes each),
and the echo buses at `0x03005608`.

### Builds of the driver

Other builds of this driver read the same song format, with a few differences:

| Games | Pitch bend | Voice records |
|---|---|---|
| *Ultimate Masters Edition* (`BY6J`, `BY6E`) | kept for later notes | 28 bytes |
| *Yu-Gi-Oh! GX: Duel Academy* (`BYGE`) | kept for later notes | 24 bytes |
| *Shaman King: Master of Spirits* 1 and 2 (`BSOE`, `B2ME`), *Yu-Gi-Oh! Day of the Duelist* (`BY7E`) | the playing note only | 24 bytes |

See [Pitch bend](#pitch-bend-f2) for the difference in bends. The 24-byte voice
records leave out the ADPCM decoder's state at `+0x18`, and the note-start code
points a voice that plays an ADPCM sample at `+0x4C` of the sample's header
rather than `+0x0C`. In these builds the driver's RAM follows the mixer that the
init copies to IWRAM: the state is at `+0x480`, the voice records at `+0x820`
and the echo buses at `+0x98C`. The mixer goes to `0x03000000` in `BSOE` and
`B2ME`, `0x03004788` in `BY7E` and `0x030006C8` in `BYGE`.

## Song table

The song table is an array of 36-byte entries:

| Offset | Size | Meaning |
|---|---|---|
| 0 | u32 | base address of the song's sequence data (often odd; read as two halfwords) |
| 4 | 16 × u16 | offset of each track's data from the base address |

Song data follows the table directly, so the table ends where the first song's
data begins. In `BY6J` the table is at `0x08194210` and has room for 24 entries.

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

Delays are scaled by a tempo value: `frames = (delay × tempo + frac) >> 8`,
where `frac` is the low byte of the previous result and is shared by all tracks.
The tempo starts at `0x100`, so delays initially count frames.

### Pitch units

Pitches are counted in 1/32 of a semitone. On sample tracks, pitch 0 plays a
sample at its own rate. On the square and wave tracks, pitch `n × 32` is note
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
| `F2 xh ll` | pitch bend | a first byte above `7F` takes the next one too: bend = `0xhll - 0x400` (-32 to +96 semitones); `x` is ignored |
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
pitch; the driver has no instruments or key splits. Sequences implement
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
table, and bend isn't applied. A song can play note `FF` as if it were note -1,
which is past the end of the frequency table (see [PSG](#psg-channels)).

#### Rest (`E0`-`EF`)

On a sample track the voice is stopped immediately, with no release. On a PSG
track the channel's volume is set to 0.

#### Duty byte (`00`-`8F`)

On the square tracks bits 6-7 of the duty byte give the duty cycle, so `80`,
`81`, `82` and `83` select 12.5%, 25%, 50% and 75%. On the wave track the duty
byte is the wave number, so `04` selects wave 0 and `05` wave 1. It takes effect
at the next note.

#### Vibrato (`F1`)

Each frame while vibrato is on, the phase advances by `0x18` (of 256), and the
pitch offset is `(sin[phase] × depth) >> 12`, with `sin[i] = trunc(4096 ×
sin(2πi/256))`. That's a 5.6 Hz vibrato of ±`depth`/32 semitone. The phase
isn't reset by new notes.

#### Pitch bend (`F2`)

The bend applies to the note already playing. In the builds that keep it (see
[Builds of the driver](#builds-of-the-driver)), it also applies to every later
note and is added to vibrato, until the next `F2` or a song loop. The other
builds don't store it, so the track's next note or vibrato step plays without
it.

#### Pan (`F0`)

Pan is stored as two levels, each 0-63: `pp ≤ 3F` gives right = `pp`, left =
63, and `pp ≥ 40` gives right = 63, left = `7F - pp`. Sample voices scale each
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
    to 0; tracks 0 and 1 get `80` (50% duty).
  * Tracks that ended with `FE` resume, and tracks that ended with `FD` stay
    silent.
  * Pan and echo settings carry over.
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
negative length selects a mode that agbsf2 doesn't model.

The step is relative to the mixer's output rate, so the playback rate is
`mixer rate × step / 4096`.

At pitch `p` the step becomes `(step × pitchtab[p]) >> 12`, where
`pitchtab[p] = round(4096 × 2^(p/384))` (the table is centred on index 0).

In `BY6J` the sample table is at `0x081D7F30`.

## Mixer

The mixer runs from the FIFO A DMA interrupt. Timer 0 sets the output rate:
`0xFCE2` in `BY6J`, which is 16777216 / 798 = 21024 Hz. Each call makes 16
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
| 0 | u32 | address of the next PCM byte; a note start sets the sample's data address |
| 4 | u32 | samples left before the end of the sample |
| 8 | u16 | step per mixer sample, 4.12 fixed point, with the note's pitch applied |
| 10 | u16 | the mixer's fractional position, in 1/4096 of a sample |
| 12 | u8 | low byte of the sample number |
| 14 | u8 | flags: `80` playing, `40` looped |
| 15 | u8 | volume |
| 16 | u8 | level on the right |
| 17 | u8 | level on the left |

### Volume table

`level = table[volume × 64 + pan level]`, where the pan level is 0-63 for each
side. The level is close to `volume × pan / 63` but isn't a simple rounding of
that product. `agbsf2` therefore reads the table itself. The
full-level column (`pan = 63`) equals the volume. In `BY6J` the table is at
`0x084BF1B8`.

### Echo

The driver has three echo buses, and `F9` routes a voice to one of them.
`F9 v0` takes a voice off buses 0 and 1, but not bus 2. The mixer takes a voice
routed to both buses 0 and 1 as routed to bus 1 alone. Each bus is a feedback
delay line: `y[n] = x[n] + feedback × y[n - delay]`, with its output added to
the dry mix.

* **Delay line.** It stores 8-bit stereo samples in 16-sample blocks, 256 blocks
  in all. The init puts bus 0's line at the start of the work buffer the game
  gives it, and buses 1 and 2 share the 8 KB after it. `F9` only routes a voice
  to a bus that has a line; on a bus without one, it clears that bus's routing.
* **Delay (`F8`).** `F8 dd` gives `2 × dd` blocks, so `dd × 32` samples:
  about 137 ms for `5A` and 193 ms for `7F`.
* **Feedback (`F7`).** `F7 ff` sets the target feedback to `ff`/256. The
  current feedback moves towards it by 8 per frame.

## PSG channels

**Squares.** On a note, the driver writes `SOUNDxCNT_H` as `volume << 12 |
duty byte` (no envelope) and `SOUNDxCNT_X` from the frequency table with the
restart bit. Pitch updates write only the frequency. The frequency table holds
one `u16` per 1/32 semitone of notes 0 to 83, 2688 in all; bit 15 is the
restart flag. Note 0 is C2 (65.4 Hz), so the table ends at B8; in `BY6J` it's
at `0x084C59B8`.

**Wave.** The driver triggers the wave channel once at init and never
restarts it. For each note it loads a 16-byte wave RAM image into the idle
bank and switches banks. The images come from the wave table at
`wave table + ((wave << 4) + volume) × 16`. Each wave is stored pre-scaled to
16 volumes (peak ≈ volume), and quiet notes use the coarser rows. The 32 steps
of a row play at half the square channel's rate, so wave note 0 has a 32-step
period at C1. The table is at `0x084BE7A0` in `BY6J`. The frequency comes from
the same table as the squares', but the driver only writes its low 11 bits. A
wave number from `80` up picks wave `& 7F` from a second table, which the
game's sound effects use. In the older revisions a panned square 2 plays on
the wave channel with its duty byte, `80`, as the wave (see [Output stage in
WCT 2004](#output-stage-in-wct-2004)), and so gets wave 0 of that table.

**Pitches past the frequency table.** The driver doesn't check the pitch it
looks up, so any pitch outside notes 0 to 83 reads outside the table. From
note 84 up it reads the words after the table, which hold other data rather
than higher frequencies, and note `FF`, which a song can play as if it were
note -1, reads the word 16 KB past the table's start. Whatever data lies there
sets the frequency. On a square channel, the channel only restarts if the
word's bit 15 is set: otherwise a note that's playing goes on at the new
frequency, and a silent channel stays silent. `agbsf2` does the same.

**Noise.** Each note writes `SOUND4CNT_L = volume << 12` and
`SOUND4CNT_H = noise table[note]`. The table holds the `NR43` value with the
restart bit (`0x8010`, `0x8020`, ... in `BY6J`, table at `0x084BF1A0`). Every
output of the noise track, even a pan change, writes them again. A word without
the restart bit doesn't restart the channel: a silent channel stays silent,
and a playing one goes on with the new setting. The older revisions read the
table at the noise track's pitch, the note × 32, so vibrato and bends move the
place they read, and a pitch below 0 reads the words before the table.

**Pan changes in the Ultimate Masters revision.** The output that a pan change
writes has pitch 0 unless vibrato is on. A square or wave note without vibrato
drops to note 0 (C2) and stays there until the track's next note. On the noise
track, a pan change writes entry 0 of the noise table. Entry 0 has the restart
bit in each game checked. The channel therefore restarts with entry 0's
setting. `agbsf2` does the same.

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
differ, as do parts of the output stage and the mixer.

In `BYWP` the routines are:

| Address | Routine |
|---|---|
| `0x08096694` | sound init |
| `0x08098138` | request a song: `r0` = song number |
| `0x08097F2C` | start the requested song (the game calls it every frame) |
| `0x08097BE8` | per-frame entry |
| `0x08096E88` | the sequencer; its command reader is at `0x0809722C`, and its output stage starts at `0x080977C6` |
| `0x08098938` | start a DirectSound note |

The driver's state is at `0x03005390` (the track records start at `+0x1C`, 28
bytes each), the voice records at `0x030056E0` (28 bytes each, laid out as in
the Ultimate Masters revision) and the echo buses at `0x0300587C`. The init
copies the mixer to `0x03004F18`. The song table is at `0x0893C7D0`, and the
sample table is at `0x0894E950`.

### Delays in WCT 2004

| First byte | Length | Value |
|---|---|---|
| `00`-`DF` | 1 | the byte |
| `bb yy`, `bb` from `E0` up | 2 | `(bb & 1F) << 8 \| yy` (13 bits) |

The tempo is `0x100`, as in the Ultimate Masters revision, so a delay is a
number of frames.

### Commands in WCT 2004

| Bytes | Command | Effect |
|---|---|---|
| `00`-`8F` | duty/wave | low nibble `n`: 0-3 set the duty byte to `n << 6`; 4-F set it to the wave `n - 4`, and load that wave into the wave RAM at once, at the track's volume |
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
| `F6 ii` | instrument | picks the notes' samples from a table that the game can give the driver; `agbsf2` ignores it |
| `F7 ff` | echo feedback | echo bus 0 feedback = `(ff & 7F)`/256 |
| `F8 dd` | echo delay | echo bus `dd >> 7` delay = `(dd & 7F) × 32` mixer samples |
| `F9 vb` | echo routing | as in the Ultimate Masters revision |
| `FA xx` | volume scale | scales the track's volumes by a setting that the game can give the driver; `agbsf2` ignores it |
| `FB`, `FC` | - | nothing |
| `FD` | end track | the track stops until the song loops |
| `FE` | loop song | all tracks go back to their loop points |
| `FF` | stop song | everything is silenced on the next frame |

The notes store their volume as it is. A sample note's output record starts
the voice without the retrigger byte, since the output stage sets the voice's
levels every frame anyway. On the noise track, a PSG note's pitch is the note ×
32, as on the other PSG tracks.

A song starts with every track panned to the centre (`FF`) and without attack or
decay. A song loop resets the tracks as in the Ultimate Masters revision, but
they keep their pan and their attack and decay rates.

#### Calls

`9x lo hi nn` moves the track to offset `0xhilo` from its start in the song
table, where it reads a delay. It then plays `nn` commands, each followed by
its delay, and after the last command it goes back to read the delay that
follows the call instead of the fragment's. `90`-`9E` also set the duty byte or
the wave from their low nibble, as `00`-`8F` do, before the fragment's first
delay. `9F` doesn't, and with `nn` = 0 it plays 256 commands. A call inside a
fragment replaces the return position of the first. A call runs the fragment's
`F3`, `FD`, `FE` and `FF` as the track's.

#### Attack and decay

`F4` and `F5` set a track's attack and decay rates. A PSG note starts an attack
countdown from `FF` if the attack rate isn't 0, and a decay countdown from `FF`
if the decay rate isn't 0. Each frame, the attack countdown goes down by the
attack rate, and the track plays at `((0x10F - countdown) × volume) >> 8`,
from about 1/16 of the note's volume up to 1.06 times it. Once it reaches 0,
the decay countdown goes down by the decay rate, and the track plays at
`((countdown + 0xF) × volume) >> 8`, back down to about 1/16. Each time the
level changes, the channel is triggered again at the new level.

### Output stage in WCT 2004

The output stage runs at the end of the sequencer. It differs from the
Ultimate Masters revision's in these ways:

* **Levels.** The volume table has 16 columns for each volume, one for each pan
  level, and a voice's level for a side is `table[volume × 16 + level]`. The
  mixer plays a level of *n* at *n*/16 of full scale, where the Ultimate Masters
  revision's plays it at *n*/128. The output stage sets a playing voice's levels
  every frame, from its track's volume and pan. In `BYWP` the table is at
  `0x08C53CE0`.
* **PSG pan.** The output stage writes `NR51` every frame, from the pan of
  each PSG track. `FF` plays a channel on both sides, `0F` on the right and
  `F0` on the left. Any other pan takes the next PSG channel too: the track's
  own channel plays the right side, at volume `table[volume × 16 + y]`, and the
  next channel plays a copy of the track's output on the left side, at
  `table[volume × 16 + x]`, instead of its own track's. When square 1 goes back
  to the centre or one side, and square 2's track plays nothing in that frame,
  square 2 takes a copy of square 1's output at volume 0: a retrigger silences
  it, and a pitch change moves it to square 1's pitch. The wave channel is left
  out of `NR51` until it plays a note at a volume above 0.
* **Wave.** Every output of the wave track, a pitch change included, loads the
  wave RAM image for the record's wave and volume into the idle bank and
  switches banks, and sets the channel's volume code to 100%, or to 0% for
  volume 0. A bend's record holds wave 0, so a bend loads wave 0's image,
  whatever wave the note had. Vibrato's record holds the track's wave.
* **Noise.** The noise table is read at the track's pitch, the note × 32.

### Mixer in WCT 2004

The mixer runs at 21024 Hz and its echo buses work as in the Ultimate Masters
revision, except that the sequencer masks the routing it hands the mixer with
`0x1FFF`. That keeps only voices 5-11 of bus 0, so voices 0-4 routed to bus 0
play dry.

## The Rave Master revision

*Rave Master: Special Attack Force* (`BRME`) has a revision of the driver
older than WCT 2004's. It works like the WCT 2004 revision, apart from these
differences:

* **Tracks.** A song has 12 tracks: the four PSG tracks, and 8 sample tracks
  for 8 voices. A song table entry is 28 bytes.
* **Commands.** There's no `F6`-`FA`: they do nothing, as `FB`, `FC` and
  `00`-`7F` do. `F3` carries no extra bytes on sample tracks. `80`-`83` set
  the duty byte to their low nibble as it is, where WCT 2004 shifts it into
  bits 6-7, so a duty command leaves a square at 12.5%.
* **Loops.** A song loop centres each track's pan again.
* **Delays.** They aren't scaled by a tempo, so a delay is a number of frames.
* **Sample voices.** The output stage sets a voice's levels only when the
  track's output starts a note or retriggers the voice, as the Ultimate Masters
  revision's does, rather than every frame.
* **Mixer.** There's no echo. The mixer looks each sample up in a table that
  holds `round(sample × level / 15)` (at `0x08091F9C`), so a level of 15 plays
  at full scale.
* **Timer.** The init loads timer 0's setting (`0x0080FCE2`, 21024 Hz) from a
  literal rather than from a table.

In `BRME` the routines are:

| Address | Routine |
|---|---|
| `0x0802AF4C` | sound init |
| `0x0802C3EC` | request a song: `r0` = song number |
| `0x0802C29C` | start the requested song (the game calls it every frame) |
| `0x0802C018` | per-frame entry |
| `0x0802B5A4` | the sequencer; its command reader is at `0x0802B84E`, and its output stage starts at `0x0802BB98` |
| `0x0802C8BC` | start a DirectSound note |

The driver's state is at `0x030040EC` (the track records start at `+0x0C`, 28
bytes each), and the voice records at `0x0300433C` (24 bytes each). The init
copies the mixer to `0x03004440`. The song table is at `0x0809A7E0`, and the
sample table is at `0x080E0EB0`.

## The Eternal Duelist revision

*Yu-Gi-Oh! The Eternal Duelist Soul* (`AY5E`) and *Yu-Gi-Oh! Worldwide
Edition* (`AYWE`) have a revision of the driver older than Rave Master's. The
two games have the same driver, with the driver's code 0x15A44 bytes higher in
`AYWE`'s ROM and its RAM 0x100 bytes lower. It works like the Rave Master
revision, apart from these differences:

* **Tracks.** A song has 10 tracks: the four PSG tracks, and 6 sample tracks
  for 6 voices. A song table entry is 24 bytes.
* **Delays.** A delay byte up to `EF` is the delay itself, and `Fx yy` gives
  `(x << 8) | yy` (12 bits).
* **Commands.** `F4`-`FC` are vibrato, as `F1` is, and take a byte. `F0`
  works differently on PSG and sample tracks (see below).
* **Legato.** A PSG note at the volume the track already has doesn't
  retrigger the channel. It only changes the frequency, so a run of such notes
  moves from note to note without restarting. On the wave track, the note's
  record holds wave 0, as a bend's does, so the output stage loads wave 0's
  image.
* **Bends and vibrato.** `F2` and vibrato set the output record's flags to 1,
  in place of the flags the frame's commands set. On a sample track, that
  wipes out the start of a note in the same frame. The voice then plays on
  with its old sample, at the step the output stage works out from the new
  note's sample and pitch.
* **Calls.** Returning from a call leaves the return position intact. The
  counter continues to decrement, so every 256 commands the track jumps back
  to that position.
* **Output records.** They're 8 bytes, with no pan: the pitch, duty byte,
  volume, retrigger byte, flags and sample.
* **Sample voices.** A voice has one volume, 0-15, for both sides. The output
  stage sets it whenever the track's record has its retrigger byte set. A
  record with any flag besides a note start or a stop makes the output stage
  work out the voice's step again.
* **Mixer.** It works differently (see [Mixer in Eternal
  Duelist](#mixer-in-eternal-duelist)), and there's no echo.

`F0 xx` on a PSG track sets the track's `NR51` bits. The output stage writes
`NR51` every frame as the OR of the four PSG tracks' bits, so one track can put
another track's channel on a side. A song starts with each PSG track's bits set
for its own channel on both sides (`0x11 << track`), and a loop sets them back.

`F0 xy` on a sample track sets the track's volume to `y`, and puts `x` into the
next track's output record, with the retrigger byte set, since that track has
had its turn in the frame already. The next voice then plays at volume `x`
until its own track sets its volume again. On the last track, the next record
would be the driver's variables. There `agbsf2` ignores the command and gives
a warning.

In `AY5E` the routines are:

| Address | Routine |
|---|---|
| `0x0807D578` | sound init: `r0` = 0 |
| `0x0807E690` | request a song: `r0` = song number, `r1` = fade-in rate (0 plays it at full volume at once) |
| `0x0807E554` | start the requested song |
| `0x0807E3B0` | per-frame entry: runs the sequencer, then the mixer |
| `0x0807DB58` | the sequencer; its command reader is at `0x0807DDC2`, and its output stage starts at `0x0807E072` |
| `0x0807E918` | start a DirectSound note |
| `0x0807E324` | the DMA 1 interrupt's routine, which moves the mixer's position on |

The driver's state is at `0x03005210` (the track records start at `+0x08`, 24
bytes each), and the voice records at `0x030053AC` (16 bytes each). The song
table is at `0x080E09D0`, and the sample table is at `0x0811B420`.

### Mixer in Eternal Duelist

The mixer is ARM code that runs from ROM at `0x0807EAD0`, and the per-frame
entry calls it after the sequencer, through a veneer at `0x08080A18`. Timer 0
runs the FIFOs at 19996.68 Hz (setting `0x0080FCB9`, which the init loads from
a literal). The mixer fills a ring buffer of 704 samples for each FIFO, at
`0x03005414` for FIFO A and `0x03005734` for FIFO B, up to a position that the
DMA 1 interrupt moves on 16 samples at a time. Voices 0-2 go into FIFO A and
voices 3-5 into FIFO B, and both FIFOs play on both sides. A voice at volume
*v* plays at (*v* + 1)/16 of full scale, and not at all at volume 0. The mixer
adds the voices up without clipping.

## The Dungeon Dice Monsters revision

*Yu-Gi-Oh! Dungeon Dice Monsters* (`AYDE`) uses the oldest supported driver
revision. Its sequencer still sends each track's output to an output stage
once per frame, but most of the details differ:

* **Tracks.** A song has 8 tracks: the four PSG tracks, and 4 sample tracks
  for 4 voices. A song table entry is 20 bytes.
* **Delays.** As in the Eternal Duelist revision, a delay byte up to `EF` is
  the delay itself, and `Fx yy` gives `(x << 8) | yy`.
* **Pitches.** A pitch is an entry of a table, and a note is 16 entries. The
  PSG frequency table's entries are the values the output stage writes to a
  channel's frequency register. `AYDE`'s has 1152 entries for notes 0-71,
  from C2 on, then 12 noise settings, and then a part for vibrato with 4
  entries for each note. The sample period table gives the timer period of
  each pitch of a sample note, from note 0 at 2500 Hz on, and its entry 0
  stands for a sample's rate.
* **Output records.** They're 12 bytes, on the stack: the pitch, duty byte,
  volume, retrigger byte and flags, and at bytes 6-7 an envelope for a square
  channel. There's no pan.
* **Sample voices.** A voice has one volume, 0-15, for both sides, and plays
  its sample once, at a rate its FIFO sets. There's no mixer (see [FIFOs in
  Dungeon Dice Monsters](#fifos-in-dungeon-dice-monsters)).
* **Sound effects.** Six more tracks play sound effects, which take over square
  2, the noise channel and the 4 voices while they play.

These are its commands:

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
| `F0 xx` | the PSG's volume on each side (`NR50`) |
| `F1 xx` | bend the next track's sample note by `xx - 32` sixteenths of a semitone |
| `F2 xx` | bend this track's sample note by `xx - 32` sixteenths |
| `F3 xx` | bend the PSG note by `xx - 32` sixteenths |
| `F4 lo hi nn` | call `nn` commands from offset `hilo` of the track's data |
| `F5 lo hi nn dd` | the same, starting with `dd` as a duty or wave command |
| `F6`, `F7` | clear and set a flag of the track, which nothing reads |
| `F8` | fade the track out over 4 frames |
| `F9` | fade the next track out |
| `FA dd cc` | loop point, with a delay and a command for the loop |
| `FB` | release: a fade, with an envelope on a square channel, or a rest during vibrato |
| `FC` | rest |
| `FD` | end of the track |
| `FE` | loop the song |
| `FF` | stop the song, which the driver silences on the next frame |

`FA` makes the track's data start after it. Its two bytes are a delay and a
command, usually a duty or a wave, and the track skips them until the song
loops, when it starts from them.

A call reads commands from an offset relative to the track's start in the song
table. Each command has a preceding delay and counts towards the call's length.
`F5` executes its last byte as the first command; that byte also supplies the
delay after the call. As in the Eternal Duelist revision, returning leaves the
return position intact. The counter keeps running, so every 256 commands the
track jumps back to that position.

`Ax`, `F1` and `F9` change the next track's state. That track has already run
this frame, so `Ax` and `F1` also write to its output record. After the last
track, these writes would reach the first sound effect track's state and the
driver's variables. `agbsf2` ignores those commands and issues a warning.

A fade reduces the starting volume to 3/4, 1/2 and 1/4 over three frames. On
the fourth frame it outputs duty byte 0 and volume 0. Only this last output
reaches a PSG channel. Ended tracks still run their per-frame updates, so fades
and volume output continue after a track ends. `FB` also gives square channels
an envelope that lowers the volume by one step every 1/64 second. `agbsf2`
omits this three-frame envelope.

Vibrato outputs a pitch every 4th frame, alternating between the note's entry
in the frequency table's vibrato section and the entry `x` places below it.
Each update restarts the channel. For a note's pitch `16n`, the vibrato section's
entry `4n` holds note *n*, and the three entries before it hold the pitches 1, 2
and 3 sixteenths below it, so each 4 of depth lowers the pitch by another
semitone. A pitch that `F3` has taken off a whole note is lowered by `x`
sixteenths in the main part of the table instead. A frame counter controls the
steps and resets only when the song loops.

`Cx` plays the track's pitch again, but `Dx` doesn't set the record's pitch, so
on a square channel it plays pitch 0, C2. A note, rest or `Dx` in the same frame
as `FD` or `FF` plays at volume 0, as the track then skips its per-frame work,
which hands the volume on.

In `AYDE` the routines are:

| Address | Routine |
|---|---|
| `0x0802985C` | sound init: `r0` = the game's interrupt table, or 0 |
| `0x0802B264` | request a song: `r0` = song number, `r1` = fade-in rate (0 plays it at full volume at once) |
| `0x0802B04C` | start the requested song (the game calls it every frame) |
| `0x0802AE3C` | per-frame entry: runs the sequencer, then starts the notes on the voices |
| `0x08029EC4` | the sequencer; its command reader is at `0x0802A10C`, and its output stage starts at `0x0802A5EE` |
| `0x080299C0` | a sound effect track |
| `0x0802AB58` | start the notes on the voices |
| `0x0802A9B4` | the DMA 1 interrupt's routine, which mixes voices 0 and 1 for FIFO A |
| `0x0802A7C0` | the DMA 2 interrupt's routine, which mixes voices 2 and 3 for FIFO B |

The driver's state is at `0x030009A8`: the voice records at `+0x04` (12 bytes
each), the track records at `+0x34` (24 bytes each), the sound effect tracks at
`+0xF4`, and the FIFO timer periods at `+0x1A4` and `+0x1A8`. The song table is
at `0x0805E034`. The sample table is at `0x08088534`, the sample map at
`0x0805DFE0`, the sample period table at `0x0805D918`, the PSG frequency table
at `0x0805CDC0`, the waves at `0x0805DF20`, and the wave volume table at
`0x0866CAE8`.

### Output stage in Dungeon Dice Monsters

The output stage writes a PSG channel only when the track's record has its
retrigger byte or flags set:

* **Squares.** `NRx1` gets the duty byte, `NRx2` the volume and the envelope,
  and `NRx3`-`NRx4` the frequency table's entry at the pitch. Every entry has
  bit 15 set, so every write restarts the channel.
* **Wave.** `NR32` gets the duty byte and the volume code that the wave volume
  table gives the track's volume: 0, 25, 50, 75 or 100%. `NR33`-`NR34` get the
  frequency table's entry without bit 15, so a note doesn't restart the
  channel. The wave RAM holds the last wave loaded, at full scale. Loading it
  turns the channel off and on again, and restarts it at frequency 0, until the
  next write sets the note's.
* **Noise.** `NR42` gets the volume, and `NR43`-`NR44` the entry at the pitch,
  which holds the noise setting and bit 15.

It writes `NR50` every frame, and `NR51` stays at `0xFF`, as the init leaves it.
A song starts, and loops, at `NR50` = `0x77` and with wave 0 in the wave RAM.

A sample track's record sets the voice's volume every frame. Flag `0x40` treats
the record's pitch as a timer period and applies it to the voice's FIFO. A
note-start record stops the voice at volume 0, or starts a note at any other volume.
The duty byte selects the sample, and the pitch indexes the sample period
table. Entry 0 uses the sample's rate.

### FIFOs in Dungeon Dice Monsters

There's no mixer. Each entry of the sample table points at the sample's 8-bit
signed data, and gives its length in blocks of 16 samples (bits 20-31) and the
timer period of its own rate (bits 0-15).

Voices 0 and 1 use FIFO A and timer 0; voices 2 and 3 use FIFO B and timer 1.
Each FIFO's DMA interrupt routine mixes the next 16 samples from its two voices
into a buffer for DMA to send to the FIFO. Each voice plays at (*v* + 1)/32 of
full scale for volume *v*, or silence at volume 0. The sum isn't clipped. Both
FIFOs play on both sides, and each voice plays its sample once. When both
voices are silent, the routine stops their FIFO until the next note starts.

A FIFO plays at its timer's rate, so both of its voices play at the rate of the
last note started on either, or of the last bend. Starting a note on voice 0
(or 2) also stops voice 1 (or 3) unless that voice plays at its sample's rate.
A note on voice 1 (or 3) at any other rate doesn't start while voice 0 (or 2)
plays at its sample's rate. `agbsf2` leaves these rules out. Each voice plays
at the rate of its notes and bends.

## Locating the driver

`agbsf2` first finds the driver's command reader. The reader loads a command's
opcode and compares it with `FC`: `ldrb rX,[rY]` / `cmp rX,#0xFC` / `ble`.
The next opcode comparisons identify the revision: `FF`, `FD`, `EF` and `FB`
for Ultimate Masters; `FF`, `FE`, `EF` and `FA` for WCT 2004; `FF`, `FE`, `EF`
and `F5` for Rave Master; `FF`, `FE`, `EF` and `F3` for Eternal Duelist; and
`FF`, `FE`, `EF` and `FC` for Dungeon Dice Monsters. For any other reader that
compares `FF` and then `FE`, detection fails with an error that says the game
has an older revision. If no recognised command reader is found, detection
assumes the Ultimate Masters revision.

It then finds each table from the instruction sequence that loads it. It matches
Thumb code with the literal-pool offsets wildcarded. The song and sample tables
it finds are checked against the data they point to, and the mixer timer has to
give a rate from 4000 to 65536 Hz. If the timer setup code isn't found, or its
rate is out of that range, the tool assumes 21024 Hz, and warns. The Dungeon
Dice Monsters revision has no mixer, and its samples play at their own rates.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 36 + table` | `00E0 1900 0080 49xx 1842 8811 8850 0400 4301` |
| samples | note start | `4Bxx 403B 20A0 0200 4038 2800 D1xx 49xx` |
| volume | voice output | `78D2 0191 48xx 1809` |
| PSG frequency | square output | `48xx 2700 5FE9 0049 1809 880D` |
| noise | noise output | `49xx 2424 5F18 0040 1840 8800` |
| wave | wave RAM loader | `B510 1C04 4Bxx 2A00 D0xx 2080 4008` |
| mixer timer | timer setup | `49xx 0098 1840 6800 2180 0409 4308 6010`, or `49xx 0098 1840 6803 2080 0400 4318 6010` |

The WCT 2004 revision's code is laid out differently, and these patterns find
its tables instead. Its wave RAM loader and timer setup match the ones above.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 36 + table` | `00E8 1940 0080 49xx 1843 8819 8858 0400 4301` |
| samples | note start | `49xx 48xx 4030 0080 1840 6805` |
| volume | voice and PSG output | `78F8 0100 49xx 1841 200F 4020` |
| PSG frequency | square output | `48xx 2200 5EB9 0049 1809 880C` |
| noise | noise output | `49xx 2424 5F38 0040 1840 8800` |

The Rave Master revision's code matches these, apart from a few registers and
the song table, whose entry it finds as `song × 7 × 4`. Its init loads the
timer setting from a literal, which then stands for the timer table.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 28 + table` | `00E0 1B00 0080 49xx 1842 8811 8850 0400 4301` |
| samples | note start | `49xx 48xx 4028 0080 1840 6802` |
| volume | voice and PSG output | `78F8 0100 49xx 1841 200F 4028` |
| PSG frequency | square output | `48xx 2200 5EB9 0049 1809 880D` |
| mixer timer | timer setup | `6888 3138 48xx 6008` |

The Eternal Duelist revision's code differs more. It has no volume table, and
its init loads the timer setting from a literal too.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 24 + table` | `0060 1900 00C0 49xx 1842 8811 8850 0400 4301` |
| samples | note start | `49xx 00A0 1840 6802 0058 49xx 1840 8801 6810 4348` |
| PSG frequency | square output | `48xx 2300 5EF9 0049 1809 880D` |
| noise | noise output | `49xx 2318 5EF8 0040 1840 8800` |
| wave | wave RAM loader | `B510 0109 1889 0109 4Axx 1889` |
| mixer timer | init | `49xx 48xx 6008`, whose first literal is `0x04000100` |

In the other revisions, the wave RAM loader's code goes on to load the second
wave table, for waves from `80` up: `2800 D0xx 4Bxx` follows the wave pattern in
the first table. A GSF rip may have zeroed that code, as the songs don't run it.
The Eternal Duelist revision's loader reads every wave from the one table.

The Dungeon Dice Monsters revision has its own tables. Detection requires both
the sample map and the sample period table. Noise settings start at note 72 in
the frequency table. The difference between two literals in the vibrato code
gives the starting index of the table's vibrato section.

| Table | Found in | Code (hex halfwords, `xx` = any) |
|---|---|---|
| songs | song start: `song × 5 × 4 + table` | `00A0 1900 0080 49xx 1841 884C 0420 880D 4328` |
| samples | voice start | `00D1 48xx 1808 6805 6841` |
| PSG frequency | square output | `49xx 2400 5F30 0040 1840 8800` |
| sample map | sample note | `48xx 786D 1828 7800 70A0` |
| sample periods | sample bend | `48xx 0051 1809 8808 8038 8808 8018` |
| waves | wave command | `0118 49xx 1840 6010` |
| wave volumes | wave output | `49xx 7EF2 0050 1840 7EB3 8800 4303` |
| vibrato part | vibrato | `109B 48xx 49xx 1A40` |

In the Ultimate Masters revision, the pitch bend command's code tells the builds
apart. It hands on the bent pitch with `8BB0 4647 80F8 8830 1900 8038 7978
2120 4308 7178`, and a build that keeps the bend follows that with `83F4`
(`strh r4,[r6,#0x1e]`). If that code isn't found, the tool assumes the bend is
kept, and `--info` says so.

If the song start code isn't found, the tool looks for the same computation
with any registers: a shift of the song number, an `adds` of the song number,
another shift, and then the `ldr` of the table. That's `lsls #3`, `adds` and
`lsls #2` for `song × 36`; `lsls #3`, `subs` and `lsls #2` for Rave Master's
`song × 28`; `lsls #1`, `adds` and `lsls #3` for Eternal Duelist's
`song × 24`; and `lsls #2`, `adds` and `lsls #2` for Dungeon Dice Monsters'
`song × 20`. A song table found that way is only used if it's laid out like
the driver's song data, which the data scan below checks too. Another
revision's code can compute an address the same way, but its commands differ,
and a few of its songs can parse anyway.

The song table runs up to the lowest song base address, and empty entries at
its end aren't counted as songs. If most of an entry's tracks don't parse, the
tool takes the table to end before it, and warns that it did. It also ends the
table, without a warning, at an entry that doesn't point into the ROM.

If the song-start or note-start code above loads a table within the ROM,
detection uses that table without scanning the data:

* If the song table's first song can't be parsed, detection fails with an
  error that names the table.
* At least half of the samples used by the songs must have readable headers.
  Notes using the remaining samples are skipped with a warning. If fewer than
  half have readable headers, detection fails with an error naming the table.

If no table is found in the code, the tool scans the ROM's data instead. Almost
any byte string parses as track data, so a song table found this way must also
be laid out like the driver's:

* the first song's data starts right after the table, whose entries past the
  songs counted can only be empty or point into the ROM;
* each song's tracks tile its data: the first starts at offset 0, and each
  ends exactly where the next begins (tracks may share data);
* each song ends exactly where the next begins;
* some track plays a note.

If the scan finds no song table either, detection fails. A game with a
recognised command reader has the driver, so its error names the reader and
points to `--song-table`; without the reader, the game is taken to have another
driver.

A sample table found by scanning must be followed directly by its sample
headers, in increasing order. GSF rips zero the entries of unused samples,
which breaks that pattern, so for them the tool falls back to the first table
that serves every sample the songs use, and says so.
