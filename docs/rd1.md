# Nintendo R&D1's MIDI-file sound driver

*WarioWare, Inc.: Mega Microgame$!* and *Rhythm Tengoku* play their music and
sound effects with a driver that reads Standard MIDI Files. Each song is a MIDI
file of format 0 or 1. Its tracks play on the channels of a player, and its
programs choose instruments from a bank. A software mixer plays samples at
13379 Hz in stereo, and the Game Boy's four PSG channels play the rest. This
document describes the driver as `agbsf2` models it. The example addresses come
from the US release of *WarioWare, Inc.* (game code `AZWE`). *Rhythm Tengoku*
(`BRIJ`) has a later revision, whose differences
[The Rhythm Tengoku revision](#the-rhythm-tengoku-revision) lists.

## Routines

The driver is Thumb code in ROM.

| Address | Routine |
|---|---|
| `0x080F4090` | the init: sets up the mixer at 13379 Hz with 8 sample voices, and 9 players from the table at `0x08406378` |
| `0x080F3C60` | sets the echo's four settings. The game's init passes (0x23, 2, 2, 4) at `0x080002E0` |
| `0x080F2E48` | plays entry `r0` of the song table |
| `0x080F2C88` | starts a song's record on its player |
| `0x080F3A90` | the per-frame routine, which the game's main loop calls at `0x08000420` |
| `0x080F38C4` | runs a player's tracks for a frame |
| `0x080F35E0` | runs a track's next event |
| `0x080F15D8` | runs a channel's LFO and works out its level |
| `0x080F1FF0` | note on |
| `0x080F1B74` | a voice's envelope |
| `0x080F1238` | the mixer |

## Songs

The song table at `0x08402990` has 1847 entries of 8 bytes: the song's record
and the player that plays it. 505 of them are empty. A record has 20 bytes:

| Offset | Contents |
|---|---|
| `+0` | the MIDI file |
| `+4`, bits 5-14 | the bank, an index into the bank table at `0x083F3BD4` |
| `+4`, bits 15-21 | the song's volume, 0-127 |
| `+6`, bits 6-15 | the song's priority |
| `+0xC` | the song's name, such as `m_BGM_01` |
| `+0x10` | the song's number |

The start routine reads the file's header and up to 31 tracks. A player has a
channel for each of its tracks. The driver gives each track's events to the
channel of the track's number and ignores the channel in the status byte. It
queues a note with the low 4 bits of its track's number. The conversion writes a
track for each of those channels that plays notes.

## Timing

The driver counts a track's time in 256ths of a tick. A tempo event sets the
beats a minute to 60000000 over its microseconds a quarter note, truncated. The
step for each frame is then the beats a minute times the player's speed
(0x100 is the song's tempo) times the file's division, over 3600. Until the
first tempo event, the step is 1. A new tempo applies from the next frame. Each
frame, a track runs its events while its wait is below the step. It then takes
the step from the wait. Every event therefore happens at the start of a frame.

A marker event with the text `[` starts the loop, and `]` ends it. At the first
`[`, the driver saves each track's place from the start of that frame. At `]`,
it puts every track back there, stops every channel's voices and runs the
tracks again in the same frame. A song without the markers plays once. A track
that ends stops its channel's voices.

The conversion ends a song without the markers with its last track. A track
can wait long after its last note, up to 241 s in *Rhythm Tengoku*. When that
end lies more than 10 s after the last sound, the file ends with the frame
after it. A note sounds until the driver's envelope ends its voice, and until
its zone's release ends after the MIDI note's end. A voice whose release doesn't
end within 10 s keeps the song's end.

## Channels

A channel holds its settings in 32 bytes. The driver handles these events:

| Event | What it does |
|---|---|
| note on and note off | queue up to 20 notes a frame for each track. A note on with velocity 0 is a note off |
| program change | the program of the channel's next notes |
| pitch bend | 14 bits |
| CC1 | the LFO's depth |
| CC7 | volume |
| CC10 | pan |
| CC11 | expression |
| CC14, CC16 | write a value to one of the game's variables |
| CC20 | the bend range in semitones |
| CC21 | the LFO's speed |
| CC22 | the LFO's target: 0 pitch, 1 volume, 2 pan |
| CC26 | the LFO's delay in frames |
| CC33 | the channel's priority, added to the song's |
| CC72 | marks the channel's next voices for the echo's filter (`0x080F2326`). See [The echo](#the-echo) |
| CC75 | a flag of the channel that turns the right side's sign over |
| CC73, CC74, CC76, CC77 | the echo's LFO, which the whole mix shares |
| CC78-CC81 | offsets to the echo's four settings while the song plays |
| CC82 | a random pitch for each note |
| CC83 | a random key offset for each note |
| CC84 | a counter for the random key |
| system exclusive type 0 | sets the echo's LFO |
| system exclusive type 1 | a shift in semitones for each of the 12 keys of an octave |

Each frame, the channel's level is the song's volume times CC7 times CC11
times the LFO's volume (0x80 is none), shifted right by 21 bits. The driver
keeps 7 bits of it. The pan is the channel's CC10 plus half the bus's pan, and
half the LFO's swing when the LFO moves the pan. The nearer side plays at 127,
and the other side at 254 minus twice the pan's distance from the middle.

The bend moves the pitch along a curve between the key's pitch and the pitch
CC20 semitones away. The curve's 12 steps are 2^(i/12) - 1. A half bend with a
range of 2 is 0.855 of a semitone. The LFO adds the swing times a share of the
width of the channel's modulation depth, in frequency.

## Instruments

A bank holds 128 instruments. Records differ by their first byte:

| Byte | Kind | Size |
|---|---|---|
| `0x41` and others | a sample | 32 bytes |
| `0x46` | a sample that plays at the mixer's rate on every key | 32 bytes |
| `0x50`, `0x51` | a PSG channel | 36 bytes |
| `0x52` | a drum kit: an instrument for each key from the record's base key on | 8 bytes |
| `0x53` | a key split: a table of keys that picks one of the record's instruments | 12 bytes |

A sample's record holds its base key, a pan offset (0x7F is none), the sample's
header, and its envelope: a start level, a sustain level, and the attack,
decay, fall and release rates. A PSG record also holds the channel, the length,
square 1's sweep and the duty or the noise's width. A sample's header holds its
length, its rate, its root key, its loop's start and end, and its points. A
loop end of 0 means no loop.

The envelope's level runs from 0 to 0x7F0000. Each frame it rises by the attack
rate to the full level, falls by the decay rate to the sustain level, then
falls by the fall rate. After the note's end it falls by the release rate. A
note that ends before its decay reaches the sustain level releases from above
it, and its release takes longer. A level of 0 ends the voice. A voice's volume
is the channel's level times the velocity times the envelope's level, shifted
right by 14 bits.

The driver's release falls in a straight line in level. A SoundFont release
falls 96 dB in a straight line in decibels. A sample zone releases over 7 times
the driver's frames, whatever their number. The other drivers in agbsf2 stretch
a fade 3 times up to 5 frames, rising to 6 times from 16 frames on. Most sample
notes here release in under 16 frames. Against recordings from mGBA 0.11.0, the
stretch of 7 came closer. A PSG zone keeps the stretch of the other drivers.

Pitches come from a table of each key's frequency in whole hertz: 8 Hz for key
0, 440 Hz for key 69 and 12544 Hz for key 127. A sample plays at its rate times
the key's pitch over its root key's pitch. A `0x46` sample plays at the mixer's
rate, whatever rate its header gives. The noise takes its setting from a table
of keys 21 to 80. Its note-on keeps the key in place of the pitch.

## The echo

The mixer feeds its output back. Each pass starts from the output that it wrote
1568 samples, or 117 ms, before on the left side, and 1560 samples before on
the right (`0x080F0224`). A high-pass filter and a low-pass filter shape it,
with corners near 140 Hz and 610 Hz. The echo's first setting over 128 gives
its level, after the low-pass filter's shift drops its low bits. The game's
settings (0x23, 2, 2, 4) give 32/128, or -12 dB, and each pass of the echo
comes back that much quieter. The four settings are the level, the right side's
offset in steps of 4 samples, and the shifts of the low-pass and high-pass
filters. CC78 adds twice its distance from 64 to the level while the song plays
(`0x080F3AEC`).

The feedback takes the whole output, so every sample voice has the echo. The
PSG plays outside the mixer and has none. CC72 only marks a channel's voices.
The mixer mixes the marked voices before the others, and it runs them and the
echo through a one-pole low-pass filter (`0x080F13F4`). CC77 and system
exclusive type 0 set the filter. Its default, 0, leaves the sound as it is.

## The PSG

Each frame, the routine at `0x080F2910` sets each PSG channel from its voice.
The PSG's volume is the smaller of 15 and (v >> 3) + (v >> 4), for the voice's
volume v (`0x080F28F8`). It reaches 15 at v = 80. The wave plays at 25%, 50%,
75% or 100%, one level for each 4 steps of the PSG's volume (the table at
`0x083EBFC8`). A change of volume restarts a square or the noise. The wave's
volume changes without a restart.

The routine writes the volume from the envelope's level before the frame's
step, then steps the envelope (`0x080F2B96`). A voice that the step ends goes
silent in the same frame.

Both games set SOUNDBIAS to 0x200. The GBA then samples the PSG's output 32768
times a second. The noise's samples have that rate.

The routine at `0x080F2BE4` then sets SOUNDCNT_L. It enables a PSG channel's
left side when the voice's channel has a CC10 of 64 or less, and its right
side for 64 or more. The bus's pan, the LFO and the instrument's pan offset
don't reach the PSG.

On a new note, square 1 also gets NR10 from the record's sweep, or 8 for none
(`0x080F2A52`). While a note with a sweep time sounds, the routine writes
nothing more to square 1 (`0x080F2AB0`). The note keeps the volume of its
restart, and the hardware's sweep moves its frequency. A rising sweep can pass
the top and turn the channel off. Otherwise the channel sounds until the voice
ends.

A PSG zone plays the straight line 3v/16 at its full level, up to v = 125.
Each PSG note's velocity scales the zone to the energy of the driver's volumes
over the note. The zone's energy follows its envelope as a player plays it:
the release falls 96 dB in its time, in a straight line in decibels. In the
frame of a note's end, the driver still writes the volume from before the
release. A release that doesn't end the voice at once is therefore heard from
the next frame on. The MIDI note then ends a frame later, and the energy
includes the release. A later note that restarts the PSG channel, or plays the
same key on the channel, ends it sooner. A channel's PSG notes play at CC10 0,
64 or 127, with the channel's level on each side that the driver enables.
Where a channel plays both kinds of note, its CC10 and CC11 follow the kind of
its latest note.

A note on square 1 with a sweep plays from a zone with a flat envelope, at the
volume of its restart. It ends where the voice ends, where a later note
restarts square 1, or where the sweep turns the channel off. ntdlib's model of
the PSG works out the sweep's steps, and the pitch wheel follows them. The
channel's bends don't reach the note after its start.

## The Rhythm Tengoku revision

*Rhythm Tengoku* has the same formats and events. Its driver differs in these
ways:

- The init sets up 12 sample voices and 13 players.
- The bank table has empty entries.
- A player has a level for a group of tracks and a mask of the tracks in the
  group. A channel whose bit is set plays at the group's level, and the others
  at the song's volume. The channel's level keeps 8 bits, so a level above 127
  doesn't wrap.
- Its routines are elsewhere: the play routine at `0x0804B534`, the start
  routine at `0x0804B368`, the per-frame routine at `0x0804C170`, the init at
  `0x0804C778` and the echo routine at `0x0804C340`.
- A setting of the game runs every voice through the echo's filter, through
  the byte at `0x03005B44` (`0x08049BFC`). The same setting mixes both FIFOs
  on both sides. The game reads it from its save (`0x08000292`) and changes it
  from a menu (`0x080167C2`). A new save leaves it off. The conversion follows
  that default. The echo itself is the same as in *WarioWare, Inc.*

The game's scripts set the group's level and mask through the routine at
`0x0800C0F8`, from script commands 0xAC and 0xAD. The game also changes the
speed, the pitch and the volume of the music in play. The conversion plays
every track at the song's volume, its speed and pitch as written. See
[What the conversion leaves out](#what-the-conversion-leaves-out).

## Locating the driver

`agbsf2` finds the driver by its code. The play routine gives the song table
and the player table, and its call gives the start routine. The start routine's
lookup gives the bank table. The init's literals give the mixer's rate, the
players' setup and the sample voices. The LFO routine gives the sine table. The
pitch table is found by its values. The game's call of the echo routine just
after the init gives the echo's settings. *Metroid Fusion*'s MP2K song start
has the same code as the play routine, so detection also needs the per-frame
routine and the echo routine.

## What the conversion leaves out

- The echo. Every sample note sends the echo's first setting to CC91, and
  PSG notes send 0. A SoundFont player's reverb only stands in for the echo.
- The echo's LFO, its filter and the offsets of CC78-CC81.
- The length of each note's release. A sample zone has one release time, which
  the conversion takes from the mean level at which the song's notes on the
  zone end. With `--single-sf2`, the mean covers every song.
- The game's variables that CC14 and CC16 write.
- The random pitch and key of CC82 and CC83. Notes play at their written pitch.
- CC75, which inverts a voice's right side (`0x080F22B4`). In the game the
  voice sounds hollow on headphones and cancels in mono.
- What the game's code changes in the music: the speed, the pitch and the
  volume, and *Rhythm Tengoku*'s group levels. The conversion is the song as
  its file plays it at the player's first state.
