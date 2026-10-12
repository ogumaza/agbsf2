# Rare's GBA sound driver

This document covers the tune format, sequencer and mixer in Rare's Game Boy
Advance sound driver. It's based on the driver's code in the US release of
*Donkey Kong Country* (game code `A5NE`), and the addresses given as
examples come from that game. Other games put the driver and its data
elsewhere, and `supergbamidi` finds them from the code (see [Locating the
driver](#locating-the-driver)).

The tune data is close to a Standard MIDI File: tracks of MIDI-like commands
with delays in ticks, a tempo in microseconds per quarter note, MIDI channels,
programs, controllers and pitch bends. The driver plays the tunes with sampled
instruments only. It doesn't use the Game Boy's PSG channels.

## Games and revisions

The games read their tracks in one of two formats. In the first, a command
that has a MIDI channel gives it in a byte after the command byte. In the
second, the command byte holds the command number in its low nibble and the
channel in its high nibble. The revisions differ in a few other details:

| Game | Commands | Note slots per channel | Sound effect records | Controller 100 |
|---|---|---|---|---|
| *Donkey Kong Country* (`A5NE`) | channel byte | 6 | 5 | stored |
| *Sabre Wulf* (`AWUE`) | channel byte | 6 | 5 | ignored |
| *It's Mr. Pants* (`BPIE`) | channel byte | 6 | 4 | ignored |
| *Banjo-Kazooie: Grunty's Revenge* (`BKZX`) | channel nibble | 6 | 4 | stored |
| *Donkey Kong Country 2* (`B2DE`) | channel nibble | 6 | 5 | stored |
| *Banjo-Pilot* (`BAJE`) | channel nibble | 5 | 4 | stored |
| *Donkey Kong Country 3* (`BDQE`) | channel nibble | 6 | 5 | stored |

The rest of this document applies to all these revisions, apart from what it
says of *Donkey Kong Country 3*'s.

### Donkey Kong Country 3's revision

*Donkey Kong Country 3* uses a later revision with these changes:

* Controllers 20 to 23 set the attack, decay, sustain and release of a
  channel's notes, in place of their instrument's (see
  [Controllers](#controllers) and [Envelopes](#envelopes)).
* The game can start a tune partway through, at a marker that its tracks hold
  (see [Starting a tune](#starting-a-tune)).
* Each channel has a level that the game can turn down, and a note at a level
  of 0 doesn't count towards the voice limit (see [Mixing](#mixing)).
* The routine that moves the notes on frees a slot in loop mode 1 at the
  sample's end, which the older revisions miss (see [Sample
  positions](#sample-positions)).

Its routines are at `0x080AED34` (request a tune), `0x080AED90` (init) and
`0x080AEE82` (per-frame routine), and the routine that looks for a marker is
at `0x080AF61C`. Its RAM holds:

| Address | Contents |
|---|---|
| `0x03001464` | the requested tune, or -1 |
| `0x030012A0` | the tune playing, or -1 |
| `0x030023C0` | the marker to start the next tune at, or -1 |
| `0x030023F0` | each channel's settings from controllers 20 to 23: 16 words of 4 bytes |
| `0x030023C4` | the music volume, out of 128: a word |
| `0x03001400` | each channel's level: 16 bytes |
| `0x03001380` | each channel's volume (controller 7): 16 bytes |
| `0x030014C0` | the note slots: 6 for each of the 16 channels |
| `0x03003C60` | the sound effect records: 5 |
| `0x03001294` | samples a frame |
| `0x03003D30` | the output buffer flag |

## Architecture

The driver has two parts. The routines that the game calls are Thumb code in
ROM. The sequencer, the envelopes and the mixer are ARM code, which the init
copies to IWRAM: in `A5NE`, 0x10A0 bytes from `0x080311A8` to `0x03002B40`.

The game calls the per-frame routine once a frame. The routine runs these steps:

1. If the game has requested a tune, it starts it (see [Starting a
   tune](#starting-a-tune)).
2. It clears the mix buffer.
3. If a tune is playing and the music isn't paused, it runs the tracks, advances
   each channel's vibrato, and updates the note envelopes.
4. It updates the sound effects and their envelopes.
5. It mixes the voices into the mix buffer, and writes the result as 8-bit
   samples into the half of the output buffer that the DMA isn't playing.
6. It advances every voice's sample position by one frame.

The output is mono. Timer 0 runs at the mix rate, and DMA1 feeds the samples
to FIFO A, which plays on both sides at full volume. At each VBlank the game
calls a routine that swaps the two halves of the output buffer and restarts
DMA1. The init sets a mix rate of 13379 Hz, or 224 samples per display frame
(59.7275 Hz).

In `A5NE` the routines are:

| Address | Routine |
|---|---|
| `0x08032294` | init: sets up the sound hardware, copies the ARM code, and sets the tune and sound effect tables, the mix rate and the voice limit |
| `0x08032248` | request a tune: `r0` = tune number; it starts on the next frame |
| `0x0803237C` | per-frame routine |
| `0x0803226C` | buffer swap: flips the output buffer and restarts DMA1 |
| `0x08032876` | set the mix rate: `r0` = rate in Hz |
| `0x080328F6` | set the voice limit: `r0` = the most voices to mix |
| `0x08032734` | set the music volume: `r0` = volume, 0x80 for full |
| `0x0803273A` | set the sound effect volume |
| `0x08032538` | vibrato update |
| `0x030032B0` | sequencer (ARM); its command table is at `0x03003378` |
| `0x03002E14` | envelopes of the notes (ARM); `0x03002DFC` updates those of the sound effects |
| `0x03002B84` | mixer (ARM); `0x03003884` mixes one voice, and `0x03003A3C` works out its pitch |
| `0x03002CF4` | moves the notes on (ARM); `0x03002CDC` moves the sound effects on, and while the music is paused it's the only one that runs |

Its RAM holds:

| Address | Contents |
|---|---|
| `0x030015C4` | the requested tune, or -1 |
| `0x03001420` | the tune playing, or -1 |
| `0x03004004` | the address of the tune table |
| `0x030014F0` | the address of the sound effect table |
| `0x03001410` | the number of tracks |
| `0x03001540` | ticks per quarter note |
| `0x03001500` | each track's position: 16 words |
| `0x030028F0` | each track's counter: 16 64-bit values |
| `0x03001430` | each track's loop position: 16 words |
| `0x0300141C` | the address of the program map |
| `0x03002B30` | the address of the instrument table |
| `0x03003FFC` | the tempo, in microseconds per quarter note |
| `0x030014F4` | the frame time, in 1/256 microseconds |
| `0x030015D0` | each channel's instrument: 16 words |
| `0x03001560` | each channel's volume (controller 7): 16 bytes |
| `0x03001580` | each channel's pitch bend: 16 words |
| `0x03001470` | each channel's modulation (controller 1) and vibrato phase: 16 pairs of words |
| `0x03001610` | each channel's mono flag: 16 bytes |
| `0x03001550` | each channel's controller 100: 16 bytes |
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
`supergbamidi` doesn't convert them. This document covers only their role in the
mixer.

## Tunes

### Tune table

The init stores the address of the tune table, an array of words, each the
address of a tune's header. The driver doesn't know how many tunes there are, so
`supergbamidi` counts the entries up to the first that isn't a valid header.

### Tune header

A tune's header is 5 words:

| Offset | Contents |
|---|---|
| `+0x00` | the number of tracks, 1-16 |
| `+0x04` | ticks per quarter note |
| `+0x08` | the address of the track list: a word for each track, the address of its first command |
| `+0x0C` | the address of the program map: 128 bytes, each MIDI program's index in the instrument table, or 0xFF |
| `+0x10` | the address of the instrument table: a word for each instrument, its address |

### Starting a tune

The request routine only stores the tune's number. On the next frame the
per-frame routine starts it: it copies the header's values and the track list
into its RAM, sets every track's counter to 0, the frame time to 1 and the
tempo to 0, frees every note slot, centres every channel's pitch bend and
turns every channel's modulation off.

It doesn't reset the channels' instruments, volumes or mono flags, or the
tracks' loop positions. If a tune plays a note before setting the channel's
program, that note uses the instrument left over from the previous tune. The
init sets every channel's volume to 0x7F and turns mono mode off.

*Donkey Kong Country 3*'s revision also sets each channel's level to 128 and
takes away its envelope settings when it starts a tune. If the game has given
it a marker, every track but track 0 then skips ahead to its first controller
104 whose value is the marker, keeping the last loop start (controller 102) it
passes, and the marker goes back to -1. A track that reaches its end first
ends the search, and it and the tracks after it start from their beginnings.
The tracks' counters aren't changed, so the tracks start from the markers
together. `supergbamidi` plays every tune from its start.

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
| 6 | key, velocity | note off; the velocity is ignored |
| 7 | controller, value | controller (see [Controllers](#controllers)) |
| 8 | program | program change |
| 9 | 1 byte | channel pressure, ignored |
| 10 | 2 bytes | pitch bend: 0x2000 is the centre |
| 11 | | end of the track |
| 12 | | does nothing |

Commands 4-10 apply to a MIDI channel. In the channel-byte format the channel
is a byte after the command byte, before the arguments, so a note on is
`05 cc kk vv`. In the channel-nibble format, the channel is the high nibble of
the command byte, so a note on is `c5 kk vv`. Commands 4 and 5 run the same
code.

The sequencer reads the command byte (`ldrb r0, [fp], #1`) and jumps through its
table of 13 commands without checking the number. A number above 12 jumps to
whatever follows the table. `supergbamidi` stops a track at such a command, with
a warning.

### Timing

Each track has a 64-bit counter of song time in 1/256 microseconds. Each frame
the sequencer subtracts the frame time from every track's counter, and while a
track's counter is negative, it runs the track's commands. A delay adds its
length to the counter, ticks × tempo / ticks per quarter note, in 1/256
microseconds. The driver works this out with its 16-bit division routine, so
the fraction is truncated, and it multiplies a one-byte delay in 32 bits. If the
counter is still negative after a delay, the track goes on to the next command
in the same frame.

The frame time is 1 until the first tempo command runs. The tempo command sets
the tempo and works out the frame time as 60000000 / 3600 µs, which is
4266666 in 1/256 microseconds, or 1/60 s. So the driver counts each frame as
1/60 s, but the GBA shows 59.7275 frames a second, and every tune plays about
0.46% slower than its tempo says.

When track 0 starts with the tempo command, its counter goes down by 1 in the
first frame, and the other tracks' counters by a whole frame. From then on,
track 0 runs up to a frame later than the other tracks.

The end command doesn't move its track on, so the track reads it again every
frame. In a frame in which every track reads its end, the sequencer stops the
tune: it sets the tune playing to -1. From the next frame the driver doesn't
run the tracks, the vibrato or the envelopes, but it goes on mixing the notes
that were still playing, until the game starts another tune.

### Controllers

| Controller | Effect |
|---|---|
| 1 | modulation: the depth of the channel's vibrato. 0 turns the vibrato off. |
| 7 | the channel's volume |
| 20 | in *Donkey Kong Country 3*'s revision, the attack of the channel's notes: its number of steps |
| 21 | in *Donkey Kong Country 3*'s revision, the decay of the channel's notes: its fade table entry |
| 22 | in *Donkey Kong Country 3*'s revision, the sustain level of the channel's notes, in 1/128ths of the full level |
| 23 | in *Donkey Kong Country 3*'s revision, the release of the channel's notes: its fade table entry |
| 100 | stores the value in a byte for each channel, which the driver itself doesn't use. *Sabre Wulf* and *It's Mr. Pants* ignore it. |
| 102 | loop start: saves the position after the command as the track's loop position |
| 103 | loop end: goes back to the track's loop position |
| 104 | in *Donkey Kong Country 3*'s revision, a marker that the game can start the tune at (see [Starting a tune](#starting-a-tune)). It does nothing as the track plays. |
| 126 | mono: the channel plays every note in its first slot |
| 127 | poly: the channel goes back to picking a slot for each note |

The driver ignores the others. The loop position belongs to the track, and a
tune loops forever by ending each looping track with controller 103. A loop end
without a loop start goes back to whatever loop position the track had from an
earlier tune. `supergbamidi` ignores such a loop end, with a warning.

Controllers 20 to 23 keep their values until a program change on the channel,
which sets all four back to 0xFF. A value of 0x80 or more leaves the
instrument's setting, and [Envelopes](#envelopes) says how the others count.

## Instruments

### Program changes

A program change looks up the program in the tune's program map, then uses
that index to select the channel's instrument from the instrument table.
The driver doesn't check for 0xFF, the program map's entry for a program that
the tune doesn't have, and reads the 256th word of the instrument table.

### Instrument records

An instrument is 17 words:

| Offset | Contents |
|---|---|
| `+0x00` | type: 0x20 or 0x21 for a sample, 0x22 for a drum kit, 0x23 for a key split |
| `+0x04` | loop mode: 1 plays the sample once, 2 loops it. The driver also knows 3 and 4 (see [Mixing](#mixing)). |
| `+0x08` | the sample's rate in Hz |
| `+0x0C` | root key: the MIDI key at which the sample plays at its rate |
| `+0x10` | the address of the sample's first byte |
| `+0x14` | the loop's length in bytes. The loop is the last part of the sample. |
| `+0x18` | the address of the byte after the sample's last |
| `+0x1C` | a drum kit's or key split's key map: 128 bytes, each key's index in the next table |
| `+0x20` | a drum kit's or key split's instrument table: a word for each instrument, its address |
| `+0x24` | attack, 0-99 |
| `+0x28` | decay: an index in the fade table, 0-99 |
| `+0x2C` | sustain level, 0-99 |
| `+0x30` | release: an index in the fade table, 0-99 |
| `+0x34` | fine tune in cents, signed |
| `+0x38` | the pitch bend's range in semitones |
| `+0x3C` | vibrato rate: added to the vibrato's phase each frame, as a 24-bit fraction of a cycle |
| `+0x40` | vibrato depth |

Samples are signed 8-bit PCM. The mixer interpolates between each byte and the
next, so it also reads the byte at the sample's end.

A drum kit or a key split picks one of its own instruments for each key from
its key map. A drum kit plays it at that instrument's root key, so every key
sounds at its sample's pitch, and a key split plays it at the key. The
driver takes any type except 0x20, 0x21 and 0x23 for a drum kit, and doesn't
check the key map's entries.

The instrument that a note plays gives the sample, its rate, root key and
loop, the envelope and the fine tune. The bend range and the vibrato's rate
and depth come from the channel's instrument, which is the drum kit or key
split itself when there is one.

## Notes

### Note slots

Each channel has its own note slots, 6 in all of the games except
*Banjo-Pilot*, which has 5. A note slot is 0x28 bytes, and a sound effect
record has the same layout:

| Offset | Contents |
|---|---|
| `+0x00` | state: 0x10 free, 0x11 on, 0x12 released |
| `+0x01` | the instrument's loop mode |
| `+0x02` | the key that started the note |
| `+0x03` | velocity + 1 |
| `+0x04` | the envelope's phase, 0-6 |
| `+0x05` | the key the note plays at: the key, or a drum kit instrument's root key |
| `+0x06` | the channel, signed; -1 for a sound effect |
| `+0x08` | the pitch step that the mixer used this frame |
| `+0x0C` | the address of the sample byte it's playing |
| `+0x10` | the position's fraction, 23 bits |
| `+0x14` | the envelope's level |
| `+0x18` | the envelope's step |
| `+0x1C` | the sustain level |
| `+0x20` | the address of the instrument it plays |
| `+0x24` | a pitch scale, 0x10000 for 1, which notes don't change |

### Note on and note off

A note on picks a slot from the channel's slots. In mono mode it always takes
the first slot, cutting off the note playing there. Otherwise it takes the
first free slot, or if there's none, the first slot whose note is released. If
every slot holds a note that hasn't been released, the driver drops the new
note.

The note starts at the sample's first byte, with its envelope in phase 0 at a
level of 0, and the slot keeps the instrument and the key it plays at.

A note off releases the first of the channel's slots whose note is on and was
started by the key, and only that one. If a channel plays a key twice without
a note off in between, the next note off releases the note in the lower slot,
and the other goes on until another note off for the key.

## Envelopes

The envelope's level runs from 0 to 0x80000. Each frame the driver updates the
envelope of every note that's on or released, by its phase. Where a phase
ends, the next often starts in the same frame.

* **Phase 0** sets up the attack. It takes (99 − attack) × 32 / 99 steps,
  truncated. With 0 steps, the note goes straight to the full level in
  phase 2. Otherwise the step is 0x8000 / (steps + 1) × 16, with the division's
  remainder × 16 as the starting level, and the attack goes on in phase 1.
* **Phase 1** adds the step each frame until the level would reach 0x80000,
  then sets it to 0x80000 and moves to phase 2.
* **Phase 2** sets up the decay. The sustain level is sustain / 99 of the full
  level, worked out as 8.8 fixed point, so it's slightly below that. The decay
  takes the number of frames that the fade table gives for the decay index. A
  decay of 0 frames goes straight to the sustain level. Otherwise the step is
  (full level − sustain level) / frames, and the decay goes on in phase 3.
* **Phase 3** subtracts the step each frame until the level reaches the
  sustain level, then moves to phase 4.
* **Phase 4** holds the sustain level.
* **Phase 5** sets up the release, from the level the note had, over the
  number of frames that the fade table gives for the release index, and goes
  on in phase 6. A release of 0 frames ends the note at once.
* **Phase 6** subtracts the step each frame and ends the note when the level
  would reach 0.

When a note has been released, the next update of phase 1, 3 or 4 moves it to
phase 5 instead. During the decay, a level below 0x1000 ends the note, so a
note with a sustain level of 0 ends when its decay is over.

In *Donkey Kong Country 3*'s revision, the settings that controllers 20 to 23
give a note's channel take the place of the instrument's, each where it's
below 0x80. Phase 0 takes controller 20 as the attack's number of steps,
phase 2 takes controller 22 × 256 as the sustain level and controller 21 as
the decay's fade table entry, and phase 5 takes controller 23 as the release's
entry. Each phase reads the channel's settings as it starts, so a note that
was playing when they changed takes the new ones in its later phases. Where a
channel has a sustain level from controller 22 but no decay from controller
21, the driver reads the decay through a register that holds whatever the code
before left in it, so `supergbamidi` takes the instrument's decay there. Sound
effects keep their instruments' envelopes.

The fade table is 100 bytes, at `0x08032C94` in `A5NE`. Entry *i* gives a
decay or release with index *i* a length of entry + 1 frames, and an entry of
0 means no fade at all. The entries mostly fall, from 0xFE (255 frames, about
4.3 s) at index 0 to 0 at index 99.

## Pitch

The driver works out each voice's pitch every frame, as a step through its
sample for each output sample, in 1/2^23 bytes:

1. It adds the instrument's fine tune and the channel's bend and vibrato as
   32.32 fixed-point semitones. The fine tune is in cents, × 2^32 / 100. The
   bend is (bend − 0x2000) × range × 0x80000, which makes a full bend equal to
   the bend range.
2. It adds the difference between the key the note plays at and the
   instrument's root key, in whole semitones.
3. It looks the semitones up in the pitch table, 128 words of 2^(*n*/12) in
   9.23 fixed point for *n* from -64 to 63 (`0x08032A94` in `A5NE`, with
   *n* = 0 at `0x08032B94`), and interpolates linearly between the entry and
   the next by the fraction.
4. It multiplies by the instrument's rate × the pitch scale for the mix rate
   (2^38 / 13379 at 13379 Hz) / 2^14, and divides by 2^24.

The pitch only changes from one frame to the next, so a bend applies to the
whole frame in which its command runs, and vibrato moves in steps of a frame.

### Vibrato

Each channel has a vibrato phase. While modulation is above 0, the phase
increases each frame by the vibrato rate of the channel's instrument,
wrapping at 2^24. A modulation of 0 sets it back to 0. The vibrato adds
(modulation + 1) × sine(phase) × depth to the pitch, in 32.32 semitones,
where the sine comes from a table of 257 words (`0x08032CF8` in `A5NE`) with a
peak of 2^32 / 12800. With the modulation at 127, the vibrato's depth in cents
is the instrument's vibrato depth.

## Mixing

### Volume

The mixer gives each voice a level from 0 to 128:

* For a note, the music volume × (the channel's volume + 1) / 128, and in
  *Donkey Kong Country 3*'s revision × the channel's level / 128. A tune
  starts with each channel at a level of 128, and only the game's code turns
  one down.
* For a sound effect, the sound effect volume.
* Then × (velocity + 1) / 128 and × the envelope's level / 2^19.

Each step is truncated, so the level is linear in the volume, the velocity and
the envelope, and a note at full volume and velocity has a level of 128.

The init sets the music volume to 128. *Donkey Kong Country 3*'s code sets it
again after it requests a tune, through a routine at `0x080AF270`. Most tunes
play at a music volume of 69. `supergbamidi` models every tune of that game at
69.

### Voices

The mixer takes the sound effects that are on, then those that are released,
then, unless the music is paused, the notes that are on, then those that are
released, each in the order of their records, and stops at the voice limit.
The init sets the limit to 8. The voices past the limit are silent for the
frame. Note slots are in channel order, so when more than 8 voices play, the
mixer leaves out released notes before notes that are on, and notes on higher
channels before those on lower ones. In *Donkey Kong Country 3*'s revision, a
note whose level is 0 before its velocity and envelope count isn't mixed and
doesn't count towards the limit. At a music volume of 69, that's every note of
a channel at volume 0.

`supergbamidi`'s model makes the same choices, frame by frame. Where the mixer
leaves out every note of a key on a channel, the MIDI file mutes the key with
polyphonic key pressure until the mixer takes one of its notes again. The
README describes the mutes.

For each output sample, the mixer interpolates the voice's sample linearly
between the byte at its position and the next, by the position's fraction,
multiplies by the level / 128, and adds the result to the mix buffer. The
first voice of a frame writes the mix buffer rather than adding to it. When all
voices are mixed, the mixer clips each value to the range -128 to 127 and
writes it to the output buffer.

When a voice reaches the end of its sample, a loop (loop mode 2 or 4) goes
back by the loop's length. Any other loop mode frees the voice's slot, and the
rest of the frame is silent.

### Sample positions

After mixing, the driver advances every active or released voice's sample
position by one frame, including voices the mixer skipped. It moves a voice on
by the step that the mixer left in its slot, and clears the step. Where the
step is 0, as for a voice that the mixer skipped, it works the step out again.
A slot that the mixer frees keeps its step, though, so a note that starts
there and isn't mixed in its first frame moves on by the step of the note
before it. Past the sample's end, a loop goes back by its length until it's
inside the sample. In loop mode 3 the slot is freed. In loop mode 1 the older
revisions store the free state through a register that doesn't hold the
slot's address, so the slot stays in use until the mixer next takes the voice
and frees it. *Donkey Kong Country 3*'s revision frees it, as in mode 3.

## Locating the driver

`supergbamidi` finds the driver in a ROM from the init's code, without fixed
addresses. Every revision's init copies the ARM code with the same Thumb
sequence:

```
ldr r1, =code in ROM
ldr r2, =destination in IWRAM
ldr r3, =address of the code's size
ldr r3, [r3]               ; 681B
ldr r4, =size limit
cmp r3, r4                 ; 42A3
```

`supergbamidi` takes the first match whose size word is between 0x100 and 0x8000
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

This gives the tune table, the mix rate and the voice limit. In the ARM code,
`supergbamidi` looks for the command reader to tell the formats apart. In the
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

Three more patterns tell *Donkey Kong Country 3*'s revision. Its controller
handler compares the controller with 20 to 23 in turn:

```
cmp r2, #20                ; E3520014
beq ...                    ; 0Axxxxxx
cmp r2, #21                ; E3520015
beq ...
cmp r2, #22                ; E3520016
beq ...
cmp r2, #23                ; E3520017
```

and where the routine that moves the notes on takes a sample past its end, it
stores the free state for loop modes 3 and 1 through `r0`, which holds the
slot's address:

```
mov  r3, #0x10             ; E3A03010
strb r3, [r0]              ; E5C03000
b    ...                   ; EAxxxxxx
mov  r3, #0x10             ; E3A03010
strb r3, [r0]              ; E5C03000
```

The older revisions store mode 1's through `r8` (`strb r3, [r8]`, `E5C83000`).

Its mixer scales a note's level by the channel's level, and leaves the note out
where that gives 0:

```
mul  r4, r3, r4            ; E0040493
lsr  r4, r4, #7            ; E1A043A4
cmp  r4, #0                ; E3540000
beq  ...                   ; 0Axxxxxx
```

It finds the driver's tables from the instruction that loads each one's
address (`ldr rX, [pc, #imm]`) and the instruction after it:

| Table | Load | Next instruction |
|---|---|---|
| a channel's slots, in bytes (a word in ROM: 0xF0 for 6 slots) | `ldr r4, =...` | `ldr r4, [r4]` (`E5944000`) |
| pitch table, at *n* = 0 | `ldr r6, =...` | `add r2, r6, r3, lsl #2` (`E0862103`) |
| sine table | `ldr r6, =...` | `add r6, r6, r1, lsl #2` (`E0866101`) |
| fade table | `ldr r2, =...` | `ldrb r1, [r2, r1]` (`E7D21001`) |

The size of a channel's slots gives the number of slots per channel. Without the
slot size, `supergbamidi` assumes 6 slots, and without the fade table, it uses
an approximation of it. Without the pitch and sine tables, it works the pitch
out in floating point, which is within a fraction of a cent.

The rate routine knows four mix rates, with their samples per frame and pitch
scales: 10512 Hz (176 samples), 13379 Hz (224), 18157 Hz (304) and 21024 Hz
(352). It returns -1 and changes nothing for a rate above its limit, which is
13379 Hz in every game except *It's Mr. Pants*, where it's 21024 Hz.
`supergbamidi` reads the rate from the init and assumes 13379 Hz for any other.

If the command reader can't be found, `supergbamidi` tries every track of
every tune in both formats and chooses the format in which every track ends.
This can happen when `--driver rare` and `--song-table` supply a tune table
but detection can't find the init routine. If both formats work, it chooses
the one that reads more commands: the wrong format usually mistakes a note's
argument for an end command. Use `--song-count` if the count read from the
table is wrong.
