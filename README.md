# agbsf2

`agbsf2` converts the music of Game Boy Advance games that use Konami's,
Rare's, Quintet's, Nintendo R&D2's, Brownie Brown's or Ubisoft Milan's sound
driver, Nintendo's MP2K, or Krawall, to MIDI files and SoundFonts.

MP2K, often called the "Sappy" engine, is the sound driver in most GBA games.
Konami's, Rare's and Quintet's games, *The Legend of Zelda: A Link to the Past*,
*Super Mario Advance 2*, *Sword of Mana*, *Magical Vacation* and *Tomb Raider:
The Prophecy* use drivers of their own, and others play tracker modules with
Krawall, a player of XM and S3M modules, none of which the usual MP2K tools such
as Sappy, gba-mus-ripper and agbplay can read. `agbsf2` finds the game's
driver in a ROM, plays each song through a model of that driver, and converts it
to a Standard MIDI File with a matching SoundFont built from the game's samples
and instruments. For Konami's, Quintet's, Nintendo R&D2's, Brownie Brown's and
Ubisoft Milan's drivers and MP2K, the SoundFonts also include Game Boy PSG
waveforms.

## Supported games

### Konami's driver

This part of the tool was developed on *Yu-Gi-Oh! Ultimate Masters Edition:
World Championship Tournament 2006*, and it supports these games too:

* *Shaman King: Master of Spirits*
* *Shaman King: Master of Spirits 2*
* *Yu-Gi-Oh! Day of the Duelist: World Championship Tournament 2005*
* *Yu-Gi-Oh! GX: Duel Academy*

These games use variants of the *Ultimate Masters Edition* driver.

The following games use older driver revisions with different commands.
These are supported too:

* *Yu-Gi-Oh! World Championship Tournament 2004*
* *Rave Master: Special Attack Force*
* *Yu-Gi-Oh! The Eternal Duelist Soul*
* *Yu-Gi-Oh! Worldwide Edition: Stairway to the Destined Duel*
* *Yu-Gi-Oh! Dungeon Dice Monsters*

The tool reports unrecognised revisions of Konami's driver.

### Rare's driver

* *Donkey Kong Country*
* *Donkey Kong Country 2*
* *Donkey Kong Country 3*, which has a later revision of the driver
* *Banjo-Kazooie: Grunty's Revenge*
* *Banjo-Pilot*
* *Sabre Wulf*
* *It's Mr. Pants*

### Quintet's driver

* *Super Robot Taisen A*
* *Super Robot Taisen D*
* *Super Robot Taisen R*
* *Super Robot Taisen J*, which has a later revision of the driver

### Nintendo R&D2's driver

* *The Legend of Zelda: A Link to the Past*, in *A Link to the Past & Four
  Swords*, whose game selection and *Four Swords* play their music with MP2K
* *Super Mario Advance 2: Super Mario World*, which has an older revision of
  the driver
* *Super Mario Advance 3: Yoshi's Island*

### MP2K

* *Pokémon Emerald*
* the game selection and *Four Swords* in *The Legend of Zelda: A Link to the
  Past & Four Swords*
* *The Legend of Zelda: The Minish Cap*
* *Kingdom Hearts: Chain of Memories*
* *Super Robot Taisen: Original Generation 2*
* *Golden Sun: The Lost Age*, with Camelot's version of the driver

### Brownie Brown's driver

* *Sword of Mana*, whose sound effects are converted with its music
* *Magical Vacation*, with an older revision of the driver, whose sound effects
  are converted with its music too

### Krawall

* *Digimon Racing*, with the build of Krawall dated 2003/09/01

Krawall's later builds changed its data and code, and a game with one of them
gets an error that names its build.

### Ubisoft Milan's driver

* *Tomb Raider: The Prophecy*. Only its sequenced music is converted, not the
  samples that the game plays on their own, outside its sequences.

### Other games

Each driver is located from its own code rather than from fixed addresses, so
other games that use the same driver revisions should work too. Their songs
may still use unsupported commands. An unknown command stops the track and
produces a warning. `--info` names the driver it found and lists the detected
tables and any assumptions made during detection. If detection fails, you can
name the driver and supply table addresses with the override options.

## Building

For 64-bit Windows, download the zip containing `agbsf2.exe` from
GitHub Releases.

agbsf2 supports macOS on Apple Silicon and Windows x64 built with
clang-cl. Other platforms and compilers aren't supported.

You need Git, a C++20 compiler and CMake 3.20 or later. The MIDI and
SoundFont code comes from seqlib. The PSF container reader, the file and
console code and the parsing of numbers come from xsflib. The repository
holds both as Git submodules, in `externals/seqlib` and `externals/xsflib`.
There are no other dependencies.

* **Windows x64:** Visual Studio 2022 with the "Desktop development with C++"
  workload and its "C++ Clang tools for Windows" component. The workload
  includes CMake. Configure with `cmake -S . -B build -A x64 -T ClangCL` in
  place of the first command below.
* **macOS on Apple Silicon:** the Xcode command line tools
  (`xcode-select --install`) and CMake, for example from Homebrew
  (`brew install cmake`).

Clone the repository with `git clone --recursive`. In a clone made without
`--recursive`, run `git submodule update --init` first. Then, in the source
folder:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The binary is `build/agbsf2` (`agbsf2.exe` on Windows). Visual
Studio and Xcode builds put it in `build/Release/` instead. On macOS the build
also makes `build/Agbsf2.app`. You can drop files on the app.

`ctest` runs the unit tests. On macOS, `ctest` also checks the report that
`Agbsf2.app` puts together from the program's output. CI builds
agbsf2 and runs the tests on macOS on Apple Silicon and on Windows x64
with clang-cl.

## Usage

```sh
agbsf2 game.gba
```

This writes one `.mid` and one `.sf2` per song to a folder beside the input
file, with the same name: for example `game/game_00.mid` and
`game/game_00.sf2`. A game with more than one sound driver gets every driver's
songs (see [Options](#options)). Pass several files to convert them together.
Characters that Windows forbids in filenames are replaced with underscores.
Trailing dots and spaces are removed, and reserved device names such as `CON`
get an underscore prefix.

Input can be a raw `.gba` ROM or a GSF rip (`.gsflib`, `.minigsf` or `.gsf`).
A `.minigsf` selects a song from the library named in its `_lib` tag. The
library is loaded from the same folder and supplies the output name. If you
pass several `.minigsf` files from one set, the library is converted or listed
once. If the library has no music that agbsf2 reads, the reason is given
once.
Conversion includes all songs in the ROM, regardless of the GSF's selected song.

Use a ROM if you have one. A GSF rip keeps only the bytes used during playback,
so a song that the rip didn't play may be missing. If every song cuts a
sample short, the unused part is silent in the SoundFont.

### Drag and drop

* **Windows:** drop `.gba`, `.gsflib` or `.minigsf` files on
  `agbsf2.exe`. A console window shows what was converted and stays open
  until you press Enter.
* **macOS:** drop them on `Agbsf2.app`, or open it and choose them. It
  then shows what was converted and offers to open the output folders.

Each file's results go in a folder next to it, as above.

### Options

| Option | Meaning |
|---|---|
| `-o, --output DIR` | output directory (default: a folder next to each input, named like the output files, with `_output` after the name for an input without an extension) |
| `-n, --name NAME` | base name of the output files (default: input name) |
| `-s, --songs LIST` | only these songs, e.g. `0,3,7-9` |
| `-l, --loops N` | play each song's loop N times (default 2) |
| `-t, --tracks LIST` | only these tracks, e.g. `4-15`; in Konami's driver, 0-3 are the PSG channels and 4 and up the sample voices, in Quintet's, 0-3 are the PSG channels and 4 and 5 the PCM channels, in Brownie Brown's, 0-3 are the music's PSG channels, 4-7 the sound effects' and 8 and up the sample channels, in Ubisoft Milan's, 0-2 are the PSG channels and 9 the kit, and in Krawall, the module's channels, 0-19 |
| `--single-sf2` | one SoundFont for all songs (see [Output](#output) for its banks) |
| `--voice-channels` | MP2K only: a MIDI channel for each of the driver's sound channels instead of each track, so that notes stop where the game cuts them off, though a track's notes then move between channels (see [MIDI from MP2K](#midi-from-mp2k)) |
| `--frame-timing` | put each event on the frame the game plays it in, rather than on the song's beat (see [Output](#output)) |
| `--dump` | also write a text listing of every command of each song (`NAME_NN.txt`) |
| `--info` | print each driver found, with its tables and a list of its songs, then exit. The track count, length and loop reflect the current conversion options |
| `--driver NAME` | use only `konami`, `rare`, `quintet`, `rd2`, `brownie`, `ubimilan`, `krawall` or `mp2k`, instead of every driver detection finds |
| `--song-table ADDR`, `--song-count N` | override detection: the song table's address (hex), or for Nintendo R&D2's driver the address of the game's settings for it, for Ubisoft Milan's driver the sound bank, and for Krawall a table of the game's modules, and the number of songs |
| `--sample-table ADDR`, `--mix-rate HZ` | override detection in Konami's driver: the sample table's address (hex) and the mixer's rate |
| `-q, --quiet` | only print warnings and errors |
| `--trace SONG`, `--trace-frames N` | print the driver model's state after each frame (for the driver's `compare_trace.py` in `tools/`) |

Detection checks for Rare's driver first, followed by Quintet's, Nintendo
R&D2's, MP2K, Brownie Brown's, Krawall and Ubisoft Milan's. These are identified
from their code alone. Konami's is checked last.
If a game has several drivers, each driver's songs are converted. The first
driver uses the usual file names; the others add the driver's name after the
base name, as in `game_mp2k_000.mid`. Messages also include the driver's
name before the song number. `--songs` and `--tracks` apply to every driver.
`--driver` selects one driver and uses the usual file names.
A driver whose tables can't be read is left out with a warning, and the others
are converted.

`--trace` uses the first driver found. Without `--driver`, `--song-table`
and `--song-count` also select the first driver and convert only its songs.
Supply `--driver` as well if the game's driver isn't recognised.
`--sample-table` and `--mix-rate` imply `--driver konami`.

To listen, load the pair into any SoundFont player, for example:

```sh
fluidsynth -ni -F song10.wav game/game_10.sf2 game/game_10.mid
```

### Known issues

* `--info` lists each song's length and loop without converting the song. A
  song that fails to convert still gets a length.
* On Windows, the console window waits for Enter even when the output is
  redirected, the console's code page isn't restored afterwards, and redirected
  output and error messages can come out in a different order.

## Output

Songs that loop are written with the loop played twice by default. The loop is
also marked with `loopStart` and `loopEnd` marker events, which loop-aware
players honour.

A song's tracks can loop from different points and at different lengths. The
loop starts where every track is looping and every track without a loop has
finished, and it lasts until every track is back where its loop started: 12
bars for tracks that loop 3 and 4 bars, for example, so that it repeats
seamlessly. If that would be more than 8 times as long as the longest
track's loop, the loop is the longest track's, and the other tracks fall out
of step each time a player repeats it. Konami's driver sends every track
back to its loop point at once. Each of the driver's songs loops from the
first of those points. Points far before the looping track's loop point don't
count, and neither do those of tracks that end for good. A track keeps
settings such as its volume, pan or octave when
it loops, and a song keeps its tempo. If the intro leaves different ones
from the end of the loop, the first pass can sound different from later
passes. With Konami's, Quintet's, Nintendo R&D2's, Brownie Brown's and Ubisoft
Milan's drivers and Krawall, and with MP2K's tempo, the marked loop then
starts on the second pass, and the file plays the first pass once before it.
Quintet's driver also discards fractional frames when a track loops. If this
would make a looping player play a note at the loop's start twice, the marked
loop moves to the second pass as well. A player that silences its notes when it
repeats the loop also cuts short any note held across the loop's start.

When a loop sets a track's program, volume, pan, expression, reverb or pitch
bend, the MIDI file writes that setting even if its value hasn't changed.
Otherwise, a player jumping back to `loopStart` would retain the value from
the end of the loop. Konami conversions restore all these settings at each
track's first event in the loop. Konami's, Brownie Brown's and Ubisoft Milan's
conversions also give the tempo again at `loopStart`. The beat they work out can
have another tempo at the loop's end, and a player that keeps its tempo when it
jumps back would play the loop's start at that one.

Each event goes on the song's beat, as in a quantised MIDI file, so that the
notes line up with the bars and beats in an editor. The game plays each event
at the start of a frame, so an event on the beat can be up to a frame from
where the game plays it, or a frame and a half with Konami's, Brownie Brown's
and Ubisoft Milan's drivers. `--frame-timing` puts each event on the frame the
game plays it in instead, as closely as the MIDI file's ticks allow, at the same
tempos. Krawall plays each tick at the sample where it falls, rather than at the
start of a frame, so its events are on the module's ticks either way.

Konami's, Brownie Brown's and Ubisoft Milan's drivers store note lengths in
whole frames, rounded by the composers' tools. A 16th note that should last 6.5
frames therefore lasts 6 or 7, and the original beat isn't stored.
`agbsf2` estimates the beat from the note lengths and finds steady passages
and tempo changes. For ritardandos and rubatos, it sets a tempo for each note.
Notes within a frame and a half of the nearest beat are moved onto it, using
divisions from a quarter note down to a 32nd note or a 16th-note triplet. Echo
channels keep their delay, as do the quick notes of an arpeggiated chord. Songs
too short to establish a beat, such as most sound effects, use a quarter note of
30 frames. At possible tempo changes, such as loops, the quarter-note length is
adjusted so that the preceding stretch ends on the beat. The next stretch reuses
the quarter-note length of an earlier stretch with the same beat, if there is
one. Otherwise it starts with the adjusted length.

### MIDI from Konami's driver

Each MIDI file has a conductor track and one track per game track that plays
notes. Tracks are named `Square 1`, `Square 2`, `Wave`, `Noise`, and
`Voice 0`-`Voice 11` (game tracks 4-15, which play the driver's voices 0-11).
The older Rave Master, Eternal Duelist and Dungeon Dice Monsters revisions have
8, 6 and 4 voices. MIDI channel 10 is used only if a song needs all 16
channels.

The game counts time in frames and doesn't store a tempo. The MIDI file has
480 ticks per quarter note, and the tempos of the beat that `agbsf2`
works out from the notes (see [Output](#output)).

The game's controls map to MIDI like this:

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume and pan | CC11 (expression) for loudness, CC10 for pan (see below) |
| pitch bend, vibrato | pitch bend, with the bend range set per channel by RPN 0 |
| echo | CC91 (reverb send) on the voices routed to it |
| instrument changes | program changes |

In the game, volume can change during a note. CC11 is used instead of velocity
so that such changes carry over.

CC11 and CC10 together reproduce the game's per-side levels under the
constant-power pan law that most SoundFont players use.

In the Eternal Duelist revision, a PSG note at the current volume changes the
channel's frequency without restarting it. The MIDI file represents this as a
pitch bend of the current note. A series of these notes can span more than
24 semitones; the bend range is adjusted to fit.

In the Dungeon Dice Monsters revision, songs set separate left and right PSG
volumes. These are included in each PSG track's CC10 and CC11 values.

### SoundFont from Konami's driver

Each song's SoundFont contains exactly the instruments that song uses.

* **Sample tracks** get one preset per track (sometimes more, for samples
  whose keys would collide). The preset maps MIDI keys to the samples the
  track plays:
  * Samples played at several pitches are placed at their musical pitch, which
    is estimated from the waveform, so melodies read as the right notes.
  * Samples played at a single pitch, typically drums, get a key of their own.
* **Square channels** get one preset per duty cycle.
* **Wave channel** gets one preset per wave and volume. The driver stores a
  scaled copy of each wave for every volume. Quiet notes have coarser waveforms,
  which the presets preserve. The Dungeon Dice Monsters revision keeps each
  wave at full scale and sets the volume in the hardware, so its songs get one
  preset per wave.
* **Noise channel** gets one preset with a key per noise note, synthesised from
  the Game Boy's LFSR, and more presets if a song plays more noise notes than
  there are keys.

Samples keep the game's rate and loop points.

With `--single-sf2`, each song's instruments are in the bank numbered after the
song.
MIDI selects banks 0 to 127. So the instruments of a song numbered 128 or more,
and a song's instruments past the 128th, take free programs in the other banks.

### MIDI from Rare's driver

The tune format resembles MIDI: note, controller, program and pitch bend
commands on 16 MIDI channels, separated by delays in ticks. The MIDI file
keeps the tune's ticks per quarter note, channels, keys, velocities, programs
and volumes (controller 7). Each source track that plays notes gets a MIDI
track named `Track 0`, `Track 1` and so on. A separate first track holds the
tempo and loop markers.

The driver counts each frame as 1/60 s, but the GBA shows 59.73 frames a
second, so the games play every tune 0.46% slower than its tempo says. The
MIDI file's tempos are slowed down to match.

The MIDI file follows what the driver plays, where that differs from the
tune's commands:

* Each channel has a limited number of note slots. The driver drops a new
  note if all of its channel's slots are playing. The MIDI file leaves such
  notes out, and ends each note where the driver's slot stops playing it, such
  as when a note in mono mode (controller 126) cuts off the one before.
* The driver's mixer plays at most 8 voices in each frame. It leaves the
  other notes out of that frame. Where it leaves out every note of a key on a
  channel, the MIDI file mutes the key there with polyphonic key pressure
  (aftertouch) of 127. A key pressure of 0 unmutes the key when the mixer takes
  one of its notes again. The SoundFont turns a key pressure of 64 or more into
  96 dB of attenuation. A player that applies the SoundFont's modulators, such
  as FluidSynth, then silences the muted notes. A player that ignores key
  pressure or the modulator plays every note. Where the mixer takes one note of
  a key and leaves out another on the same channel, the MIDI file plays both.
  In *Donkey Kong Country 3*, the mixer also leaves out the notes of a channel
  at volume 0. Those notes don't count towards the limit.
* A note off releases only one note with that key on the channel. Since a
  MIDI channel can't play the same key twice at once, the conversion ends
  the older note when the newer one starts.
* The driver's vibrato (controller 1) is written out as pitch bends, a frame
  at a time, together with the tune's bends. Each channel's bend range is
  set with RPN 0 to fit the bends and the vibrato.
* Notes that share a tick with a program change use the same program as in
  the driver.

### SoundFont from Rare's driver

Each tune's SoundFont has a preset for each program the tune plays, named
`Program N` and with the same number, in bank 0. Programs played on channel 10,
which General MIDI players keep for drums, are in bank 128 as well, so that
such players play the same instruments there.

In *Donkey Kong Country 3*, tunes can override the attack, decay, sustain
and release of a channel's instruments until its next program change. A note
that plays with such settings gets a preset with the same program number in
a separate bank, one for each set of settings, from bank 1 up. The MIDI file
selects that bank before the note. Each envelope phase uses the channel's
settings at the start of that phase, as in the driver. Changes therefore
affect the later phases of notes already playing.
A note still playing at the end of the file takes the release that its
channel's settings give there. That's the release the driver would give it.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their
  rate, root key, fine tune and loop. A very short loop is repeated until it's
  at least 32 points long, since some players can't play shorter ones.
* **Drum kits and key splits** become a zone for each range of keys that plays
  the same instrument. A drum kit's zones play their samples at their own
  pitch on every key, as the driver does.
* **Envelopes** follow the driver's attack, decay, sustain and release. The
  driver's fades are straight lines in level, where a SoundFont's are straight
  lines in decibels, so decays and releases are stretched to keep the
  loudness close.
* **Volume and velocity** scale the level in a straight line, as in the
  driver, through modulators in each instrument. The SoundFont default would
  square them.
* **Mutes.** Where the MIDI file mutes notes that the mixer leaves out, each
  instrument has a modulator that adds 96 dB of attenuation while the note's
  key has a key pressure of 64 or more. That takes a 16-bit sample below its
  smallest step.

With `--single-sf2`, the tunes that use the first tune's instruments use bank
0, and each other set of instruments gets a bank of its own, which the tunes
select with a bank change at the start. The banks for envelope settings come
after those.
Each set's programs on channel 10 are in bank 128 as well. Where two sets have
the same program there, bank 128 keeps the first one, and the later tune gives
a warning.

### MIDI from Quintet's driver

Each MIDI file has a conductor track with the tempo and loop markers, and a
track for each of the song's channels that plays notes: `Square 1`,
`Square 2`, `Wave`, `Noise`, `PCM A` and `PCM B`, on MIDI channels 1-6.

The song's tempo comes from its first channel. The MIDI file has 3552 ticks
per quarter note, or 37 per song tick. At tempo T, a driver frame lasts
exactly T MIDI ticks. The driver plays each note at the start of the frame
that its tick falls in, and the MIDI file keeps each note on its own tick,
within that frame. When a channel loops or the tempo changes, the
driver drops the part of a frame it has counted, and the MIDI file's notes
follow it from there.

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume, pan and the master volume | CC11 for loudness and CC10 for pan, as with Konami's driver |
| detune, slides, the pitch envelope, the LFO and the frequency sweep | pitch bend, a frame at a time, with the bend range set per channel by RPN 0 |
| duty and wave bank switches | instruments that switch samples during each note |

Square and wave notes are at the key of their pitch: the frequency table
starts at C2, which is key 36. The bends hold the difference between the pitch
the channel plays and its table entry, so the keys stay equal-tempered (see
[Accuracy](#accuracy)). The frequency sweep changes square 1's pitch within a
frame, so its steps become extra bends between the frame boundaries. PCM notes
play the sample at its own rate on key 60, and each step of the driver's PCM
pitch table is a key. Noise notes are drums, with a key for each different sound.

### SoundFont from Quintet's driver

Each song's SoundFont holds the instruments its notes play:

* **Square channels** get a sample for each duty, and the wave channel a
  sample for each wave pattern of 32 or 64 steps. An instrument that switches
  its duty or wave bank during a note switches samples at the same point.
* **Noise drums** are rendered from the driver's writes to the noise channel,
  including the note's macro and volume changes, using the Game Boy's LFSR,
  envelope and length counter. A drum that ends on a steady sound loops its
  last two frames, which is sooner than the hardware's noise repeats, so a long
  held drum can pick up a 30 Hz buzz.
* **PCM samples** are the game's 8-bit samples, widened to 16 bits. Each plays
  as the driver plays it: from the note's start until the driver stops it or
  goes back to its loop, and then the frames of each pass through the loop. The
  driver checks a sample once a frame, so the lengths depend on the rate a note
  plays at, and a sample that plays at several rates gets a copy for each.

The levels follow the GBA's mixer: a square at full volume is 30/128 as loud as
a full-scale PCM sample. With `--single-sf2`, each song's instruments are in
the bank numbered after the song.
MIDI selects banks 0 to 127. So the instruments of a song numbered 128 or more,
and a song's instruments past the 128th, take free programs in the other banks.

### MIDI from MP2K

Each of the song's tracks that plays notes gets a MIDI track, named `Track 0`,
`Track 1` and so on, on the MIDI channel with its number. A separate first
track holds the tempo and loop markers. The MIDI file has 960 ticks per
quarter note, 40 for each of the driver's 24. A player such as FluidSynth
rounds its position to a whole tick at each tempo change, and at 24 ticks it
rushed songs that change the tempo often. The file keeps each note's key and
velocity. As with Rare's driver, the tempos are slowed down by 0.46% to match
the GBA's frame rate.

| Game | MIDI |
|---|---|
| note | note on/off, with the note's velocity |
| `VOICE` | program change, with the voice's number |
| `VOL`, `PAN` | CC7 (volume), CC10 (pan) |
| `BEND`, `BENDR`, `TUNE`, `KEYSH`, vibrato | pitch bend, with the bend range set per channel by RPN 0 |
| the song's reverb | CC91 (reverb send) |

In Camelot's version of the driver, in *Golden Sun: The Lost Age*, the mixer
has an echo in place of the reverb, the same in every song, so every track gets
a reverb send of 53, the driver's reverb that comes closest to it.

The MIDI file follows what the driver plays, where that differs from the
song's commands:

* The driver plays a note on one of a few DirectSound channels, or on the
  PSG channel its voice uses, and drops a note that finds none it can take.
  The MIDI file leaves such notes out, and ends a note where a later note takes
  its channel.
* The MIDI file leaves out notes that the driver releases before they start,
  since they never sound. It also leaves out PSG notes whose volume rounds to
  silence on the PSG's 16 levels.
* Where a song changes the tempo, the driver has already counted part of the
  next tick at the old tempo, and the MIDI file's tempo for that tick does the
  same.
* The driver's LFO, when it modulates the pitch, is written out as pitch bends,
  a tick at a time. When it modulates the volume or the pan, it's written into
  CC7 or CC10.
* The key shift is part of the pitch bend, so that key splits and drum kits
  pick the same voices as in the game.
* While every note a track plays is a PSG note, its pitch bends follow what the
  driver plays: a noise note steps through the driver's noise settings, which
  move about 3 semitones a key, and a square or wave note doesn't go below C2.

When the driver gives a channel to a new note, it stops the note that was
playing there at once, even partway through its release, but a SoundFont player
lets that note fade out, so busy passages can sound fuller than in the game.
`--voice-channels` fixes that, at the cost of files that are harder to edit. It
gives each of the driver's sound channels that plays notes a MIDI channel and a
track, named `DirectSound 1` to `DirectSound 12`, `Square 1`, `Square 2`, `Wave`
and `Noise`, and leaves channel 10, which players keep for drums, until last.
Each note plays on the channel the driver gives it, and an All Sound Off (CC120)
stops its sound where the driver's channel stops. So a track's notes move
between channels, and each channel's program, volume, pan and pitch bend follow
the track whose note it plays; `--tracks` still picks the song's tracks.

### SoundFont from MP2K

Each song's SoundFont has a preset for each voice the song plays, named
`Program N` and with the voice's number, in bank 0, and in bank 128 too for
those played on channel 10.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their
  rate and loop. A compressed sample is decoded, and a reversed one is stored
  backwards.
* **Fixed-pitch samples**, which the driver plays at its mixer's rate whatever
  the key, play at that rate on every key, and the pitch bend doesn't move them.
* **Synth voices**, in Camelot's version of the driver, play waves that its
  mixer makes from a sample's data: a pulse wave whose duty moves up and down
  each frame, a saw wave through a filter, and a triangle wave. A pulse or saw
  voice gets a sample for each key the song plays it at, made at the mixer's
  rate and looped over the duty's cycle, and a triangle voice one cycle that
  plays at every key.
* **PSG voices** play the Game Boy's square waves, the voice's 32-point wave
  pattern, or noise from the Game Boy's LFSR. Noise gets a zone for each of the
  driver's noise settings, at its rate, though settings below 32 Hz play at
  about 32 Hz, the lowest a zone can be tuned to. The PSG voices are 6.3 dB
  quieter than a sample at its full level, as on the GBA.
* **Key splits and drum kits** become a zone for each range of keys that plays
  the same voice. A drum kit's zones play at their voices' keys and pans.
* **Envelopes** follow the voice's attack, decay, sustain and release. A
  sample's decay and release fall by a fraction each frame, which is a straight
  line in decibels, as in a SoundFont. The PSG's fades, and the releases of
  Camelot's mixer, are straight lines in level, and they're stretched to keep
  the loudness close.
* **Volume and velocity** scale the level in a straight line, as in the driver,
  and the driver's master volume scales the samples. Camelot's mixer leaves the
  master volume out, and plays the samples a little louder against the PSG.
  The samples themselves hold these levels and the PSG's. A zone's attenuation
  would hold them in other players, but FluidSynth applies only 0.4 of it.

With `--single-sf2`, each voice keeps its number as its program, and banks tell
apart the voices that songs play under the same number: the first one is in
bank 0, the next in bank 1, and so on. Each program change in the MIDI files
comes with a bank change. Track 9 would play on channel 10, where players look
for programs in bank 128, so it moves to the first free channel after 10, or
else the last free one before it. A song that plays on all 16 channels keeps
track 9 on channel 10, with copies of its voices in bank 128, and a warning if
another song has one of those programs there already. If the songs played more
than 128 voices under one number, the rest would take free program numbers.

### MIDI from Nintendo R&D2's driver

Each of the sequence's tracks that plays notes gets a MIDI track, named
`Track 0`, `Track 1` and so on. Tracks 0 to 8 are on MIDI channels 1 to 9, and
track 9 is on channel 11, away from the drum channel. A separate first track
holds the tempo and loop markers.

The driver counts time in 150ths of a tick, with 24 ticks to the quarter note,
and each frame takes the tempo off the count. The MIDI file has 3600 ticks per
quarter note, one for each 150th, so a frame at the driver's tempo T is exactly
T ticks long, and each note stays on its own tick.

| Game | MIDI |
|---|---|
| note | note on/off; a velocity of v is written as 127 × √(v/127), since a SoundFont player's level goes with the square of the velocity |
| `C2` instrument, `C7` bank | program change, numbering the sequence's instruments in the order it first plays them |
| `C3` pan, `E0` volume, `EA` the player's volume | CC10 for pan and CC11 for loudness, as with Konami's driver |
| slides, `E1` bend, the LFO | pitch bend, a frame at a time, with the bend range set per channel by RPN 0 |

The MIDI file follows what the driver plays:

* The driver has 7 voices for samples and one for each PSG channel, and a note
  only takes a voice that its priority allows. The MIDI file leaves out the
  notes the driver can't play, and ends a note where another takes its voice.
* A note ends when the driver releases it. Its length in frames depends on the
  tempo when it starts. The driver also releases it when its track starts or
  ends legato, ends, or restarts.
* A track's pitch bend follows its newest note.

### SoundFont from Nintendo R&D2's driver

Each sequence's SoundFont has a preset for each instrument the sequence plays,
named after its bank and number.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their rate
  and loop, tuned to play at their rate on the note their region gives.
* **Drum kits and key splits** become a zone for each key or range of keys that
  plays the same region. A drum plays at its region's pitch and pan, and so
  does an instrument with a sample for each key.
  As in the driver, the track's pan doesn't move a drum.
* **PSG voices** play the Game Boy's square waves, the voice's 32-step wave, or
  noise from the Game Boy's LFSR, with a zone for each key's noise setting.
  Their level relative to the samples matches the driver's mix. As with MP2K,
  their samples hold that level, rather than their zones' attenuation.
  The driver steps some voices through a table of duties or noise widths, a
  frame each, and holds the last entry. Their zones play that entry.
* **Envelopes.** The driver's envelopes are straight lines in level between
  points, and the SoundFont's attack, hold, decay and sustain approximate them.
  A sample's release reduces its level by a fraction each frame, which is a
  straight line in decibels, as in a SoundFont.
  The driver leaves out the wave voice's envelope. The voice holds its level
  until its release, and the release fades the voice like a sample.

With `--single-sf2`, each sequence's instruments are in the bank numbered after
the sequence.
MIDI selects banks 0 to 127. So the instruments of a sequence numbered 128 or
more, and a sequence's instruments past the 128th, take free programs in the
other banks.

### MIDI from Brownie Brown's driver

The song table lists every sound the game plays, music and sound effects alike,
and each one is converted as a song. Each MIDI file has a conductor track with
the tempo and loop markers, and a track for each channel that plays notes:
`Square 1`, `Square 2`, `Wave` and `Noise` for the music's PSG channels,
`Effect square 1` to `Effect noise` for the sound effects', and `Samples 1` to
`Samples 5`, or in *Magical Vacation*'s revision of the driver `Samples 1`,
`Samples 2`, `Effect samples 1` and `Effect samples 2`, on MIDI channels 1-9 and
11-14, away from the drum channel.

The driver counts time in frames, and has no tempo: a song picks a set of note
lengths, each for a tempo. The MIDI file has 480 ticks per quarter note, and
the tempos of the beat that `agbsf2` works out from the notes (see
[Output](#output)). It starts on the song's first beat, in the second frame,
when the driver first reads the song's data.

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume, envelopes, the hardware's envelope, pans, the master level and fades | CC11 for loudness and CC10 for pan, a frame at a time, as with Konami's driver |
| detune and the frequency sweep | pitch bend, with the bend range set per channel by RPN 0 |

Each note uses the MIDI key nearest the driver's pitch. In *Sword of Mana*,
the driver's key 0 is 33 cents below C#2, so it becomes MIDI key 37, with
the SoundFont's square and wave samples tuned down 33 cents. In *Magical
Vacation*, key 0 is C2, MIDI key 36. Pitch bends carry the difference
between the channel's pitch and its key's frequency-table entry. Sample
notes use the same keys. Noise notes are drums, with a key for each distinct
sound and 92 sounds per kit. Notes last as long as their channel sounds: a
rest that turns off or mutes a PSG channel, silences a voice or stops a FIFO
ends the note.

### SoundFont from Brownie Brown's driver

Each song's SoundFont holds the instruments its notes play:

* **Square channels** get a sample for each duty, and the wave channel a sample
  for each wave pattern, tuned as the driver's frequency table is.
* **Noise drums** are rendered from the driver's writes to the noise channel,
  including each step of the note's envelope, which starts the channel again,
  using the Game Boy's LFSR, envelope and length counter at 32768 Hz. As with
  Quintet's driver, a drum that ends on a steady sound loops its last two
  frames.
* **Sample sets** get an instrument each, with a zone for each key the song
  plays: the semitone's sample, one of the game's 8-bit samples widened to 16
  bits, with its loop, played at the octave's rate, from an eighth of the
  mixer's 16384 Hz up to the full rate. In *Magical Vacation*'s revision, each
  sample gets an instrument, with a zone for each key the song plays, tuned to
  the rate that the driver's table gives the key, from 1024 Hz at its key 0 up,
  and a detune is a pitch bend.

The levels follow the GBA's mixer: a square at full volume is 30/128 as loud as
a full-scale sample. With `--single-sf2`, the instruments of all the songs are
numbered together, in the order the songs first play them, with 128 to a bank.

### MIDI from Krawall

Each module becomes a song. Modules are numbered in the order of the game's
module table, followed by any modules absent from that table. A module whose
orders separate songs with `+++` becomes a song for each of them, numbered in
turn. Each of those songs loops back to its start at its end, as in Krawall's
song mode. Each MIDI file
has a conductor track for tempo and loop markers, plus a track for every
module channel that plays notes. Tracks are named `Channel 1`, `Channel 2`
and so on, using MIDI channels 1-9 and 11-16 to avoid the drum channel.
Modules with more than 15 channels reuse MIDI channels from the 16th module
channel onwards.

A module defines tick lengths and ticks per row, but has no explicit beat. The
converter chooses 1, 2, 4, 8 or more rows per quarter note, or per eighth note
in 12/8, to put the beat's tempo between 105 and 210 BPM. It uses 4/4 unless
more of the patterns fit whole bars of 12/8 than of 4/4. For example, a 48-row
pattern fits two bars of 12/8 at two rows per eighth note, with a dotted
quarter note as the beat.

Bars follow pattern boundaries. A pattern cut short ends with a shorter bar.
If a break cuts the first order short, as with a lead-in, its partial bar goes
at the start as a pickup, so the next pattern starts on a full bar.

At the module's most common speed, or that speed doubled or halved any
number of times, each player tick takes the same number of MIDI ticks.
This lets a module halve its speed for finer rows without changing the
MIDI tempo. At other speeds, such as a slow lead-in, a row takes the usual
number of MIDI ticks, with a tempo change to preserve its playback duration.
Playback starts on the module's first tick. A loop is detected when a row
returns to an earlier position with the same speed, tempo and pattern-loop
state, for example at a jump or the end of the order list.

| Game | MIDI |
|---|---|
| a sample that a note, a note delay, a retrig or an offset starts | note on/off; velocity is always 127 |
| the sample, and the point that the note starts from | program change: a preset for each |
| volume, channel volume, global volume, volume and pan envelopes, slides, tremolo, tremor, pan and panbrello | CC11 for loudness and CC10 for pan, a tick at a time, as with Konami's driver |
| portamentos, the slide to a note, vibratos and arpeggios | pitch bend, with the bend range set per channel by RPN 0 |

Each note uses the MIDI key nearest its playback rate, with the sample's C-4
on key 60. Pitch differences of 3 cents or more are corrected with a bend.
This threshold exceeds the period table's deviation from equal temperament
in the octaves used by modules. The note lasts until its sample ends, the
player stops it, or the module channel starts another note.

### SoundFont from Krawall

Each module's SoundFont has a preset for each sample and point that its notes
start from, named after the sample's number in the game's table of samples.
The samples are the game's 8-bit samples, widened to 16 bits, with their loops,
at the rate at which the player plays their C-4, on key 60. A loop back and
forth plays its points forwards and then backwards. A note that starts within
its sample, with an offset, plays a zone that starts there. With
`--single-sf2`, the presets of all the modules are numbered together, in the
order the modules first play them, with 128 to a bank.

### MIDI from Ubisoft Milan's driver

Each piece of music in the game's sound bank is converted as a song, in the
order of the bank's resources. A piece plays a list of sequences one after
another, and a piece that loops starts its list again. Each MIDI file has a
conductor track with the tempo and loop markers, and a track for each channel
that plays notes: `Square 1`, `Square 2` and `Wave` for sequence channels 0-2,
on MIDI channels 1-3, and `Kit` for channel 9, on MIDI channel 11. Players treat
notes on MIDI channel 10 as drums. Some ignore a drum's note-off or its
sample's loop, and the kit's notes require both.

The driver counts time in frames, and has no tempo. The MIDI file has 480
ticks per quarter note, and the tempos of the beat that `agbsf2` works
out from the notes (see [Output](#output)). It starts with the piece, whatever
silence comes before its first note.

| Game | MIDI |
|---|---|
| note on channel 9 | note on MIDI channel 11: its key plays one of the kit's samples; a velocity of v is written as 127 × v / 64 |
| note on channels 0-2 | note at the square's or the wave's pitch, with the starting volume as its velocity |
| program on channel 9 | bank select 9 and program change: an offset into the kit. Key k plays the kit's entry k + offset |
| program on channels 0-2 | bank select of the channel's number and program change: the channel's instrument |

Channel 9's keys up to 11 play the PSG's noise, and its keys above that the
kit's samples. The mixer plays a sample at a fixed rate. A key chooses a
sample rather than a pitch, and a melody has a sample for each of its notes.
A program up to 11 on channel 9 goes to the PSG and changes nothing. Each of
channel 9's notes takes a free voice, and a note-off stops the voice of its
last note, whatever its key. The MIDI file ends that note there, and another
note sounding then plays until its sample ends. Notes that the driver plays at
volume 0 make no sound, and the MIDI file leaves them out.

### SoundFont from Ubisoft Milan's driver

Each song's SoundFont holds the instruments its notes play:

* **The kit** gets a preset in bank 9 for each offset that the song uses. Each
  preset has a zone for each key that the song plays. Each zone plays its key's
  sample unpitched, at the rate at which the mixer moves through it: 16352 Hz.
  The SoundFont widens the game's 8-bit points to 16 bits. The mixer moves
  through a sample 8 points at a time, and a loop lasts a whole number of 8
  points. As in the game, a note stops at once at its note-off.
* **Noise keys** of the kit play the PSG's noise at the clock that the
  driver's table gives the key.
* **The PSG's instruments** get a preset in the bank numbered after their
  channel: a square with the instrument's duty, or the wave program's 64
  points, tuned as the driver's frequency table is.

As in the driver, velocity scales the level in a straight line. The driver
sets the PSG's master volume to 3 of 7. A PSG channel at full volume is
therefore 60/512 as loud as a full-scale sample. With `--single-sf2`, the kit's
presets and the PSG's instruments of all the songs go in the one SoundFont.

### Sequence listing (`--dump`)

The listing shows every track command with its address, raw bytes and
meaning. For Konami's driver, it includes the frame and the delay after the
command, and follows each call; for Rare's, it includes the tick. For
Quintet's and Nintendo R&D2's
drivers and MP2K, it follows each track through its repeats, calls and
patterns, to the end of the track or its first loop. Each command is listed
with its tick. Brownie Brown's driver is listed the same way, using frames.
For Krawall, it lists the module's header and orders, and each pattern's rows,
with each channel's note, instrument, volume column and effect. For Ubisoft
Milan's driver, it lists each sequence of the piece, with each command's frame.
Nintendo R&D2's listing also follows each track that `F8` starts.
Use the listing to study a song or check the format documentation against real
data.

## Accuracy

Some things differ from the hardware, or can't be expressed in MIDI and
SoundFonts.

### Konami's driver

* **Timing.** The MIDI file puts each note on the beat that `agbsf2` works out.
  A note can be up to a frame and a half from where the game plays it.
  `--frame-timing` keeps each note on its frame instead. Where a song has no
  steady beat, its notes keep their frames.
* **PSG tuning.** The Game Boy's 11-bit frequency registers can't hit every
  pitch, so the game plays its high PSG notes slightly out of tune. The
  driver's frequency table is within about 3 cents of equal temperament below
  C4, but up to about 7 cents off in the C4 octave, 13 in the C5 octave, 25
  in the C6 octave and 50 in the C7 octave. The wave channel plays an octave
  lower than the squares from the same table. The MIDI files keep the
  equal-tempered pitch.
* **Echo.** The driver's echo is a feedback delay (up to about 190 ms). It's
  approximated with the reverb send; the delay and feedback are written to the
  conductor track as text events.
* **Mixer character.** The GBA mixer resamples without interpolation at
  20 or 21 kHz and writes 8-bit output. The Dungeon Dice Monsters revision has
  no mixer, and plays 8-bit samples at their own rate of 10 kHz. A SoundFont
  player plays the same samples more cleanly.
* **PSG DC offset.** The Game Boy channels output a unipolar signal. The
  SoundFont uses the same waveforms without the DC offset, which the hardware's
  output capacitor removes anyway.

### Rare's driver

* **Voice limit.** The driver mixes at most 8 voices at a time and leaves the
  rest silent until voices free up. The MIDI file mutes the notes that the
  driver leaves out with key pressure (see "MIDI from Rare's driver"). A
  player that ignores key pressure or the SoundFont's mute modulator plays
  every note. A busy passage then has notes that the game doesn't let you
  hear. Where the driver leaves out one of two notes of a key on a channel, the
  MIDI file plays both.
* **Envelopes.** The SoundFont's envelopes approximate the driver's straight
  fades, and some players treat very short envelope phases differently.
* **Timing.** The driver runs once a frame, so it plays every event on a frame
  boundary. The MIDI file keeps each event on its tick, which can be up to a
  frame later, or on the track that sets the tempo, up to a frame earlier,
  unless `--frame-timing` puts it at the start of its frame. A delay that's
  counting down when the tempo changes runs on at the old tempo, so after a
  change partway through a tune, the MIDI file can put the other tracks' notes
  several frames off, unless `--frame-timing` is used.
* **Mixer character.** The driver mixes in mono at 13379 Hz with linear
  interpolation, and writes 8-bit output. A SoundFont player plays the same
  samples more cleanly, and the MIDI files leave every channel in the centre.
* **The game's settings.** In *Donkey Kong Country 3*, the game can fade
  channels out and start a tune at a marker partway through. The conversion
  plays each tune from its start, with every channel at its own volume.
* **Sound effects** aren't converted. The driver plays them from a separate
  table, which only the game's code uses.

### Quintet's driver

* **Timing.** The driver plays each note at the start of the frame its tick
  falls in. The MIDI file keeps each note on its tick, which can be up to a
  frame later, unless `--frame-timing` puts it at the start of its frame.
* **Tuning.** The driver's frequency table is within about 3 cents of equal
  temperament in the C4 octave, but up to about 27 cents flat in the C2 octave,
  9 cents in the C3 octave, 19 to 34 cents off in the C5 to C7 octaves, and 43
  in the top octave. The PCM pitch table is within 7 cents, apart from its top
  entry, which is 14 cents flat. The MIDI files keep the keys equal-tempered.
* **Noise drums.** The noise channel's envelope steps on the sound hardware's
  64 Hz clock, which isn't tied to the frames, so in the game a drum's volume
  steps can come up to 1/128 s earlier or later than in its sample.
* **Pitch changes during a PCM note.** A sample's loop is as long as the driver
  makes it at the rate the note starts at. A note whose rate then changes, with
  a slide or the LFO, keeps those lengths.
* **Mixer character.** The GBA plays the FIFOs' 8-bit samples without
  interpolation. A SoundFont player plays them more cleanly. The PSG's
  waveforms are without the DC offset, which the hardware's output capacitor
  removes anyway.
* **Sound effects** aren't converted. The driver plays them on channels of its
  own, which only the game's code starts.

### Nintendo R&D2's driver

* **Timing.** The driver plays each note at the start of the first frame that
  reaches its tick. The MIDI file keeps each note on its tick, which can be up
  to a frame earlier, unless `--frame-timing` puts it at the start of that
  frame.
* **Pitch bends.** The driver bends each voice on its own, and its LFO starts
  again with each note. A MIDI channel has one bend, which follows the track's
  newest note, so notes that play together on a track share it.
* **Envelopes.** The SoundFont's envelopes approximate the driver's straight
  lines between points.
* **Legato.** The driver plays a track's notes in legato on one voice, which
  keeps its envelope. A SoundFont player starts each note's envelope again.
* **Echo.** The driver can add an echo to the voices of tracks with an echo
  send, but only the game's code turns it on, so the conversion leaves it out.
* **PSG.** The PSG's envelope steps the level through 16 values, where the
  SoundFont's envelope is smooth. The PSG's notes keep equal-tempered keys.
* **Mixer character.** The driver mixes in stereo at 10512 Hz without
  interpolation, and writes 8-bit output. A SoundFont player plays the same
  samples more cleanly.
* **Doubled notes.** A track that plays a key twice at once is up to 6 dB
  louder in the game than the one MIDI note it becomes, since a MIDI channel
  plays a key only once.
* **Tracks the game mutes.** A game can mute some of a sequence's tracks when
  it plays it, as *Super Mario Advance 2* does to choose between two endings of
  a jingle. The MIDI file has every track.
* **Voices and volumes the game sets.** A game can also give the driver fewer
  sample voices, or lower the music's volume, when it plays a sequence.
  `agbsf2` reads the settings that *Super Mario Advance 3*'s play-music
  routine makes. For another game, the MIDI file plays every sequence with all
  7 sample voices, at the driver's default volume.
* **Sound effects** aren't converted. Only the game's code starts them.

### MP2K

* **Timing.** The driver plays each event at the start of the first frame that
  reaches its tick. The MIDI file keeps each event on its tick, which can be up
  to a frame earlier, unless `--frame-timing` puts it at the start of that
  frame. With `--frame-timing`, the MIDI file has 3600 ticks per quarter note,
  one for each 150th of a tick, so that each frame starts on a whole tick.
* **PSG tuning.** As with Konami's driver, the PSG's 11-bit frequency registers
  put the game's high PSG notes slightly out of tune, and the driver's table
  and its bends round down. The MIDI files keep the equal-tempered pitch.
* **PSG bends.** A track that plays samples and PSG notes at once has one pitch
  bend for both, which bends a noise note smoothly and can take a square or
  wave note below C2. The driver steps through its noise settings and stops at
  C2.
* **PSG sweep and length.** Square channel 1 can sweep a note's frequency up or
  down, and a PSG voice can have a length, after which the hardware stops the
  note. The SoundFont leaves both out.
* **Notes cut off.** The driver stops a note at once when it gives the note's
  channel to another, even partway through its release. Without
  `--voice-channels`, a SoundFont player lets the note fade out instead.
* **Changes that miss older notes.** When a track changes its volume, pan or
  pitch in the tick in which it starts a note, the driver gives the change to
  the new note alone, and the track's older notes keep their volume, pan and
  pitch until the track's next change. The MIDI file changes them all.
* **Echo.** A track can give its notes a quiet echo after their release. The
  SoundFont's release doesn't include it.
* **Reverb.** The driver's reverb is a short echo of its output. It's
  approximated with the reverb send.
* **Changed voices.** A track can change its copy of a voice with extended
  commands, and a note can start partway into its sample. The SoundFont plays
  the voice group's voices as they are, from the start.
* **Sound register writes.** A track can write to the sound registers. The
  conversion leaves these out, with a warning.
* **Mixer character.** The driver mixes at its own rate, often 13379 Hz, with
  linear interpolation, and writes 8-bit output. A SoundFont player plays the
  same samples more cleanly. Some games' mixer is mono, and plays every
  DirectSound note in the middle, whatever its pan; the MIDI file keeps each
  track's pan.
* **Camelot's synth voices.** The mixer makes its waves at its rate, 31536 Hz,
  and the SoundFont's samples hold them at the pitch of each key. A pitch bend,
  and a track's key shift, which the MIDI file puts in the pitch bend, move a
  pulse wave's cycle of duties and a saw wave's filter with the pitch, where
  the game keeps them as they are. A sample's loop ends at the nearest whole
  cycle of its wave, up to half a cycle from where the duty's cycle ends. The
  triangle wave's single cycle leaves out the faint aliasing of the mixer's.
* **Camelot's echo.** The mixer's echo comes back a frame and four frames
  later, and from the right side to the left after 704 points. The reverb send
  only approximates it.

### Brownie Brown's driver

* **Timing.** The MIDI file puts each note on the beat that `agbsf2` works out.
  A note can be up to a frame and a half from where the game plays it.
  `--frame-timing` keeps each note on its frame instead. Where a song has no
  steady beat, as in most sound effects, its notes keep their frames.
* **The hardware's envelope.** A PSG channel's envelope steps on the sound
  hardware's 64 Hz clock, which isn't tied to the frames. CC11 follows a model
  of it a frame at a time, so a step can come up to a frame later than in the
  game, and a noise drum's steps up to 1/128 s earlier or later.
* **Mixer character.** The driver mixes at 16384 Hz and plays the lower octaves
  by repeating each point, interpolating only between the points it plays twice
  when it mixes all five voices, and writes 8-bit output. A SoundFont player
  plays the same samples more cleanly. The PSG's waveforms are without the DC
  offset. In *Magical Vacation*'s revision, which plays each sample straight
  through a FIFO, the driver scales each point to the volume, a step lower for
  most negative points, and a sample that doesn't loop stops about 20 points
  before its end, which then play at the start of the FIFO's next note.
* **Sound effects** play on their own in their conversions. In the game, a
  sound effect takes its PSG channel from the music while it plays.

### Krawall

* **Mixer character.** The mixer mixes at 16384 Hz and writes 8-bit output.
  Unless the game asks it to interpolate, it plays each sample point by point,
  where a SoundFont player plays the same samples more cleanly. A level past
  the mixer's full scale, which the game clips, plays at full scale.
* **Jingles.** A game can play a module once, as a jingle, but nothing in the
  module says so, and its conversion loops like any other.
* **The game's settings.** The conversion plays each module on its own, at the
  music volume that the library starts with. A game that changes the music's
  volume, or fades it, does so outside the module.
* **Sound effects** that the game plays on the mixer's channels aren't part of
  the modules, so the conversion leaves them out.

### Ubisoft Milan's driver

* **Timing.** The MIDI file puts each note on the beat that `agbsf2` works out.
  A note can be up to a frame and a half from where the game plays it.
  `--frame-timing` keeps each note on its frame instead.
* **Mixer character.** The mixer moves through each sample at 16352 Hz, makes
  twice as many points by putting one halfway between each pair, averages each
  point with the one before, and writes 8-bit output. A SoundFont player plays
  the same samples more cleanly and brighter: the averaging takes up to 3 dB
  off the top octave.
* **The PSG.** A PSG note keeps its starting volume: the conversion leaves out
  the hardware's envelope, and the vibrato and pitch modes that an instrument
  can have, with a warning. A noise note ends at its note-off. In the game,
  the noise's envelope and length run on.
* **A key outside the kit.** In the game, a note on a key that channel 9's kit
  has no sample for plays bytes from elsewhere in the cartridge, past the end
  of the sound bank's table, until its note-off. The conversion leaves the
  note out, with a warning.
* **Sound effects** aren't converted. The game plays them straight from its
  samples.

## Internals

`docs/konami.md`, `docs/rare.md`, `docs/quintet.md`, `docs/rd2.md`,
`docs/mp2k.md`, `docs/brownie.md`, `docs/krawall.md` and `docs/ubimilan.md`
document the drivers and their data formats in full, including how the tool
locates them. The code for each is in `src/konami/`, `src/rare/`,
`src/quintet/`, `src/rd2/`, `src/mp2k/`, `src/brownie/`, `src/krawall/` and
`src/ubimilan/`, behind the interface in `src/music.h`.

`tools/` holds the scripts used to reverse engineer the drivers and validate
the conversion: a disassembler, harnesses that run each game's driver
under an ARM emulator, and comparison scripts. See `tools/README.md`.

## License

`agbsf2` is released under the MIT License; see `LICENSE`. The DEFLATE
decoder in `src/inflate.cpp` is adapted from Mark Adler's puff under the zlib
license. Its notice is in that file and in `THIRD_PARTY_NOTICES`. The MIDI
and SoundFont code comes from seqlib, under the MIT License. Its notice is in
`externals/seqlib/LICENSE` and in `THIRD_PARTY_NOTICES`. The PSF container
reader, the file and console code and the parsing of numbers come from
xsflib, under the MIT License. xsflib's notice is in
`externals/xsflib/LICENSE` and in `THIRD_PARTY_NOTICES`. Game data is not
covered by these licenses.
