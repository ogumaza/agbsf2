# Rare's GBA sound driver

This document covers the tune format, sequencer and mixer in Rare's Game Boy
Advance sound driver. It's based on the code of the driver in the US release
of *Donkey Kong Country* (game code `A5NE`). The example addresses come from
that game. Other games put the driver and its data
elsewhere. `agbsf2` finds them from the code (see [Locating the
driver](#locating-the-driver)).

The tune data is close to a Standard MIDI File: tracks of MIDI-like commands
with delays in ticks, a tempo in microseconds per quarter note, MIDI channels,
programs, controllers and pitch bends. The driver plays the tunes with sampled
instruments only. It doesn't use the Game Boy's PSG channels.

## Games and revisions

The games read their tracks in one of two formats. In the first, a command
that has a MIDI channel has it in a byte after the command byte. In the
second, the command byte contains the command number in its low nibble and the
channel in its high nibble. The revisions differ in a few other details:

| Game | Commands | Note slots per channel | Sound effect records | Controller 100 |
|---|---|---|---|---|
| *Donkey Kong Country* (`A5NE`) | channel byte | 6 | 5 | stored |
| *Sabre Wulf* (`AWUE`) | channel byte | 6 | 5 | ignored |
| *It's Mr. Pants* (`BPIE`) | channel byte | 6 | 4 | ignored |
| *Battletoads*, the prototype (`RARE`) | channel byte | 6 | 4 | ignored |
| *Banjo-Kazooie: Grunty's Revenge* (`BKZX`) | channel nibble | 6 | 4 | stored |
| *Donkey Kong Country 2* (`B2DE`) | channel nibble | 6 | 5 | stored |
| *Banjo-Pilot* (`BAJE`) | channel nibble | 5 | 4 | stored |
| *Donkey Kong Country 3* (`BDQE`) | channel nibble | 6 | 5 | stored |

The rest of this document applies to all these revisions, apart from what it
says of the revision of *Donkey Kong Country 3*.

### Donkey Kong Country 3's revision

*Donkey Kong Country 3* uses a later revision with these changes:

* Controllers 20 to 23 set the attack, decay, sustain and release of the notes
  of a channel, in place of the settings of their instrument (see
  [Controllers](#controllers) and [Envelopes](#envelopes)).
* The game can start a tune partway through, at a marker in its tracks (see
  [Starting a tune](#starting-a-tune)).
* Each channel has a level that the game can turn down, and a note at a level
  of 0 doesn't count towards the voice limit (see [Mixing](#mixing)).
* The routine that moves the notes on frees a slot in loop mode 1 at the end
  of the sample. The older revisions miss that (see [Sample
  positions](#sample-positions)).

The routines are at `0x080AED34` (request a tune), `0x080AED90` (init) and
`0x080AEE82` (per-frame routine). The routine that looks for a marker is at
`0x080AF61C`. The RAM contains:

| Address | Contents |
|---|---|
| `0x03001464` | the requested tune, or -1 |
| `0x030012A0` | the tune playing, or -1 |
| `0x030023C0` | the marker to start the next tune at, or -1 |
| `0x030023F0` | the settings of each channel from controllers 20 to 23: 16 words of 4 bytes |
| `0x030023C4` | the music volume, out of 128: a word |
| `0x03001400` | the level of each channel: 16 bytes |
| `0x03001380` | the volume of each channel (controller 7): 16 bytes |
| `0x030014C0` | the note slots: 6 for each of the 16 channels |
| `0x03003C60` | the sound effect records: 5 |
| `0x03001294` | samples a frame |
| `0x03003D30` | the output buffer flag |

### Donkey Kong Country 3's channel levels

The game changes the levels of the channels in one tune only. Tune 12, Enchanted
Riverbank, has two arrangements. Channels 0-3 play the arrangement above
water. Channels 4-8 play the one under water. A routine at `0x08085CE0` runs
as a level starts and then in every frame. While tune 12 plays, the routine
checks whether the player is swimming. It then lowers the levels of the
channels of the arrangement that the player has left by 16 a frame, down to 0.
Next it raises the levels of the channels of the other arrangement in the same
way, up to 128. A full change therefore takes 16 frames, about 0.27 s. The
driver sets every channel to a level of 128 as it starts a tune. The first
frame of the tune therefore plays both arrangements.

`agbsf2` writes a pair of files for each arrangement. The channels of the other
arrangement have a level of 0 from the start (see [Voices](#voices)). The files
don't have the fades. Detection finds the choice of channels in the routine by
its Thumb code:

```
mov  r1, r8                ; 4641: 0 under water, 1 above
cmp  r1, #0                ; 2900
beq  under                 ; D0xx
movs r2, #first out        ; 22xx: the channels to fade out, from r2 to r5
movs r5, #last out         ; 25xx
movs r0, #first in         ; 20xx: the channels to fade in, from ip to r3
mov  ip, r0                ; 4684
movs r3, #last in          ; 23xx
b    ...                   ; E0xx
under:
movs r2, #first out        ; 22xx
movs r5, #last out         ; 25xx
movs r1, #first in         ; 21xx
mov  ip, r1                ; 468C
movs r3, #last in          ; 23xx
```

Detection requires each state to fade in the channels that the other fades
out. Up to 0x100 bytes before those moves, the routine checks the tune:

```
ldr  r0, [r0]              ; 6800
cmp  r0, #tune             ; 28xx
bne  ...                   ; D1xx
movs r0, #1                ; 2001
mov  r8, r0                ; 4680
```

`agbsf2 --info` gives the tune and the channels of each arrangement.

## Architecture

The driver has two parts. The routines that the game calls are Thumb code in
ROM. The sequencer, the envelopes and the mixer are ARM code. The init copies
that code to IWRAM: in `A5NE`, 0x10A0 bytes from `0x080311A8` to `0x03002B40`.

The game calls the per-frame routine once a frame. The routine runs these steps:

1. If the game has requested a tune, it starts it (see [Starting a
   tune](#starting-a-tune)).
2. It clears the mix buffer.
3. If a tune is playing and the music isn't paused, it runs the tracks, advances
   the vibrato of each channel, and updates the note envelopes.
4. It updates the sound effects and their envelopes.
5. It mixes the voices into the mix buffer, and writes the result as 8-bit
   samples into the half of the output buffer that the DMA isn't playing.
6. It advances the sample position of every voice by one frame.

The output is mono. Timer 0 runs at the mix rate, and DMA1 feeds the samples
to FIFO A. FIFO A plays on both sides at full volume. At each VBlank the game
calls a routine that swaps the two halves of the output buffer and restarts
DMA1. The init sets a mix rate of 13379 Hz, or 224 samples per display frame
(59.7275 Hz).

In `A5NE` the routines are:

| Address | Routine |
|---|---|
| `0x08032294` | init: sets up the sound hardware, copies the ARM code, and sets the tune and sound effect tables, the mix rate and the voice limit |
| `0x08032248` | request a tune: `r0` = tune number. The tune starts on the next frame |
| `0x0803237C` | per-frame routine |
| `0x0803226C` | buffer swap: flips the output buffer and restarts DMA1 |
| `0x08032876` | set the mix rate: `r0` = rate in Hz |
| `0x080328F6` | set the voice limit: `r0` = the most voices to mix |
| `0x08032734` | set the music volume: `r0` = volume, 0x80 for full |
| `0x0803273A` | set the sound effect volume |
| `0x08032538` | vibrato update |
| `0x030032B0` | sequencer (ARM), with the command table at `0x03003378` |
| `0x03002E14` | envelopes of the notes (ARM). `0x03002DFC` updates those of the sound effects |
| `0x03002B84` | mixer (ARM). `0x03003884` mixes one voice, and `0x03003A3C` computes its pitch |
| `0x03002CF4` | moves the notes on (ARM). `0x03002CDC` moves the sound effects on. While the music is paused, only that routine runs |

The RAM contains:

| Address | Contents |
|---|---|
| `0x030015C4` | the requested tune, or -1 |
| `0x03001420` | the tune playing, or -1 |
| `0x03004004` | the address of the tune table |
| `0x030014F0` | the address of the sound effect table |
| `0x03001410` | the number of tracks |
| `0x03001540` | ticks per quarter note |
| `0x03001500` | the position of each track: 16 words |
| `0x030028F0` | the counter of each track: 16 64-bit values |
| `0x03001430` | the loop position of each track: 16 words |
| `0x0300141C` | the address of the program map |
| `0x03002B30` | the address of the instrument table |
| `0x03003FFC` | the tempo, in microseconds per quarter note |
| `0x030014F4` | the frame time, in 1/256 microseconds |
| `0x030015D0` | the instrument of each channel: 16 words |
| `0x03001560` | the volume of each channel (controller 7): 16 bytes |
| `0x03001580` | the pitch bend of each channel: 16 words |
| `0x03001470` | the modulation (controller 1) and vibrato phase of each channel: 16 pairs of words |
| `0x03001610` | the mono flag of each channel: 16 bytes |
| `0x03001550` | controller 100 of each channel: 16 bytes |
| `0x03001620` | the note slots: 6 for each of the 16 channels, 0x28 bytes each |
| `0x03003F30` | the sound effect records: 5 of 0x28 bytes |
| `0x03002970` | the mix buffer: a 16-bit value for each sample |
| `0x03004000` | the output buffer flag |
| `0x03002520` | the music volume |
| `0x03001544` | the sound effect volume |
| `0x03001548` | the pause flag: 1 pauses the music |
| `0x03001570` | the voice limit |
| `0x03001414` | samples a frame |
| `0x030015C0` | the pitch scale for the mix rate |

Sound effects have a separate table, playback routines and records.
`agbsf2` doesn't convert them. This document covers only their role in the
mixer.

## Tunes

### Tune table

The init stores the address of the tune table, an array of words, each the
address of the header of a tune. Nothing in the driver gives the number of tunes.
`agbsf2` counts the entries up to the first that isn't a valid header.

### Tune header

The header of a tune is 5 words:

| Offset | Contents |
|---|---|
| `+0x00` | the number of tracks, 1-16 |
| `+0x04` | ticks per quarter note |
| `+0x08` | the address of the track list: a word for each track, the address of its first command |
| `+0x0C` | the address of the program map: 128 bytes, the index of each MIDI program in the instrument table, or 0xFF |
| `+0x10` | the address of the instrument table: a word for each instrument, its address |

### Starting a tune

The request routine only stores the number of the tune. On the next frame the
per-frame routine starts the tune. It copies the values of the header and the
track list into its RAM. It sets the counter of every track to 0, the frame
time to 1 and the tempo to 0. It frees every note slot, centres the pitch bend
of every channel and turns the modulation of every channel off.

It doesn't reset the instruments, volumes or mono flags of the channels, or
the loop positions of the tracks. If a tune plays a note before it sets the
program of the channel, that note uses the instrument left over from the
previous tune. The init sets the volume of every channel to 0x7F and turns
mono mode off.

The revision of *Donkey Kong Country 3* also sets the level of each channel to
128 and clears its envelope settings when it starts a tune. If the game has
passed a marker, every track but track 0 then skips ahead to its first
controller 104 whose value is the marker. The track keeps the last loop start
(controller 102) that it passes. The marker then goes back to -1. A track that
reaches its end first ends the search. That track and the tracks after it
start from their beginnings. The counters of the tracks aren't changed. The
tracks therefore start from the markers together. `agbsf2` plays every tune from its start.

## Tracks

### Commands

Each track is a stream of commands, with values of more than a byte in
little-endian order. The commands are:

| Command | Arguments | Meaning |
|---|---|---|
| 0 | 3 bytes | tempo: microseconds per quarter note |
| 1 | 1 byte | delay: ticks to the next command |
| 2 | 2 bytes | delay |
| 3 | 3 bytes | delay |
| 4, 5 | key, velocity | note on |
| 6 | key, velocity | note off, with the velocity ignored |
| 7 | controller, value | controller (see [Controllers](#controllers)) |
| 8 | program | program change |
| 9 | 1 byte | channel pressure, ignored |
| 10 | 2 bytes | pitch bend: 0x2000 is the centre |
| 11 | | end of the track |
| 12 | | does nothing |

Commands 4-10 apply to a MIDI channel. In the channel-byte format the channel
is a byte after the command byte, before the arguments. A note on is therefore
`05 cc kk vv`. In the channel-nibble format, the channel is the high nibble of
the command byte. A note on is therefore `c5 kk vv`. Commands 4 and 5 run the same
code.

The sequencer reads the command byte (`ldrb r0, [fp], #1`) and jumps through its
table of 13 commands without checking the number. A number above 12 jumps to
whatever follows the table. `agbsf2` stops a track at such a command, with
a warning.

### Timing

Each track has a 64-bit counter of song time in 1/256 microseconds. Each frame
the sequencer subtracts the frame time from the counter of every track. While
the counter of a track is negative, it runs the commands of the track. A delay
adds its length to the counter, ticks × tempo / ticks per quarter note, in
1/256 microseconds. The driver computes this with its 16-bit division routine.
The fraction is therefore truncated. The driver multiplies a one-byte delay in
32 bits. If the
counter is still negative after a delay, the track goes on to the next command
in the same frame.

The frame time is 1 until the first tempo command runs. The tempo command sets
the tempo and computes the frame time as 60000000 / 3600 µs: 4266666 in 1/256
microseconds, or 1/60 s. The driver therefore counts each frame as 1/60 s. The
GBA shows 59.7275 frames a second. Every tune therefore plays about 0.46%
slower than its tempo says.

When track 0 starts with the tempo command, its counter goes down by 1 in the
first frame, and the counters of the other tracks by a whole frame. From then on,
track 0 runs up to a frame later than the other tracks.

The end command doesn't move its track on. The track therefore reads it again
every frame. In a frame in which every track reads its end, the sequencer
stops the tune: it sets the tune playing to -1. From the next frame the driver
doesn't run the tracks, the vibrato or the envelopes. It goes on mixing the
notes that were still playing, until the game starts another tune.

### Controllers

| Controller | Effect |
|---|---|
| 1 | modulation: the depth of the vibrato of the channel. 0 turns the vibrato off. |
| 7 | the volume of the channel |
| 20 | in the revision of *Donkey Kong Country 3*, the attack of the notes of the channel: its number of steps |
| 21 | in the revision of *Donkey Kong Country 3*, the decay of the notes of the channel: its fade table entry |
| 22 | in the revision of *Donkey Kong Country 3*, the sustain level of the notes of the channel, in 1/128ths of the full level |
| 23 | in the revision of *Donkey Kong Country 3*, the release of the notes of the channel: its fade table entry |
| 100 | stores the value in a byte for each channel. The driver itself doesn't use it. *Sabre Wulf*, *It's Mr. Pants* and the *Battletoads* prototype ignore it. |
| 102 | loop start: saves the position after the command as the loop position of the track |
| 103 | loop end: goes back to the loop position of the track |
| 104 | in the revision of *Donkey Kong Country 3*, a marker that the game can start the tune at (see [Starting a tune](#starting-a-tune)). It does nothing as the track plays. |
| 126 | mono: the channel plays every note in its first slot |
| 127 | poly: the channel goes back to picking a slot for each note |

The driver ignores the others. The loop position belongs to the track. A tune
loops forever by ending each looping track with controller 103. A loop end
without a loop start goes back to whatever loop position the track had from an
earlier tune. `agbsf2` ignores such a loop end, with a warning.

Controllers 20 to 23 keep their values until a program change on the channel.
The program change sets all four back to 0xFF. A value of 0x80 or more leaves
the setting of the instrument. [Envelopes](#envelopes) says how the others
count.

## Instruments

### Program changes

A program change looks up the program in the program map of the tune. It then
uses that index to select the instrument of the channel from the instrument
table. The driver doesn't check for 0xFF, the entry of the program map for a
program that the tune doesn't have. It then reads the 256th word of the
instrument table.

### Instrument records

An instrument is 17 words:

| Offset | Contents |
|---|---|
| `+0x00` | type: 0x20 or 0x21 for a sample, 0x22 for a drum kit, 0x23 for a key split |
| `+0x04` | loop mode: 1 plays the sample once, 2 loops it. The driver also supports 3 and 4 (see [Mixing](#mixing)). |
| `+0x08` | the rate of the sample in Hz |
| `+0x0C` | root key: the MIDI key at which the sample plays at its rate |
| `+0x10` | the address of the first byte of the sample |
| `+0x14` | the length of the loop in bytes. The loop is the last part of the sample. |
| `+0x18` | the address of the byte after the last byte of the sample |
| `+0x1C` | the key map of a drum kit or key split: 128 bytes, the index of each key in the next table |
| `+0x20` | the instrument table of a drum kit or key split: the address of each instrument, a word each |
| `+0x24` | attack, 0-99 |
| `+0x28` | decay: an index in the fade table, 0-99 |
| `+0x2C` | sustain level, 0-99 |
| `+0x30` | release: an index in the fade table, 0-99 |
| `+0x34` | fine tune in cents, signed |
| `+0x38` | the range of the pitch bend in semitones |
| `+0x3C` | vibrato rate: added to the phase of the vibrato each frame, as a 24-bit fraction of a cycle |
| `+0x40` | vibrato depth |

Samples are signed 8-bit PCM. The mixer interpolates between each byte and the
next. It therefore also reads the byte at the end of the sample.

A drum kit or a key split selects one of the instruments in its table for each
key from its key map. A drum kit plays it at the root key of that instrument.
Every key therefore sounds at the pitch of its sample. A key split plays it at
the key. The driver takes any type except 0x20, 0x21 and 0x23 for a drum kit,
and doesn't check the entries of the key map.

The instrument that a note plays sets the sample, its rate, root key and loop,
the envelope and the fine tune. The bend range and the rate and depth of the
vibrato come from the instrument of the channel. That is the drum kit or key
split itself when there is one.

## Notes

### Note slots

Each channel has separate note slots: 6 in all the games except
*Banjo-Pilot*, and 5 in *Banjo-Pilot*. A note slot is 0x28 bytes, and a sound
effect record has the same layout:

| Offset | Contents |
|---|---|
| `+0x00` | state: 0x10 free, 0x11 on, 0x12 released |
| `+0x01` | the loop mode of the instrument |
| `+0x02` | the key that started the note |
| `+0x03` | velocity + 1 |
| `+0x04` | the phase of the envelope, 0-6 |
| `+0x05` | the key that the note plays at: the key, or the root key of an instrument of a drum kit |
| `+0x06` | the channel, signed, or -1 for a sound effect |
| `+0x08` | the pitch step that the mixer used this frame |
| `+0x0C` | the address of the sample byte it's playing |
| `+0x10` | the fraction of the position, 23 bits |
| `+0x14` | the level of the envelope |
| `+0x18` | the step of the envelope |
| `+0x1C` | the sustain level |
| `+0x20` | the address of the instrument it plays |
| `+0x24` | a pitch scale, 0x10000 for 1 (notes don't change it) |

### Note on and note off

A note on picks a slot from the slots of the channel. In mono mode it always takes
the first slot, cutting off the note playing there. Otherwise it takes the
first free slot, or if there's none, the first slot whose note is released. If
every slot contains a note that hasn't been released, the driver drops the new
note.

The note starts at the first byte of the sample, with its envelope in phase 0 at a
level of 0, and the slot keeps the instrument and the key it plays at.

A note off releases the first slot of the channel whose note is on and was
started by the key, and only that one. If a channel plays a key twice without
a note off in between, the next note off releases the note in the lower slot,
and the other goes on until another note off for the key.

## Envelopes

The level of the envelope runs from 0 to 0x80000. Each frame the driver updates the
envelope of every note that's on or released, by its phase. Where a phase
ends, the next often starts in the same frame.

* **Phase 0** sets up the attack. It takes (99 − attack) × 32 / 99 steps,
  truncated. With 0 steps, the note goes straight to the full level in
  phase 2. Otherwise the step is 0x8000 / (steps + 1) × 16, with the remainder
  of the division × 16 as the starting level, and the attack goes on in phase 1.
* **Phase 1** adds the step each frame until the level would reach 0x80000,
  then sets it to 0x80000 and moves to phase 2.
* **Phase 2** sets up the decay. The sustain level is sustain / 99 of the full
  level, computed as 8.8 fixed point. It is therefore slightly below that. The
  decay takes the number of frames in the fade table for the decay index. A
  decay of 0 frames goes straight to the sustain level. Otherwise the step is
  (full level − sustain level) / frames, and the decay goes on in phase 3.
* **Phase 3** subtracts the step each frame until the level reaches the
  sustain level, then moves to phase 4.
* **Phase 4** holds the sustain level.
* **Phase 5** sets up the release, from the level that the note had, over the
  number of frames in the fade table for the release index, and goes on in
  phase 6. A release of 0 frames ends the note at once.
* **Phase 6** subtracts the step each frame and ends the note when the level
  would reach 0.

When a note has been released, the next update of phase 1, 3 or 4 moves it to
phase 5 instead. During the decay, a level below 0x1000 ends the note. A note
with a sustain level of 0 therefore ends when its decay is over.

In the revision of *Donkey Kong Country 3*, the settings that controllers 20
to 23 set for the channel of a note take the place of the settings of the
instrument, each where it's below 0x80. Phase 0 takes controller 20 as the
number of steps of the attack. Phase 2 takes controller 22 × 256 as the
sustain level and controller 21 as the fade table entry of the decay. Phase 5
takes controller 23 as the entry of the release. Each phase reads the settings
of the channel as it starts. A note that was playing when they changed
therefore takes the new ones in its later phases. Where a channel has a
sustain level from controller 22 but no decay from controller 21, the driver
reads the decay through a register. That register contains whatever the code
before left in it. `agbsf2` takes the decay of the instrument there instead.
Sound effects keep the envelopes of their instruments.

The fade table is 100 bytes, at `0x08032C94` in `A5NE`. Entry *i* sets a
length of entry + 1 frames for a decay or release with index *i*. An entry of
0 means no fade at all. The entries mostly fall, from 0xFE (255 frames, about
4.3 s) at index 0 to 0 at index 99.

## Pitch

The driver computes the pitch of each voice every frame, as a step through its
sample for each output sample, in 1/2^23 bytes:

1. It adds the fine tune of the instrument and the bend and vibrato of the
   channel as 32.32 fixed-point semitones. The fine tune is in cents, × 2^32 /
   100. The bend is (bend − 0x2000) × range × 0x80000. A full bend therefore
   equals the bend range.
2. It adds the difference between the key that the note plays at and the root
   key of the instrument, in whole semitones.
3. It looks the semitones up in the pitch table, 128 words of 2^(*n*/12) in
   9.23 fixed point for *n* from -64 to 63 (`0x08032A94` in `A5NE`, with
   *n* = 0 at `0x08032B94`), and interpolates linearly between the entry and
   the next by the fraction.
4. It multiplies by the rate of the instrument × the pitch scale for the mix
   rate (2^38 / 13379 at 13379 Hz) / 2^14, and divides by 2^24.

The pitch changes only from one frame to the next. A bend therefore applies to
the whole frame in which its command runs, and vibrato moves in steps of a
frame.

### Vibrato

Each channel has a vibrato phase. While modulation is above 0, the phase
increases each frame by the vibrato rate of the instrument of the channel,
wrapping at 2^24. A modulation of 0 sets it back to 0. The vibrato adds
(modulation + 1) × sine(phase) × depth to the pitch, in 32.32 semitones. The
sine comes from a table of 257 words (`0x08032CF8` in `A5NE`) with a peak of
2^32 / 12800. With the modulation at 127, the depth of the vibrato in cents is
the vibrato depth of the instrument.

## Mixing

### Volume

The mixer gives each voice a level from 0 to 128:

* For a note, the music volume × (the volume of the channel + 1) / 128, and in
  the revision of *Donkey Kong Country 3* × the level of the channel / 128. A
  tune starts with each channel at a level of 128. Only the code of the game
  turns one down.
* For a sound effect, the sound effect volume.
* Then × (velocity + 1) / 128 and × the level of the envelope / 2^19.

Each step is truncated. The level is therefore linear in the volume, the
velocity and the envelope. A note at full volume and velocity has a level of
128.

The init sets the music volume to 128. Most games set it again after they
request a tune. The level scales every note of the tune alike. `agbsf2` keeps
the volume at which each listed game starts each tune, with the options of the
game as they start. It sets that volume as the expression of every channel in
the MIDI file of the tune. Expression can't go above 127. A volume above 128 plays at 128.

| Game | Music volume |
|---|---|
| *Donkey Kong Country* (`A5NE`) | 0x7F for most tunes, from the calls of the setter at `0x08032734` in the game. The level music comes from the switch at `0x0801B08C`, at 0x7F, and at 0x96 for tune 25. After tune 21 ends, `0x080003D0` starts tune 22 at 0x8C, and then plays it again at 0x7F. `agbsf2` takes 0x8C. Tune 24 starts at 0x8C, tune 33 at 0x5A, tune 3 at 0x73, tunes 5 and 13 at 0x69, tune 18 at 0x77, and tunes 26, 27, 29 to 32 and 34 at 0x6E. The sound test plays every tune at 0x7F. |
| *Donkey Kong Country 2* (`B2DE`) | `0x08027B88` sets a volume from 0x50 to 0x7F for each tune and multiplies it by the music level of the options / 256. The options start at 0xB3 (`0x08020B42`), and the tunes at 55 to 88. |
| *Donkey Kong Country 3* (`BDQE`) | `0x08085B98` sets a volume of 40 for tune 2, 80 for tunes 3 and 4 and 70 for the rest, times the music level of the options / 256. `0x0807ADDC` sets the level from the music slider: the slider × 25.5, rounded down. At boot the slider is at 10, a level of 255. The tunes then start at 39, 79 and 69. The reset of the options at `0x080A3BA8` sets the level to 256. After boot, the level is still 255 when the first tune starts. The setter is at `0x080AF270`. |
| *Sabre Wulf* (`AWUE`) | 0x7F for most tunes. Tunes 5, 7 and 35 start at 0x50 and tune 29 at 0x3C. The scripts of the game start tunes through their table at `0x0828A40C`: tunes 2 and 1 at 0x50, the others at 0x7F. The intro of a new game starts tune 1 from a script at 0x50, before `0x080279DE` starts it at 0x7F. `agbsf2` takes 0x50. The level-music switch at `0x08016170` starts tune 2 at 0x7F in seven levels. Tune 4 starts at 0x7F at `0x08012534` and from the scripts, and at 0x50 at `0x08068852`. Nobody has found which start of tunes 2, 4, 5 and 7 comes first in normal play. `agbsf2` takes the volume that more of their starts give: 0x7F for tunes 2 and 4, and 0x50 for tunes 5 and 7. |
| *Banjo-Pilot* (`BAJE`) | `0x08016094` starts a tune at the music level of the options × 100 / 256, and `0x080300A4` at the level × 127 / 256. The options start at 0x80: 50 and 63. Tunes 24 to 26 and 28 to 30 start through the second, and the rest at 50. Tune 3 starts through both. `agbsf2` takes 50. |
| *Banjo-Kazooie: Grunty's Revenge* (`BKZX`) | The music level of the options runs from 0 to 9 and starts at 4 (`0x08040C96`). The table at `0x080BF604` sets a volume of 56 for that level, for every tune. |
| *It's Mr. Pants* (`BPIE`) | 128, from the init. The game doesn't set it. |
| *Battletoads*, the prototype (`RARE`) | 0x46 for tunes 0 and 1 and 0x50 for tune 2, from the calls of the setter at `0x08007848` in the game. `0x08005E48` starts tune 0, and `0x08001130` and `0x08001144` start tunes 1 and 2. |

A game or build that isn't listed plays at 128, except in the revision of
*Donkey Kong Country 3*, where it plays at 69.

### Voices

The mixer takes the sound effects that are on, then those that are released,
then, unless the music is paused, the notes that are on, then those that are
released, each in the order of their records, and stops at the voice limit.
The init sets the limit to 8. The voices past the limit are silent for the
frame. Note slots are in channel order. When more than 8 voices play, the
mixer therefore leaves out released notes before notes that are on, and notes
on higher channels before those on lower ones. In the revision of *Donkey Kong
Country 3*, a note whose level is 0 before its velocity and envelope count
isn't mixed and doesn't count towards the limit. At the music volume of 69 in
*Donkey Kong Country 3*, that's every note of a channel at volume 0. At the
volume of 39 of tune 2, it's every note of a channel at volume 2 or less.
So is every note of a channel that the game holds at a level of 0 (see
[Donkey Kong Country 3's channel levels](#donkey-kong-country-3s-channel-levels)).

The model in `agbsf2` makes the same choices, frame by frame. Where the mixer
leaves out every note of a key on a channel, the MIDI file mutes the key with
polyphonic key pressure until the mixer takes one of its notes again. Each
channel that mutes a key also starts with the General MIDI 2 Controller
Destination Setting message. With the message, a pressure of 127 on a key
silences the notes of that key on the channel. The README describes the mutes.

For each output sample, the mixer interpolates the sample of the voice
linearly between the byte at its position and the next, by the fraction of the
position. It multiplies the result by the level / 128 and adds it to the mix
buffer. The
first voice of a frame writes the mix buffer rather than adding to it. When all
voices are mixed, the mixer clips each value to the range -128 to 127 and
writes it to the output buffer.

When a voice reaches the end of its sample, a loop (loop mode 2 or 4) goes
back by the length of the loop. Any other loop mode frees the slot of the
voice, and the rest of the frame is silent.

### Sample positions

After mixing, the driver advances the sample position of every active or
released voice by one frame, including voices that the mixer skipped. It moves a voice on
by the step that the mixer left in its slot, and clears the step. Where the
step is 0, as for a voice that the mixer skipped, it computes the step again.
A slot that the mixer frees keeps its step, though. A note that starts there
and isn't mixed in its first frame therefore moves on by the step of the note
before it. Past the end of the sample, a loop goes back by its length until
it's inside the sample. In loop mode 3 the slot is freed. In loop mode 1 the
older revisions store the free state through a register that doesn't contain
the address of the slot. The slot therefore stays in use until the mixer next
takes the voice and frees it. The revision of *Donkey Kong Country 3* frees
it, as in mode 3.

## Locating the driver

`agbsf2` finds the driver in a ROM from the code of the init, without fixed
addresses. The init of every revision copies the ARM code with the same Thumb
sequence:

```
ldr r1, =code in ROM
ldr r2, =destination in IWRAM
ldr r3, =address of the code's size
ldr r3, [r3]               ; 681B
ldr r4, =size limit
cmp r3, r4                 ; 42A3
```

`agbsf2` takes the first match whose size word is between 0x100 and 0x8000
and whose code is inside the ROM. The init starts with `push {r4-r7, lr}`
(`B5F0`) a little before it. Within 0x60 bytes after the copy, the init sets up
the tables and the mixer:

```
ldr r0, =tune table variable
ldr r1, =tune table
str r1, [r0]               ; 6001
ldr r0, =sound effect table variable
ldr r1, =sound effect table
str r1, [r0]               ; 6001
ldr r0, =mix rate
bl  set the mix rate
movs r0, #voice limit      ; 20xx
bl  set the voice limit
```

From this code, `agbsf2` reads the tune table, the mix rate and the voice
limit. In the ARM code,
`agbsf2` looks for the command reader to tell the formats apart. In the
channel-byte format it's

```
ldrb r0, [fp], #1          ; E4DB0001
ldr  r0, [ip, r0, lsl #2]  ; E79C0100
```

and in the channel-nibble format it splits the byte first:

```
ldrb r0, [fp], #1          ; E4DB0001
lsr  r1, r0, #4            ; E1A01220
bic  r0, r0, #0xF0         ; E3C000F0
ldr  r0, [ip, r0, lsl #2]  ; E79C0100
```

Three more patterns identify the revision of *Donkey Kong Country 3*. Its
controller handler compares the controller with 20 to 23 in turn:

```
cmp r2, #20                ; E3520014
beq ...                    ; 0Axxxxxx
cmp r2, #21                ; E3520015
beq ...
cmp r2, #22                ; E3520016
beq ...
cmp r2, #23                ; E3520017
```

Where the routine that moves the notes on takes a sample past its end, it
stores the free state for loop modes 3 and 1 through `r0`. `r0` contains the
address of the slot:

```
mov  r3, #0x10             ; E3A03010
strb r3, [r0]              ; E5C03000
b    ...                   ; EAxxxxxx
mov  r3, #0x10             ; E3A03010
strb r3, [r0]              ; E5C03000
```

The older revisions store the state for mode 1 through `r8` (`strb r3, [r8]`,
`E5C83000`).

Its mixer scales the level of a note by the level of the channel, and leaves
the note out where the result is 0:

```
mul  r4, r3, r4            ; E0040493
lsr  r4, r4, #7            ; E1A043A4
cmp  r4, #0                ; E3540000
beq  ...                   ; 0Axxxxxx
```

`agbsf2` finds the tables of the driver from the instruction that loads the
address of each one (`ldr rX, [pc, #imm]`) and the instruction after it:

| Table | Load | Next instruction |
|---|---|---|
| the slots of a channel, in bytes (a word in ROM: 0xF0 for 6 slots) | `ldr r4, =...` | `ldr r4, [r4]` (`E5944000`) |
| pitch table, at *n* = 0 | `ldr r6, =...` | `add r2, r6, r3, lsl #2` (`E0862103`) |
| sine table | `ldr r6, =...` | `add r6, r6, r1, lsl #2` (`E0866101`) |
| fade table | `ldr r2, =...` | `ldrb r1, [r2, r1]` (`E7D21001`) |

The size of the slots of a channel sets the number of slots per channel.
Without the slot size, `agbsf2` assumes 6 slots. Without the fade table,
`agbsf2` approximates the table. Without the pitch and sine tables, it
computes the pitch in floating point, within a fraction of a cent.

The rate routine supports four mix rates, with their samples per frame and
pitch scales: 10512 Hz (176 samples), 13379 Hz (224), 18157 Hz (304) and
21024 Hz (352). It returns -1 and changes nothing for a rate above its limit.
The limit is 13379 Hz in every game except *It's Mr. Pants* and the prototype
of *Battletoads*. In those two, it's 21024 Hz.
`agbsf2` reads the rate from the init and assumes 13379 Hz for any other.

If the command reader can't be found, `agbsf2` tries every track of
every tune in both formats and chooses the format in which every track ends.
This can happen when `--driver rare` and `--song-table` supply a tune table
but detection can't find the init routine. If both formats work, it chooses
the one that reads more commands: the wrong format usually mistakes the
argument of a note for an end command. Use `--song-count` if the count read from the
table is wrong.
