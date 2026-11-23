# Ubisoft Milan's driver

Ubisoft Milan wrote a sound engine of its own for *Tomb Raider: The Prophecy*.
The music is a set of short sequences in a MIDI-like format. The notes mostly
play samples through a kit with a sample for each key. A software mixer plays
the samples on four voices, and the sequences can also play the PSG of the
Game Boy. This document describes the engine as `agbsf2` models it. The
example addresses come from the European release of the game (game code
`AL9P`). The sound effects of the game are samples that the game plays
straight from the same sound bank. They aren't part of the music. `agbsf2`
doesn't convert them. Nine long samples that the game plays the same way are
music, and `agbsf2` converts them (see [Samples that play as
music](#samples-that-play-as-music)).

*The Mummy* (game code `AUME`) has the same engine. Its music uses the PSG
and the kit (see [The Mummy](#the-mummy)). *The Sum of All Fears* (`AA6E`) and
*Rogue Spear* (`AR6E`) have an older build. Their music is recorded samples
(see [The older build](#the-older-build)).

## Architecture

The engine is Thumb code in ROM. Only the mixer is ARM code, and the game
copies it to IWRAM. The state of the engine is in EWRAM at `0x02003B00` (the
tables of the sound bank) and `0x02003B80` (the music, with the track at
`0x02003BB0`). The voices of the mixer are in IWRAM at `0x03000014`.

| Address | Routine |
|---|---|
| `0x08002AA8` | returns file r0 of the game archive, with the table of the archive at `0x08061D48` |
| `0x080031A0` | the init of the engine: takes the sound bank, then starts the sequencer if it hasn't started |
| `0x08003E90` | takes the sound bank: the counts and the addresses of the tables |
| `0x08003130` | the sound update that the main loop of the game calls once a frame |
| `0x08003280` | the step of the sequencer that the VBlank of the game runs as a task |
| `0x080044B4` | starts a piece of music: r0 = the resource, r1 = 1 to start it now |
| `0x08004878` | the step of the track: the countdown, then the commands |
| `0x08004950` | the command of a channel: note on or off, program |
| `0x08004BB0` | a PSG note: channels 0-2, and the noise for the low keys of channel 9 |
| `0x080057D0` | a note of the kit, on channel 9 |
| `0x08000F14` | starts a sample on a voice |
| `0x080030B0` | plays a sound event: r0 = the number |
| `0x08004450` | starts a resource on a voice: r0 = the address, r2 = the voice, r3 = the volume |
| `0x080326B8` | starts the background sound of a level: r0 = three sound events |
| `0x0800097C` | the mixer (ARM, copied to IWRAM) that the DMA interrupts of the FIFOs run |

At the start of the game, the code passes the sound bank to the engine: file 2
of the archive, with `movs r0, #2` and a call of the archive lookup, then a
call of the init of the engine.

## The sound bank

The bank starts with two halfword counts and three words. The second count is
the number of resources. The words are offsets from the start of the bank: of
a table of the sound events of the game, of the resource table and of the kit
table. The resource table has a word for each resource, the offset of the
resource from the bank. The kit table has a halfword for each kit of channel
9: a resource number, or `0x8000` for none. The init of the engine sets the
first kit on channel 9.

Each resource starts with a byte for the kind:

| Kind | Resource |
|---|---|
| 0 | a piece of music: the list of sequences |
| 3 | a sample |
| 4 | a list of resources, one of which plays |
| 6 | a sequence |
| 7 | a kit: a halfword for each key, the resource number of a sample or `0x8000` |

A sample has 16 bytes of header: the kind, a byte that is 1 if the sample
loops, two bytes that the engine doesn't read, the length in points, and the
start and end of the loop in points from the first. A sample without a loop
has `0x42424242` in those two words. The points follow, 8-bit and signed.

## Pieces of music

A piece of music has the kind and three bytes that the engine doesn't read,
then a halfword count of sequences, a halfword of flags and the resource
numbers of the sequences. Flag 1 makes the list start again after the last
sequence. Flag 8 reserves a voice for channel 9 for the whole piece. A piece of
one sequence with flag 1 plays it again and again.

Starting a piece silences the PSG channels and stops the voice of channel 9.
It takes a voice for channel 9 if the piece has flag 8, and starts the track
on the first sequence, with the next one queued. When a sequence ends, the
track takes the queued one at once, in the same frame. The callback of the
engine then passes the resource of the sequence after that to the track. The
next sound update puts the address of the commands of that sequence in its
place and moves the piece on. A piece without flag 1 stops when the last
sequence ends.

## Samples that play as music

The code of the game plays sound events, and a sound event can start a sample
with no sequence. The table of events in the bank has an offset for each
event, from the start of the bank. An event starts with a byte for the kind, a
byte for the priority, two bytes of `0xFF`, a halfword number and a halfword
volume. A word of flags follows. Kind 0 plays the resource of that number, and
kind 1 stops the event of that number. Flag 2 puts the sample on the
background voice. One event at a time has that voice.

The background sound of a level is three event numbers. `0x080611F8` contains
a table of them for the states of the background sound in the game. A level
starts with events 9 and 10. Their resources 38 and 39 are lists with an entry
for each level. The number of the level in `0x02007A8C` selects the entry.
Resource 38 contains a loop for each level, and resource 39 contains
one-shots. Objects in a level can ask for state 1 or for states 2 and 3.
State 1 plays event 13, and states 2 and 3 play event 65.

Most of these loops are short noise. Nine long samples have chords and a beat,
and `agbsf2` converts them as songs 14-22:

| Song | Resource | Length | Loop | What plays it |
|---|---|---|---|---|
| 14 | 103 | 15.60 s | from 838 points | event 13, levels 12 and 14 |
| 15 | 104 | 14.03 s | whole | events 55 and 153, levels 11, 13, 27, 37, 40, 48 and 50 |
| 16 | 105 | 15.38 s | whole | event 66, levels 24 and 30 |
| 17 | 106 | 15.50 s | whole | event 65 |
| 18 | 107 | 14.78 s | none | nothing |
| 19 | 108 | 7.88 s | whole | events 128 and 156: the game over screen |
| 20 | 224 | 8.01 s | whole | event 107: the story screens |
| 21 | 259 | 16.02 s | whole | nothing |
| 22 | 308 | 16.02 s | whole | event 187: the credits |

The events, lists and kit of the bank don't name resources 107 and 259. The
game never plays them. Songs 8 and 9 contain two more such loops, resources
245 and 109, through keys 35 and 34 of the kit. The language menu and the main
menu of the game play the same samples through events 127 and 98.

A song of this kind has one note on each pass of the loop of the sample, on a
key from 81 up, at full volume. In the game, the voice goes on looping with no
new start. A file that loops on its markers requires a note for each pass.
Each note starts on the frame nearest the start of its pass. The loop of
resource 103 starts 838 points after the first point. The later passes of that
resource play a second key, 20 above the first. The zone of the second key
starts at the start of the loop. The markers go on the second pass.

The routine at `0x08004450` plays each of the nine through FIFO A. The
averaging in the mixer takes the treble off. The second pass of each loop
starts on the point that the length of the loop sets. Resource 103 is the
exception, by 4 points. The start of its loop isn't on a word boundary, and
the DMA of the mixer moves whole words.

## Sequences

A sequence has the kind and three bytes that the engine doesn't read, then a
halfword countdown, then commands. Each command is a status byte, with the
channel in the low 4 bits, then the arguments, then the countdown of the next
command:

| Status | Arguments | Command |
|---|---|---|
| `8c` | key, velocity | note off on channel c |
| `9c` | key, velocity | note on on channel c |
| `Cc` | program | program change on channel c |
| `Ec` | none | nothing |
| `FC` | none | the end of the sequence |

Any other status makes the game reset the console.

The step of the track runs once a frame. If the countdown isn't 0, the step
counts it down. Otherwise it runs commands until a countdown isn't 0. A
countdown of n therefore puts the next command n + 1 frames later, and a
countdown of 0 runs it in the same frame. The countdown of the first sequence
is set before the step of the first frame. The first command therefore comes
n frames in.

Channels 0, 1 and 2 play square 1, square 2 and the wave channel of the PSG.
On channel 9, the keys up to 11 play the noise channel, and the keys above
that play the kit. A note on another channel plays nothing. The first argument
of a command selects between the PSG and the kit on channel 9. A program
change to 11 or below therefore goes to the PSG. The PSG stores it past the
end of the records of its three channels, and it changes nothing that the
music plays.

## The kit

A note of the kit plays the sample in the entry of the kit for key + program.
The program is the last program change above 11 on channel 9, or 0 at first.
The engine doesn't check the entry. `0x8000` makes it read resource 32768,
past the end of the resource table. In *Tomb Raider* that results in an
address in a mirror of the ROM, and the note plays whatever bytes are there.

The mixer plays a sample at a fixed rate, and a key selects a sample. The
velocity is the volume of the voice. Each note takes a voice: the voice of the
piece, if the piece has flag 8, or else the last free one of the four. A
note-off on channel 9, whatever the key, stops the voice of the last note on
channel 9 and frees it. Another note that still plays goes on until the sample
ends. A voice is also free again at the sound update after the mixer reaches
the end of the sample. If no voice is free, the engine writes the note before
the records of the voices.

The track has a switch for the kit, a word at `+0x138`. The init of the track
sets it. While the switch is clear, the channel command skips the notes and
note-offs of the kit. The notes of the PSG and the program changes still play.
In *The Mummy*, the tests are at `0x08004942` and `0x08004982`, and the
routine at `0x08003284` sets or clears the switch.

## The mixer

The game sets SOUNDCNT_H to `0xA90E`: the PSG and both FIFOs at full volume,
FIFO A on the right and FIFO B on the left, both clocked by Timer 0. The sound
init computes the reload of Timer 0 in floating point. It adds the double 0.5
to `1024.17` and takes the whole part of the sum from `0xFFFF`. The result is
`0xFBFF`, 1025 cycles a point, or 16368 Hz. The DMA of each FIFO interrupts
the CPU at every 16 points. Whichever interrupt comes second runs the mixer,
and the mixer makes the next 16 points. Both FIFOs play them, and the music is
mono.

Each run, the mixer copies the next 8 points of each voice that plays with
DMA 3, and moves the voice on 8 points. A voice that loops goes back to the
start of the loop once it reaches the end of the loop. A voice that doesn't
loop stops once it reaches the end of the sample, after those 8 points. A
loop therefore lasts a whole number of 8 points from the start of the loop.
The points after the end of the loop, up to the next 8, come from beyond it.
A sample without a loop leaves the loop points of the voice as the last sample
that looped set them. The mixer multiplies each point by the volume of the
voice and adds the voices. The result is 8 points at 8184 Hz. The mixer puts a
point halfway between each pair, averages each point with the one before,
shifts the sums right by 6, and clips them to 8 bits. A full-scale sample at
volume 64 fills the scale.

## The PSG

Each PSG channel has a table of 16-byte instruments: for square 1 at
`0x0803D10C`, for square 2 at `0x0803D2EC` and for the wave channel at
`0x0803D4CC`. The three tables follow each other, 30 instruments apart. A note
on square 1 writes NR10 from the first byte of the instrument and NR11 and
NR12 from the second and third, with the velocity ORed into NR12. It writes
NR13 and NR14 from the frequency setting of the key and the fourth byte. The
instruments of square 2 have no sweep byte. A note on the wave channel sets
NR30 to `0xA0` and writes the velocity to NR32 as the volume. The place of the
key in the frequency table at `0x08038084` is 36 keys down on the squares and
12 down on the wave channel. Every key therefore plays at its MIDI pitch. A
program change on the wave channel loads 32 bytes of the wave table at
`0x08038154` into both banks of the wave RAM. It selects each bank by writing
`0x20` and then `0x60` to NR30. Both writes turn the wave DAC off until the
next note. The two banks play as 64 points. A note-off silences the channel,
if the key of the note-off is the last one that a note on the channel set.

The halfword at offset 12 of an instrument contains flags: 1 for a modulation,
2 for a pitch mode and 4 for legato. The fine table at `0x080385FC` contains a
frequency setting for each cent, 100 to a key, from the first key of the
frequency table. The modulation and the pitch mode select an entry in it.

**Modulation.** A note on an instrument with flag 1 starts a triangle in cents.
The halfwords at offsets 4, 6 and 8 of the instrument contain the delay in
frames, the depth in cents and the rate. The triangle moves by the depth ×
65536 / (2 × rate) each frame, in 16.16 fixed point. The division in the game
library rounds towards 0. The triangle starts at 0 once the delay has counted
down, rises past the depth, then falls past the depth below 0, and so on. It
turns one frame after its integer part passes the depth. During the delay, the
modulation is 0 cents.

**Pitch mode.** A note on an instrument with flag 2 glides. Each channel keeps
a state: 0 off, 1 at the pitch of a note, 2 gliding and 3 at the end of a
glide. The first note in the pitch mode starts at its key and sets state 1. A later note
restarts the channel at the key of the note before, or in states 2 and 3 at
the fine-table setting that the glide has reached. The note then glides to its
key over the number of frames at offset 10. The step is the distance in cents
divided by the frames, rounded towards 0. Each frame, the glide moves by the
step until it reaches or passes the key. On the next frame, the glide jumps to
the key and sets state 3. A glide therefore overshoots by up to a step for a
frame. A note on the key of the target before the last gets a step of 0. A
note on an instrument without the pitch mode sets state 0.

**Legato.** A note on an instrument with flag 4 doesn't restart the channel if
it comes after a note-on and before the note-off. The note writes the frequency
setting of its key without a restart, or in the pitch mode glides there. On the
wave channel, the pitch-mode check reads all the flags of the instrument. A
wave instrument with legato therefore always uses the glide code. In state 0,
the frequency stays. Neither *Tomb Raider* nor *The Mummy* has an instrument
with legato.

**The update.** After the commands of each frame, or the countdown, the track
step runs the PSG update at `0x08004CC4` for the three channels. The update
runs while the track plays, and in the frame in which the piece stops. It moves
the modulation and the glide on. If either sets a place that isn't 0, the
update writes the fine-table setting for that place to NRx3 and NRx4, without a
restart. The place is the glide position plus the modulation, or in state 0
the key position plus the modulation. The write clears the length enable bit
in NRx4. A modulation of 0 cents with no glide writes nothing.

The engine init clears the state of each channel. The start of a piece
doesn't. The start only writes 0 to NR11, NR21, NR31 and NR41. In the game, a
modulation, a glide and the wave program carry over from one piece to the
next.

The noise notes of channel 9 take 4 bytes for the key from the table at
`0x080385CC`: NR41, NR42, with the velocity ORed into it, NR43 and NR44. A
noise note-off writes nothing. The noise envelope and length counter run on.
The next noise note replaces the note on the channel.

The init of the engine sets SOUNDCNT_L to `0xFF33`: every PSG channel on both
sides, at a master volume of 3 of 7. It loads the wave of program 0 into the
wave RAM. Bit 14 of SOUNDBIAS sets the PSG output rate to 65536 Hz.

`agbsf2` records the PSG registers at the end of each frame. A model of the
GBA PSG runs them: the envelopes, the length counters, the square 1 sweep and
the DAC. Each note-on starts a stretch of its channel, up to the next note-on
on that channel. A MIDI note is a run of frames in which the channel sounds.
CC11 sets the level of each frame, and the pitch bend sets the pitch relative
to the zone. The noise has a track of its own. Each sound that the noise
registers make from a note-on becomes a drum, rendered at 65536 Hz.

## Quirks

* A note on a key outside the kit plays bytes from elsewhere in the
  cartridge (see [The kit](#the-kit)). `agbsf2` leaves the note out,
  with a warning.
* A note-off on channel 9 stops the voice of the last note on the channel,
  whatever the key.
* A countdown of n waits n + 1 frames. The first countdown of the first
  sequence waits n.

## Locating the driver

`agbsf2` looks for the code of the engine:

* The step of the track:
  `B530 1C04 68E0 2800 D0xx 8820 1C25 3514 2800 D1xx 6920 2800 D0xx 2000 6120
  68A2 2A00 D0xx 6062 7811 7850 0200 4301 1C90 6060 8021`.
* The routine that takes the sound bank:
  `4Axx 6110 8801 8011 8841 8051 6841 1841 6051 6881 1841 6091 68C1 1840 60D0
  4770`. The game calls it from a routine that starts with `push {lr}`. The
  start of the game calls that routine after `movs r0, #N` and a call of the
  archive lookup. `agbsf2` runs the lookup with N to find the bank.
* The instrument lookup of the PSG, with the three literals that point to the
  tables of the channels:
  `B500 0600 0E00 2901 D0xx 2901 DCxx 2900 D0xx Exxx 2902 D0xx Exxx 0100 49xx
  1840`.
* The noise note, the frequency lookup of a note on square 1 and the program
  change of the wave channel, for the noise, frequency and wave tables:
  `00B1 48xx 1809 780A 3101 7808 3101 4338 0200 4310 4Axx 8010`,
  `49xx 4642 0050 1840 8807 6BE0 0040 1840 8800` and `0168 49xx 1840`.
* The PSG update, with the `ldr` at offset 20 that loads the address of the
  fine table:
  `1962 2C00 D1xx 4640 303C 4448 6801 2064 4348 1812 49xx 0050 1840 8801`.
* The sound init, with the last `ldr` that loads the float for the reload of
  Timer 0.

The pieces of music are the resources of kind 0 in the bank, in order. In the
three releases of *Tomb Raider*, the samples that play as music follow them,
by resource number and game code. `--song-table` sets the address of the sound
bank instead.

## The Mummy

*The Mummy* (`AUME`) has the same code as *Tomb Raider*, at other addresses.
From the music start on, the routines are `0x74` bytes lower: the music start
at `0x08004440`, the track step at `0x08004804` and the PSG update at
`0x08004C50`. The sequencer step is at `0x08003268`, and the track at
`0x02003470`. The archive lookup and the engine init are at the same
addresses as in *Tomb Raider*. The sound bank is file 2 of the archive, at
`0x08060398`. It has 256 resources: 26 pieces of music, 26 sequences, 174
samples, 29 lists and a kit. The PSG instrument tables are at `0x0803E384`,
`0x0803E564` and `0x0803E744`. The frequency, wave, noise and fine tables are
at `0x080392FC`, `0x080393CC`, `0x08039844` and `0x08039874`.

The music plays mostly on the PSG. Of about 14,200 notes, two thirds are on
the squares and the wave, 3,642 on the noise and 561 on the kit. The square
instruments with a modulation wait 38 frames, then swing about 43 cents either
way at 5 Hz. The wave instruments with a modulation have depths of 10 and 35
cents. The glides last 5 to 200 frames. Noise keys 0-11 turn on the length
counter.

### The levels' music and the kit

The music settings of a level are in the doors of the level, the `MPEV`
objects. A door contains its number, the level and the door that it leads to,
a type and three sound events. The player arrives in a level through one of
its doors. The level start (`0x08035C4C`) finds that door by its number
(`0x080359CC`). It clears the switch of the kit and starts the music of the
door by the type (`0x08035B70`). A level of type 0 switches the kit on and
plays the first event. A level of type
2 switches the kit on and plays all three. A level of type 1 (`0x08035BB8`)
plays the first event with the kit off. When the player reaches a door, the
game changes to the level and door that the door leads to (`0x080359B4`). Each
place on the world map, level 34, leads to door 0 of a level (`0x080371F8`).
A new game starts at level 35 (`0x08037112`). Levels 35 to 38 are cutscenes,
and level 38 leads to level 0.

The state routine of the level (`0x08035A78`) switches the kit only in a level
of type 1. In state 2, it plays two sound events that aren't music, then the
first event again, and switches the kit on. The piece already plays, and the
sound event doesn't start it again. The kit comes in where the piece is. State
1 switches the kit off again the same way. The AI of a character sends state 2
when the character turns to mode 3, and state 1 when it turns to mode 1
(`0x0801F830`). In level 33, the character Alex turns to mode 3 two frames
after the level starts. A sound area (an `SNDA` object) contains two more
settings, for the two sides of a line in the level (`0x080362F8`).

The `MPEV` and `SNDA` objects of each level start the pieces below. Level 34,
the world map, has no music settings. Levels 46 and up don't exist.

| Piece | Levels of type 1 | Other starts |
|---|---|---|
| 0 | | the title screen |
| 1 | 18, 20 | |
| 2 | 8, 9, 10, 11 | |
| 3 | 2, 3 | |
| 4 | 27, 28, 29 | |
| 5 | 7 | |
| 6 | 16 | |
| 7 | 21, 22 | |
| 8 | 17, 19 | |
| 9 | 6 | |
| 10 | 12, 15 | |
| 11 | 0 | |
| 12 | 1 | |
| 13 | 4 | |
| 14 | 24 | |
| 15 | 25, 26, 30 | |
| 16 | 32, at a door that nothing leads to | level 39 (type 2), then level 33 (type 0) |
| 17 | | levels 7, 8, 11 and 14 (type 2), and state 5: no notes |
| 18 | | the screen of the ending (`0x083C0DA0`) |
| 19, 20 | | none found |
| 21 | | levels 13 and 23 (type 0) |
| 22 | | the game over screen (`0x083BBBA8`) |
| 23 | | level 14 (type 0) |
| 24 | | levels 37, 40, 42, 43 and 44 (type 2) |
| 25 | | level 45 (type 2) |

Levels 5 and 31 have settings of type 1 with no music, and so do one
setting each of levels 13 and 32. The other levels of type 2 play only sound
events that aren't music. A screen such as the title has a description in
the ROM. Byte 10 of the description is the sound event of the screen. The
routine that starts a screen (`0x0800B76C`) doesn't touch the switch. But each
return from a level to the screens builds the screen manager again. The
constructor of the screen manager switches the kit on (`0x0800AB1C`). The game over (`0x0801D29C` with event 10) plays piece
22 with the kit on screen 5, after a level of either type. The ending plays
piece 18 with the kit on screen 6. The door of level 41 leads there, through
level 46, past the last level.

`agbsf2` converts pieces 1-15 without the kit. The levels of type 1 start them
that way. A second version, with the kit, goes in files with the suffix `_kit`. It
plays the piece as it sounds in state 2. Door 1 of level 32 starts piece 16
with the kit off, and door 1 leads to level 39. No door and no place on the map
leads to door 1 itself. The piece therefore starts in level 39 (type 2), and
level 39 leads to level 33 (type 0). Both play it with the kit. `agbsf2`
converts piece 16 with the kit only.

## The older build

*The Sum of All Fears* (`AA6E`) and *Rogue Spear* (`AR6E`) have an older
build of the engine. The bank routine of that build copies two tables, not
three: `4Axx 60D0 8801 8011 8841 8051 6841 1841 6051 6881 1840 6090 4770`, at
`0x08003324` in `AA6E` and `0x08003328` in `AR6E`. The game start calls it in
the same way as in *Tomb Raider*, with file 1 of the archive. The bank starts
with two halfword counts, for the sound events and the resources, then the
offsets of their tables. The bank of `AA6E` at `0x0804A8FC` has 175 events and
282 resources. The bank of `AR6E` at `0x08045F10` has 175 events and 279
resources. The resources are samples, lists and resources of kind 0, with no
sequence or kit. The sound init at `0x080010CC` loads the same float as in
*Tomb Raider*, 1024.17. It copies two ARM routines to IWRAM. The code of this
sound init doesn't match the sound init of the newer build. `agbsf2` finds the routine and the
bank, and reports that the music is recorded samples. It doesn't convert them.
