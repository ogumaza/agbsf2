# Nintendo R&D1's MIDI-file sound driver

*WarioWare, Inc.: Mega Microgame$!* and *Rhythm Tengoku* play their music and
sound effects with a driver that reads Standard MIDI Files. Each song is a MIDI
file of format 0 or 1. The tracks play on the channels of a player, and the
programs select instruments from a bank. A software mixer plays samples at
13379 Hz in stereo, and the four PSG channels of the Game Boy play the rest.
This document describes the driver as `agbsf2` models it. The example
addresses come from the US release of *WarioWare, Inc.* (game code `AZWE`).
*Rhythm Tengoku* (`BRIJ`) has a later revision. [The Rhythm Tengoku
revision](#the-rhythm-tengoku-revision) lists the differences.

## Routines

The driver is Thumb code in ROM.

| Address | Routine |
|---|---|
| `0x080F4090` | the init: sets up the mixer at 13379 Hz with 8 sample voices, and 9 players from the table at `0x08406378` |
| `0x080F3C60` | sets the four settings of the echo. The init of the game passes (0x23, 2, 2, 4) at `0x080002E0` |
| `0x080F2E48` | plays entry `r0` of the song table |
| `0x080F2C88` | starts the record of a song on its player |
| `0x080F3A90` | the per-frame routine: the main loop of the game calls it at `0x08000420` |
| `0x080F38C4` | runs the tracks of a player for a frame |
| `0x080F35E0` | runs the next event of a track |
| `0x080F15D8` | runs the LFO of a channel and computes the level |
| `0x080F1FF0` | note on |
| `0x080F1B74` | the envelope of a voice |
| `0x080F1238` | the mixer |

## Songs

The song table at `0x08402990` has 1847 entries of 8 bytes: the record of the
song and the player that plays it. 505 of them are empty. A record has 20
bytes:

| Offset | Contents |
|---|---|
| `+0` | the MIDI file |
| `+4`, bits 5-14 | the bank, an index into the bank table at `0x083F3BD4` |
| `+4`, bits 15-21 | the volume of the song, 0-127 |
| `+6`, bits 6-15 | the priority of the song |
| `+0xC` | the name of the song, such as `m_BGM_01` |
| `+0x10` | the number of the song |

The start routine reads the header of the file and up to 31 tracks. A player
has a channel for each of its tracks. The driver sends the events of each
track to the channel with the number of the track, and ignores the channel in
the status byte. It queues a note with the low 4 bits of the track number. The
conversion writes a track for each of those channels that plays notes.

## Timing

The driver counts the time of a track in 256ths of a tick. A tempo event sets
the beats a minute to 60000000 over the microseconds a quarter note in the
event, truncated. The step for each frame is then the beats a minute times the
speed of the player (0x100 is the tempo of the song) times the division of the
file, over 3600. Until the first tempo event, the step is 1. A new tempo
applies from the next frame. Each frame, a track runs its events while the
wait is below the step. It then takes the step from the wait. Every event
therefore happens at the start of a frame.

A marker event with the text `[` starts the loop, and `]` ends it. At the first
`[`, the driver saves the place of each track from the start of that frame. At
`]`, it puts every track back there, stops the voices of every channel and
runs the tracks again in the same frame. A song without the markers plays
once. A track that ends stops the voices of its channel.

The conversion ends a song without the markers with the last track. A track
can wait long after the last note, up to 241 s in *Rhythm Tengoku*. When that
end is more than 10 s after the last sound, the file ends with the frame after
the last sound. A note sounds until the envelope in the driver ends the voice,
and until the release of the zone ends after the end of the MIDI note. A voice
with a release that doesn't end within 10 s keeps the end of the song.

## Channels

A channel keeps its settings in 32 bytes. The driver handles these events:

| Event | What it does |
|---|---|
| note on and note off | queue up to 20 notes a frame for each track. A note on with velocity 0 is a note off |
| program change | the program of the next notes on the channel |
| pitch bend | 14 bits |
| CC1 | the depth of the LFO |
| CC7 | volume |
| CC10 | pan |
| CC11 | expression |
| CC14, CC16 | write a value to one of the variables of the game |
| CC20 | the bend range in semitones |
| CC21 | the speed of the LFO |
| CC22 | the target of the LFO: 0 pitch, 1 volume, 2 pan |
| CC26 | the delay of the LFO in frames |
| CC33 | the priority of the channel, added to the priority of the song |
| CC72 | marks the next voices of the channel for the filter of the echo (`0x080F2326`). See [The echo](#the-echo) |
| CC75 | a flag of the channel that turns the sign of the right side over |
| CC73, CC74, CC76, CC77 | the LFO of the echo, shared by the whole mix |
| CC78-CC81 | offsets to the four settings of the echo while the song plays |
| CC82 | a random pitch for each note |
| CC83 | a random key offset for each note |
| CC84 | a counter for the random key |
| system exclusive type 0 | sets the LFO of the echo |
| system exclusive type 1 | a shift in semitones for each of the 12 keys of an octave |

Each frame, the level of the channel is the volume of the song times CC7 times
CC11 times the volume of the LFO (0x80 is none), shifted right by 21 bits. The
driver keeps 7 bits of it. The pan is CC10 of the channel plus half the pan of
the bus, plus half the swing of the LFO when the LFO moves the pan. The nearer
side plays at 127, and the other side at 254 minus twice the distance of the
pan from the middle.

The bend moves the pitch along a curve between the pitch of the key and the
pitch CC20 semitones away. The 12 steps of the curve are 2^(i/12) - 1. A half
bend with a range of 2 is 0.855 of a semitone. The LFO adds the swing times a
share of the width of the modulation depth of the channel, in frequency.

## Instruments

A bank contains 128 instruments. Records differ by the first byte:

| Byte | Kind | Size |
|---|---|---|
| `0x41` and others | a sample | 32 bytes |
| `0x46` | a sample that plays at the mixer rate on every key | 32 bytes |
| `0x50`, `0x51` | a PSG channel | 36 bytes |
| `0x52` | a drum kit: an instrument for each key from the base key of the record on | 8 bytes |
| `0x53` | a key split: a table of keys that selects one of the instruments of the record | 12 bytes |

The record of a sample contains the base key, a pan offset (0x7F is none), the
header of the sample, and the envelope: a start level, a sustain level, and
the attack, decay, fall and release rates. A PSG record also contains the
channel, the length, the sweep of square 1 and the duty or the width of the
noise. The header of a sample contains the length, the rate, the root key, the
start and end of the loop, and the points. A loop end of 0 means no loop.

The level of the envelope runs from 0 to 0x7F0000. Each frame it rises by the
attack rate to the full level, falls by the decay rate to the sustain level,
then falls by the fall rate. After the end of the note it falls by the release
rate. A note that ends before the decay reaches the sustain level releases
from above that level, and the release takes longer. A level of 0 ends the
voice. The volume of a voice is the level of the channel times the velocity
times the level of the envelope, shifted right by 14 bits.

The release in the driver falls in a straight line in level. A SoundFont
release falls 96 dB in a straight line in decibels. A sample zone releases
over 7 times the frames of the driver, whatever their number. The other
drivers in agbsf2 stretch
a fade 3 times up to 5 frames, rising to 6 times from 16 frames on. Most sample
notes here release in under 16 frames. Against recordings from mGBA 0.11.0, the
stretch of 7 came closer. A PSG zone keeps the stretch of the other drivers.

Pitches come from a table of the frequency of each key in whole hertz: 8 Hz
for key 0, 440 Hz for key 69 and 12544 Hz for key 127. A sample plays at its
rate times the pitch of the key over the pitch of the root key. A `0x46`
sample plays at the mixer rate, whatever rate the header sets. The noise takes
its setting from a table of keys 21 to 80. The note-on of the noise keeps the
key in place of the pitch.

## The echo

The mixer feeds its output back. Each pass starts from the output that the
mixer wrote 1568 samples, or 117 ms, before on the left side, and 1560 samples
before on the right (`0x080F0224`). A high-pass filter and a low-pass filter
shape it, with corners near 140 Hz and 610 Hz. The first setting of the echo
over 128 sets the level, after the shift of the low-pass filter drops the low
bits. The settings of the game (0x23, 2, 2, 4) set 32/128, or -12 dB. Each
pass of the echo comes back that much quieter. The four settings are the
level, the offset of the right side in steps of 4 samples, and the shifts of
the low-pass and high-pass filters. CC78 adds twice its distance from 64 to
the level while the song plays (`0x080F3AEC`).

The feedback takes the whole output. Every sample voice therefore has the
echo. The PSG plays outside the mixer and has none. CC72 only marks the voices
of a channel. The mixer mixes the marked voices before the others, and runs
them and the echo through a one-pole low-pass filter (`0x080F13F4`). CC77 and
system exclusive type 0 set the filter. The default, 0, leaves the sound as it
is.

## The PSG

Each frame, the routine at `0x080F2910` sets each PSG channel from its voice.
The PSG volume is the smaller of 15 and (v >> 3) + (v >> 4), for the volume v
of the voice (`0x080F28F8`). It reaches 15 at v = 80. The wave plays at 25%,
50%, 75% or 100%, one level for each 4 steps of the PSG volume (the table at
`0x083EBFC8`). A change of volume restarts a square or the noise. The volume
of the wave changes without a restart.

The routine writes the volume from the level of the envelope before the step
of the frame, then steps the envelope (`0x080F2B96`). A voice that the step
ends goes silent in the same frame.

Both games set SOUNDBIAS to 0x200. The GBA then samples the output of the PSG
32768 times a second. The samples of the noise have that rate.

The routine at `0x080F2BE4` then sets SOUNDCNT_L. It enables the left side of
a PSG channel when the channel of the voice has a CC10 of 64 or less, and the
right side for 64 or more. The pan of the bus, the LFO and the pan offset of
the instrument don't reach the PSG.

On a new note, square 1 also gets NR10 from the sweep in the record, or 8 for
none (`0x080F2A52`). While a note with a sweep time sounds, the routine writes
nothing more to square 1 (`0x080F2AB0`). The note keeps the volume of its
restart, and the hardware sweep moves the frequency. A rising sweep can pass
the top and turn the channel off. Otherwise the channel sounds until the voice
ends.

A PSG zone plays the straight line 3v/16 at its full level, up to v = 125.
The velocity of each PSG note scales the zone to the energy of the volumes
that the driver writes over the note. The energy of the zone follows its
envelope as a player plays it: the release falls 96 dB in its time, in a
straight line in decibels. In the frame of the end of a note, the driver still
writes the volume from before the release. A release that doesn't end the
voice at once is therefore heard from the next frame on. The MIDI note then
ends a frame later, and the energy includes the release. A later note that
restarts the PSG channel, or plays the same key on the channel, ends the note
sooner. The PSG notes of a channel play at CC10 0, 64 or 127, with the level
of the channel on each side that the driver enables. Where a channel plays
both kinds of note, CC10 and CC11 follow the kind of the latest note.

A note on square 1 with a sweep plays from a zone with a flat envelope, at the
volume of its restart. It ends where the voice ends, where a later note
restarts square 1, or where the sweep turns the channel off. The PSG model in
agblib computes the steps of the sweep, and the pitch wheel follows them. The
bends of the channel don't reach the note after its start.

## The Rhythm Tengoku revision

*Rhythm Tengoku* has the same formats and events. The driver differs in these
ways:

- The init sets up 12 sample voices and 13 players.
- The bank table has empty entries.
- A player has a level for a group of tracks and a mask of the tracks in the
  group. A channel with its bit set plays at the level of the group, and the
  others at the volume of the song. The level of the channel keeps 8 bits. A
  level above 127 therefore doesn't wrap.
- The routines are elsewhere: the play routine at `0x0804B534`, the start
  routine at `0x0804B368`, the per-frame routine at `0x0804C170`, the init at
  `0x0804C778` and the echo routine at `0x0804C340`.
- A setting of the game runs every voice through the echo's filter, through
  the byte at `0x03005B44` (`0x08049BFC`). The same setting mixes both FIFOs
  on both sides. The game reads the setting from the save (`0x08000292`) and
  changes it from a menu (`0x080167C2`). A new save leaves it off. The
  conversion follows that default. The echo itself is the same as in
  *WarioWare, Inc.*

The scripts of the game set the level and mask of the group through the
routine at `0x0800C0F8`, from script commands 0xAC and 0xAD. The game also
changes the speed, the pitch and the volume of the music in play. The
conversion plays every track at the volume of the song, with the speed and
pitch as written. See [What the conversion leaves
out](#what-the-conversion-leaves-out).

## Locating the driver

`agbsf2` finds the driver by its code. The play routine points to the song
table and the player table, and the call in the play routine leads to the
start routine. The lookup in the start routine points to the bank table. The
literals of the init set the mixer rate, the setup of the players and the
sample voices. The LFO routine points to the sine table. The pitch table is
found by its values. The call of the echo routine just after the init sets the
settings of the echo. The MP2K song start of *Metroid Fusion* has the same code
as the play routine. Detection therefore also requires the per-frame routine
and the echo routine.

## What the conversion leaves out

- The echo. Every sample note sends the first setting of the echo to CC91,
  and PSG notes send 0. The reverb of a SoundFont player only stands in for
  the echo.
- The LFO of the echo, the filter and the offsets of CC78-CC81.
- The length of the release of each note. A sample zone has one release time.
  The conversion takes it from the mean level at which the notes of the song
  on the zone end. With `--single-sf2`, the mean covers every song.
- The variables of the game that CC14 and CC16 write.
- The random pitch and key of CC82 and CC83. Notes play at their written pitch.
- CC75. It inverts the right side of a voice (`0x080F22B4`). In the game the
  voice sounds hollow on headphones and cancels in mono.
- What the code of the game changes in the music: the speed, the pitch and
  the volume, and the group levels of *Rhythm Tengoku*. The conversion is the
  song as the file plays it at the first state of the player.
