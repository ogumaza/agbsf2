# Nintendo R&D2's GBA sound driver

This document covers the sequence format, sequencer, voices and mixer in
Nintendo R&D2's sound driver for *The Legend of Zelda: A Link to the Past*
on the Game Boy Advance. The cartridge, *The Legend of Zelda: A Link to the
Past & Four Swords*, plays the rest of its music with Nintendo's MP2K. The
document is based on the code of the driver in the US release (game code
`AZLE`). The example addresses come from there. Other games put the driver and
its data elsewhere. `agbsf2` finds them from the code (see [Locating the
driver](#locating-the-driver)). *Super Mario Advance 2: Super Mario World* has
an older revision of the driver. [The Super Mario Advance 2
revision](#the-super-mario-advance-2-revision) lists the differences.
*Super Mario Advance 3: Yoshi's Island* has the revision of A Link to the Past,
with another volume for sample voices (see
[The Super Mario Advance 3 revision](#the-super-mario-advance-3-revision)).
*Super Mario Advance 4: Super Mario Bros. 3* has the same revision (see
[Super Mario Advance 4](#super-mario-advance-4)).

The driver mixes samples in software, at 10512 Hz in stereo, into a buffer for
each of the two DirectSound FIFOs: FIFO A plays the left side and FIFO B the
right. It has 7 sample voices for the mixer, and a voice for each of the Game
Boy's four PSG channels. There are 20 players for sequences, each with up to
10 tracks drawn from a shared pool of 24.

## Architecture

The driver is Thumb code in ROM, apart from three ARM routines that its init
copies to IWRAM: the inner loop of the mixer (it adds the samples of a voice
into the mix), the echo, and the conversion of the mix to the 8-bit samples of
the FIFOs. The game calls the init of the driver with its settings (see
[The game's settings](#the-games-settings)). It calls the VBlank routine of the
driver at each VBlank, and the per-frame routine after that. The per-frame
routine runs these steps:

1. It handles the requests that the game has made since the last frame:
   start a sequence on a player, start a sound effect, stop or fade a player,
   set the echo, and others (see [Requests](#requests)).
2. The sequencer runs the tracks of each player (see [Players and tracks](#players-and-tracks)).
3. The PSG voices write the registers of their channels (see [PSG voices](#psg-voices)).
4. The mixer mixes the sample voices into the buffers. The DMA of the VBlank
   routine sends them to the FIFOs during the next frame (see [The mixer](#the-mixer)).

In `AZLE` the routines are:

| Address | Routine |
|---|---|
| `0x08129E60` | the sound init of the game: calls the init of the driver with the settings at `0x08198C30` |
| `0x0812A6CC` | init: stores the settings, sets up the sound registers, copies the ARM routines from `0x0812CC58` to `0x03001400`, and sets up the voices, tracks and players |
| `0x0812A044` | the play-music routine of the game: `r0` is the music number of the game, and the byte table at `0x08198C50` turns it into a sequence |
| `0x0812C718` | request a sequence: `r0` = player, `r1` = sequence |
| `0x0812C73C` | request a sound effect: `r0` = player, `r1` = the sequence of sound effects, `r2` = the track |
| `0x0812C704` | hand over the requests made since its last call, for the next frame to handle |
| `0x0812A798` | the VBlank routine: restarts timer 0 and the DMA of the next output buffers |
| `0x0812A7A4` | the per-frame routine |
| `0x0812CC24` | handle the requests, through the table of handlers at `0x08198EC8` |
| `0x0812C3DC` | the sequencer: each player in turn |
| `0x0812BE54` | run a track for a frame, with the command table for `C2` to `FF` at `0x0812BF98` |
| `0x0812B7B4` | note on |
| `0x0812AB30` | look up an instrument |
| `0x0812BC64` | allocate a voice |
| `0x0812B980` | release the note of a voice |
| `0x0812BA6C` | stop a voice |
| `0x0812B520` | the frame of the PSG voices |
| `0x0812B328` | the frame of the mixer |
| `0x0812A964` | mix the frame of a sample voice |

The music plays on player 0x12. The per-frame sound routine of the game, at
`0x08129E7C`, fades the music in and out through requests. It hands over the
requests at its end.

The driver's variables in `AZLE` are:

| Address | Variable |
|---|---|
| `0x03000140` | the two output buffers, each 176 left samples then 176 right ones |
| `0x03000152` | the output buffer the mixer writes this frame |
| `0x03000158` | the region that instruments with a sample for each key play (see [Instruments](#instruments)) |
| `0x03000160` | the echo level: 0-15 shifts the echo right by that many bits each time round, and 16 turns it off |
| `0x03000164` | the wave in the wave RAM |
| `0x03000168` | the first sample voice that plays, in the order of the mixer |
| `0x0300016C` | the first free sample voice |
| `0x03000175` | the echo buffer the mixer reads this frame |
| `0x03000176` | the echo delay in frames (18) |
| `0x03000177` | the echo level that the game requests (the level moves towards it by 1 a frame) |
| `0x03000180` | a routine of the game that plays the notes of a player in place of the driver, when the player has flag 1 |
| `0x03000184` | a routine of the game that `CA` calls |
| `0x03000188` | the requests, 12 bytes each |
| `0x030003CC` | the game's settings |
| `0x0300069C` | the mix: 176 left samples and 176 right ones, 16 bits each |
| `0x0300095C` | the mix of the voices with an echo send |
| `0x03000C20` | the 24 tracks, 0x54 bytes each |
| `0x03001760` | the 7 sample voices, then the 4 PSG voices, 0x78 bytes each |
| `0x03001C88` | the 20 players, 0x48 bytes each |
| `0x02036000` | the echo history: a mix like `0x0300095C` for each frame of the echo |

## The game's settings

The game passes init the address of 8 words:

| Offset | Setting |
|---|---|
| `0x00` | the sample sets: a table of offsets of the table of samples of each set |
| `0x04` | the banks: a table of offsets of each bank |
| `0x08` | the music sequences: a table of offsets of each sequence |
| `0x0C` | the sequences of sound effects, in the same form |
| `0x10` | a halfword for each bank: the sample set that its instruments play |
| `0x14` | a table of offsets of the list of banks of each sequence |
| `0x18` | the same for the sound effects |
| `0x1C` | a word that the driver doesn't read |

Each table of offsets contains a word for each entry, counted from the start
of the table. The tables don't give their length. `agbsf2` takes a table to
end where the data of its first entry starts. The table of samples of a sample
set has the same form: an offset from its start for each sample.

## Requests

A request is 12 bytes: a halfword, then two words of arguments. The high byte
of the halfword selects a table of handlers, and the low byte selects the
handler. The handlers of the first table start a sequence on a player (`0`,
with the player and the sequence), start a sound effect (`1`), fade a player
out and stop it (`2`), pause or resume it (`3`), and set its tempo change
(`4`), its second volume (`5`) and its flags (`6`). Handler `1` takes the
player in the high half of the first word, the sequence of the sound effect in
the low half, and the track in the second word. The handlers of the second
table change the tracks of a player that a mask selects, such as their mute
and their second volume. The handlers of the fourth table include the echo.
The driver handles a request in the first frame after the game hands the
requests over.
Handler `4` of the fourth table sets how many of the 7 sample voices the
driver uses. To take a voice out of use, it allocates one as a note of the
highest priority would. That takes the first free voice, or else stops the
first voice that plays. The handler then keeps the voice on a separate list.
Neither the notes nor the mixer use that list. To give voices back, the
handler moves them from that list to the free voices.

A sound effect is a track of one of the sequences of sound effects (see
[Sound effects](#sound-effects)).

## Sequences

A sequence starts with a signed byte, its number of tracks, and a byte of 0.
Then come the offsets of the tracks, a halfword each, from the start of the
sequence. An offset of 0 means that the track doesn't start with the sequence.
`F8` can start it later. The list of banks of a sequence is a list of
halfwords: entry `n` is the bank that `C7 n` selects. A track starts with
entry 0. The lists of
different sequences may share entries.

## Sound effects

A sound effect is a track of one of the sequences of sound effects. Such a
sequence has no count of tracks. It starts with the offsets of its tracks, a
halfword each, from the start of the sequence. The table doesn't give its
length. `agbsf2` takes it to end where the first track starts. Each sequence
of sound effects has a list of banks in the same form as the music (see
[The game's settings](#the-games-settings)).

A request of the game for a sound effect names the sequence and an entry of its
table. Like a request for a sequence, it stops what the player was playing and
resets the player. It then sets byte `0x43` of the player to 1 and starts the
track as track 0 of the player. A track that starts on such a player starts at
a priority of 12 with an echo send of `7F`. Like a track after `C8`, it makes
each note wait for its length. A track that `F8` starts gets the same
settings. `F8` then copies the priority and the echo send of the track that
runs it. The request leaves the sample voices as they are. The games play
their sound effects on other players than the music: *A Link to the Past* on
players 0x10 and up, and *Super Mario Advance 2* and *3* on players 0x11 and
up.

`agbsf2` converts each sound effect as a song. The sequences come first, then
the sound effects: the tracks of the first sequence of sound effects in order,
then those of the next. An entry whose offset is 0 gives no song. A sound
effect plays with the 7 sample voices that init sets up. Its conversion lasts
until the last of its tracks ends, the tracks that `F8` starts included.

## Players and tracks

Starting a sequence on a player stops what the player was playing, and starts
each of its tracks on the first free track of the 24. The record of a player:

| Offset | Field |
|---|---|
| `0x00` | the list of banks of the sequence |
| `0x04` | the sequence |
| `0x08` | the 10 tracks, a word each |
| `0x30` | the tempo, 150 at the start; `E4` sets it |
| `0x32` | a signed change to the tempo that the game can set |
| `0x34` | the fade level, `0x8000` for full |
| `0x36` | the step of the fade a frame |
| `0x38` | the last level of the fade |
| `0x3A` | the frames left in the fade |
| `0x3C` | flags: bit 0 pauses the player (a paused player releases the notes of its tracks each frame) |
| `0x40` | the volume, `0x80` at the start; `EA` sets it |
| `0x41` | a second volume, `0x80`; a request sets it |
| `0x42` | 1 while the player plays, 2 to stop it when its fade ends |
| `0x43` | 1 for a sound effect |
| `0x44` | flags: bit 0 passes the notes of the player to the routine of the game at `0x03000180` |

The record of a track:

| Offset | Field |
|---|---|
| `0x00` | the next byte to read |
| `0x04` | the table of samples of the sample set of its bank |
| `0x08` | its player, or 0 if the track is free |
| `0x0C` | its voices: the newest, linked to the next through `0x74` of the voice |
| `0x10` | the LFO's delay in frames (`E5`), a halfword |
| `0x14` | the LFO's rate (`E6`), `0x22` at the start |
| `0x18` | the LFO's depth (`E7`); 0 turns the LFO off |
| `0x1C` | 1 while the next note slides (`Dx`) |
| `0x1D` | the flags of the slide: the low nibble of `Dx` |
| `0x1E` | the slide's note |
| `0x20` | the slide's delay in frames |
| `0x22` | the slide's length, in 256ths of the frames of the note |
| `0x24` | 3 return addresses for `F4` |
| `0x30` | the next free return address |
| `0x34` | the time left before the next command, a signed word (see [Timing](#timing)) |
| `0x40` | the entry of the list of banks of the sequence (`C7`) |
| `0x42` | the instrument (`C2`) |
| `0x44` | the notes' length, 127 at the start |
| `0x46` | the wait of `C0` |
| `0x48` | the notes' velocity, 127 at the start |
| `0x49` | 1 in legato (`C5`) |
| `0x4A` | 1 to mute the notes of the track; a request sets it |
| `0x4B` | the pan, `0x40` for the centre (`C3`) |
| `0x4C` | the echo send (`E3`); 0 at the start, `7F` for a sound effect |
| `0x4D` | the volume (`E0`), `0x80` at the start |
| `0x4E` | a second volume, `0x80`; a request sets it |
| `0x4F` | the bend, a signed byte (`E1`) |
| `0x50` | the bend's range (`E2`), 2 at the start |
| `0x51` | the transpose (`E9`) |
| `0x52` | the notes' priority (`C4`), 3 at the start, 12 for a sound effect |
| `0x53` | 1 if notes wait for their length (`C8`), as the notes of a sound effect do |

Each frame, the sequencer takes the players in order, and the tracks of each
player in the order of its 10 entries. A track that ends frees its entry, and a player
whose tracks have all ended stops.

## Commands

The bytes of a track are notes and commands. A length is a byte, or two bytes
with the top bit of the first set: `((b0 & 0x7F) << 8) | b1`.

| Bytes | Meaning |
|---|---|
| `00`-`5F` | a note, with the track's length and velocity |
| `60`-`BF` length velocity | a note `nn - 0x60`, with a new length and velocity that the track keeps |
| `C0` | wait for the wait of `C0` |
| `C1` length | wait, and keep the length as the wait of `C0` |
| `C2` n | instrument `n` of the bank |
| `C3` n | pan, `0x40` for the centre |
| `C4` n | the priority of the track's notes |
| `C5` | release the track's notes, and start legato |
| `C6` | release the track's notes, and end legato |
| `C7` n | the bank in entry `n` of the sequence's list, with its first instrument |
| `C8` | notes wait for their length |
| `C9` | notes don't wait |
| `CA` n | call the game's routine at `0x03000184` with the track and `n`, if it has one |
| `Dx` note length [delay] | slide the next note (see [Slides](#slides)); the delay byte comes with `x & 1` |
| `E0` n | volume |
| `E1` n | bend, a signed byte |
| `E2` n | bend range |
| `E3` n | echo send |
| `E4` length | tempo |
| `E5` n | the LFO's delay in frames |
| `E6` n | the LFO's rate |
| `E7` n | the LFO's depth |
| `E8` | no slide for the next note |
| `E9` n | transpose |
| `EA` n | the player's volume |
| `F0` offset | jump to the halfword offset from the start of the sequence |
| `F4` offset | call: keep the address of the next byte and go to the offset |
| `F8` n offset | start track `n` of the player at the offset, with this track's bank, instrument, pan, echo send, volumes, priority, bend, bend range and transpose; a track already in entry `n` stops first |
| `FF` | return from a call, or end the track |

The other bytes from `C2` up do nothing and take no operand. A note plays the
track's transposed note number: see [Note on](#note-on).

## Timing

A track counts time in 150ths of a tick. The tick is the unit of lengths and
waits. A wait of `n` ticks adds `150n` to the count of the track. While the
count is 0 or less, the track reads commands. After that, each frame takes the
tempo of the player, and its tempo change, off the count. A tempo of 150
therefore plays a tick a frame. The count keeps the part of a frame left over
from one command to the next. A tempo change takes effect at the end of the
frame of its command, for the track itself and for the tracks after it in the
order of the player. For the tracks before it, the change takes effect from
the next frame.

A track that `F8` starts runs from the first frame in which the sequencer
reaches it: the same frame if it comes later in the order of the player, and
the next frame if it comes earlier.

The length of a note is in frames: `150 * length / tempo`, rounded down. The
tempo includes the tempo change unless the region of the note has flag `0x10`.
A note without `C8` doesn't wait. Notes at the same time therefore make a
chord.

## Instruments

A bank starts with a table of halfwords: the offset of each instrument from the
start of the bank. The instrument numbers that `C2` sets are bytes. A bank
therefore has up to 256 instruments. The table doesn't give its length. The
first byte of an instrument sets its kind:

| Kind | Instrument |
|---|---|
| `00`-`0F` | a region of its own |
| `10` | a drum kit: halfword 2 is the offset of its table of drums, and byte 4 the note of its first drum. Each drum takes 4 bytes: the offset of its region, a pan, and a byte that the driver doesn't read. A drum plays at the pitch of its region, with a pan of its own |
| `11` | a sample for each key: halfword 2 is the offset of a table with a halfword for each note, the number of the sample. The driver puts the sample in its region in RAM. That region plays a sample voice at the sample rate, with the envelope at `0x08198EB4` and no release |
| `12` | a key split: halfword 2 is the offset of a table of 4-byte entries. Each entry has the highest note that it plays as a byte, then the offset of the region at its halfword 2. A note plays the first entry whose highest note is at least as high |

The offsets are from the start of the bank. A region:

| Offset | Field |
|---|---|
| `0x00` | the voice type: 0 for a sample, 1 and 2 for the squares, 3 for the wave and 4 for noise |
| `0x01` | flags: bit 0 puts a table at halfword 2 (see [PSG voices](#psg-voices)), and bit 4 leaves the tempo change out of the length of the note |
| `0x02` | the number of the sample in the sample set of the bank, the duty of a square, the width of noise, or the offset of a table or a wave |
| `0x04` | the offset of the envelope |
| `0x06` | the release |
| `0x07` | the tuning: the note that plays at the sample rate |
| `0x08` | NR10 of square 1, for its frequency sweep |

The wave of the wave voice, 16 bytes, comes from halfword 2 of the instrument,
not of its region.

An envelope is a list of points, each a signed halfword of frames and a signed
halfword of level, from 0 up to `0x7FFF` for full. The level starts at 0 and
moves in a straight line to the level of each point over its frames. A point
with negative frames marks the end: the level stays at the level of the last
point. Each frame the level moves on by the step of the segment: the
difference divided by its frames, rounded towards 0. The level lands on the
level of the point when the segment ends.

A sample has a header of 4 words: its length in points, its rate in Hz, and
the start and the end of its loop, or 0 for no loop. Its 8-bit signed points
follow.

## Note on

A note adds the transpose of the track and looks up the instrument of the
track. A track in legato plays the note on the voice that it already has. That
voice keeps its envelope, its region and its release, and takes the pitch and
sample of the new note. Otherwise the track allocates a voice of the type of
the region. The new voice starts its envelope (see [Voices](#voices)). A muted
track plays nothing.

The pitch of the note comes from the pitch index: the note, plus `0x30`, less
the tuning of the region, kept from 0 to `0x78`. Index `0x78` reads one entry
past the tables. The driver doesn't guard against that. A drum or a sample for
each key plays note `0x30`. The index selects:

* for a sample voice, a step from the pitch table at `0x08199040`, a word for
  each index, `0x800 * 2^(index/12)` (a step of `0x8000` plays the sample at
  its sample rate);
* for a square or the wave, a frequency setting from the table at `0x08198ED8`,
  a halfword for each index, middle C at index `0x30` on a square and C3 on the
  wave, and 0 below index `0x18`;
* for noise, an index into the noise table at `0x08198FC8`, with the values of NR43.

A note of no frames is released at once.

### Slides

`Dx` makes the next note slide between the pitch of the note and the note of
the slide. `Dx` sets the note of the slide, and the transpose of the track
applies to it. After its delay, the slide lasts its length times the frames of
the note, divided by 256. It moves the pitch by a step each frame: the pitch
difference divided by the frames of the slide, rounded towards 0. With `x & 2`
the slide goes from the note to the note of the slide, and without it from the
note of the slide to the note. With `x & 4` the slide goes on for each note
after it, from the note before. Without it, the slide is for one note.

## Voices

Each voice has a record:

| Offset | Field |
|---|---|
| `0x00` | its type |
| `0x01` | 0 free, 1 playing, 2 released |
| `0x04` | its track, while it plays |
| `0x08` | the priority of its note |
| `0x09` | the note, before the transpose |
| `0x0A` | the velocity |
| `0x0C` | the pitch of the note, or the start of the slide |
| `0x10` | the pitch in the frame |
| `0x14` | the level |
| `0x18` | frames before the release |
| `0x1A` | the echo send |
| `0x1B` | 1 for a drum (a drum has a pan of its own) |
| `0x1C` | the pan of the drum, or the pan of the track at the release |
| `0x20` | the LFO's phase |
| `0x24` | the LFO's delay |
| `0x28` | the LFO settings of its track |
| `0x2C`-`0x3C` | the slide: delay, frames, offset so far, offset at the end, step |
| `0x40`-`0x50` | the envelope: level, the end of the segment, its frames, its step, the points, and the next point |
| `0x54` | the region |
| `0x58` | the release |
| `0x5C` | the sample |
| `0x60` | the position in the sample in 256ths of a point, or the frames that a PSG voice has played |
| `0x64` | the duty of a square or the width of noise, or the address of a table of them, or the wave |
| `0x68`, `0x6C` | the links of the lists of sample voices |
| `0x70`, `0x74` | the links of the voices of the track |

The type of a region selects the voices that can play it, through the byte
table at `0x08198EAC`. The table gives each type a separate class. A PSG note
takes the voice of its channel if the voice is free or released, or if the
priority of its note isn't above the priority of the new note. A sample note
takes a free sample voice, or else the first voice in the list of the mixer,
on the same terms. The list of the mixer has the released voices first and
then the ones that play. Each group is kept in order of priority, and among
equal priorities in order of age. A note therefore takes a released voice
first, and then the oldest voice of the lowest priority. A note that finds no
voice isn't played.

The frames of a voice count down each frame that it plays. When they run out,
the voice is released, unless its track is in legato. A released sample voice
fades out by itself. A released square or noise voice passes its release to
the envelope of its channel and is free at once. A voice stops at once, and is free,
when another note takes it, when its sample without a loop ends, or when its
fade reaches 0. When a track ends, `C5` or `C6` comes, or `F8` starts a track
in its place, its notes are released.

The free sample voices form a list too. The init of the driver links them from
voice 0 to voice 6, through the links at `0x68` and `0x6C`. A voice that stops
goes to the front of the list. A new note takes the voice at the front. The
voices that the sounds of a game play before its music can therefore leave the list
in another order (see [Super Mario Advance 4](#super-mario-advance-4)). The
order decides which voice record a note takes. When the frames of two voices
of one priority run out in the same frame, the driver releases them in the
order of their indexes. They are in the list of the mixer in that order. A new
note takes the first of them. Which note it cuts then depends on the order of
the free list.

When a new note takes a sample voice that still sounds, the old note stops at
once: the voice plays the new note from the next mix on. In most such cuts the
voice is already in its release. The MIDI file ends the old note at its
release, and a SoundFont player would play the release on. `agbsf2` therefore
writes All Sound Off (CC120) on the channel of the cut note at the cut, where
the channel is clear:

* Every sample voice that played a note of the track of the cut note in the
  frame before the cut is cut in the same frame too.
* No PSG note of the track sounds across the tick of the cut.

CC120 stops every note of the channel. Where another note of the track still
sounds, the release of the cut note plays on. The CC120 comes at the tick of the cut, or
at the start of an earlier note of the track in the same frame. The MIDI file
puts controllers before the note-ons of a tick. Such a note therefore starts
after the CC120. A cut in the marked pass of a loop keeps its CC120 only where the
next pass has a clear cut on the same track one loop length later, in the
units of the driver. A player that loops on the markers would otherwise stop a
release that the game lets play. A sequence with a note that its instrument
has no region for gets no CC120. The driver reads the sample of such a note
from past the table of the sample set. The hardware then keeps a silent voice
for the note. `agbsf2` ends the note at once, and the cuts can differ.
Sound effects follow the same rules.

## Pitch

Each frame, the pitch of a voice that plays starts from the pitch of its note, and adds:

* the slide's offset;
* the track's bend, if it isn't 0. With `b` the bend and `r` the entry of the
  pitch table for index `0x30` plus the bend's range, the pitch of a sample is
  multiplied by `(|b| * r + 0x400000) / 128 / 0x8000` for a bend up, and divided
  by it for a bend down. A square or the wave works on its period, 2048 less
  the frequency setting, the other way round;
* the LFO, once its delay has run out: a sine of 256 signed bytes at
  `0x08199220`, read at the phase divided by 2. The phase goes up by the rate
  each frame, and wraps at 512. The pitch of a sample goes up by `sine * pitch *
  depth / 2^19` for the positive half, and down by the same share for the
  negative half, as a division. The period of a square or the wave does the
  same with a scale of its own.

## Volume

The level of a sample voice in a frame, while it plays, is the velocity times
the fade and volumes of the player, the volumes of the track and the level of
the envelope, with these shifts:

    level = velocity << 7
    level = level * fade >> 8
    level = level * player volume >> 7
    level = level * player volume 2 >> 8
    level = level * track volume >> 8
    level = level * track volume 2 >> 15
    level = level * envelope >> 11
    volume = level * 3 >> 9

The mixer gets `volume`, and splits it between the sides by the pan:
`(127 - pan) * volume >> 8` on the left and `pan * volume >> 8` on the right,
each kept to 8 bits. After the release, the level is multiplied by
`(release + 230) / 512` each frame, and the voice stops when `volume` reaches
0.

The decay of a SoundFont falls in a straight line in decibels. It can't follow
the straight lines of the envelope in level. `agbsf2` plays each sample at a
constant level until its release. CC11 in the MIDI file follows the envelope
frame by frame.

## The mixer

For each sample voice of its list, the mixer computes the step of the frame
from the pitch and the sample rate:

    step = (pitch >> 2) * rate / 10512 >> 5

The step is in 256ths of a point. A pitch of `0x8000` therefore plays the
sample at its sample rate. The inner loop adds each point, without
interpolation, times the left and right volumes into the mix. At the end of
the loop, the position goes back by the length of the loop. A sample without a
loop stops its voice when the mix reaches its end. Then the mixer releases the
sample voices whose frames have run out.

When the game turns the echo on, the echo is a delay of 18 frames. The voices
with an echo send mix into a buffer. The buffer starts with the echo of 18
frames before, and the mix adds that echo. The driver keeps the buffer for 18
frames, shifted right by the echo level. The game sets the echo through a
request. The sequences only set the sends. Last, the mix is divided by 128,
kept to 8 bits, and written to the output buffers.

Init sets the echo level to 16. That level turns the echo off. While the
level is 15 or less, a sample voice whose track has an echo send mixes into
the echo buffer in place of the mix. In `AZLE`, only the routine at
`0x08129E94` requests the echo. The routine makes request `0x0300` with an
entry of the byte table at `0x08198C4C`: 31, 2, 3 or 0. Nothing in the game
calls that routine. A Link to the Past therefore plays every voice without
the echo.

## PSG voices

Each frame, a PSG voice that plays computes its pitch as a sample voice does.
It also computes the pan of its channel: both sides of NR51 for a pan of
`0x40`, the left for less, and the right for more. A voice on one side counts
twice as loud.

The envelope of the PSG plays each segment of the envelope of the voice. At
the start of each segment, the voice computes the levels at the start and the
end of the segment, from 0 to 15, as the velocity times the volumes and the
level of the envelope. NRx2 gets the start level, with a step of the frames of
the segment plus 14, divided by the difference of the levels, at most 7, up or
down. A segment that doesn't change the level keeps it. In its first frame,
the voice starts its channel at the pitch of the note, with NRx2 and its duty
or width. After that, it writes its pitch each frame, and NRx2 and a restart
when a segment starts. The wave voice leaves the level of the envelope out: at the start of each segment, it turns the velocity
times the volumes into one of its five volumes (none, 25%, 50%, 75% and 100%),
from the table at `0x08198EC0`. It loads its wave into the wave RAM when the
wave changes.

With flag 0 of its region, a square or noise voice has a table of duties or
widths: a halfword count, then a byte for each frame from its first, and the
last byte stays. Without the flag, halfword 2 of the region sets the duty or
width. A square 2 voice with a table starts with a duty from the address of
the table, not from the table. A square 1 voice writes NR10 of its region when
it starts. Between the restarts at the segments of its envelope, it writes its
pitch only if NR10 is 8, without a sweep.

The release of a square or noise voice writes NRx2 with its last level and a
step from the top 3 bits of the release, and restarts the channel. A step
above 0 fades the channel, and a step of 0 turns it off. A released wave voice
fades itself: its level is multiplied by `(release + 230) / 512` each frame,
and it stops at 0. The released wave voice reads the settings of its track at
the start of each segment. It has no track by then. It therefore reads them
from address 0.

## The Super Mario Advance 2 revision

*Super Mario Advance 2: Super Mario World* (US game code `AA2E`) predates *A
Link to the Past* and uses an older revision of the driver. Apart from the
differences below, its code is the code of *A Link to the Past*. The compiler
chose different prologues, registers and literal pools. The cartridge also
has a second copy of the driver in the multiboot image sent to other GBAs for
*Mario Bros.* That copy stores its settings in EWRAM. `agbsf2` reads the
first copy.

In `AA2E` the routines are:

| Address | Routine |
|---|---|
| `0x0809B258` | the sound init of the game: calls the init of the driver with the settings at `0x08177044` |
| `0x0809B9A8` | init: copies the ARM routines from `0x0809DCDC` to `0x03001518` |
| `0x0809B348` | the play-music routine of the game: the low byte of each halfword of the table at `0x08177064` turns the music number of the game into a sequence |
| `0x0809B3CC` | the routine of the game that mutes track 6 of the music (`r0` = 0) or lets it play (`r0` = 1) if the entry of the music in the table has bit 9 set |
| `0x0809D840` | request a sequence: `r0` = player, `r1` = sequence |
| `0x0809D864` | request a sound effect: `r0` = player, `r1` = the sequence of sound effects, `r2` = the track |
| `0x0809D984` | request the mute of tracks of a player: `r0` = player, `r1` = a mask of its tracks, `r2` = the mute |
| `0x0809D82C` | hand over the requests made since its last call |
| `0x0809BA74` | the VBlank routine |
| `0x0809BA80` | the per-frame routine |
| `0x0809DCA8` | handle the requests, through the table of handlers at `0x08177578` |
| `0x0809D50C` | the sequencer |
| `0x0809CF9C` | run a track for a frame, with the command table for `C2` to `FF` at `0x0809D0E8` |
| `0x0809C900` | note on |
| `0x0809BDEC` | look up an instrument |
| `0x0809CDB4` | allocate a voice |
| `0x0809CAD4` | release the note of a voice |
| `0x0809CBB8` | stop a voice |
| `0x0809C66C` | the frame of the PSG voices |
| `0x0809C4A8` | the frame of the mixer |
| `0x0809BC30` | mix the frame of a sample voice |

The music plays on player 0x13. The driver's variables are:

| Address | Variable |
|---|---|
| `0x03000090` | the two output buffers |
| `0x030000A2` | the output buffer the mixer writes this frame |
| `0x030000A8` | the region that instruments with a sample for each key play |
| `0x030000B0` | the wave in the wave RAM |
| `0x030000B4`, `0x0300012C` | the nodes at the ends of the mixer list of playing sample voices (the first voice is the next voice of the first node, at `0x03000120`) |
| `0x030001A4`, `0x0300021C` | the nodes at the ends of the list of free sample voices |
| `0x03000294` | the echo buffer the mixer reads this frame |
| `0x03000295` | the echo delay in frames (18) |
| `0x03000296` | the echo level, 31 at the start |
| `0x03000297` | the echo level that the game requests, 31 at the start |
| `0x03000298` | a routine of the game that plays the notes of a player in place of the driver, when the player has flag 1 |
| `0x0300029C` | a routine of the game that `CA` calls |
| `0x030002A0` | the requests, 12 bytes each |
| `0x030004E4` | the game's settings |
| `0x030007B4` | the mix |
| `0x03000A74` | the mix of the voices with an echo send |
| `0x03000D38` | the 24 tracks |
| `0x03001878` | the 7 sample voices, then the 4 PSG voices |
| `0x03001DA0` | the 20 players, 0x44 bytes each |
| `0x02036000` | the echo history |

The revision differs in these ways:

* **Players.** The record of a player is 0x44 bytes, without the second
  volume. The volume is at `0x40`, the byte that is 1 while the player plays at
  `0x41`, the byte for a sound effect at `0x42`, and the flags at `0x43`.
* **Commands.** `E4` takes a byte, the tempo, not a length. `EA` is one of the
  commands that do nothing and take no operand. The volume of the player
  therefore stays at `0x80` unless a request sets it.
* **Voices.** A voice keeps the velocity of its note at `0x09`, and doesn't
  keep the note.
* **Note on.** The length of the note in 150ths of a tick is kept in 16 bits,
  and so are its frames. A note of 437 ticks or more is therefore shorter than
  its length.
* **Bends.** The bend scales the pitch of a sample, or the period of a square
  or the wave, by a factor in 256ths. The factor goes in a straight line from 1
  at no bend to the step of the range at a bend of 128, up and down alike:
  `f = ((r - 0x8000) * b + 0x400000) >> 14`, with `b` the bend and `r` the
  entry of the pitch table for index `0x30` plus the bend's range. The pitch of
  a sample becomes `pitch * f >> 8`. The setting of a square or the wave
  becomes `2048 - ((2048 - setting) << 8) / f`, without a limit.
* **Volume.** The level of a sample voice leaves out the second volume of the player:

      level = velocity << 8
      level = level * fade >> 7
      level = level * player volume >> 7
      level = level * track volume >> 15
      level = level * track volume 2 >> 7
      level = level * envelope >> 15
      volume = level >> 8

  `volume` therefore reaches 126, not 190. The level of a PSG voice leaves out
  the second volume too. It shifts the product with the volume of the player
  right by 8.
* **The mixer.** The lists of sample voices run between nodes at their ends,
  in the same order as in *A Link to the Past*. The mixer multiplies the step
  of the frame by 176 and divides it by 176 again. That changes the step only
  where the product wraps. It splits the volume of a voice between the sides
  as `(127 - pan) * volume >> 7` and `pan * volume >> 7`, each kept to 9 bits.
  It adds each point times the volume of the side, shifted right by 8, into
  the mix. The mix isn't divided by 128 before it's kept to 8 bits. A
  full-scale sample at the default volumes plays at about 31/128 on each side.
  In *A Link to the Past*, it plays at about 47/128.
* **The echo.** The voices with an echo send always mix into the echo buffer.
  The echo level starts at 31. That level leaves nothing of the mix in the
  history. The shift of the echo rounds. Only the code of the game turns the
  echo on.

The play-music routine of the game mutes a track of sequence 30 with request
`0x0100`, the first handler of the second table: track 8 when it plays the
sequence as music 17, and track 9 as music 18. The request comes after the
request for the sequence. It sets the mute of each track of the player in its
mask. A muted track starts no notes. `agbsf2` converts the sequence once as
music 17 and once as music 18. The files of the second version end in
`_30_music18`.

Unless the player rides Yoshi, the game also mutes track 6 of the music whose
entry in the table has bit 9 set. Music 5, 6, 7 and 23 have the bit. They play
sequences 16, 15, 17 and 18. The routine at `0x0809B3CC` makes the request:
`r0` = 0 mutes the track, and 1 lets it play. The code of the game that starts
the music of a level, from `0x08003D52`, calls the play-music routine and then that
routine. That code passes the byte at offset `0x10FA` of the record that
`0x03003F98` points to. The game sets that byte to 1 when the player mounts a
Yoshi, and clears it when the player loses Yoshi. Each time, the game also
calls the routine. `agbsf2` converts each of the four sequences twice. A new
game starts without Yoshi. The first version therefore mutes track 6. The
second version plays track 6. Its files end in `_yoshi`.

The same code requests the echo in three kinds of level. The low five bits of
the halfword at offset `0xFC` of the record that `0x03002360` points to select
the music of the level from the table at `0x080D4870`. When the low four bits are
6, the code calls the routine at `0x0809B28C` with 1. The argument is 3 for 7,
2 for 8 and 0 for the rest. The routine makes request `0x0300` with that entry
of the byte table at `0x08177060`: 31, 2, 3 or 3. Handler 0 of the fourth table
stores the value at `0x03000297`. The mixer then moves the echo level 1
towards it each frame. The code at `0x080223E8` requests level 31 again. Music
index 6 plays sequence 17 with the echo at level 2. Indices 7 and 8 play
sequences 21 and 22 at level 3. Each 18 frames, the echo repeats the voices
with an echo send. Each repeat is 12 dB quieter than the last at level 2, and
18 dB quieter at level 3.

No track of the music of the game has an echo send. A track of a sequence
starts with a send of 0. The sequences of the game have no `E3` but `E3 00`.
No request sets the send of a track. The echo therefore reaches only the sound effects. Their tracks
start with a send of `7F`. Leaving the echo out changes nothing in the
conversions of the three sequences. `agbsf2` converts each sound effect
without the echo. The game plays the effects that way in the other levels.

Last, the level start code calls `0x0809B42C` with two bits of the state of the
game. For music whose entry in the table has bit 8 set, that routine sets the
tempo change of the music player with request 4: +20 for the first bit and −25 for
the second. `agbsf2` converts the music without a tempo change.

The table of offsets of a bank can leave an instrument out with an offset of
0. A drum kit or a key split can leave a region out the same way. A note on
such an entry reads the table of offsets of the bank as its region. In *Super
Mario Advance 2*, the envelope of that region has a first point of no frames.
That point holds the level at 0 for 65535 frames, and the note never sounds.
The sample of the region is past the end of the table of its sample set. Its
address can be outside the memory of the GBA. `agbsf2` leaves out
every note that its instrument has no region for, with a warning.

## The Super Mario Advance 3 revision

*Super Mario Advance 3: Yoshi's Island* (US game code `A3AE`) came out
between *Super Mario Advance 2* and *A Link to the Past*. Its driver has the
code of *A Link to the Past* with two changes:

* **Volume.** The volume of a sample voice is `level >> 8` rather than
  `level * 3 >> 9`. `volume` therefore reaches 126 rather than 190. A
  full-scale sample at the default volumes plays at about 31/128 on each side.
  That's the level of the Super Mario Advance 2 revision. A released voice
  stops once its level falls below 256. In the revision of A Link to the Past, it
  stops below 171.
* **Init.** Init doesn't clear the output buffers.

In `A3AE` the routines are:

| Address | Routine |
|---|---|
| `0x0812C214` | the sound init of the game: calls the init of the driver with the settings at `0x083077C4` |
| `0x0812CE84` | init: copies the ARM routines from `0x0812F3FC` to `0x03001368` |
| `0x0812C3B4` | the play-music routine of the game: the low byte of each halfword of the table at `0x083077E4` turns the music number of the game into a sequence |
| `0x0812EEBC` | request a sequence: `r0` = player, `r1` = sequence |
| `0x0812EEE0` | request a sound effect: `r0` = player, `r1` = the sequence of sound effects, `r2` = the track |
| `0x0812EEA8` | hand over the requests made since its last call |
| `0x0812CF50` | the VBlank routine |
| `0x0812CF5C` | the per-frame routine |
| `0x0812F3C8` | handle the requests, through the table of handlers at `0x08307D4C` |
| `0x0812EB80` | the sequencer |
| `0x0812E5F8` | run a track for a frame, with the command table for `C2` to `FF` at `0x0812E73C` |
| `0x0812DF58` | note on |
| `0x0812D2D8` | look up an instrument |
| `0x0812E408` | allocate a voice |
| `0x0812E124` | release the note of a voice |
| `0x0812E210` | stop a voice |
| `0x0812DCC4` | the frame of the PSG voices |
| `0x0812DACC` | the frame of the mixer |
| `0x0812D10C` | mix the frame of a sample voice |
| `0x0812D7C4` | compute the level of a sample voice |

The music plays on player 0x13. The variables of the driver are those of A
Link to the Past, 0x98 bytes lower. The tracks are at `0x03000B88`, the sample voices
and then the PSG voices at `0x030016C8`, and the players at `0x03001BF0`.

The play-music routine of the game makes two more requests. With request
`0x0304`, it sets the driver to 6 sample voices for the music whose entry in
its table has bit 10 set, and to 7 for the rest. For music `0x1D` and `0x1E`,
it also sets the second volume of the music player, to `0x37` and to `0x40`.
The requests come in this order: the sample voices, the sequence, then the
second volume. The play request sets the volumes of the player back to `0x80`. A second volume therefore
lasts until the game plays other music.

`agbsf2` converts each sequence with the settings of the first music
number that plays it. The second volume scales the level of every voice,
sample or PSG, by volume / `0x80`, and CC11 in the MIDI file follows it.

The code at `0x0802C2D4` starts the music of each part of a level. The same
code then sets the echo. The halfword at offset `0x29AA` of the record that
`0x03007240` points to selects the music from the table at
`0x0816932C`. For entry 4 of that table, music `0x12`, the code calls the
routine at `0x0812C248` with 1. For entry 2, music `0x13`, the argument is 2.
For the other entries the argument is 0. Eight values of the halfword at
offset `0x2AAC` of the record give 1 whatever the music. Seventeen others give
0. A few parts return before the call and keep the echo as it was. The
routine makes request `0x0300` with that entry of the byte table at
`0x083077E0`: 31, 2 or 3. The code at `0x080FB52A` makes the same choice from
the entry of the music again. The other calls of the routine in the game pass
0.

No track of the music of the game has an echo send. The sequences of the game
have no `E3`. No request sets the send of a track. The echo therefore reaches only the
sample voices of the sound effects. Their tracks start with a send of `7F`.
Music `0x12` and `0x13` play sequences 29 and 27. Leaving the echo out changes
nothing in their conversions. `agbsf2` converts each sound effect without the
echo. The game plays the effects that way in the other levels.

## Super Mario Advance 4

*Super Mario Advance 4: Super Mario Bros. 3* (US game code `AX4E`, revision 1)
has the Super Mario Advance 3 revision. The variables of the driver are `0x70`
bytes higher than in `A3AE`.

In `AX4E` the routines are:

| Address | Routine |
|---|---|
| `0x080E4414` | the sound init of the game: calls the init of the driver at `0x080E4D10` with the settings at `0x08263C8C` |
| `0x080E4598` | the play-music routine of the game: the low byte of each halfword of the table at `0x08263CAC` is the sequence |
| `0x080E4624` | the routine of the game for the tempo change of the music |
| `0x080E4536` | the routine of the game for the second volume of the music |
| `0x080E6D94` | request a sequence: `r0` = player, `r1` = sequence |
| `0x080E6DB8` | request a sound effect: `r0` = player, `r1` = the sequence of sound effects, `r2` = the track |
| `0x080E6D80` | hand over the requests made since its last call |
| `0x080E4DDC` | the VBlank routine |
| `0x080E4DE8` | the per-frame routine |
| `0x080E72A0` | handle the requests |

The game plays its music on player `0x13`. The play-music routine requests the
sequence and sets nothing else. The entries of the table name sequences up to
music number 66. Another table follows.

A level starts its music with no request for the tempo change. For music whose
entry in the table has bit 8 set (numbers 16 to 28 and 50 to 54),
`0x080E4624` sets the tempo change with request 4: +52 for the first bit of
its argument and -25 for the second. The timer code of the level sets the
first bit at `0x0800459E`, when the time reaches 100. It sets the second at
`0x0800457C`, while the byte at `0x03003D10` isn't 0. The start of the level
calls the routine at `0x08014B04` after the play-music routine. That routine
sets the second bit while the same byte isn't 0. Two other bytes of the record
of the level can stop it. Nobody has identified the state that sets the byte at
`0x03003D10`. `agbsf2` converts the music without a tempo change.

The boot of the game plays sound effect 169 at frame 660, through `0x080E6DB8` with
player 5, sequence 0 and track `0xA9`. The effect plays on voices 0 and 1. At
the first request for music, at frame 758, the free list at `0x03000144` runs
voice 1, voice 0, then voices 2 to 6. `agbsf2` starts each song of the game
from that order. From the order of the init, 103 of the 9,805 cuts in the first
60 s of the 87 sequences of the game take the other of two voices released in
the same frame. The other three games start their music from the order of the
init.

## Locating the driver

`agbsf2` finds the init routine of the driver by the start of its code:

    B530 49xx 6008 49xx 2000 7008 2080 7008 3904 4Axx 1C10 8008 3102 200D 7008

A cartridge can contain a second copy of the driver in the multiboot image that
the game sends to other GBAs. The two copies can be different revisions. In
*Super Mario Advance 3*, the image of *Mario Bros.* has the Super Mario
Advance 2 revision, and the game has a later one. `agbsf2` reads the
first copy. Detection searches for the code below only from the init of that
copy up to the init of the next copy.

The settings of the game are the word that the `ldr r0` just before a call to
init loads. The revision is the Super Mario Advance 2 revision if the routine
that computes the level of a sample voice starts with

    B530 1C05 7868 2801 D1xx 7A6C 0224

Otherwise, the revision is the Super Mario Advance 3 revision if the routine ends with

    30E6 6969 4348 0A40 6168 1C04 0A24 1C20

If neither matches, the revision is that of A Link to the Past. The tables of
the driver come from the code that reads them:

| Table | Code | ldr |
|---|---|---|
| pitch table, frequency table | `B500 0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D107 48xx` | halfwords 20 and 30 |
| noise table | `B500 0400 0C01 2977 D900 2177 48xx 1808 7800` | 6 |
| LFO table | `68A0 6883 2B00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809` | 7 |
| wave volumes | `2904 D900 2104 0609 0E09 4Axx 48xx 1809 7808 7010` | 6 |
| voice classes | `49xx 7830 1840 7800 1C29 3152 7809` | 0 |
| the envelope for a sample for each key | `8869 1859 4Axx 0070 1840 8800 8050 6022 48xx 6060` | 8 |

In the Super Mario Advance 2 revision, the code of four of them differs:

| Table | Code | ldr |
|---|---|---|
| pitch table, frequency table | `0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D108 48xx` | halfwords 19 and 30 |
| noise table | `0400 0C01 2977 D900 2177 48xx 1808 7800` | 5 |
| LFO table | `68A0 6882 2A00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809` | 7 |
| voice classes | `49xx 7830 1840 7800 1C29 3152 780A 1C29` | 0 |

The sequence count is the smaller of the entry counts in the sequence table
and the table of bank lists. `--song-table` overrides the detected settings
address, and `--song-count` overrides the sequence count.

The count of the sequences of sound effects is the smaller of the entry counts
in their table and in the table of their bank lists. The table of tracks of a
sequence of sound effects ends where its first track starts.

Detection also looks for the code in the play-music routine of Super Mario
Advance 3 that looks up the entry of the music in the table of the game:

    49xx 0060 1840 8801 7805 2080 00C0 4008 2800 D0xx 20xx Fxxx Fxxx

The `ldr` loads the address of the table. The `movs` and `lsls` that follow
set the bit that selects the voices. The `movs r0` before the `bl` sets the
voices with the bit set. The `beq` goes to a `movs r0` that sets the voices
without the bit, and a `bl` to the same routine. Both ways go on with `adds r1, r5, #0`,
`movs r0, #player` and a `bl` to the request for the sequence. A run of
`cmp r4, #music`, `bne`, `movs r0, #player`, `movs r1, #volume` and a `bl` to
the request for the second volume follows. The table doesn't give its length.
`agbsf2` reads it up to the first entry whose sequence the game doesn't
have.

Where that lookup isn't found, detection looks for the one in the play-music
routine of Super Mario Advance 2:

    48xx 0069 1809 780C 20xx 1C21 Fxxx Fxxx 1C28 Fxxx Fxxx 2Cxx D1xx 21xx 0xx9
    2Dxx D1xx 21xx 0xx9 20xx 2201 Fxxx Fxxx

The `ldr` loads the address of the table. `ldrb r4` reads the sequence. After
the request for the sequence and a `bl` that keeps the music number, the
routine mutes tracks of the sequence that `cmp r4` names. The first `movs r1`
and `lsls` set the mask of the tracks to mute. Where the music number is the
one that `cmp r5` names, the second pair sets the mask instead. The last `bl`
must call the request for the mute. That routine starts with

    B510 0612 0E12 4Bxx 681C 2380 005B 8023 0400 4310 6060 60A1

The first music number that plays a sequence gives the sequence its first
version. A later music number gives the sequence another version if that
number mutes other tracks than each version so far.

Detection then looks for the routine of Super Mario Advance 2 for the track
that plays while the player rides Yoshi:

    B510 0600 0E04 Fxxx Fxxx 0400 1400 2800 DBxx 49xx 0040 1840 8801 20xx 00xx
    4008 2800 D0xx 2200 2C00 D1xx 2201 20xx 21xx Fxxx Fxxx

The first `bl` returns the music number that plays. The `ldr` must load the
table of the play-music routine. The `movs r0` and `lsls` after the `ldrh` set
the bit of the entry. The `movs r0, #player` must name the player of the
play-music routine. The `movs r1` sets the mask of the tracks, and the last
`bl` must call the request for the mute. Each sequence of the music whose entry has the
bit gets two versions. The first has the tracks of the mask muted. The second,
"riding Yoshi", plays them.
