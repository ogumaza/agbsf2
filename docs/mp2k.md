# MP2K, Nintendo's GBA sound driver

This document describes MusicPlayer2000 (MP2K), the sound driver in
Nintendo's Game Boy Advance SDK, which most GBA games use. It's often called
the "Sappy" engine, after a fan-made tool for it. This document covers the
song format, sequencer, sound channels and mixer. It's based on the driver's
code in *Pokémon Emerald* (game code `BPEE`), and the addresses given as
examples come from that game. Other games put the driver and its data
elsewhere. `agbsf2` finds them from the code (see [Locating the
driver](#locating-the-driver)).

The SDK calls the driver's library m4a. The routine names below are its known
function names.

## Versions

The driver changed little over the SDK's life. `agbsf2` handles these
differences:

* **The mixer.** Later versions can play a sample backwards, or decode a
  compressed one (see [Samples](#samples)). Earlier ones, such as the one in
  *The Legend of Zelda: A Link to the Past & Four Swords* (`AZLE`), ignore
  both voice types and play the sample forwards as it is. A mono mixer, as in
  *The Legend of Zelda: The Minish Cap* (`BZME`) and *Super Robot Taisen:
  Original Generation 2* (`B2RE`), has a single output buffer, which DMA1
  feeds to FIFO A, and the sound control register sends FIFO A to both sides.
  It plays each DirectSound channel at the average of its right and left
  volumes, so the pan has no effect on them, though it still does on the PSG
  channels.
* **The settings.** Each game's init gives the driver its mix rate, the
  number of DirectSound channels and the master volume (see
  [Architecture](#architecture)).
* **The music players.** Each game has its own table of players, with their
  track counts.
* **Camelot's version.** *Golden Sun: The Lost Age* (`AGFE`) changes how a
  note takes a channel and works out its volume and rate, and has a mixer of
  Camelot's with straight-line releases, synth voices and an echo (see
  [Camelot's version](#camelots-version)).

Some games change the driver's code itself. `agbsf2` models the SDK's
driver and Camelot's version. Another modified driver requires a study of the
driver's code to find how the driver behaves differently.

## Architecture

The driver is Thumb code in ROM, apart from the mixer, which is ARM code that
the init copies to IWRAM. The game calls the init once, the main routine once
a frame from its VBlank interrupt, and a routine that restarts the sound DMA
once a frame from its VCount interrupt.

The main routine runs these steps:

1. It runs each music player's sequencer, in the order the init opened them:
   the ticks that are due, then the volume and pitch of the channels whose
   tracks changed them (see [Tracks](#tracks)).
2. It runs the PSG routine, which moves the four PSG channels' envelopes on
   and writes the sound registers (see [PSG envelopes](#psg-envelopes)).
3. It runs the mixer, which moves each DirectSound channel's envelope on and
   mixes it into the output buffer (see [Mixer](#mixer)).

The output is stereo, apart from the mono mixer's (see [Versions](#versions)):
DMA1 feeds the right side's buffer to FIFO A, and DMA2 the left side's to
FIFO B. Timer 0 runs at the mix rate.

In `BPEE` the routines are:

| Address | Routine |
|---|---|
| `0x082E0070` | `m4aSoundInit`: copies the mixer to IWRAM, sets up the driver and the PSG, sets its mode, and opens the music players |
| `0x082E0130` | `m4aSongNumStart`: `r0` = song number; starts the song on its music player |
| `0x082E0124` | `m4aSoundMain`: the main routine, which calls `SoundMain` (`0x082DF05C`) |
| `0x082DFA10` | `m4aSoundVSync`: restarts the sound DMA at the start of the output buffer |
| `0x082DFA5C` | `MPlayMain`: a music player's sequencer |
| `0x082DFD38` | `ply_note`: starts a note |
| `0x082E0CA8` | `CgbSound`: the PSG routine |
| `0x082DF0E0` | `SoundMainRAM`: the mixer, which the init copies to `0x03001AA8` |
| `0x082DFFCC` | `MidiKeyToFreq`: a sample's rate for a key |
| `0x082E0B34` | `MidiKeyToCgbFreq`: a PSG channel's frequency setting for a key |
| `0x082E0678` | `m4aSoundMode`: sets the driver's mode |

The driver's state is in a `SoundInfo` structure whose address the init
stores at `0x03007FF0`, where the BIOS reserves a word for it. In `BPEE` it's
at `0x03006380`:

| Offset | Contents |
|---|---|
| 0x00 | an ID, "Smsh" (`0x68736D53`), which the driver changes while it works on the structure |
| 0x04 | the frames left before the DMA restarts at the start of the buffer |
| 0x05 | the reverb, 0-127 |
| 0x06 | the number of DirectSound channels the mixer uses |
| 0x07 | the master volume, 0-15 |
| 0x08 | the mix rate's setting, 1-12 |
| 0x0A | the PSG envelopes' frame counter, 14 down to 0 |
| 0x0B | the frames the output buffer holds |
| 0x10 | samples a frame |
| 0x14 | the mix rate in Hz, as the driver works it out |
| 0x18 | the mixer's step for each Hz of a sample's rate (see [Mixer](#mixer)) |
| 0x1C | the address of the four PSG channels' records |
| 0x20 | the first music player's sequencer, and 0x24 its state |
| 0x28 | the PSG routine |
| 0x34 | the address of the command table |
| 0x38 | `ply_note` |
| 0x50 | 12 DirectSound channel records of 0x40 bytes |
| 0x350 | the output buffer: 1584 bytes for the right side, then 1584 for the left |

## Songs

### Song table

The song table is an array of 8-byte entries: the address of the song's
header, and two halfwords, each the number of the music player that plays it.
`m4aSongNumStart` uses the first. Nothing in the driver gives the number of
songs. `agbsf2` counts the entries up to the first whose header isn't in
the ROM or isn't a valid header, or whose player isn't in the player table. An
entry that's all zero doesn't end the table, since a GSF rip can zero the
entries of the songs it doesn't hold. It's listed as empty, and all-zero entries
after the last song aren't counted.

An entry whose header has no tracks is a placeholder, and `--info` lists it as
empty.

### Music players

The music player table has a 12-byte entry for each player: the address of its
state, the address of its tracks' records, the number of tracks it has, and a
halfword. With the halfword set, a song can't take the player over from a song
of higher priority that's still playing.
The init opens each player, up to a count in its code. `agbsf2` reads
the players up to the first entry whose state or tracks aren't in RAM, or that
has no tracks.

A song plays only as many tracks as its player has. In `BPEE` the music
player has 10 tracks, and the three sound effect players 3, 9 and 1.

### Song header

| Offset | Size | Contents |
|---|---|---|
| 0 | 1 | number of tracks, up to 16 |
| 1 | 1 | the number of blocks the song takes up, which the driver doesn't use |
| 2 | 1 | priority |
| 3 | 1 | reverb: with bit 7 set, the song sets the driver's reverb to the low 7 bits |
| 4 | 4 | the address of the song's voice group |
| 8 | 4 each | each track's start |

### Starting a song

`m4aSongNumStart` starts the song on its player, unless its priority keeps it
out (see [Music players](#music-players)). It sets the player's tempo to 150, its tick counter
to 0, and its priority to the song's. Each of the song's tracks, up to the
player's track count, starts at its first command, and the rest of the player's
tracks stop. The player plays its first tick in the next frame.

## Tracks

### Commands

A track is a stream of command bytes, each followed by its arguments:

| Byte | Name | Arguments | Meaning |
|---|---|---|---|
| `80`-`B0` | `W00`-`W96` | | wait: the track's next command runs that many ticks later (see [Lengths](#lengths)) |
| `B1` | `FINE` | | end of the track |
| `B2` | `GOTO` | 4-byte address | go on from the address |
| `B3` | `PATT` | 4-byte address | play the pattern at the address, which ends with `PEND`. Patterns can call patterns, 3 deep; a fourth call ends the track |
| `B4` | `PEND` | | return from a pattern; outside a pattern it does nothing |
| `B5` | `REPT` | count, 4-byte address | go back to the address, so that the part plays count times in all; a count of 0 goes back for ever. The track has one repeat counter for all its repeats |
| `B9` | `MEMACC` | operation, byte, value, [4-byte address] | change or test a byte of a 16-byte memory area that the game can read and write. Operations 0-5 set, add or subtract the value or another byte of the area; 6-17 compare and jump to the address if the comparison holds |
| `BA` | `PRIO` | priority | the track's priority |
| `BB` | `TEMPO` | tempo | the tempo, in half quarter notes a minute at 60 frames a second |
| `BC` | `KEYSH` | shift | key shift in semitones (signed) |
| `BD` | `VOICE` | voice | choose a voice of the song's voice group; the track keeps a copy of it |
| `BE` | `VOL` | volume | the track's volume, 0-127 |
| `BF` | `PAN` | pan | the track's pan, with `40` the centre |
| `C0` | `BEND` | bend | the pitch bend, with `40` the centre |
| `C1` | `BENDR` | range | the bend range in semitones |
| `C2` | `LFOS` | speed | the LFO's speed |
| `C3` | `LFODL` | delay | the ticks after each note before the LFO starts |
| `C4` | `MOD` | depth | the LFO's depth |
| `C5` | `MODT` | type | the LFO's target: 0 pitch, 1 volume, 2 pan |
| `C8` | `TUNE` | tune | fine tuning in 64ths of a semitone, with `40` the centre |
| `CC` | `PORT` | register, value | write the value to a sound register |
| `CD` | `XCMD` | number, arguments | an extended command (see below) |
| `CE` | `EOT` | [key] | end a tied note |
| `CF` | `TIE` | [key, velocity, extra length] | a note that lasts until its `EOT` |
| `D0`-`FF` | `N01`-`N96` | [key, velocity, extra length] | a note of that many ticks, plus the extra length |

The command table gives the other byte values, `B6`-`B8`, `C6`, `C7` and
`C9`-`CB`, the same handler as `FINE`, so they end the track.

A byte below `80` where a command should be repeats the last command from
`BD` (`VOICE`) up, with that byte as its first argument. This running status
holds across waits, which don't change it.

A note takes up to three argument bytes below `80`: key, velocity and extra
length. Any trailing arguments can be left out. A left-out key or velocity
keeps its value from the track's last note, and a left-out extra length is 0.

The extended commands are:

| Number | Arguments | Meaning |
|---|---|---|
| `00`, `03` | | end of the track |
| `01` | 4-byte address | the track's voice plays this sample |
| `02` | type | the track's voice type |
| `04`-`07` | value | the track's voice's attack, decay, sustain or release |
| `08` | level | the echo's level (see [DirectSound envelopes](#directsound-envelopes)) |
| `09` | frames | the echo's length |
| `0A` | length | the track's PSG voice's length |
| `0B` | sweep | the track's PSG voice's sweep |
| `0C` | 2-byte count | wait that many ticks |
| `0D` | 4-byte offset | the point in the sample that the track's notes start at |

Commands `01`, `02`, `04`-`07`, `0A` and `0B` change the track's copy of its
voice, until its next `VOICE` command. The driver's table has no entry past
`0D`. Some other versions of the SDK have a table of 12 entries, `00`-`0B`,
without `0C` and `0D`.

### Lengths

The waits and notes take their lengths from a table of 49 values, in ticks:
0-24 one by one, then 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66,
68, 72, 76, 78, 80, 84, 88, 90, 92 and 96. A quarter note is 24 ticks. `TIE`
takes entry 0, so it has no length of its own.

### Timing

Each frame, a player adds its tempo to a counter, and plays a tick for each 150
in it. A tempo of 150, the default, plays a tick a frame. A tempo change takes
effect from the next frame, so the progress the counter had made towards the
next tick was made at the old tempo. `TEMPO` sets the tempo
to twice its argument, so it's in quarter notes a minute when a frame is 1/60 s.
The GBA shows 59.7275 frames a second, so the games play every song 0.46%
slower than its tempo says. The game can scale the tempo, but no song can.

In a tick, the sequencer takes the tracks in order. For each track, it first
counts down the length of each of its notes, and releases those that reach 0.
Then, if the track has no wait to finish, it runs commands until it reaches a
wait. Then it moves the track's LFO on (see [LFO](#lfo)). A player's tracks
that existed at the start of a tick count as playing, so the player stops in
the tick after its last track ended.

After the ticks, the sequencer works out the volumes and pitch of the tracks
whose commands changed them, and passes them on to their channels. A change
made in the middle of a frame reaches the channels at the end of the frame.

## Voices

### Voice groups

A song's voice group is an array of 12-byte voices, which `VOICE` indexes:

| Offset | Size | Contents |
|---|---|---|
| 0 | 1 | type |
| 1 | 1 | key: the key a drum kit's voice plays at |
| 2 | 1 | a PSG voice's length |
| 3 | 1 | a drum kit voice's pan, with bit 7 set, or channel 1's sweep |
| 4 | 4 | a sample's address, a square wave's duty, a wave pattern's address, a noise type, or a voice table's address |
| 8 | 1 each | attack, decay, sustain, release; or, for a key split, the address of its key table |

The type's low 3 bits choose a PSG channel: 1 and 2 are the square channels,
3 the wave channel and 4 noise, and 0 a DirectSound sample. The other bits are
flags:

| Bit | Meaning |
|---|---|
| `08` | a sample plays at the mixer's rate, whatever its key; a square or wave voice's frequency is rounded to suit the output's resolution |
| `10` | the sample plays backwards (later versions) |
| `20` | the sample is compressed (later versions) |
| `40` | key split: each key plays a voice of the voice table, chosen by the key table, at its own key |
| `80` | drum kit: key *n* plays voice *n* of the voice table, at that voice's key and pan |

A key split or drum kit's voice can't be another key split or drum kit. A note
on one plays nothing.

### Samples

A sample is a 16-byte header followed by its data:

| Offset | Size | Contents |
|---|---|---|
| 0 | 2 | type: 0 for 8-bit signed PCM, otherwise compressed |
| 2 | 2 | flags: the sample loops if bit 14 or 15 is set |
| 4 | 4 | 1024 times the rate at which key 60 plays it |
| 8 | 4 | loop start, in points |
| 12 | 4 | length, in points |

A loop runs from its start to the sample's end. A compressed sample keeps each
block of 64 points in 33 bytes. The first byte is the first point. Each other
point is the one before it plus a step from the table 0, 1, 4, 9, 16, 25, 36, 49,
-64, -49, -36, -25, -16, -9, -4, -1, chosen by the low half of the second byte for
the second point, and by each half of the following bytes, high half first,
for the rest.

## Notes

### Starting a note

A note sets the track's length for it, and reads its key, velocity and extra
length. A key split picks a voice of its voice table from its key table and
the note's key, and a drum kit takes voice *key* of its table, with that voice's
key as the key the note plays at. With bit 7 set, the drum kit voice's pan byte
gives the note a pan of twice the byte less `0xC0`.

The note's priority is the player's plus the track's, up to 255. A PSG voice
can only play on its channel. It takes it if the channel is off or its note is
released, or if its note has a lower priority, or the same priority and comes
from this track or a later one. Otherwise the note is dropped.

A sample takes the first DirectSound channel that's off, up to the number the
mixer uses. Failing that, if any channel's note is released, it takes one of
those, whatever their priority: the one with the lowest priority, from the
latest track among those, and the last in the list of channels among those.
If no note is released, it takes a channel whose note has a lower priority
than the new one, or the same priority from this track or a later one, chosen
in the same way. If there's none, the new note is dropped.

The note's channel leaves the track it played for and goes first in the new
track's list of channels. The note restarts the LFO's delay, and its phase if
the delay isn't 0. Then the channel gets the voice's type, sample and envelope,
the track's echo, the note's keys, velocity and priority, its volumes (see
[Volume and pan](#volume-and-pan)) and its rate or frequency setting (see
[Pitch](#pitch)), and the start flag. A DirectSound channel also gets the
track's sample offset.

### Releasing a note

A note is released when its length runs out, when an `EOT` matches its key,
or when its track ends. `EOT` releases the newest of the track's notes that
plays its key and hasn't been released. Releasing sets the channel's stop flag,
and its envelope then falls (see [DirectSound envelopes](#directsound-envelopes)
and [PSG envelopes](#psg-envelopes)).

A note released before the mixer or the PSG routine has started it, such as a
note of one tick at a tempo that plays two ticks in a frame, never sounds.

## Volume and pan

The sequencer works out a track's levels on the right and left from its volume
and pan:

```
level = volume × 64 / 32                      (the 64 is the fade level the game can change)
pan = 2 × pan                                 (from -128 to 126)
right = (pan + 128) × level / 256
left = (127 − pan) × level / 256
```

Each channel then gets:

```
right volume = velocity × (128 + drum pan) × right / 16384    (up to 255)
left volume = velocity × (127 − drum pan) × left / 16384
```

Each step rounds down. A note at full volume and velocity in the centre has a
volume of 126 on the right and 124 on the left, and the pan moves it in a
straight line from one side to the other.

## Pitch

The sequencer works out a track's pitch in 256ths of a semitone:

```
pitch = (tune + bend × bend range) × 4 + key shift × 256
```

with the LFO's swing added when it modulates the pitch. A channel plays its
key plus the pitch's whole semitones, which can't go below 0, raised by the
remaining 256ths.

A sample's rate is its rate for key 60 times 2^(n/12), from a table of 12
semitones and octaves of it. The fraction runs in a straight line in rate to
the next key. Keys above 178 play key 178 raised by 255/256.

A PSG square or wave channel's setting is its 11-bit frequency register *x*:
a square wave plays 131072 / (2048 − *x*) Hz, and a wave pattern of 32 points
an octave lower. The settings come from a table of the periods 2048 − *x* for
C2 to B2, halved for each octave up, with the period rounded up. The fraction
runs in a straight line in the setting to the next key. Keys up to 35 play C2, with no fraction. A noise
channel's setting comes from a table of 60 noise settings for keys 21 to 80, 4
to an octave of the noise clock, and keys below 21 and above 80 play the ends
of the table.

### LFO

The LFO moves on in each tick of a track that has a speed and a depth, once
its delay has run out. Its phase, a byte, goes up by the speed, and its shape is
a triangle from -64 to 64: the phase as a signed byte from 0 to 63 and from 192
to 255, and 128 less the phase from 64 to 191. The driver adds the speed before
it works out the shape, so with a speed above 64 the shape can jump. The swing
is the depth times the shape / 64. On the pitch, the swing is in 16ths of a
semitone. On the volume, it scales the level by (128 + swing) / 128. On the pan,
it's added to twice the pan.

## Envelopes

### DirectSound envelopes

The mixer moves each DirectSound channel's envelope on once a frame, before it
mixes the channel. The level runs from 0 to 255:

* In the frame a note starts, the level is 0, and the attack adds to it.
* The attack adds its value each frame, up to 255. An attack of 0 never gets
  louder than 0.
* The decay multiplies the level by decay / 256 each frame, rounding down,
  until it falls to the sustain level, where it stays. At a sustain level of 0,
  the note ends there.
* After the release, each frame multiplies the level by release / 256,
  rounding down.
* When the release falls to the echo's level, the note carries on at that
  level for the echo's length in frames, if the track has set an echo. Then the
  channel is off.

The rounding makes a long decay or release fall faster than a straight line in
decibels at low levels: by 1 a frame once the level is below 256 / (256 − rate).

### PSG envelopes

The PSG routine moves the PSG channels' envelopes on once a frame, in steps of
1 on the channel's 16 volume levels. A counter counts the frames between steps,
and every 15th frame it counts twice, which keeps it in step with the
hardware's envelope, which moves 64 times a second.

* The full level is the channel's two volumes added up, / 16. A note panned at
  least two to one plays on one side, and its full level is at most 15.
* The attack steps up from 0 to the full level, a step every *attack* counts,
  or starts at the full level if the attack is 0.
* The decay steps down to the sustain level, (full level × sustain + 15) / 16,
  a step every *decay* counts. At a sustain of 0, the note ends there.
* After the release, it steps down to 0, a step every *release* counts.
* The echo works as for DirectSound channels, at (full level × echo level +
  255) / 256.

The hardware runs the same envelope between the routine's writes to the sound
registers. The wave channel has four volumes and silence, and it takes the one
that the envelope's level falls in.

## Mixer

The mixer clears its part of the output buffer for the frame. With the reverb
set, it fills it instead with the average of both sides at the same point a
buffer's length before and a buffer's length less a frame before, times the
reverb / 128, which gives a short echo that fades. The buffer holds 1584
samples a side, which is 7 frames at 13379 Hz. Then the mixer takes each
DirectSound channel up to the number it uses, moves its envelope on, and adds
it to the buffer:

```
level = envelope × (master volume + 1) / 16
right = right volume × level / 256
left = left volume × level / 256
```

For each output sample, the mixer adds the sample's point times right / 256 to
the right buffer, and likewise on the left. It interpolates a sample in a
straight line between the point at its position and the next, and moves the
position on by the channel's step:

```
step = rate × (16777216 / mix rate + 1) / 2    (in 2^-23 points)
```

A fixed-pitch sample plays a point for each output sample, without
interpolation. When a sample runs past its end, a loop goes back by its length
as many times as it takes. A sample without a loop, or whose loop has no
length, turns the channel off, and the rest of the frame is silent.

A later mixer plays a reversed sample backwards from its end, without its loop,
and decodes compressed samples a block at a time.

The init's mode gives the mix rate, as one of 12 settings, from which the
driver takes the samples a frame and works out the rate in Hz:

| Setting | Samples a frame | Rate |
|---|---|---|
| 1 | 96 | 5734 Hz |
| 2 | 132 | 7884 Hz |
| 3 | 176 | 10512 Hz |
| 4 | 224 | 13379 Hz |
| 5 | 264 | 15768 Hz |
| 6 | 304 | 18157 Hz |
| 7 | 352 | 21024 Hz |
| 8 | 448 | 26758 Hz |
| 9 | 528 | 31536 Hz |
| 10 | 608 | 36314 Hz |
| 11 | 672 | 40137 Hz |
| 12 | 704 | 42048 Hz |

Without a mode, the driver plays at 13379 Hz with 8 DirectSound channels and a
master volume of 15.

## Camelot's version

*Golden Sun: The Lost Age* (`AGFE`) has a version of the driver that Camelot
changed, which loveemu calls the Bon sound driver. Its songs, commands, voices
and samples are the SDK's, and so is most of its code, from `0x081C1000` on.
Camelot rewrote `ply_note` (`0x081C07A2`), `MPlayMain` (`0x081C09D0`),
`ChnVolSetAsm` (`0x081C077A`) and `MidiKeyToFreq` (`0x081C05F0`), and put an
unused copy of `m4aSongNumStart` at `0x081C0550` with its literal pool zeroed.
The game reaches the driver through a table of jumps at `0x081C0000`.

### The game's routines

The game's boot code copies 4 KB of code from `0x080006B8` to IWRAM at
`0x03000100`, Camelot's mixer among it, and 56 bytes from `0x080178B4` over
part of it. Its sound init (`0x081C0C1C`) calls `m4aSoundInit`, which copies
the SDK's mixer to `0x03006000` as usual, though the game never runs it, and
sets the mode `0x0099FA00`: 31536 Hz, 10 DirectSound channels and a master
volume of 15. It then sets Timer 0 to twice the mix rate, and points DMA1 and
DMA2 at buffers of 3168 bytes for each side at `0x02003A90` and `0x020046F0`,
which hold three frames of two bytes for each mixed point.

The VBlank handler runs the driver each frame:

1. Camelot's `m4aSoundVSync` (`0x080005E8`) restarts the sound DMA at the
   start of the buffers every third frame.
2. The handler copies the frames left before the DMA restarts to
   `0x03001139`.
3. Camelot's `SoundMain` (`0x08000630`) checks that the stack has room and
   that the `SoundInfo`'s ID is "Smsh", runs the sequencers and the PSG
   routine, and then the mixer's loop over the DirectSound channels, at
   `0x030007B4` in IWRAM, which mixes them into 32-bit sums at `0x03006FC0`,
   one a point with the right side in the low half and the left in the high.
4. The mixer's output stage (`0x03000D40`) turns the sums into the frame's
   part of the buffers, and sets up each sum with the echo for the next frame.

Some of the mixer's ARM code writes instructions into its own loops: the loop
for a sample that plays at the mixer's rate gets the code for the sample's
alignment, and the loop for other samples gets the code for its step, below 1,
from 1 to 2, or from 2 up. The ARM7 runs the new instructions straight away,
having no cache. When a frame's points of a sample fit, the mixer has DMA3
copy them to the stack and mixes them from there.

### Notes and channels

A sample takes the third DirectSound channel that's off, in the list's order.
With only one or two off, it takes the last of them, unless a note of the same
track is released, when it takes the channel of the last such note instead.
With none off, it takes a released note's channel if any note is released,
whatever their priorities, or else a channel whose note has a lower priority
than the new one, or the same priority from this track or a later one. Of
those, it takes the one with the lowest priority, then from the latest track,
then the first in the list. The search goes through 10 channels, the number the
game's mode sets.

The left volume starts from 128 rather than 127:

```
left volume = velocity × (128 − drum pan) × left / 16384
```

`MidiKeyToFreq` keeps each semitone's step to the next in its table, and
shifts the step down by the octaves, where the SDK shifts both semitones'
rates and takes the difference. A fine-tuned low key's rate can then be 1 Hz
lower.

`MPlayMain` counts the LFO's delay down in each tick of a track that has a
depth, and moves the LFO on once it has run out if the track has a speed.
The SDK does neither without a speed.

### Envelopes and levels

The attack and decay work as the SDK's do. After the release, the level falls
by 256 − release each frame, in a straight line, to the echo's level. The mixer
leaves out the master volume:

```
level = envelope + envelope / 8
right = right volume × level / 512
left = left volume × level / 512
```

and each output point is the sum of the channels' points times their levels,
/ 128, so a sample channel plays at 9/8 of the SDK mixer's level for the same
volumes. A channel whose right and left come to 0 isn't mixed, and doesn't
move on through its sample.

### Synth voices

A sample of no length, with nothing left to play when the mixer comes to mix
it, plays a wave that the mixer makes, unless its voice plays at the mixer's
rate. The mixer sets bit 6 of the channel's type, and keeps the wave's phase in
the channel's fraction, which moves on by 8 times a sample's step for each
output point, so a cycle lasts 64 of a sample's points. The sample's data
starts with `0x80` and the wave's type:

| Type | Wave | Level |
|---|---|---|
| 0 | a pulse wave, high while the phase is below the duty | ±64 |
| 1 | a saw wave, through a filter | about ±112 |
| other | a triangle wave | ±128 |

A pulse wave's next four bytes are its duty *d*, speed *s*, depth *p* and
offset *o*. Each frame, the channel's count, whose top byte is the duty's
phase, goes up by *s* × 2^24, and the duty becomes

```
t = count + o × 2^24, with its bits inverted if bit 31 is set
duty = (t / 256) × p + d × 2^24
```

so it follows a triangle wave from *d*/256 of a cycle to (*d* + *p*/2)/256,
which repeats every 256 / gcd(256, *s*) frames.

A saw wave rises from −112 to 111 over a cycle, by three quarters of a step for
each of the phase's 256 steps, with a jump of 32 halfway:

```
x = phase / 2^24 − 112 − (phase / 2^26 mod 32)
y = x + y / 2
```

The channel's count keeps the filter's *y*, and the mixer plays it at half
the level. A triangle wave is (phase / 2^23) − 128 for the first half of the
cycle and 384 − (phase / 2^23) for the second.

`agbsf2` makes a sample of each pulse and saw voice for each key that a song
plays it at. The sample is at the mixer's rate and lasts one cycle of the duty,
or about 2048 points of the saw after its filter settles. The sample loops at
the nearest whole cycle of the wave. A triangle wave gets one cycle of 64
points. The cycle plays at the rate a sample's header gives.

### Echo

The output stage adds an echo of its earlier output to each frame's sums. The
right side gets back a quarter of its output from a frame before (528 points),
53/128 of it from four frames before (2112 points), and −8/128 of the left
side's from four frames before. The left side gets a quarter of the right
side's output from 704 points before, 52/128 of its own from four frames
before, and −8/128 of the right side's. The game doesn't change the echo, and
the songs' reverb settings don't reach it. `agbsf2` therefore gives every song a
reverb send of 53. Of the driver's reverb settings, 53 has the closest feedback.

## Locating the driver

`agbsf2` finds the driver in a ROM from its code, without fixed
addresses. `m4aSongNumStart` is the same in every version, apart from the
registers some games' compiler chose:

```
push {lr}                    ; B500
lsls r0, r0, #16             ; 0400
ldr  r2, =music player table ; 4Axx
ldr  r1, =song table         ; 49xx
lsrs r0, r0, #13             ; 0B40
adds r0, r0, r1              ; 1840
ldrh r3, [r0, #4]            ; 8883
lsls r1, r3, #1              ; 0059
adds r1, r1, r3              ; 18C9
lsls r1, r1, #2              ; 0089
adds r1, r1, r2              ; 1889
ldr  r2, [r1]                ; 680A
ldr  r1, [r0]                ; 6801
adds r0, r2, #0              ; 1C10
bl   MPlayStart
pop  {r0}                    ; BC01
```

The other compiler swaps `r2` and `r3`: `ldr r3, =music player table` (4Bxx),
`ldrh r2, [r0, #4]` (8882), `lsls r1, r2, #1` (0051), `adds r1, r1, r2` (1889)
and `adds r1, r1, r3` (18C9), with the rest the same. Its literal pool gives
the music player table and the song table. A copy of the routine whose pool
doesn't give two addresses in the ROM, such as `AGFE`'s unused one, is passed
over. The init's literal pool also holds
the music player table's address, and a little before it the init sets the
driver's mode:

```
ldr  r0, =mode               ; 48xx
bl   m4aSoundMode
```

`agbsf2` takes a mode with a mix rate setting from 1 to 12, an output
resolution, and nothing in its top byte:

| Bits | Contents |
|---|---|
| 0-6 | reverb, set if bits 0-7 aren't all 0 |
| 8-11 | DirectSound channels, up to 12 |
| 12-15 | master volume |
| 16-19 | mix rate setting |
| 20-23 | the output's resolution: 8 for 9 bits, 9 for 8 bits, 10 for 7 bits, 11 for 6 bits |

Each field that isn't 0 replaces the driver's default. A later mixer has a test
for reversed and compressed samples in its ARM code. `agbsf2` looks for this
code:

```
ldrb r0, [r4, #1]            ; E5D40001
tst  r0, #0x30               ; E3100030
```

The mono mixer works out each channel's volume in Thumb code. `agbsf2` looks
for this code too. When it finds the code, `--info` reports the mixer as mono:

```
ldrb r0, [r4, #2]            ; 78A0: the right volume
ldrb r1, [r4, #3]            ; 78E1: the left volume
adds r0, r0, r1              ; 1840
muls r0, r5, r0              ; 4368: times the envelope's level
lsrs r0, r0, #9              ; 0A40
strb r0, [r4, #10]           ; 72A0
```

Camelot's note start looks through the DirectSound channels with Thumb code.
`agbsf2` looks for that code to find Camelot's changes to the sequencer.
Camelot's mixer moves a pulse wave's duty on with ARM code. `agbsf2` finds the
mixer by that code:

```
movs r3, #10                 ; 230A: the channels to look through
adds r4, #0x50               ; 3450: the first channel's record
movs r7, #0xc7               ; 27C7
movs r2, #0                  ; 2200
movs r6, #0                  ; 2600
movs r0, #3                  ; 2003: take the third free channel
ldrb r1, [r4]                ; 7821
tst  r1, r7                  ; 4239

ldrb  r6, [r3, #2]           ; E5D36002: the duty's speed
add   r2, r2, r6, lsl #24    ; E0822C06
ldrb  r6, [r3, #4]           ; E5D36004: its offset
adds  r6, r2, r6, lsl #24    ; E0926C06
mvnmi r6, r6                 ; 41E06006
```

With `--driver mp2k`, `--song-table` gives the song table of a game whose
`m4aSongNumStart` isn't found. Its songs then play on players of 16 tracks, at
the driver's default settings, with reversed and compressed samples if the ROM
has the later mixer's test for them. `--song-count` sets the number of songs
when the count from the table is wrong.
