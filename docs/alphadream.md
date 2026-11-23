# AlphaDream's sound driver

*Mario & Luigi: Superstar Saga* plays its music and sound effects with a driver
by AlphaDream, the developer of the game. The songs are sequences of byte
commands on up to 12 tracks. Tracks 0 to 7 play samples through a software
mixer at 15768 Hz, and tracks 8 to 11 play the four PSG channels of the Game
Boy. This document describes the driver as `agbsf2` models it. The addresses
come from the US release (game code `A88E`). The game also contains two copies
of *Mario Bros. Classic*. They play their music with MP2K.

## Routines

The driver is Thumb code in ROM. The inner loops of the mixer are ARM code
that the driver decompresses into IWRAM.

| Address | Routine |
|---|---|
| `0x0819AFA8` | the init: `r0` is the state of the driver, kept at `0x02000480`, with the address stored at `0x03007FF0` |
| `0x0819A43C` | the setup of the mixer: decompresses the mixer (LZ77) from `0x0819A4AC` to `0x03001080`, and sets up timer 0 and the DMA to both FIFOs |
| `0x0819B418` | queues a 16-bit command in an 8-entry ring at `+0x480` of the state |
| `0x0819B95C` | the per-frame routine: the commands (`0x0819B31C`), the players (`0x0819B864`) and the output of the PSG (`0x0819AF88`) |
| `0x0819B110` | starts song `r1` on player `r0` |
| `0x0819B450` | runs a track for a frame |
| `0x0819A600` | mixes a frame: the envelopes and points of the sample channels (`0x0819A2F0`) |
| `0x0819A204` | the step of the mixer from the rate of a sample and a pitch |

The top 4 bits of a command are the kind, the next 4 the player, and the low
byte the setting. Kind 0 starts a song on the player, 1 stops the player, 2
resumes it, 8 sets the tempo divisor and 9 the volume. Kind 5 starts a sound
effect on player 0 from the table at `0x0821CC40`, 6 stops one and 7 stops
them all. The low 12 bits of kinds 5 and 6 select the sound effect. The start
(`0x0819B110`) stops the player and sets the divisor to 0x4B. It leaves the
volume of the player as it was. A stop (`0x0819B0AC`) ends each sounding note,
marks the channel of each track as taken by a lower player and clears the flag
of the player. A resume (`0x0819B19C`) sets the flag again. The tracks then
restore their channels, and the song goes on from where it stopped. The game
fades its music with command 9.

## Players and tracks

The driver has 3 players of 12 tracks. The state of a player is 12 records of
32 bytes from the start of the state, `0x180` bytes a player. Each frame, the
players run in order. A track that a lower player plays keeps running, but the
lower player has the channel. The track of the higher player runs with
handlers that do nothing. When the channel comes free, the track restores its
pitch and levels and starts its note again. The restore of a PSG channel
(`0x0819ABF8`) uses the pitch routine of the squares, for the noise too. The
game starts its music on player 1.

The record of a track:

| Offset | Contents |
|---|---|
| `+0` | flags: 0x01 in use, 0x02 a lower player has the channel, 0x20 a rest follows each note, 0x40 the note goes on into the next, 0x80 a note sounds, 0x100 to 0x400 the pitch LFO, 0x800 to 0x2000 the level LFO |
| `+2` | the tempo |
| `+3` | the running count of the tempo |
| `+4` | the data of the track |
| `+8` | the offset of the next byte |
| `+0xA` | ticks to the next event |
| `+0xB` | the key |
| `+0xC` | the instrument |
| `+0xD` | the volume, 0-255 |
| `+0xE` | the pan |
| `+0xF` | the bend range |
| `+0x10`, `+0x11` | the bend and the fine tune, signed |
| `+0x12`, `+0x13` | the channel and the player |
| `+0x14`-`+0x17` | the level LFO: depth, period, delay and count |
| `+0x18`-`+0x1B` | the pitch LFO: the same |
| `+0x1C`-`+0x1F` | four loop counters |

A song starts with a 16-bit mask of the tracks that it plays, then a 16-bit
offset from the start of the song for each of them. The song table at
`0x0821CB70` contains a pointer for each of 51 songs and ends with 0. A track
starts with tempo 0x78, volume 0xC8, pan 0x7F, bend range 2 and a wait of 1
tick. The flags start at 0x83. The first frame therefore restores the channel
and starts a note with the key in the record and instrument 0.

## Timing

Each frame, a track adds the tempo to the count. It runs a tick each time the
count passes the divisor of the player, and takes the divisor off. The divisor
is 0x4B unless the game sets another. A tempo of 75 therefore runs a tick each
frame, and the tempo is close to the beats a minute at 48 ticks a quarter
note. Every event happens at the start of a frame.

## Commands

A byte below 0xE0 is the length of a note in ticks. The next byte is the key,
with bit 7 set if the length of a rest follows the note. Once a track has a
note with bit 7 set, the length of a rest follows each of its notes. A length
of 0 ties the note to the next one. The tie changes the pitch without starting
the note again. A tie with bit 7 set in the key reads the wait from the next
byte.

| Command | Bytes | What it does |
|---|---|---|
| `E0` | 3 | the depth, period and delay of the pitch LFO |
| `E1`, `E2` | | turns the pitch LFO on and off |
| `E3` | 3 | the depth, period and delay of the level LFO |
| `E4`, `E5` | | turns the level LFO on and off |
| `E8`-`EB` | 3 | a count and a 16-bit jump with counter 0-3. See below |
| `EC`-`EF` | 3 | the same |
| `F0` | 1 | the instrument |
| `F1` | 1 | the volume |
| `F2` | 1 | the pan |
| `F3` | 1 | the fine tune, in 256ths of a semitone |
| `F4` | 1 | the bend range |
| `F5` | 1 | the bend |
| `F6` | 1 | a rest |
| `F7` | | ends the note now |
| `F8` | 2 | a jump by a 16-bit offset from after the command |
| `F9` | 1 | the tempo |
| `FF` | | ends the track |

Bytes `E6`, `E7` and `FA` to `FE` are note lengths. Each of `E8` to `EF` adds 1
to its counter. While the count is at least the counter, `E8`-`EB` clear the
counter and `EC`-`EF` jump and clear it. Once the counter passes the count,
`E8`-`EB` jump and `EC`-`EF` go on. No song of `A88E` uses them, nor the LFOs,
`F3` or `F7`. A song loops by a jump back with `F8`.

The LFOs are square waves. At the start of a note, an LFO waits for the delay
in frames, then turns on at the depth and swings to the other side each
period.

## Channels and instruments

A sample track plays the sample channel with the same number. The record of a
sample channel is 16 bytes at `0x03001B50`: the status, the level of the
envelope, the sample, the position, the right and left volumes, the pitch and
the 4 envelope bytes of the region. The pitch is the key times 256, plus the
bend times the bend range times 2, plus the fine tune. On a PSG track, the
pitch has the bend times the range alone.

The volume and pan set the volumes of the channel (`0x0819A834`). A pan below
0x80 plays the right side at 127 and the left side at the pan. From 0x80, the
left side plays at 127 and the right at 255 minus the pan. The volume is
clamped to 0-255 and scaled by the volume of the player. Each side is then the
volume plus 1 times the level of the side, over 256. A PSG channel gets the
volume over 16. It plays on the right for a pan up to 0x80 and on the left
from 0x7F.

An instrument is a list of regions of 8 bytes. The 16-bit offsets at
`0x0821D1CC` point to the lists. A region contains the lowest key, the
highest key, a 16-bit sample and 4 envelope bytes. A note plays the first
region that contains its key. A note without a region stops the channel. For a
sample, bit 15 of the sample number plays the sample at key 60 on every key.
For a PSG channel, the low byte of the sample is the duty of a square or the
pattern of the wave channel, 16 bytes each at `0x0821DB5E`. The noise takes
the NR43 setting from the key: `0xD7` up to key 20, the table at `0x0821CA74`
up to key 80, and 0 above.

The frequency setting of a square or the wave comes from the pitch
(`0x0819A96C`). The table at `0x0821C9A8` contains a halfword for each key
from 36: 131072 over the frequency of the key, rounded down. The setting is
2048 less the value in the table, with a straight line between two keys by the
low byte of the pitch. A pitch below key 36 gets the setting 0. The hardware
plays a square at 131072 / (2048 - setting) Hz, and the setting 0 plays at 64
Hz. The rounding makes the squares sharp: about 3 cents at key 60, 5 at key
69, 11 at key 81, and up to 25 above key 89.

The sample table at `0x08A806B8` contains a 32-bit offset from the table for
each sample. The header of a sample has 4 bytes with 0x40 in the last one for
a loop, the rate in 1024ths of a hertz at key 60, the start of the loop and
the end. The points follow, unsigned 8-bit. The mixer steps through a sample
at its rate times 2^(semitones from key 60 / 12). Within a semitone, it moves
in a straight line in frequency. An entry of 0 points the header at the table
itself. The driver then plays the bytes of the table as points, at a level
that hardly moves. In `B85A`, the regions of kits 123 and 127 on keys such as
40, 43 and 44 play such samples. The conversion leaves them out of the
SoundFont.

## Envelopes

The mixer runs the envelope of a sample channel each frame. The level starts
at the first envelope byte and rises by that byte each frame to 255. The decay
compares the status of the channel with the sustain level. It treats the
status as the level. A level of 255 is always at least the step of the decay.
The level therefore drops to the sustain level, the third byte, on the next frame.
After the end of the note, the level falls by the fourth byte each frame, and
the channel stops when the level would reach 0. A sample without a loop stops
the channel at the end of the sample.

The squares and the noise play the hardware envelope. The routine at
`0x0819AC84` sets it in turns. An envelope byte b sets a step of (255 - b) /
32, rounded down. The hardware moves the volume by 1 every step 64ths of a
second, and a step of 0 keeps it. The record of the channel keeps the turn,
the setting that the driver last wrote and the frames since. The driver
computes the hardware volume from them (`0x0819AC14`). It counts a step of
frames for each move, where the hardware counts 64ths of a second.

| State | What the routine does |
|---|---|
| 1 | a note has started: restarts the channel at volume 0, rising at the step of the attack, and goes to 2. An attack byte of 255 skips the rise |
| 2 | once the volume reaches the volume of the channel, restarts the channel at that volume, falling at the step of the decay, and goes to 3 |
| 3 | once the volume falls to the sustain level, restarts the channel at that level without a step and goes to 4. The sustain level is the volume of the channel times the third byte, over 256 |
| 4 | the sustain |
| 5 | the note has ended: restarts the channel at the volume, falling at the step of the release, and goes to 6. Without a step, or at volume 0, turns the channel off and goes to 0 |
| 6 | once the volume reaches 0, turns the channel off and goes to 0 |

In states 2, 3, 4 and 6 the frequency goes to the hardware each frame. A
change of the channel volume reaches the hardware only with the next restart.
An attack byte from 224 to 254 sets a step of 0 with the volume at 0. The
division by 0 in the driver returns 0. The rise therefore never ends, and the
note stays silent. No region of A88E has such an attack.

The routine of the wave (`0x0819AF18`) restarts the wave at the start of a
note, at 25, 50, 75 or 100% by the volume of the channel over 4 (the table at
`0x0821CB50`). It silences the wave at the end of the note. A change of the
channel volume reaches the hardware with the next note. The note-on of the
wave (`0x0819AB30`) loads the pattern of the region when it differs from the
last one (`0x0819A9DC`). The loader sets the 32-step mode. In that mode the
hardware plays the pattern an octave below a square at the same setting. In
`B85A` the routine is at `0x0809598C` with the table at `0x080DECC0`, and in
`A84J` at `0x0814AAC0` with the table at `0x084DDDFC`. All three tables
contain the same settings.

## Other versions

Two Hamtaro games play their music with versions of the driver. Apart from
what this section lists, their routines are the same code as in `A88E`, with
the fields of the state at other offsets.

*Tottoko Hamtaro: Hamu-Hamu Sports* (`B85A`) has the same ten commands from
the game. A note has a third byte after the key: the volume of the track. The
driver keeps it at `+0xD`. For a note that isn't tied, the driver computes the
levels before the pitch and the note-on. A tied note changes the pitch alone,
and the volume of the note waits for the next change of levels. `F1` is the
length of a note. `FA` sets the priority of the channel for sound effects from
the next byte. The driver keeps the priority at `+0x4E0` of the state. A track
starts with flags 0x03. The first frame therefore starts no note. Each frame,
each player also adds 0x4B to a count at `+0x4B0`. Each time the count reaches
the divisor, the player takes the divisor off and adds 1 to a second count at
`+0x4BC`. The tracks don't read either count.

*Tottoko Hamtaro 4* (`A84J`) contains an older version. The notes have the
volume byte too. The driver computes the levels at once, before the pitch of a
tied note and before the LFOs start again. `F1` and `FA` are note lengths. The
divisor of the tempo is a fixed 0x4B. The game can send eight commands, with
no divisor or volume command. Commands 3 and 4 fade a player out and in. The
volume of a player applies only while the player fades. A channel that comes
back restores its pitch and levels but doesn't start its note again. The
handler tables of the tracks leave out the second note-on of `A88E`. The offset
of each handler is therefore 4 lower.

In `A84J`, the envelope routine of the squares and the noise (`0x0814A800`)
counts the frames since the last setting on every frame, up to 255. In
`A88E` the count runs only once a setting is written. A release from volume
0 restarts the channel at 0 with the step of the release. When the channel
goes off, the record keeps the setting. The rise, the fall and the sustain
differ from those in `A88E` too. The sustain, for example, restarts the
channel each frame. No song of `A84J` plays the squares or the noise. The model
runs those turns as in `A88E`.

In all three games, the song table ends where the table of sound effects
starts: 51 songs and a 0 in `A88E`, 56 songs in `B85A` and 40 in `A84J`.

## Locating the driver

`agbsf2` finds these routines by their code: the routine that starts a song,
the one that finds a region, the one that mixes a sample channel, the step
routine, and the pitch, noise and wave routines of the PSG. The literals in
them point to the tables. The start of a note in the routine that runs a track
shows the version. The routine that runs the commands of the game
(`0x0819B31C`) leads to the table of sound effects. Command 5 of that routine
starts an effect through a routine with the table as the first literal.

## The game's code

The sound module of the game, from `0x08019308` to `0x08019760`, sends the
commands to the driver. It plays music on two slots. Slot 0 is player 1, and
slot 1 is player 2. Player 0 plays the sound effects.

| Address | Routine |
|---|---|
| `0x08019308` | plays song `r1` on slot `r0` at volume `r2`. Song 0 stops the slot. A song that already plays on the slot goes on |
| `0x080193B4` | moves the volume of slot `r0` to `r1` over `r2` frames, or at once for 0 |
| `0x08019460` | moves the tempo divisor of slot `r0` to `r1` over `r2` frames, or at once for 0 |
| `0x08019628` | stops slot `r0` |
| `0x08019650` | resumes slot `r0` and moves the volume to `r1` over `r2` frames |
| `0x080195B4` | starts sound effect `r0` |
| `0x08018FC4` | runs each frame: moves each volume and divisor a step toward the target |

The play routine leaves the divisor at 0x4B. A volume of -1 keeps the level of
the slot. Where that level is 0, the routine plays the song at 0xFF. The
driver multiplies a volume by 257. A volume of 0xFF sets the player to 0xFFFF.

The game starts its music in these ways:

- The song of a map is byte `+0x14` of the map record. The table at
  `0x083A78D4` contains 512 records of 24 bytes, and they name 25 songs. On
  entering a map, the game starts the song at volume 0 and raises it to 0xFF
  over 16 frames (`0x08023A86`). A song that already plays goes on.
- A battle plays the song that the battle record names, or song 10, on slot 1
  at volume -1 (`0x080FC91C`).
- The event scripts play, fade and stop music with opcodes 0x23 and 0x24
  (`0x080EAFB4` and `0x080EAF4C`). Most of them play at 0xFF. A script stops
  slot 0 or fades it out before it plays a song on slot 1, such as the jingle
  of song 26.
- The title plays song 41 at 0xFF. The file screen plays song 30 with the
  same rise from 0 as a map.

While a song plays, the game changes it in these ways:

- The field fades the music to 0x80 over 8 frames in some states
  (`0x08027558` and three more places), and back at `0x08026FE4`. Scripts
  lower it to 0x80 in 34 places. They play song 20 at 0x80 in 4 places and
  song 31 in 8. Both songs are also map songs at 0xFF, in 14 and 31 maps.
- Only a script changes a divisor, with opcode 0x24 and subcommand 3. One
  cutscene does, from `0x0826D29B` to `0x0826D71B`. It plays song 29 and
  moves the divisor to 55 over 32 frames. Later it moves the divisor to 40
  over 96 frames and sets it to 125. Seven other scripts play song 29 at the
  divisor of 0x4B.
- The 355 sound effects play only tracks 0, 1, 10 and 11. 48 of the 50 songs
  play none of those tracks. Song 7 plays tracks 0 to 7, and song 40 tracks 1
  to 9. A sound effect on track 0 or 1 takes that channel while it plays. The
  track of the song on that channel falls silent.

A song on slot 1 beside one on slot 0 would lose every track that both play.
Most songs play tracks 2 to 9.

## What the conversion leaves out

- What the game changes while a song plays: the fades, the music at half
  volume in some scenes, the divisors of one cutscene and the sound effects. The
  conversion plays each song on player 1 as the game starts it, at the
  divisor 0x4B and the volume 0xFFFF.
- The straight line in frequency within a semitone. It differs from the curve
  of a bend by up to 2 cents.
