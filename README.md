# agbsf2

`agbsf2` converts the music of Game Boy Advance games that use Konami's,
Rare's, Quintet's, Nintendo R&D1's, Nintendo R&D2's, AlphaDream's, Brownie
Brown's, Ubisoft Milan's, Logik State's, Webfoot Technologies' or Engine
Software's sound driver, Frontline Studios' MOD or XM-like player, Nintendo's
MP2K, or Krawall, to MIDI files and SoundFonts.

MP2K, often called the "Sappy" engine, is the sound driver in most GBA games.
Many games use another driver, among them Konami's, Rare's and Quintet's games,
*The Legend of Zelda: A Link to the Past*, *Super Mario Advance 2*, *Sword of
Mana*, *Magical Vacation* and *Tomb Raider: The Prophecy*. Others play tracker
modules with Krawall, a player of XM and S3M modules. The usual MP2K tools, such
as Sappy, gba-mus-ripper and agbplay, can't read the music of these games.
`agbsf2` finds the sound driver in a ROM, plays each song through a model of
that driver, and converts it to a Standard MIDI File with a matching SoundFont
built from the game's samples and instruments. For Konami's, Quintet's,
Nintendo R&D1's, Nintendo R&D2's, AlphaDream's, Brownie Brown's and Ubisoft
Milan's drivers and MP2K, the SoundFonts also include Game Boy PSG waveforms.

## Supported games

### Konami's driver

This part of the tool was developed on *Yu-Gi-Oh! Ultimate Masters Edition:
World Championship Tournament 2006*, and it supports these games too:

* *Shaman King: Master of Spirits*
* *Shaman King: Master of Spirits 2*
* *Yu-Gi-Oh! Day of the Duelist: World Championship Tournament 2005*
* *Yu-Gi-Oh! GX: Duel Academy*
* *Animal Yokochou: Doki Doki Kyuushutsu Daisakusen no Maki*
* *Tennis no Ouji-sama 2004: Glorious Gold* and *Stylish Silver*
* *Mermaid Melody Pichi Pichi Pitch: Pichi Pichitto Live Start!*

These games use variants of the *Ultimate Masters Edition* driver.

These games use older revisions of the driver, with different commands, and
are supported too:

* *Yu-Gi-Oh! World Championship Tournament 2004*
* *Mermaid Melody Pichi Pichi Pitch*
* *Rave Master: Special Attack Force*
* *Groove Adventure Rave: Hikari to Yami no Daikessen 2*
* *Hikaru no Go 2*
* *Yu-Gi-Oh! The Eternal Duelist Soul*
* *Yu-Gi-Oh! Worldwide Edition: Stairway to the Destined Duel*
* *Hikaru no Go*
* *Yu-Gi-Oh! Duel Monsters 6 Expert 2*
* *Yu-Gi-Oh! Dungeon Dice Monsters*
* *GetBackers Dakkanya: Jigoku no Scaramouche*
* *Yu-Gi-Oh! Duel Monsters 5 Expert 1*
* *Tennis no Ouji-sama*
* *Kami no Kijutsu: Illusion of the Evil Eyes*
* *Captain Tsubasa: Eikou no Kiseki*
* *Groove Adventure Rave*
* *Yu-Gi-Oh! Double Pack 2* (*Dungeon Dice Monsters*)
* *Hunter X Hunter: Minna Tomodachi Daisakusen!!*
* *Tennis no Ouji-sama 2003: Passion Red* and *Cool Blue*
* *Tennis no Ouji-sama: Aim at the Victory!*
* *Gegege no Kitarou: Kikiippatsu! Youkai Rettou*
* *Minna no Ouji-sama*

The tool reports unrecognised revisions of Konami's driver.

### Rare's driver

* *Donkey Kong Country*
* *Donkey Kong Country 2*
* *Donkey Kong Country 3*, with a later revision of the driver
* *Banjo-Kazooie: Grunty's Revenge*
* *Banjo-Pilot*
* *Sabre Wulf*
* *It's Mr. Pants*
* *Battletoads*, the unreleased prototype

### Quintet's driver

* *Super Robot Taisen A*
* *Super Robot Taisen D*
* *Super Robot Taisen R*
* *Super Robot Taisen J*, with a later revision of the driver
* *Suta Komi: Star Communicator*, with a revision of the driver that reads
  another song format

These games have the later revision too. They keep their music in music files:

* *Blender Bros.*, with the music of the three programs that it sends to
  another Game Boy Advance
* *Robopon 2: Ring Version* and *Cross Version*, each with the music of the
  program that it sends to another Game Boy Advance
* *Bouken Yuuki Plaston GP*
* *Bouken Yuuki Densetsu Plaston Gate*
* *Bouken Yuuki PlaStar World: Densetsu no Plust Gate EX*
* *Legend of Dynamic: Goushouden*
* *Doubutsu no Shima no Chobi Gurumi*
* *Gachasute! Dyna Device Red* and *Blue*
* *Magical Fengshen*

The sound effects of these games are converted with their music.

### Nintendo R&D2's driver

* *The Legend of Zelda: A Link to the Past*, in *A Link to the Past & Four
  Swords*. The game selection and *Four Swords* play their music with MP2K.
* *Super Mario Advance 2: Super Mario World*, with an older revision of the
  driver
* *Super Mario Advance 3: Yoshi's Island*
* *Super Mario Advance 4: Super Mario Bros. 3*, with the same revision of the
  driver as *Super Mario Advance 3*

The sound effects of these games are converted with their music.

### MP2K

* *Pokémon Emerald*
* *Pokémon Ruby Version*
* the game selection and *Four Swords* in *The Legend of Zelda: A Link to the
  Past & Four Swords*
* *The Legend of Zelda: The Minish Cap*
* *Kingdom Hearts: Chain of Memories*
* *Super Robot Taisen: Original Generation 2*
* *Pokémon Mystery Dungeon: Red Rescue Team*
* *Golden Sun: The Lost Age*, with Camelot's version of the driver
* *Fire Emblem: The Binding Blade*
* *Fire Emblem*, released in Japan as *The Blazing Blade*
* *Fire Emblem: The Sacred Stones*
* *Metroid Fusion* and *Metroid: Zero Mission*, with a reworked sequencer in
  the driver

### Brownie Brown's driver

* *Sword of Mana*
* *Magical Vacation*, with an older revision of the driver

The sound effects of both games are converted with their music.

### Krawall

* *Racing Fever*, with build 1.2.2.14 of Krawall's `player.c`, dated 2002/09/12
* *007: Everything or Nothing*, with build 1.4 of `player.c`, dated 2003/03/19
* *Digimon Racing*, *The Lord of the Rings: The Third Age*, *The Urbz: Sims
  in the City*, *Robots* and *Franklin the Turtle*, with the build of Krawall
  dated 2003/09/01
* *The Sims: Bustin' Out*, with the same build, played with interpolation
* *Juka and the Monophonic Menace*, with the build dated 2004/09/17
* *Babar to the Rescue*, *Care Bears: Care Quest*, *The Koala Brothers:
  Outback Adventures* and *The Land Before Time: Into the Mysterious Beyond*,
  with the build dated 2004/11/14
* *Eragon*, *Cocoto Kart Racer*, *Cocoto Platform Jumper*, *Let's Ride!
  Friends Forever*, *Let's Ride! Dreamer*, *Crash of the Titans*, *Scurge:
  Hive* and *Shrek SuperSlam*, with the build dated 2005-04-21
* *Puppy Luv: Spa and Resort*, with a build that has no version string
* *Big Mutha Truckers*, *Street Racing Syndicate*, *M&M's Break 'Em* and
  *Princess Natasha*, with a changed form of the build dated 2005-04-21 that
  keeps each module packed in a music file

`agbsf2` also reads builds 1.2.2.10, 1.2.2.15 and 1.2.2.17 of `player.c`.
Homebrew games and a prototype use them.

A game with another build of Krawall that has a version string gets an error
that gives the string.

### Nintendo R&D1's driver

* *WarioWare, Inc.: Mega Microgame$!*
* *Rhythm Tengoku*

Their songs are Standard MIDI Files. The driver plays them with the
instruments in the game's banks. The sound effects of these games are
converted with their music.

### AlphaDream's driver

* *Mario & Luigi: Superstar Saga*. The main game's music plays with this
  driver. The two copies of *Mario Bros. Classic* in the game play theirs with
  MP2K, and `agbsf2` converts those as well.
* *Tottoko Hamtaro 4: Nijiiro Daikoushin Dechu* and *Tottoko Hamtaro: Hamu-Hamu
  Sports*. They play their music with two other versions of the driver.

### Ubisoft Milan's driver

* *Tomb Raider: The Prophecy*. Songs 0-13 are its sequenced pieces. Songs 14-22
  are nine samples that the game plays as music, with no sequence. The game's
  sound effects and its other background loops aren't converted.
* *The Mummy*. Songs 0-25 are its pieces, mostly on the PSG. Song 17 plays no
  note, and `agbsf2` writes no files for it. Songs 1-15 have a second version
  with the drum kit (see below).
* *The Sum of All Fears* and *Rogue Spear* have an older build of the driver.
  Their music is recorded samples, with no sequences. `agbsf2` reports that and
  converts nothing.

### Logik State's driver

* *Aero the Acro-Bat: Rascal Rival Revenge*. Songs 0-21 are its modules, in
  the driver's `GBAMOD2.` format. The sound effects are single samples that
  the game's code starts. They aren't converted.
* *Zapper: One Wicked Cricket!*. Songs 0-18 are its modules, in the same
  format, with another build of the driver.
* *Pinball Challenge Deluxe*. Its 11 modules are in the driver's `GBAMOD1.`
  format. Each module contains many short pieces. Songs 0-314 are those
  pieces: the title music, the music of the eight tables and their jingles.
  Eight pieces play no note and are skipped.
* 26 later games, whose modules are in the driver's `GBAMOD30` format:
  *Alex Rider: Stormbreaker*, *Alien Hominid*, *Arctic Tale*, *Arthur and the
  Invisibles*, *Asterix & Obelix XXL*, *Bionicle: Maze of Shadows*, *Driv3r*,
  *Duel Masters: Kaijudo Showdown*, *Duel Masters: Shadow of the Code*,
  *Family Feud*, *Garfield and His Nine Lives*, *Hot Wheels: Stunt Track
  Challenge*, *Kid Paddle*, *LEGO Knights' Kingdom*, *Medal of Honor:
  Infiltrator*, *Need for Speed Carbon: Own the City*, *Need for Speed: Most
  Wanted*, *Need for Speed: Porsche Unleashed*, *Need for Speed: Underground
  2*, *Noddy: A Day in Toyland*, *Premier Manager 2004-2005*, *Premier Manager
  2005-2006*, *Stuntman*, *Super Puzzle Fighter II Turbo*, *Totally Spies!*
  and *Totally Spies! 2: Undercover*. They have eight builds of the driver
  between them. The songs of each game are numbered in the order of its table
  of songs. The sound effects are single samples, and they aren't converted.

A game with `GBAMOD30` modules that isn't in this list gets an error. Each
game's code gives the driver its settings, and `agbsf2` contains those
settings only for these games.

### Webfoot Technologies' driver

* *Dragon Ball Z: The Legacy of Goku*. Songs 0-17.
* *Dragon Ball Z: The Legacy of Goku II*. Songs 0-43.
* *Dragon Ball Z: Buu's Fury*. Songs 0-52.
* The cartridge with *Dragon Ball Z: Buu's Fury* and *Dragon Ball GT:
  Transformation*. It contains three programs, each with a copy of the driver.
  Song 0 is the menu music, songs 1-53 come from *Buu's Fury* and songs 54-78
  from *GT Transformation*. `--info` gives the range of songs of each program.

The sound effects are single samples that the game's code starts, and they
aren't converted. Each song plays at the music volume that the game starts
with: 4 of 8, and 3 of 8 in *GT Transformation*.

### Frontline Studios' MOD player

* *Hardcore Pool*. Songs 0-10.
* *Garfield: The Search for Pooky*. Songs 0-17: songs 0-10 come from the
  first module and songs 11-17 from the second. The European release's
  revision 2 converts as the US release.
* *Games Explosion!*. Songs 0-14.

The player plays one module of four channels for all the music of a game, and
the game starts each song at an order of the module. A song is the part of the
module from that order up to the jump that loops it. A part that the game never
starts becomes a song too, after the game's songs from the same module. *Games
Explosion!* has three. `--info` gives the order of each song, and marks the
parts that the game doesn't start.

A game or release that isn't in this list gets an error. Each game's code
gives the player its settings, and `agbsf2` contains those settings only for
these games. The sound effects are single samples that the game's code starts,
and they aren't converted.

### Frontline Studios' XM-like player

* *Chicken Shoot*. Songs 0-4.
* *Chicken Shoot 2*. Songs 0-4.

The player plays modules of four channels, and the game plays each module
through an order list that the game supplies. Song 0 is the title music, and
songs 1-4 are the lists for the game's levels. `--info` gives the module, the
list and the master volume of each song. A game that isn't in this list gets
an error. The songs are defined in the game's code, and `agbsf2` contains them
only for these games.

Each song plays at the master volume that the game passes on a new save. The
game scales the master volume by the music volume in its options, over 9.
A new save sets that volume to 9. The sound effects aren't converted.

### Engine Software's driver

* *The Santa Clause 3: The Escape Clause*. Songs 0-11.

The driver plays modules in the manner of FastTracker 2 through a software
mixer. The game's sound manager starts each song and raises the music volume
from 0 to 210 of 256 over its first 53 frames. Song 6 starts at that volume
at once. Song 5 is a jingle: it plays once at that volume and ends. In the
game, the song before it then goes on from where it was. Song 6 plays once
too, with no loop markers: the driver would loop it, but the game fades it out
before its second pass.

*Dragon Ball Z: Taiketsu* and *Ty the Tasmanian Tiger 2: Bush Rescue* have
other builds of the driver. `agbsf2` finds the driver in them and gives an
error. The sound effects are single samples that the game's code starts, and
they aren't converted.

### Other games

Detection finds each driver by its code rather than at fixed addresses. Other
games that use the same revisions of a driver should therefore work too,
unless a section above says that a game must be in its list. Their songs may
still use unsupported commands. An unknown command stops the track with a
warning. `--info` names the driver it found and lists the tables it detected,
with any assumptions it made. If detection fails, you can name the driver and
give the table addresses with the override options.

## Building

For 64-bit Windows, download the zip containing `agbsf2.exe` from
GitHub Releases.

agbsf2 supports macOS on Apple Silicon and Windows x64 built with
clang-cl. Other platforms and compilers aren't supported.

You need Git, a C++20 compiler and CMake 3.20 or later. The MIDI and
SoundFont code comes from seqlib. The PSF container reader, the file and
console code and the parsing of numbers come from xsflib. The ROM and GSF
reader and all fifteen sound drivers, with their conversions, come from
agblib. agblib builds on ntdlib. The four libraries are Git submodules of the
repository, in `externals/seqlib`, `externals/xsflib`, `externals/ntdlib` and
`externals/agblib`. There are no other dependencies.

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
`Agbsf2.app` makes from the program's output. CI builds agbsf2 on macOS on
Apple Silicon and on Windows x64 with clang-cl, and runs the tests only in the
Windows build.

## Usage

```sh
agbsf2 game.gba
```

This writes one `.mid` and one `.sf2` per song to a folder beside the input
file, with the same name: for example `game/game_00.mid` and
`game/game_00.sf2`. Pass several files to convert them together. A game with
more than one sound driver gets the songs of every driver (see
[Options](#options)).

A song that the game plays in two versions gets a second pair of files.
Enchanted Riverbank in *Donkey Kong Country 3* gets one for the arrangement
that the game plays under water (see
[MIDI from Rare's driver](#midi-from-rares-driver)). Sequence 30 of *Super
Mario Advance 2* gets one for the ending that the game plays as music 18, and
sequences 15 to 18 get one for the track that plays while the player rides
Yoshi (see
[MIDI from Nintendo R&D2's driver](#midi-from-nintendo-rd2s-driver)). The
songs of the live stages in the two *Mermaid Melody* games and the songs of
*The Mummy* with the drum kit get one too.

Characters that Windows forbids in filenames are replaced with underscores.
Trailing dots and spaces are removed, and reserved device names such as `CON`
get an underscore prefix.

Input can be a raw `.gba` ROM or a GSF rip (`.gsflib`, `.minigsf` or `.gsf`).
A `.minigsf` selects a song from the library named in its `_lib` tag. The
library is loaded from the same folder and gives the output its name. If you
pass several `.minigsf` files from one set, the library is converted or listed
once. If the library has no music that agbsf2 reads, the reason is given
once. A conversion includes every song in the ROM, whatever song the GSF
selects.

Use a ROM if you have one. A GSF rip keeps only the bytes that the game read
while the rip played, and a song that the rip didn't play may be missing. If
every song cuts a sample short, the unused part is silent in the SoundFont. A
sample played at a high pitch can have gaps too. At a high pitch the game skips
some bytes of the sample. The rip keeps only the bytes that the game read. A
SoundFont player then plays silence in the gaps. agbsf2 can't restore the
missing bytes, and it doesn't fill the gaps with a guess.

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
| `-t, --tracks LIST` | only these tracks, e.g. `4-15`; in Konami's driver, 0-3 are the PSG channels and 4 and up the sample voices, in Quintet's, 0-3 are the PSG channels and 4 and 5 the PCM channels, in Brownie Brown's, 0-3 are the music's PSG channels, 4-7 the sound effects' and 8 and up the sample channels, in Ubisoft Milan's, 0-2 are the PSG channels and 9 the kit, in Krawall, the module's channels, 0-31, in Logik State's, the module's channels, 0-7, in Webfoot Technologies', the song's channels, 0-14, in Frontline Studios' players, the module's channels, 0-3, and in Engine Software's, the song's channels, 0-7 |
| `--single-sf2` | one SoundFont for all songs (see [Output](#output) for its banks) |
| `--voice-channels` | MP2K only: a MIDI channel for each of the driver's sound channels instead of each track. Notes then stop where the game cuts them off, but the notes of a track move between channels (see [MIDI from MP2K](#midi-from-mp2k)) |
| `--psg-exclusive` | MP2K only: an exclusive class for each PSG channel in the SoundFont. A track's next note on a PSG channel then ends the slow release of the note before it. The game cuts that note off there too. A note that another track cuts off still fades out (see [SoundFont from MP2K](#soundfont-from-mp2k)) |
| `--frame-timing` | put each event on the frame the game plays it in, rather than on the song's beat (see [Output](#output)) |
| `--dump` | also write a text listing of every command of each song (`NAME_NN.txt`) |
| `--info` | print each driver found, with its tables and a list of its songs, then exit. The track count, length and loop reflect the current conversion options. A song that the game plays in two versions gets a line for each |
| `--driver NAME` | use only `konami`, `rare`, `quintet`, `rd1`, `rd2`, `alphadream`, `brownie`, `ubimilan`, `logikstate`, `webfoot`, `frontline_mod`, `frontline_xm`, `engine_software`, `krawall` or `mp2k`, instead of every driver detection finds |
| `--song-table ADDR`, `--song-count N` | override detection: the address of the song table (hex) and the number of songs. In Nintendo R&D2's driver the address is that of the game's settings for the driver, in Ubisoft Milan's driver that of the sound bank, and in Krawall that of the game's table of modules |
| `--sample-table ADDR`, `--mix-rate HZ` | override detection in Konami's driver: the address of the sample table (hex) and the mixer rate |
| `--music-volume N` | Logik State's and Webfoot Technologies' drivers only: play at this music volume, not at the volume that the game sets at its start. The volume goes from 1 to 64 in Logik State's driver, or to 256 for `GBAMOD30` modules, and from 1 to 8 in Webfoot Technologies' driver (see [Accuracy](#accuracy)) |
| `-q, --quiet` | only print warnings and errors |
| `--trace SONG`, `--trace-frames N` | print the driver model's state after each frame |

Detection looks for Rare's driver first, then for Quintet's, Nintendo R&D2's,
Nintendo R&D1's, AlphaDream's, MP2K, Brownie Brown's, Krawall, Ubisoft
Milan's, Logik State's and Webfoot Technologies' drivers, Frontline Studios'
two players and Engine Software's driver. It finds these by their code alone.
Konami's driver comes last.

If a game has several drivers, the songs of each are converted. The files of
the first driver get the usual names. The others add the name of the driver
after the base name, as in `game_mp2k_000.mid`, and their messages give that
name before the song number. `--songs` and `--tracks` apply to every driver.
`--driver` selects one driver and uses the usual file names. A driver whose
tables can't be read is left out with a warning, and the others are
converted.

`--trace` uses the first driver found. Without `--driver`, `--song-table` and
`--song-count` also select the first driver and convert only its songs. Give
`--driver` as well if detection doesn't recognise the driver.
`--sample-table` and `--mix-rate` imply `--driver konami`. Without `--driver`,
`--music-volume` looks only for Logik State's and Webfoot Technologies'
drivers.

To listen, load the pair into any SoundFont player, for example:

```sh
fluidsynth -ni -F song10.wav game/game_10.sf2 game/game_10.mid
```

### Known issues

* `--info` lists each song's length and loop without converting the song. A
  song that fails to convert still gets a length.
* With `--single-sf2`, the instruments of some games don't fit in one
  SoundFont. The format allows at most 65535 zones, generators and modulators,
  and `agbsf2` stops with "soundfont too large". Among these games are
  *Pokémon Ruby Version*, *Pokémon FireRed Version*, *Pokémon Emerald*, *Mario
  Kart: Super Circuit*, *Groove Adventure Rave*, *Hikaru no Go*, *Yu-Gi-Oh! The
  Eternal Duelist Soul*, *Yu-Gi-Oh! Duel Monsters 6 Expert 2* and *Rhythm
  Tengoku*. Convert them without the option.

## Output

By default, a song that loops is written with its loop played twice. The loop
is also marked with `loopStart` and `loopEnd` marker events for players that
loop on them.

The tracks of a song can loop from different points and at different lengths.
The loop starts where every track is looping and every track without a loop
has finished. It lasts until every track is back where its loop started: 12
bars for tracks that loop 3 and 4 bars, for example. The loop then repeats
seamlessly. If that would be more than 8 times as long as the longest loop of
a track, the file uses that longest loop, and the other tracks fall out of
step each time a player repeats it. Konami's driver sends every track back to
its loop point at once, and each song of the driver loops from the first of
those points. Points far before the loop point of the looping track don't
count, and neither do those of tracks that end for good.

A track keeps settings such as its volume, pan or octave when it loops, and a
song keeps its tempo. If the intro leaves other settings than the end of the
loop, the first pass can sound different from later passes. With Konami's,
Quintet's, Nintendo R&D2's, Brownie Brown's and Ubisoft Milan's drivers and
Krawall, the marked loop then starts on the second pass. With MP2K, the marked
loop starts on the second pass where the tempo differs between passes. It
does the same where a player that loops on the markers would play a note more
than 1 dB louder or quieter on either side than the next pass. The file plays
the first pass once before the marked loop. Quintet's driver also discards
fractional frames when a track loops. Where that would make a looping player
play a note at the start of the loop twice, the marked loop moves to the
second pass as well. A player that silences its notes when it repeats the
loop also cuts short any note held across the start of the loop.

A song without a loop can end long after its last sound. A track can wait, or
change a controller, after its last note. When the end of the song
comes more than 10 s after its last sound has died away, the file ends where
that sound dies away. Nothing after that point can be heard.

When a loop sets the program, volume, pan, expression, reverb or pitch bend of
a track, the MIDI file writes that setting even if its value hasn't changed.
Otherwise, a player that jumps back to `loopStart` would keep the value from
the end of the loop. Konami conversions restore all these settings at the
first event of each track in the loop. The conversions of Konami's, Brownie
Brown's and Ubisoft Milan's drivers also give the tempo again at `loopStart`.
The beat that they work out can have another tempo at the end of the loop. A
player that keeps its tempo when it jumps back would play the start of the
loop at that tempo.

Each event goes on the beat of the song, as in a quantised MIDI file. The
notes then line up with the bars and beats in an editor. The game plays each
event at the start of a frame. An event on the beat can therefore be up to a
frame from where the game plays it, or a frame and a half with Konami's,
Brownie Brown's and Ubisoft Milan's drivers. `--frame-timing` puts each event
on the frame in which the game plays it, as closely as the ticks of the MIDI
file allow, at the same tempos. Krawall, the drivers of Logik State and
Webfoot Technologies and Frontline Studios' XM-like player play each tick at
the sample where it falls, not at the start of a frame. Frontline Studios' MOD
player plays a tick at each frame. The events of these drivers are on the
ticks of the module either way.

Konami's, Brownie Brown's and Ubisoft Milan's drivers store note lengths in
whole frames, rounded by the composers' tools. A 16th note that should last
6.5 frames lasts 6 or 7, and the original beat isn't stored. `agbsf2`
estimates the beat from the note lengths and finds steady passages and tempo
changes. For ritardandos and rubatos, it sets a tempo for each note. Notes
within a frame and a half of the nearest beat move onto it, with divisions
from a quarter note down to a 32nd note or a 16th-note triplet. Echo channels
keep their delay, and so do the quick notes of an arpeggiated chord. Songs too
short to establish a beat, such as most sound effects, use a quarter note of
30 frames. At possible tempo changes, such as loops, `agbsf2` adjusts the
length of the quarter note so that the stretch before ends on the beat. The
next stretch reuses the length of an earlier stretch with the same beat, if
there is one. Otherwise it starts with the adjusted length.

### MIDI from Konami's driver

Each MIDI file has a conductor track and one track per game track that plays
notes. Tracks are named `Square 1`, `Square 2`, `Wave`, `Noise`, and
`Voice 0`-`Voice 11` (game tracks 4-15, for voices 0-11 of the driver). The
older Rave Master, Eternal Duelist and Dungeon Dice Monsters revisions have 8,
6 and 4 voices. MIDI channel 10 is used only if a song requires all 16
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

In the game, the volume can change during a note. CC11 can follow such
changes, and velocity can't.

Together, CC11 and CC10 reproduce the level of each side in the game under
the constant-power pan law that most SoundFont players use.

In the Eternal Duelist revision, a PSG note at the current volume changes the
frequency of the channel without restarting it. The MIDI file writes this as a
pitch bend of the current note. A series of these notes can span more than 24
semitones, and the bend range then widens to fit.

In the Dungeon Dice Monsters revision, songs set separate left and right PSG
volumes. The CC10 and CC11 values of each PSG track include them.

Two games play some songs both in a live stage and elsewhere, and the live
stage changes how they sound. *Mermaid Melody Pichi Pichi Pitch* plays songs
0-8 that way. Its live stage silences track 12 from a point in each song. In
songs 1 and 4-7 it also swaps some notes of track 13 for other samples, some
of them many seconds long. *Mermaid Melody Pichi Pichitto Live Start!* plays
songs 0-2, 4-8, 13 and 17-20 that way. Its live stage silences track 12 or 14
from a point in songs 2, 4 and 17-20. In songs 0, 1, 4-8 and 13 it swaps some
notes of track 14 for such samples. Each of these songs gets two pairs of
files:
`game_02.mid` and `game_02.sf2` as the game plays it elsewhere, and
`game_02_live.mid` and `game_02_live.sf2` as the live stage plays it.

### SoundFont from Konami's driver

Each song's SoundFont contains exactly the instruments that the song uses.

* **Sample tracks** get one preset per track (sometimes more, for samples
  whose keys would collide). The preset maps MIDI keys to the samples that the
  track plays:
  * Samples played at several pitches go at their musical pitch, estimated
    from the waveform. Melodies then read as the right notes.
  * Samples played at a single pitch, typically drums, each get a separate key.
* **Square channels** get one preset per duty cycle.
* **Wave channel** gets one preset per wave and volume. The driver stores a
  scaled copy of each wave for every volume. Quiet notes have coarser
  waveforms, and the presets keep them. The Dungeon Dice Monsters revision
  keeps each wave at full scale and sets the volume in the hardware. Its songs
  get one preset per wave.
* **Noise channel** gets one preset with a key per noise note, synthesised from
  the Game Boy's LFSR, and more presets if a song plays more noise notes than
  there are keys.

Samples keep the rate and loop points of the game.

With `--single-sf2`, the instruments of each song are in the bank numbered
after the song. MIDI selects only banks 0 to 127. The instruments of a song
numbered 128 or more, and those of a song past its 128th, therefore take free
programs in the other banks.

### MIDI from Rare's driver

The tune format resembles MIDI: note, controller, program and pitch bend
commands on 16 MIDI channels, separated by delays in ticks. The MIDI file
keeps the tune's ticks per quarter note, channels, keys and programs. A
velocity or volume (controller 7) of v is written as 127 × √(v/127). Each
source track that plays notes gets a MIDI track named `Track 0`, `Track 1`
and so on. A separate first track contains the tempo and loop markers.

The game's code sets a music volume for each tune. The volume scales every
note of the tune alike. Each channel starts with that volume, out of 128, as
expression (controller 11): 127 × √(volume/128). The games start their tunes at
39 to 150 (`docs/rare.md` lists them). Expression can't go above 127. The few
tunes above 128 therefore play at 128, up to 1.4 dB quieter than in the game.

The driver counts each frame as 1/60 s, but the GBA shows 59.73 frames a
second. The games therefore play every tune 0.46% slower than its tempo says,
and the tempos in the MIDI file are slowed down to match.

The MIDI file follows what the driver plays, where that differs from the
tune's commands:

* Each channel has a limited number of note slots. The driver drops a new
  note if all the slots of its channel are playing. The MIDI file leaves such
  notes out. It ends each note where the slot in the driver stops playing it,
  for example where a note in mono mode (controller 126) cuts off the one
  before.
* The driver's mixer plays at most 8 voices in each frame. It leaves the other
  notes out of that frame. Where it leaves out every note of a key on a channel,
  the MIDI file mutes the key there with polyphonic key pressure (aftertouch) of
  127. A key pressure of 0 unmutes the key when the mixer takes one of its notes
  again. The SoundFont turns a key pressure of 64 or more into 96 dB of
  attenuation. A player that applies the modulators of the SoundFont, such as
  FluidSynth, then silences the muted notes. Each channel that mutes a key also
  starts with the General MIDI 2 Controller Destination Setting message. With
  the message, the pressure of a key scales the level of the notes of that key
  on the channel. The level goes from full at a pressure of 0 to silence at 127.
  A player that follows the message, such as BASSMIDI, then silences the muted
  notes. A player that ignores key pressure, or both the modulator and the
  message, plays every note. Where the mixer takes one note of a key and leaves
  out another on the same channel, the MIDI file plays both. In *Donkey Kong
  Country 3*, the mixer also leaves out the notes of a channel at volume 0, and
  in tune 2, at volume 2 or less. Those notes don't count towards the limit. A
  file whose SoundFont has the mute modulator starts with a key pressure of 0
  for each key that the file plays before giving the key a pressure. A player
  that keeps key pressures from one file to the next would otherwise mute the
  keys that the file before left muted. With `--single-sf2`, the shared
  SoundFont has the modulator once any tune of the set has mutes. Every file of
  the set then starts with those key pressures.
* A note off releases only one note with that key on the channel. A MIDI
  channel can't play the same key twice at once. The conversion therefore
  ends the older note when the newer one starts.
* The vibrato of the driver (controller 1) is written out as pitch bends, a
  frame at a time, together with the bends of the tune. RPN 0 sets the bend
  range of each channel to fit the bends and the vibrato.
* Notes that share a tick with a program change use the same program as in
  the driver.

*Donkey Kong Country 3* plays Enchanted Riverbank (tune 12) as two
arrangements: channels 0-3 above water and channels 4-8 under water. The
game's code keeps the channels of the other arrangement at a level of 0. The
mixer then leaves out their notes. Those notes don't count towards the limit of
8 voices either. The tune therefore gets two pairs of files: `game_12.mid` and
`game_12.sf2` above water, and `game_12_underwater.mid` and
`game_12_underwater.sf2` under water. Each pair leaves out the channels of the
other arrangement. In the game, the music fades from one arrangement to the
other over about a quarter of a second when the player dives in or climbs out.
The files don't have those fades.

### SoundFont from Rare's driver

Each tune's SoundFont has a preset for each program the tune plays, named
`Program N` and with the same number, in bank 0. General MIDI players keep
channel 10 for drums. The programs played on channel 10 are therefore in bank
128 as well, and such players play the same instruments there.

In *Donkey Kong Country 3*, tunes can override the attack, decay, sustain
and release of a channel's instruments until its next program change. A note
that plays with such settings gets a preset with the same program number in
a separate bank, one for each set of settings, from bank 1 up. The MIDI file
selects that bank before the note. As in the driver, each envelope phase uses
the settings of the channel at the start of that phase. Changes therefore
affect the later phases of notes already playing. A note still playing at the
end of the file gets the release that the channel settings give there. The
driver would give it the same release.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their
  rate, root key, fine tune and loop. Some players can't play a very short
  loop. Such a loop is repeated until it's at least 32 points long.
* **Drum kits and key splits** become a zone for each range of keys that plays
  the same instrument. Like the driver, the zones of a drum kit play each
  sample at its pitch on every key.
* **Envelopes** follow the attack, decay, sustain and release of the driver.
  The fades of the driver are straight lines in level, and those of a
  SoundFont are straight lines in decibels. Decays and releases are therefore
  stretched to keep the loudness close.
* **Volume and velocity** scale the level in a straight line. The driver does
  the same. A SoundFont player squares both by default. The MIDI file
  therefore contains their square roots.
* **Mutes.** Where the MIDI file mutes notes that the mixer leaves out, each
  instrument has a modulator that adds 96 dB of attenuation while the key of the
  note has a key pressure of 64 or more. That takes a 16-bit sample below its
  smallest step.

With `--single-sf2`, the tunes that use the instruments of the first tune use
bank 0. Each other set of instruments gets a separate bank, and the tunes
select it with a bank change at the start. The banks for envelope settings
come after those. The programs of each set on channel 10 are in bank 128 as
well. Where two sets have
the same program there, bank 128 keeps the first one, and the later tune gives
a warning.

### MIDI from Quintet's driver

Each MIDI file has a conductor track with the tempo and loop markers, and a
track for each channel of the song that plays notes: `Square 1`, `Square 2`,
`Wave`, `Noise`, `PCM A` and `PCM B`, on MIDI channels 1-6.

The songs of a music file are numbered first, then its sound effects. A game
with several music files numbers the songs and sound effects of each file
after those of the file before. A sound effect has the same format as a song,
and the game plays it on the sound effect channels of the driver. A channel
whose data starts with `FF` is left to the music. The MIDI file ends when the
last channel of the sound effect ends.

The tempo of a song comes from its first channel. A sound effect that leaves
its first channel to the music keeps the starting tempo of the driver: 90, or
120 in the games with the later revision. The MIDI file has 3552 ticks per
quarter note, or 37 per song tick. At tempo T, a driver frame lasts exactly T
MIDI ticks. The driver plays each note at the start of the frame that its tick
falls in. The MIDI file keeps each note on the exact tick, within that frame.
When a channel loops or the tempo changes, the driver drops the part of a
frame that it has counted, and the notes in the MIDI file follow it from
there.

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume, pan and the master volume | CC11 for loudness and CC10 for pan, as with Konami's driver |
| detune, slides, the pitch envelope, the LFO and the frequency sweep | pitch bend, a frame at a time, with the bend range set per channel by RPN 0 |
| duty and wave bank switches | instruments that switch samples during each note |

Square and wave notes are at the key of their pitch. The frequency table
starts at C2, key 36. The bends contain the difference between the pitch that
the channel plays and its entry in the table. The SoundFont follows the table
too (see [Accuracy](#accuracy)). The frequency sweep changes the pitch of
square 1 within a frame, and its steps become extra bends between the frame
boundaries. PCM notes play the sample at its rate on key 60, and each step of
the PCM pitch table of the driver is a key. Noise notes are drums, with a key
for each different sound.

### SoundFont from Quintet's driver

Each song's SoundFont contains the instruments that its notes play:

* **Square channels** get a sample for each duty, and the wave channel a
  sample for each wave pattern of 32 or 64 steps. An instrument that switches
  its duty or wave bank during a note switches samples at the same point.
* **Noise drums** are rendered from the writes of the driver to the noise
  channel, with the macro and volume changes of the note, through the Game
  Boy's LFSR, envelope and length counter. A drum that ends on a steady sound
  loops its last two frames. That loop repeats sooner than the noise of the
  hardware does, and a long held drum can pick up a 30 Hz buzz.
* **PCM samples** are the game's 8-bit samples, widened to 16 bits. Each plays
  as the driver plays it: from the start of the note until the driver stops it
  or goes back to its loop, and then the frames of each pass through the loop.
  The driver checks a sample once a frame. The lengths therefore depend on the
  rate that a note plays at, and a sample that plays at several rates gets a
  copy for each.

The levels follow the GBA mixer: a square at full volume is 30/128 as loud as
a full-scale PCM sample. With `--single-sf2`, the instruments of each song are
in the bank numbered after the song. MIDI selects only banks 0 to 127. The
instruments of a song numbered 128 or more, and those of a song past its
128th, therefore take free programs in the other banks.

### MIDI from MP2K

Each track of the song that plays notes gets a MIDI track, named `Track 0`,
`Track 1` and so on, on the MIDI channel with its number. Players keep channel
10 for drums. Track 9 moves from it to the first free channel after 10, or else
to the last free one before it. A song that plays on all 16 channels keeps
track 9 on channel 10. A separate first track contains the tempo and loop
markers. The MIDI file has 960 ticks per quarter note, 40 for each of the 24
of the driver. A player such as FluidSynth rounds its position to a whole tick
at each tempo change. At 24 ticks, that rushed songs that change the tempo
often. The file keeps the key of each note. As with Rare's driver, the tempos
are slowed down by 0.46% to match the frame rate of the GBA.

| Game | MIDI |
|---|---|
| note | note on/off; a velocity of v is written as 127 × √(v/127) |
| `VOICE` | program change, with the voice's number |
| `VOL`, `PAN` | CC11 (expression) for loudness and CC10 for pan (see below) |
| `BEND`, `BENDR`, `TUNE`, `KEYSH`, vibrato | pitch bend, with the bend range set per channel by RPN 0 |
| the song's reverb | CC91 (reverb send) |

The driver computes the level of each side from the volume and pan of the
track. Together, CC11 and CC10 give those levels under the constant-power pan
law of MIDI RP-036. Most SoundFont players use that law. CC7 stays at 127. As in
the files of the other drivers, CC11 can't go above the level of the loudest
centred track. A track that the game pans hard to one side at a volume above
90 therefore plays up to 3 dB quieter on that side.

The PSG plays a note in the middle, or on one side alone where the level of
that side is at least twice the other. Each side that plays gets the same
level at every pan. While every note of a track is a PSG note, the CC10 and
CC11 of the track follow that rule. In some games the mixer is mono and plays
each DirectSound note in the middle, at the average level of the two sides.
The MIDI files of those games keep CC10 at 64 on every track that the PSG rule
doesn't cover.

In Camelot's version of the driver, in *Golden Sun: The Lost Age*, the mixer
has an echo in place of the reverb, the same in every song. Every track has a
reverb send of 53, the reverb setting of the driver closest to the echo. The
later version, in *Mario Tennis: Power Tour*, has a weaker echo. Its tracks
have a reverb send of 32, the echo level.

The MIDI file follows what the driver plays, where that differs from the
song's commands:

* The driver plays a note on one of a few DirectSound channels, or on the PSG
  channel that its voice uses. It drops a note that finds no channel that it can
  take. The MIDI file leaves such notes out, and ends a note where a later note
  takes its channel. The number of DirectSound channels is the one that the game
  gives the song. *Fire Emblem* and *Fire Emblem: The Sacred Stones* set 8 for
  some songs and 7 for the others. The later version of Camelot's driver, in
  *Mario Tennis: Power Tour*, mixes 12 whatever the game sets.
* The MIDI file leaves out notes that the driver releases before they start.
  Those notes never sound. It also leaves out PSG notes whose volume rounds to
  silence on the 16 levels of the PSG.
* A PSG note plays at the level that the PSG gives it. The driver gives the
  note a whole level from 0 to 15: the sum of the two volumes of the channel,
  divided by 16 and rounded down. The wave channel plays that level at one of
  the five volumes of NR32: silence, 25%, 50%, 75% or 100%. Below 100%, the
  hardware scales each point of the wave pattern and drops the fraction. The
  CC11 of the track and the velocity of the note give the level before the
  rounding. The velocity of each PSG note is therefore scaled by the ratio of
  the hardware level to that level, over the frames of the note until its
  sound ends. A velocity can't go above 127, and a note that would require more
  plays quieter than in the game.
* Where a song changes the tempo, the driver has already counted part of the
  next tick at the old tempo. The tempo for that tick in the MIDI file does the
  same.
* When the LFO of the driver modulates the pitch, it's written out as pitch
  bends, a tick at a time. When the LFO modulates the volume or the pan, CC11
  and CC10 follow it.
* The key shift is part of the pitch bend. Key splits and drum kits then pick
  the same voices as in the game.
* While every note of a track is a PSG note, the pitch bends of the track
  follow what the driver plays. A noise note steps through the noise settings
  of the driver, about 3 semitones a key apart, and a square or wave note
  doesn't go below C2.

When the driver gives a channel to a new note, it stops the note that was
playing there at once, even partway through its release. A SoundFont player
lets that note fade out, and busy passages can sound fuller than in the game.
`--voice-channels` fixes that, at the cost of files that are harder to edit.
It gives each sound channel of the driver that plays notes a MIDI channel and
a track, named `DirectSound 1` to `DirectSound 12`, `Square 1`, `Square 2`,
`Wave` and `Noise`. Players keep channel 10 for drums, and the option uses it
last. Each note plays on the channel that the driver gives it, and an All
Sound Off (CC120) stops its sound where the channel of the driver stops. The
notes of a track then move between channels. The program, volume, pan and
pitch bend of each channel follow the track whose note it plays. `--tracks`
still picks the tracks of the song.

`--psg-exclusive` shortens the fades of the PSG notes that the next note of
the same track cuts off. It changes only the SoundFont.
[SoundFont from MP2K](#soundfont-from-mp2k) says how.

### SoundFont from MP2K

Each song's SoundFont has a preset in bank 0 for each voice that the song plays.
The preset is named `Program N` and has the number of the voice. A song that
plays on all 16 channels also has copies in bank 128 of the voices that track 9
plays on channel 10.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their
  rate and loop. A compressed sample is decoded, and a reversed one is stored
  backwards.
* **Fixed-pitch samples** play at the mixer rate on every key, as in the
  driver, and the pitch bend doesn't move them.
* **Samples of 4-bit steps**, in the later version of Camelot's mixer, in
  *Mario Tennis: Power Tour*, play for a fixed-pitch voice whose sample has a
  negative size. The SoundFont contains the points that the mixer decodes from
  the steps.
* **Synth voices**, in Camelot's version of the driver, play waves that the
  mixer makes from the data of a sample: a pulse wave whose duty moves up and
  down each frame, a saw wave through a filter, and a triangle wave. A pulse
  or saw voice gets a sample for each key that the song plays it at, made at
  the mixer rate and looped over the cycle of the duty. A triangle voice gets
  one cycle that plays at every key.
* **PSG voices** play the Game Boy's square waves, the 32-point wave pattern of
  the voice, or noise from the Game Boy's LFSR. Noise gets a zone for each
  noise setting of the driver, at its rate. Settings below 32 Hz play at about
  32 Hz, the lowest that a zone can be tuned to. As on the GBA, the PSG voices
  are 6.3 dB quieter than a sample at its full level. CC91 gives them no
  reverb: on the GBA, the reverb of the driver works on the mixer output, and
  the PSG doesn't pass through the mixer.
* **Key splits and drum kits** become a zone for each range of keys that plays
  the same voice. The zones of a drum kit play at the keys and pans of their
  voices. The zone of each sample drum splits it between the sides in the
  ratio of the driver. A sample drum panned hard to one side plays up to 3 dB
  quieter there than in the game. A PSG drum follows the PSG rule on pans (see
  [MIDI from MP2K](#midi-from-mp2k)). On one side alone, its zone has 7.5 dB
  of attenuation. FluidSynth, BASSMIDI and most other SoundFont players apply
  0.4 of it, and that takes off the 3 dB that the RP-036 pan law adds on that
  side. A player that follows the SoundFont specification plays the drum
  4.5 dB too quietly there.
* **Envelopes** follow the attack, decay, sustain and release of the voice.
  Between the attack and decay of a sample, the full level lasts one frame, as
  in the driver. The decay and release of a sample fall by a fraction each
  frame. That's a straight line in decibels, as in a SoundFont. The fades of
  the PSG, and the releases of Camelot's mixer, are straight lines in level.
  They're stretched to keep the loudness close.
* **Velocity** scales the level in a straight line. The driver does the same.
  A SoundFont player squares the velocity by default. The MIDI file therefore
  contains the square root of the velocity in the driver. The velocity of a
  PSG note is scaled to the PSG level first (see
  [MIDI from MP2K](#midi-from-mp2k)). The master volume of the driver scales
  the samples. Camelot's mixer leaves the master volume out and plays the
  samples a little louder against the PSG. The samples themselves contain
  these levels and the PSG level. The attenuation of a zone could contain them
  too, but FluidSynth and BASSMIDI apply only 0.4 of it.

With `--psg-exclusive`, each zone of a PSG voice with a slow release gets the
exclusive class of its channel: 1 and 2 for the squares, 3 for the wave and 4
for the noise. When a note of a class starts, FluidSynth ends the notes of the
same class on its MIDI channel, and their sound fades by 96 dB in 0.32 s. Each
MIDI channel plays one track. In the game, the next note of a track on a PSG
channel takes the channel, and the note before it stops there at once. A zone
whose release is quicker than the fade in FluidSynth gets no class. The fade
would slow that release down. A note that another track cuts off
still fades out. `--voice-channels` stops it with an All Sound Off.

The SoundFont specification limits a class to the notes of one preset.
FluidSynth applies it to every note on the MIDI channel, whatever the program
of the note. That matches the driver. A player that follows the specification
lets a note of another voice fade out. The option works with `--single-sf2`
and `--voice-channels`. With `--voice-channels`, a cut-off PSG note of a class
fades out over 0.32 s in FluidSynth. The All Sound Off alone would stop it at
once (see [FluidSynth](#fluidsynth)).

With `--single-sf2`, each voice keeps its number as its program. Banks tell
apart the voices that songs play under the same number: the first one is in
bank 0, the next in bank 1, and so on. Each program change in the MIDI files
comes with a bank change. A song that plays on all 16 channels keeps track 9
on channel 10. Its voices get copies in bank 128. If another song has one of
those programs there already, a warning says so. If the songs played more
than 128 voices under one number, the rest would take free program numbers.

### MIDI from Nintendo R&D2's driver

Each track of the sequence that plays notes gets a MIDI track, named
`Track 0`, `Track 1` and so on. Tracks 0 to 8 are on MIDI channels 1 to 9, and
track 9 is on channel 11, away from the drum channel. A separate first track
contains the tempo and loop markers.

The sequences of the driver are numbered first, then its sound effects. A
sound effect is a track of one of the sequences for sound effects, and the
game plays it on another player than the music. Its MIDI file contains the
track and each track that it starts with `F8`. The file ends when the last of
them ends.

The driver counts time in 150ths of a tick, with 24 ticks to the quarter note,
and each frame takes the tempo off the count. The MIDI file has 3600 ticks per
quarter note, one for each 150th. A frame at the driver tempo T is then
exactly T ticks long, and each note stays on its exact tick.

| Game | MIDI |
|---|---|
| note | note on/off. A velocity of v is written as 127 × √(v/127). The level in a SoundFont player goes with the square of the velocity. A note of a PSG voice plays at 127 (see below) |
| `C2` instrument, `C7` bank | program change, numbering the instruments of the sequence in the order that it first plays them |
| `C3` pan, `E0` volume, `EA` the player's volume | CC10 for pan and CC11 for loudness, as with Konami's driver |
| slides, `E1` bend, the LFO | pitch bend, a frame at a time, with the bend range set per channel by RPN 0 |

The MIDI file follows what the driver plays:

* The driver has 7 voices for samples and one for each PSG channel, and a note
  only takes a voice that its priority allows. The MIDI file leaves out the
  notes that the driver can't play, and ends a note where another takes its
  voice.
* When a note takes a sample voice that still sounds, often one in its
  release, the game stops the old note at once. Where nothing else of the
  track of the old note sounds then, the MIDI file writes All Sound Off
  (CC120) on the channel of that track.
* The note of a sample voice ends when the driver releases it. Its length in
  frames depends on the tempo when it starts. The driver also releases it when
  its track starts or ends legato, ends, or restarts.
* The pitch bend of a track follows its newest note.
* The driver plays velocities up to 255, and a MIDI velocity stops at 127. On
  a track whose sample notes play above 127, each velocity v is written as
  127 × √(v/s). Here s is the loudest velocity of the track. The CC11 of the
  track raises its levels by s/127 to make up for it. Where that would take
  CC11 above 127, s is lower, and the loudest notes play quieter than in the
  game.
* The note of a PSG voice plays at full velocity, and its CC11 follows the
  volume of the channel, frame by frame. For each segment of the envelope of a
  square or the noise, the driver works out a whole volume from 0 to 15 for
  the start of the segment and one for its end. The volume comes from the
  velocity, the volumes of the track and the player, and the level of the
  envelope. The PSG envelope then steps the volume from one to the other. The
  wave channel has five levels: none, 25%, 50%, 75% and 100%. At the start of
  each segment, the driver picks one of them from the velocity and the
  volumes. The note lasts until the channel falls silent or another note
  takes the channel. The release of the driver fades the volume out, and the
  note lasts through the fade.
* The CC11 of a sample note follows the envelope of its voice, frame by frame.
  The zone of the note has only the release (see below). Notes of a track that
  sound together share CC11. CC11 then follows the mean of their envelopes,
  weighted by their power. The track plays as loud as in the game, but the
  level of each note can differ from the game. During the release, CC11 keeps
  the level of the envelope that the voice had reached.
* A track in legato plays its notes on one voice, and the voice keeps its
  envelope from note to note. A note lasts until the next note takes the
  voice, or until the voice is released.
* A sequence without a loop ends with its last track. A PSG channel can still
  sound then, while the driver goes on with the fade of a release. The MIDI
  file then ends where the last channel falls silent.
* The PSG plays a note in the middle at the centre pan, 0x40. At any other
  pan, it plays the note on one side alone, and the driver doubles the level
  of the note there, up to the highest volume of the channel. On a track whose
  notes are all PSG notes other than drums, CC10 follows that rule.

*Super Mario Advance 2* plays sequence 30, a jingle, as music 17 and as music
18. Tracks 8 and 9 each contain a version of the last hit of the jingle. The
game mutes track 8 as music 17 and track 9 as music 18. A muted track plays no
notes. The sequence therefore gets two pairs of files: `game_30.mid` and
`game_30.sf2` as music 17, and `game_30_music18.mid` and `game_30_music18.sf2`
as music 18. Each pair leaves out the track that the game mutes.

The game also mutes track 6 of sequences 15 to 18 unless the player rides
Yoshi. A new game starts without Yoshi. The first pair of each of those
sequences, such as `game_15.mid` and `game_15.sf2`, therefore leaves the track
out. A second pair, such as `game_15_yoshi.mid` and `game_15_yoshi.sf2`, plays
it as the game does while the player rides Yoshi.

### SoundFont from Nintendo R&D2's driver

Each sequence's SoundFont has a preset for each instrument that the sequence
plays, named after its bank and number.

* **Samples** are the game's 8-bit samples, widened to 16 bits, with their rate
  and loop. Each is tuned to play at its rate on the note that its region
  gives.
* **Drum kits and key splits** become a zone for each key or range of keys that
  plays the same region. A drum plays at the pitch and pan of its region, and
  an instrument with a sample for each key plays at the pitch of its region
  too. As in the driver, the pan of the track doesn't move a drum. The zone of
  each drum splits it between the sides in the ratio of the driver. A drum
  panned hard to one side plays up to 3 dB quieter there than in the game. On
  a track that the game pans, a drum plays at the loudness of the track, and
  it can then play up to 3 dB louder or quieter than in the game.
* **PSG voices** play the Game Boy's square waves, the 32-step wave of the
  voice, or noise from the Game Boy's LFSR, with a zone for the noise setting
  of each key. The square and wave zones are tuned to the frequency table of
  the driver. Their level relative to the samples matches the mix of the
  driver. As with MP2K, their samples contain that level, not the attenuation
  of their zones. The driver steps some voices through a table of duties or
  noise widths, a frame each, and keeps the last entry. Their zones play that
  entry.
* **Envelopes.** The envelopes of the driver are straight lines in level
  between points. The decay of a SoundFont falls in a straight line in
  decibels instead. The zone of a sample therefore plays at a constant level
  until the release, and the CC11 of its notes follows the envelope of the
  voice (see
  [MIDI from Nintendo R&D2's driver](#midi-from-nintendo-rd2s-driver)). The
  release of a sample reduces its level by a fraction each frame. That's a
  straight line in decibels, as in a SoundFont. The zones of the PSG voices
  play at a constant level, without an envelope, and the CC11 of their notes
  follows the volume of the channel.

With `--single-sf2`, the instruments of each sequence are in the bank numbered
after the sequence. MIDI selects only banks 0 to 127. The instruments of a
sequence numbered 128 or more, and those of a sequence past its 128th,
therefore take free programs in the other banks.

### MIDI from Brownie Brown's driver

The song table lists every sound that the game plays, music and sound effects
alike, and each one is converted as a song. Each MIDI file has a conductor
track with the tempo and loop markers, and a track for each channel that plays
notes, on MIDI channels 1-9 and 11-14, away from the drum channel:

* `Square 1`, `Square 2`, `Wave` and `Noise` for the PSG channels of the music
* `Effect square 1` to `Effect noise` for the PSG channels of the sound effects
* `Samples 1` to `Samples 5`, or in the *Magical Vacation* revision of the
  driver `Samples 1`, `Samples 2`, `Effect samples 1` and `Effect samples 2`

The driver counts time in frames, and has no tempo: a song picks a set of note
lengths, each for a tempo. The MIDI file has 480 ticks per quarter note, and
the tempos of the beat that `agbsf2` works out from the notes (see
[Output](#output)). It starts on the first beat of the song, in the second
frame, when the driver first reads the song data.

| Game | MIDI |
|---|---|
| note | note on/off; velocity is always 127 |
| volume, envelopes, the hardware's envelope, pans, the master level and fades | CC11 for loudness and CC10 for pan, a frame at a time, as with Konami's driver |
| detune and the frequency sweep | pitch bend, with the bend range set per channel by RPN 0 |

Each note uses the MIDI key nearest the pitch in the driver. In *Sword of
Mana*, key 0 of the driver is 33 cents below C#2. It becomes MIDI key 37, and
the square and wave samples in the SoundFont are tuned down 33 cents. In
*Magical Vacation*, key 0 is C2, MIDI key 36. Pitch bends contain the
difference between the pitch of the channel and the entry for its key in the
frequency table. Sample notes use the same keys. Noise notes are drums, with a
key for each distinct sound and 92 sounds per kit. Notes last as long as their
channel sounds: a rest that turns off or mutes a PSG channel, silences a voice
or stops a FIFO ends the note.

### SoundFont from Brownie Brown's driver

Each song's SoundFont contains the instruments that its notes play:

* **Square channels** get a sample for each duty, and the wave channel a sample
  for each wave pattern, tuned to the frequency table of the driver.
* **Noise drums** are rendered from the writes of the driver to the noise
  channel, through the Game Boy's LFSR, envelope and length counter at
  32768 Hz. They include each step of the envelope of the note. Each step
  starts the channel again. As with Quintet's driver, a drum that ends on a
  steady sound loops its last two frames.
* **Sample sets** get an instrument each, with a zone for each key that the
  song plays. The zone plays the sample of the semitone, one of the game's
  8-bit samples widened to 16 bits, with its loop. It plays at the rate of the
  octave, from an eighth of the mixer rate of 16384 Hz up to the full rate. In
  the *Magical Vacation* revision, each sample gets an instrument, with a zone
  for each key that the song plays. The zone is tuned to the rate that the
  table of the driver gives the key, from 1024 Hz at its key 0 up, and a
  detune is a pitch bend.

The levels follow the GBA mixer: a square at full volume is 30/128 as loud as
a full-scale sample. With `--single-sf2`, the instruments of all the songs are
numbered together, in the order the songs first play them, with 128 to a bank.

### MIDI from Krawall

Each module becomes a song. Modules are numbered in the order of the module
table of the game, followed by any modules missing from that table. A module
whose orders separate songs with `+++` becomes a song for each of them,
numbered in turn. As in the song mode of Krawall, each of those songs loops
back to its start at its end. Each MIDI file has a conductor track for tempo
and loop markers, plus a track for every module channel that plays notes.
Tracks are named `Channel 1`, `Channel 2` and so on, on MIDI channels 1-9 and
11-16, away from the drum channel.

A module can have up to 32 channels. In a module with more than 15 that play
notes, channels share MIDI channels. The program, controllers and bend of a
MIDI channel act on every note that it plays, and the channels whose settings
clash least share one. Two notes clash if they sound at once with another pan,
expression or bend, or on the same key. On a shared MIDI channel, each note
sets its program, pan, expression and bend again. It does so again where a
note of another channel on the MIDI channel ends. A note ends where the mixer
of the game keeps its levels at 0 for the rest of the note. The expression of
another channel could otherwise make it sound again. Where notes on one MIDI
channel clash, the converter warns and gives the time.

A module defines tick lengths and ticks per row, but has no explicit beat. The
converter chooses 1, 2, 4, 8 or more rows per quarter note, or per eighth note
in 12/8, to put the tempo of the beat between 105 and 210 BPM. It uses 4/4
unless more of the patterns fit whole bars of 12/8 than of 4/4. For example, a
48-row pattern fits two bars of 12/8 at two rows per eighth note, with a dotted
quarter note as the beat.

Bars follow pattern boundaries. A pattern cut short ends with a shorter bar.
If a break cuts the first order short, as with a lead-in, its partial bar goes
at the start as a pickup, and the next pattern starts on a full bar.

At the most common speed of the module, or that speed doubled or halved any
number of times, each player tick takes the same number of MIDI ticks. A
module can then halve its speed for finer rows without a change of the MIDI
tempo. At other speeds, such as a slow lead-in, a row takes the usual number
of MIDI ticks, with a tempo change that keeps its duration. Playback starts
on the first tick of the module. A loop is detected when a row
returns to an earlier position with the same speed, tempo and pattern-loop
state, for example at a jump or the end of the order list.

A game can play a module once, as a jingle. `agbsf2` finds the jingles
through the routine of the game that plays them. Detection reads that routine in
the form that *Digimon Racing* has. The MIDI file of a jingle plays the module
to its end and stops, with no loop markers. Krawall plays a jingle that way
while no other module plays. `--info` lists the jingles. In *Digimon Racing*,
module 16 is a jingle.

A game can also start a module with a direct call of `krapPlay()`. If no such
call loops the module, the module plays once. Its MIDI file then plays to the
end of the module and stops. *Cocoto Kart Racer* plays five modules that way.
`--info` lists them.

A game can also play its music through a table of what it plays. Each entry
gives a module, the mode for `krapPlay()` and a song. Detection reads such a
table through the routine of the game that plays its entries, in the form that
*Eragon* has. Each entry then becomes a song, in the order of the table, before
the modules that no entry plays. An entry that repeats an earlier one is left
out. The song plays the module in the mode of the entry, from the song of the
entry where the module has markers. In a mode without the loop, the song stops
at the end of the module, or in song mode at the next `+++`. Its MIDI file then
has no loop markers. A module that jumps back to an earlier order still loops,
in any mode. `--info` lists the table and the songs whose mode doesn't loop.

Some games keep each module packed in a music file. Their changed build of
Krawall unpacks a file into RAM before it plays the module, and `agbsf2`
unpacks the files in the same way. It finds them through the words in the
code and data of the game that point to them. The module of each file becomes
a song. The files that the tables of the game list come first, in the order
of the tables. In a game whose code and data point to no music file, `agbsf2`
searches the ROM for music files instead. `--info` lists the files and the
module of each one, and says which of the two ways found them. *Princess
Natasha* plays modules 8 and 9 once, as jingles. `agbsf2` finds them through
the calls in the game that play a file once.

| Game | MIDI |
|---|---|
| a sample that a note, a note delay, a retrig or an offset starts | note on/off; velocity is always 127 |
| the sample, and the point that the note starts from | program change: a preset for each |
| volume, channel volume, global volume, volume and pan envelopes, slides, tremolo, tremor, pan and panbrello | CC11 for loudness and CC10 for pan, a tick at a time, as with Konami's driver |
| portamentos, the slide to a note, vibratos and arpeggios | pitch bend, with the bend range set per channel by RPN 0 |

In a game whose mixer is mono, such as *Babar to the Rescue*, every channel
plays in the centre.

Each note uses the MIDI key nearest its playback rate, with C-4 of the sample
on key 60. A bend corrects pitch differences of 3 cents or more. That
threshold is above the deviation of the period table from equal temperament in
the octaves that modules use. The note lasts until its sample ends, the
player stops it, or the module channel starts another note.

### SoundFont from Krawall

Each module's SoundFont has a preset for each sample and point that its notes
start from, named after the number of the sample in the game's table of samples.
The samples are the game's 8-bit samples, widened to 16 bits, with their loops,
at the rate at which the player plays their C-4, on key 60. A loop back and
forth plays its points forwards and then backwards. A note that starts within
its sample, with an offset, plays a zone that starts there. With `--single-sf2`,
the presets of all the modules are numbered together, in the order the modules
first play them, with 128 to a bank.

### MIDI from Ubisoft Milan's driver

Each piece of music in the sound bank of the game is converted as a song, in
the order of the resources in the bank. A piece plays a list of sequences one
after another, and a piece that loops starts its list again. The samples that
the game plays as music follow as separate songs. Each has one track, `Kit`,
with a note for each pass of the loop of the sample, on a key from key 81 up.

Each MIDI file has a conductor track with the tempo and loop markers, and a
track for each channel that plays notes:

* `Square 1`, `Square 2` and `Wave` for sequence channels 0-2, on MIDI
  channels 1-3
* `Kit` for the samples of channel 9, on MIDI channel 11
* `Noise` for the noise notes of channel 9, on MIDI channel 4

Players treat notes on MIDI channel 10 as drums. Some ignore the note-off of a
drum or the loop of its sample, and the notes of the kit require both.

The driver counts time in frames, and has no tempo. The MIDI file has 480
ticks per quarter note, and the tempos of the beat that `agbsf2` works out
from the notes (see [Output](#output)). It starts with the piece, whatever
silence comes before its first note.

| Game | MIDI |
|---|---|
| note on channel 9 | note on MIDI channel 11: its key plays one of the samples of the kit; a velocity of v is written as 127 × √(v/64) |
| note on channels 0-2 | a MIDI note for each run of frames in which the channel sounds, at full velocity; CC11 sets the level of each frame, 127 × √(v/127) for a square at volume v and 127 × √(30s/127) for the wave at a share s of its full volume; the pitch bend follows the modulation and the glides |
| noise note on channel 9 | a note on MIDI channel 4 in bank 3: a drum that contains the noise sound from the note-on, at CC11 44, 127 × √(15/127) |
| program on channel 9 | bank select 9 and program change: an offset into the kit. Key k plays entry k + offset of the kit |
| program on channels 0-2 | bank select of the channel number and program change: the instrument of the channel |

Each track starts with CC7 (volume) and CC11 (expression) at 127, CC10 (pan)
at 64, and CC91 (reverb) and CC93 (chorus) at 0. The velocities of the kit
notes and the CC11 values of the PSG tracks set the levels. The music is mono,
and the driver adds no reverb or chorus.

On channel 9, keys up to 11 play the noise of the PSG, and the keys above that
play the samples of the kit. The mixer plays a sample at a fixed rate. A key
chooses a sample rather than a pitch, and a melody has a sample for each of
its notes. A program up to 11 on channel 9 goes to the PSG and changes
nothing. Each note on channel 9 takes a free voice, and a note-off stops the
voice of the last note, whatever its key. The MIDI file ends that note there.
Another note that sounds then plays until its sample ends. Notes that the
driver plays at volume 0 make no sound, and the MIDI file leaves them out.

*The Mummy* switches the kit of channel 9 off before most of its levels start
their music. Those levels play songs 1-15 without drums. Later, the game can
switch the kit on. The drums then come in where the song is. Each of those songs
therefore gets two pairs of files: `game_01.mid` and `game_01.sf2` without the
kit, and `game_01_kit.mid` and `game_01_kit.sf2` with it. The game plays the
other songs with the kit. Nobody has found where it plays songs 19 and 20.

### SoundFont from Ubisoft Milan's driver

Each song's SoundFont contains the instruments that its notes play:

* **The kit** gets a preset in bank 9 for each offset that the song uses. Each
  preset has a zone for each key that the song plays. Each zone plays the
  sample of its key unpitched at 8184 Hz, the rate at which the mixer reads
  the sample. The SoundFont widens the game's 8-bit points to 16 bits. The
  mixer moves through a sample 8 points at a time, and a loop lasts a whole
  number of 8 points. As in the game, a note stops at once at its note-off.
* **The noise drums** are in bank 3, from key 36. Each drum contains a sound
  that the noise channel makes from a note-on: its envelope, its length and
  any changes that the driver writes. The drums are rendered at 65536 Hz, the
  PSG output rate in the game.
* **The PSG instruments** get a preset in the bank numbered after their
  channel: a square with the duty of the instrument, or the 64 points of the
  wave program, tuned to the frequency table of the driver.

As in the driver, velocity and CC11 scale the level in a straight line. A
SoundFont player squares them by default. The MIDI file therefore contains the
square root of the level in the driver. The driver sets the master volume of
the PSG to 3 of 7. A square at volume 15 is therefore 60/512 as loud as a
full-scale sample. With `--single-sf2`, the presets of the kit and the PSG
instruments of all the songs go in the one SoundFont.

### MIDI from Nintendo R&D1's driver

Each entry of the song table is a song, and an empty entry is skipped. The
driver plays the MIDI file of the song. The conversion keeps its notes on the
ticks of that file, with as many MIDI ticks to each as make at least 480 to a
quarter note. The tempo map gives those ticks the time that the driver gives
them: the driver steps through a song at a whole number of beats a minute, a
frame at a time. Each event comes at the start of a frame. The markers `[` and
`]` of the file become `loopStart` and `loopEnd`. A song without them plays
once.

The driver plays the events of a track on the channel with the number of the
track. The conversion writes a track for each channel that plays notes, on the
melodic MIDI channels in order. Each track starts with the bank of the song
and its first program, CC7 at 127, the CC91 of the first note and CC93 at 0.
The mixer echoes every sample note, and those notes send the level of the echo
to CC91. PSG notes play outside the mixer and send 0. The level and pan of the
channel go to CC11 and CC10 where the driver changes them, and its bend and
pitch LFO to the pitch wheel. The driver plays a PSG note on the left side,
both sides or the right side, by the CC10 of the channel. The conversion
writes CC10 0, 64 or 127 for it.

The velocities contain the levels of the notes. The velocity of a PSG note
also follows the PSG volume of the driver. That volume reaches its full 15 at
about two thirds of the level of a voice, and it moves in whole steps. The
driver starts the release of a PSG note a frame after the end of the note.
Where the release doesn't silence the note at once, the MIDI note ends there.
A note on square 1 with a sweep bends with the sweep and keeps its first
level. It ends where the sweep or the voice turns it off.

### SoundFont from Nintendo R&D1's driver

Each song's SoundFont has a preset for each program that its notes play, in
the bank of the song record. The zone of a sample plays its points at its
rate. A fixed sample, a record of type `0x46`, plays at the mixer rate on
every key. A PSG instrument plays a square of its duty, the pattern of the
wave channel, or the noise at the setting that the driver gives each key. The
noise samples are at 32768 Hz, the rate at which the GBA samples its PSG in
these games. A drum kit or a key split gets a zone for each key that the song
plays. The volume envelope follows the straight steps of level that the driver
makes each frame. The release in the driver takes longer from a higher level.
The release of a sample zone therefore lasts as long as the release in the
driver from the average level at which the notes of the song on that zone
end.

### MIDI from AlphaDream's driver

Each entry of the song table is a song. Song 0 plays no track and is skipped.
The game starts its music on player 1, and the conversion plays each song
there. Each MIDI file has a conductor track with the tempo and loop markers,
and a track for each track of the driver that plays notes: `Track 0` to
`Track 7` for the samples, then `Square 1`, `Square 2`, `Wave` and `Noise`.
Each track plays on the MIDI channel of its number, skipping channel 10.

The driver runs the ticks of a track a frame at a time. The MIDI file has 480
ticks per quarter note, and the tempos of the beat that `agbsf2` works out
from the notes (see [Output](#output)). A song loops when its tracks jump
back. The loop starts where the last track first jumps back, and it lasts as
long as the most tracks fill with whole passes of their loops. In a looping
player, a track whose loop has another length jumps at each repeat. In song
22 of *Superstar Saga*, track 5 jumps by 18 frames.

Each track starts with the bank and program of its first instrument. The
instruments of a sample track are in banks 0 to 3, those of a square in banks
4 to 7, those of the wave in banks 8 to 11 and those of the noise in banks 12
to 15: 512 preset numbers for each kind of channel. The levels that the driver
gives the two sides of the channel go to CC10 (pan) and CC11 (expression), and
its bend and fine tune to the pitch wheel. A tie changes the pitch of the note
that sounds, and the MIDI file bends that note to the new key. The driver
gives the squares and the noise the hardware envelope. Their notes last while
the channel sounds, and the CC11 of each frame is the mean volume of the
hardware over that frame. The bend of a square or the wave follows the
frequency that the pitch table of the driver gives, a few cents sharp. The
driver gives notes below key 36 the setting 0, and a square plays them at
64 Hz. The wave plays at 25, 50, 75 or 100%, by its volume at the start of the
note.

### SoundFont from AlphaDream's driver

Each song's SoundFont has a preset for each instrument that its notes play.
Each region of the instrument that the song plays becomes a zone over the
keys of the region. The zone of a sample plays its 8-bit points, widened to 16
bits, at the rate of the sample at key 60. A region whose entry in the sample
table is empty gets no zone. A sample with a fixed pitch plays at that rate on
every key. The volume envelope follows the envelope of the mixer: the
straight rise of the attack, a frame at the full level, the sustain level, and
the release. A square plays the duty of its region, the wave its 32 points,
and the noise the setting that the driver gives each key. Like the hardware,
the wave plays an octave below a square at the same key. The squares and the
noise release at once. The CC11 in the MIDI file already follows their
envelopes. The wave releases at once too: the driver silences it at the end of
the note.

### MIDI from Logik State's driver

Each `GBAMOD2.` module becomes a song. Songs are numbered in the order of the
addresses of the modules in the ROM. A `GBAMOD1.` module contains many pieces,
each a run of orders that ends with a jump. Each piece becomes a song, in the
order of the modules and of the pieces within them. `--info` gives the module
and the first order of each song. Each MIDI file has a conductor track for tempo
and loop markers, plus a track for every module channel that plays notes. Tracks
are named `Channel 1`, `Channel 2` and so on, and each uses the MIDI channel of
its number. A module has up to eight channels.

Each `GBAMOD30` module becomes a song too. Songs are numbered in the order of
the game's table of songs, and the modules that the table doesn't name follow
them. Four games have no table, and their songs are numbered in the order of
the addresses of the modules. The games number their songs in that order
too.
The modules of these games have up to ten channels. The track of the tenth
uses MIDI channel 11.

The beat and the bars are chosen as for Krawall's modules: 1, 2, 4, 8 or more
rows per quarter note, in 4/4 or 12/8, with bars that follow the pattern
boundaries. A row takes 120 MIDI ticks, and the tempo gives it its length. A
tick within a row goes on the nearest MIDI tick. The driver counts rows and
ticks in samples of its mixer. `--frame-timing` therefore changes nothing.

A note starts on its row. It ends where the next note of its channel starts,
where its sample ends, or where the volume of its channel falls to 0. The
mixer pauses a sample while the volume of its channel is 0. When the volume
rises again, a new note starts from the point where the sample paused.

The driver divides a semitone into eight pitches, or into 64 in a `GBAMOD30`
module. A note takes the nearest key, and a pitch bend gives the rest where
it's 3 cents or more. Slides, vibratos and arpeggios bend their notes at each
tick. Each track sets the bend range that its widest bend requires. Every note
has velocity 127, and CC11 follows the volume of the channel, tick by tick
during a volume slide.

The game sets a volume for its music, up to 64. The driver takes the difference
between that volume and 64 off the volume of each channel. Quiet notes then fall
silent, and the balance of the channels shifts. The conversion plays each module
at the volume that the game sets when it starts. `--info` gives that volume.

The builds that play `GBAMOD30` modules multiply instead. The mixer plays a
channel at its volume times the music's volume and a master volume, each over
64. A game can set either one above 64. No note falls silent. The conversion
plays each module at the two volumes that the game sets when it starts.

A loop is detected when an order and row come back with the same speed and
tempo. The MIDI file marks it with `loopStart` and `loopEnd`. Where the second
pass starts its notes unlike the first, with another sample, level or pitch,
the marked loop starts at the second pass. A jingle ends in a loop of empty
rows. Its MIDI file has no loop and ends where that loop would start.

A `GBAMOD30` module loops only while the driver's loop flag is set. The games
set the flag for their music and clear it for a few jingles. A module without
the flag stops after its last order, and its MIDI file ends there. A jump in
the patterns of a module loops it without the flag. The build of *Super Puzzle
Fighter II Turbo* has no flag.

*Pinball Challenge Deluxe* starts a jingle through a routine that returns to the
music at the first jump of the jingle. A piece that the game starts only that
way plays once in its MIDI file, up to that jump. `--info` marks it as a jingle.
A piece that the game starts as music loops where its jumps lead back. A piece
that nothing in the game starts plays as the driver would play it.

### SoundFont from Logik State's driver

Each module's SoundFont has a preset for each sample and point that its notes
start from. The samples are the game's 8-bit samples, widened to 16 bits, with
their loops. The rate of each sample puts ProTracker's C-2, pitch 200 in the
driver, on key 60, with the fine tune of the sample. Where a loop ends before
its sample does, the driver plays the whole sample once and then the loop. The
sample in the SoundFont contains those points in that order. A note that starts
within its sample, after a pause, plays a zone that starts there. With
`--single-sf2`, the presets of all the modules are numbered together, in the
order the modules first play them, with 128 to a bank.

A game with `GBAMOD30` modules has one bank of samples for all of them. Each
song's SoundFont has the samples that the song plays. The rate of a sample
puts note 49 of the module on key 60, with the relative note and fine tune of
the sample.

### MIDI from Webfoot Technologies' driver

Each song of a song table becomes a MIDI file. Songs are numbered in the order
of the song tables in the ROM, and of the songs within each table. Each MIDI
file has a conductor track for tempo and loop markers, plus a track for every
channel of the song that plays notes. Tracks are named `Channel 1`, `Channel
2` and so on. A song has up to 15 channels, and they use the MIDI channels
other than the drum channel.

The beat and the bars are chosen as for Krawall's modules. A row takes 120
MIDI ticks, and the tempo gives it its length. The driver counts rows and
ticks in samples of its mixer. `--frame-timing` therefore changes nothing.

A note starts where its channel starts its sample: at the row of the note, at
a retrigger or at the tick of a delayed note. It ends where the channel stops
the sample or starts it again, or where the sample ends. A note takes the key
nearest the rate of the voice, and a pitch bend gives the rest. Slides and
vibratos bend their notes at each tick. At each tick, the levels of the two
sides set the pan (CC10) and expression (CC11) of the channel.

The game sets a music volume from 0 to 8. The level of a side is the music
volume × the channel volume × (64 ∓ the pan), shifted right by 10 bits. At
the volume of 4 that the games set, a channel in the centre at volume 3 or
less is silent. The
conversion plays each song at the volume that its game starts with. `--info`
gives that volume.

A loop is detected when an order and row come back with the same speed and
tempo. The MIDI file marks it with `loopStart` and `loopEnd`. A jingle ends
in a loop in which no channel sounds. Its MIDI file has no loop and ends where
that loop would start.

### SoundFont from Webfoot Technologies' driver

Each song's SoundFont has a preset for each sample and point that its notes
start from. The samples are the game's 8-bit samples, widened to 16 bits, with
their loops. The rate of each sample puts note 60 of the driver on key 60, at
the rate that the mixer plays it. A ping-pong loop becomes the points of the
sample to its end, then the points of the loop backwards, and the SoundFont
loops over both.
With `--single-sf2`, the presets of all the songs are numbered together, in
the order the songs first play them, with 128 to a bank.

### MIDI from Frontline Studios' MOD player

Each song becomes a MIDI file. Each MIDI file has a conductor track for tempo
and loop markers, plus a track for every channel of the module that plays
notes. Tracks are named `Channel 1` to `Channel 4`.

The beat and the bars are chosen as for Krawall's modules. A row takes 120
MIDI ticks, and the tempo gives it its length. The player reads a row or plays
a tick once a frame. `--frame-timing` therefore changes nothing.

A note starts where its channel starts its sample: at a note, or where a
sample offset moves the channel within its sample. It ends where the channel
stops the sample or starts it again, or where the sample ends. A note takes the
key nearest the step of the channel, and a pitch bend gives the rest. Slides
and vibratos bend their notes at each frame. On the GBA, a loop can play 3
cents or more off the step of the channel. The note then bends to the pitch
of the loop once its loop first starts again (see [Accuracy](#accuracy)). The
level of the channel sets its expression (CC11). The player sets the level at
each row, from the volume of the channel and the volume of the music. The
mixer plays one channel of sound for both sides, and every channel stays in
the centre.

The game starts a song with a fade step: the music's volume rises by the step
at each row, up to its full volume. *Hardcore Pool* and *Garfield* start every
song at full volume. *Games Explosion!* starts most songs with a fade-in over
eight rows, and the conversion plays the fade-in as the game does.

A loop is detected when an order and row come back with the same speed and the
music at the same volume. The MIDI file marks it with `loopStart` and
`loopEnd`.

### SoundFont from Frontline Studios' MOD player

Each song's SoundFont has a preset for each sample and point that its notes
start from. The samples are the game's 8-bit samples, widened to 16 bits, with
the loops of their instruments. The mixer plays a loop up to and including the
point at its end. That point can be the first byte after the sample, and the
loop in the SoundFont includes it. Period index 4608 of the player plays a
sample at 8,363 points a second, and the rate of each sample puts that index on
key 60. With `--single-sf2`, the presets of all the songs are numbered together,
in the order the songs first play them, with 128 to a bank.

### MIDI from Frontline Studios' XM-like player

Each song becomes a MIDI file. Each MIDI file has a conductor track for tempo
and loop markers, plus a track for every channel of the module that plays
notes. Tracks are named `Channel 1` to `Channel 4`.

The beat and the bars are chosen as for Krawall's modules. A row takes 120
MIDI ticks, and the tempo gives it its length. The player counts its ticks in
samples of its mixer, and a tick can fall inside a frame. `--frame-timing`
changes nothing.

A note starts where its channel starts its sample: at a note, at the tick of a
delayed note, or where a sample offset moves the channel within its sample. It
ends where the channel stops the sample or starts it again, or where the
sample ends. A note takes the key nearest the step of the channel, and a pitch
bend gives the rest. Slides, vibratos and arpeggios bend their notes at each
tick. At each tick, the channel volume × the master volume sets the expression
(CC11) of the channel. The mixer plays one channel of sound for both sides,
and every channel stays in the centre.

A loop is detected when an order and row come back with the same speed and
BPM. The MIDI file marks it with `loopStart` and `loopEnd`.

### SoundFont from Frontline Studios' XM-like player

Each song's SoundFont has a preset for each sample and point that its notes
start from. The samples are the game's 8-bit samples, widened to 16 bits, with
the loops of their instruments. When the mixer goes back from the end of a
loop to its start, it keeps the fraction of the step past the end. A loop
therefore plays at its length. The rate of each sample puts note 48 of its
instrument on key 60, with the finetune of the instrument. With
`--single-sf2`, the presets of all the songs are numbered together, in the
order the songs first play them, with 128 to a bank.

### MIDI from Engine Software's driver

Each song becomes a MIDI file. Each MIDI file has a conductor track for tempo
and loop markers, plus a track for every channel of the song that plays
notes. Tracks are named `Channel 1` to `Channel 8`.

The beat and the bars are chosen as for Krawall's modules. A row takes 120
MIDI ticks, and the tempo gives it its length. The driver counts its ticks in
samples of its mixer, and a tick can fall inside a frame. `--frame-timing`
changes nothing.

A note starts where its channel starts its sample: at a note, at a retrigger,
or where a sample offset moves the channel within its sample. It ends where
the channel stops the sample or starts it again. A note takes the key nearest
the step of the channel, and a pitch bend gives the rest. At each tick, the
level of the channel sets its expression (CC11). The level is the product of
the channel volume, the music volume, the volume envelope of the instrument
and its fade-out. The expression therefore plays the envelope and the
fade-out in steps of a tick. The mixer plays one channel of sound for both
sides. Every note stays in the centre, and the pans of the song have no
effect.

Each song starts as the game's sound manager starts it, with the music volume
rising from 0 over 53 frames. A song that loops from its start then gets its
loop from its second pass, where the volume has stopped rising. The jingle
plays once and ends. A loop is detected when an order and row come back with
the same speed and tempo. The MIDI file marks it with `loopStart` and
`loopEnd`.

### SoundFont from Engine Software's driver

Each song's SoundFont has a preset for each instrument and point that its
notes start from. A sample is named `Sample` and the number of its instrument
in the bank, from 1. The samples are the game's 8-bit samples, widened to 16
bits, with the loops of their instruments. When the mixer goes back from the
end of a loop to its start, it keeps the fraction of the step past the end. A
loop therefore plays at its length. The rate of each sample puts note 49 of
the driver on key 60, with the relative note and the fine tune of the
instrument. The mixer stops a channel at once, and the zones keep the
shortest release that the format allows.

The zones contain no volume envelope and no fade-out. The driver counts the
steps of an envelope in ticks, which follow the tempo, and its fade-out falls
in a straight line of amplitude. Neither fits a zone. The MIDI file plays both
through the expression of each note. The SoundFont played alone, or a note
added in an editor, plays each instrument at a steady level. With
`--single-sf2`, the presets of all the songs are numbered together, in the
order the songs first play them, with 128 to a bank.

### Sequence listing (`--dump`)

The listing shows every track command with its address, raw bytes and
meaning. What else it shows depends on the driver:

* **Konami's driver:** the frame and the delay after each command. The listing
  follows each call.
* **Rare's driver:** the tick of each command.
* **Quintet's and Nintendo R&D2's drivers and MP2K:** each track through its
  repeats, calls and patterns, to the end of the track or its first loop, with
  the tick of each command. For Nintendo R&D2's driver, the listing also
  follows each track that `F8` starts.
* **Brownie Brown's driver:** the same as for Quintet's, with frames in place
  of ticks.
* **Krawall:** the header and orders of the module, and the rows of each
  pattern, with the note, instrument, volume column and effect of each
  channel.
* **Ubisoft Milan's driver:** each sequence of the piece, with the frame of
  each command.
* **Nintendo R&D1's driver:** every event of the MIDI file of the song.
* **AlphaDream's driver:** the commands of each track. The listing follows a
  goto forward to its target.
* **Logik State's driver:** the header, orders and samples of the module, and
  the rows of each pattern.

Webfoot Technologies' driver and Frontline Studios' two players have no
listing yet, and `--dump` gives an error for their songs. Use the listing to
study a song or to check the format documentation against real data.

## Accuracy

Some things differ from the hardware, or can't be expressed in MIDI and
SoundFonts.

### Konami's driver

* **Timing.** The MIDI file puts each note on the beat that `agbsf2` works out.
  A note can be up to a frame and a half from where the game plays it.
  `--frame-timing` keeps each note on its frame instead. Where a song has no
  steady beat, its notes keep their frames.
* **PSG tuning.** The Game Boy's 11-bit frequency registers can't hit every
  pitch, and the game plays its high PSG notes slightly out of tune. The
  frequency table of the driver is within about 3 cents of equal temperament
  below C4, but up to about 7 cents off in the C4 octave, 13 in the C5 octave,
  25 in the C6 octave and 50 in the C7 octave. The wave channel plays an octave
  lower than the squares from the same table. The square and wave zones of the
  SoundFont play each note at its entry in the table, to the nearest cent. A
  bend moves the pitch from there in equal temperament. The game plays the entry
  in the table for the bent pitch. In the Dungeon Dice Monsters revision, the
  bends follow each note's frequency setting instead.
* **Echo.** The echo of the driver is a feedback delay (up to about 190 ms).
  The reverb send approximates it, and the conductor track contains the delay
  and feedback as text events.
* **Mixer character.** The GBA mixer resamples without interpolation at
  20 or 21 kHz and writes 8-bit output. The Dungeon Dice Monsters revision has
  no mixer, and plays 8-bit samples at their rate of 10 kHz. A SoundFont
  player plays the same samples more cleanly.
* **PSG DC offset.** The Game Boy channels output a unipolar signal. The
  output capacitor of the hardware removes the DC offset of the signal. In the
  SoundFont, the wave channel's patterns have no DC offset. The square waves
  have one at duty cycles other than 50%.
* **The game's samples.** *Hunter X Hunter: Minna Tomodachi Daisakusen!!*
  requests some songs with a list or a table of samples that it builds as it
  runs. In the game, notes of those songs play the samples that the game
  chose. The conversion plays each song with the samples that the song data
  gives its notes.

### Rare's driver

* **Voice limit.** The driver mixes at most 8 voices at a time and leaves the
  rest silent until voices free up. The MIDI file mutes the notes that the
  driver leaves out with key pressure (see
  [MIDI from Rare's driver](#midi-from-rares-driver)). A player that ignores
  key pressure, or both the mute modulator in the SoundFont and the General
  MIDI 2 message, plays every note. A busy passage then has notes that
  the game doesn't let you hear. Where the driver leaves out one of two notes of
  a key on a channel, the MIDI file plays both.
* **Envelopes.** The envelopes in the SoundFont approximate the straight fades
  of the driver, and some players treat very short envelope phases
  differently.
* **Timing.** The driver runs once a frame and plays every event on a frame
  boundary. Unless `--frame-timing` puts an event at the start of its frame,
  the MIDI file keeps it on its tick. That tick can be up to a frame later, or
  on the track that sets the tempo, up to a frame earlier. A delay that's
  counting down when the tempo changes runs on at the old tempo. After a
  change partway through a tune, the MIDI file can then put the notes of the
  other tracks several frames off, unless `--frame-timing` is used.
* **Mixer character.** The driver mixes in mono at 13379 Hz with linear
  interpolation, and writes 8-bit output. A SoundFont player plays the same
  samples more cleanly, and the MIDI files leave every channel in the centre.
* **The game's settings.** In *Donkey Kong Country 3*, the game can start a
  tune at a marker partway through. The conversion plays each tune from its
  start. The game also fades Enchanted Riverbank from one arrangement to the
  other when the player dives in or climbs out. Each file of the tune contains
  one arrangement from start to end (see
  [MIDI from Rare's driver](#midi-from-rares-driver)).
* **Sound effects** aren't converted. The driver plays them from a separate
  table that only the game's code uses.

### Quintet's driver

* **Timing.** The driver plays each note at the start of the frame that its
  tick falls in. Unless `--frame-timing` puts a note at the start of its
  frame, the MIDI file keeps it on its tick, up to a frame later.
* **Tuning.** The frequency table of the driver is within about 3 cents of
  equal temperament in the C4 octave, but up to about 27 cents flat in the C2
  octave, 9 cents in the C3 octave, 19 to 34 cents off in the C5 to C7
  octaves, and 43 in the top octave. The PCM pitch table is within 7 cents,
  apart from its top entry. That entry is 14 cents flat. The square and wave
  zones of the SoundFont play each key at its entry in the frequency table, to
  the nearest cent. PCM notes play in equal temperament.
* **Noise drums.** The envelope of the noise channel steps on the 64 Hz clock
  of the sound hardware, and that clock isn't tied to the frames. In the game,
  the volume steps of a drum can come up to 1/128 s earlier or later than in
  its sample.
* **Pitch changes during a PCM note.** A loop of a sample is as long as the
  driver makes it at the rate that the note starts at. A note whose rate then
  changes, with a slide or the LFO, keeps those lengths.
* **Mixer character.** The GBA plays the 8-bit samples of the FIFOs without
  interpolation. A SoundFont player plays them more cleanly. In the SoundFont,
  the patterns of the wave channel have no DC offset. The square waves have
  one at duty cycles other than 50%. On the hardware, the output capacitor
  removes the DC offset.
* **Sound effects** play on their own in their conversions. In the game, a
  sound effect takes each sound channel that it plays on from the music while
  it plays.

### Nintendo R&D2's driver

* **Timing.** The driver plays each note at the start of the first frame that
  reaches its tick. Unless `--frame-timing` puts a note at the start of that
  frame, the MIDI file keeps it on its tick, up to a frame earlier.
* **Pitch bends.** The driver bends each voice on its own, and its LFO starts
  again with each note. A MIDI channel has one bend, and it follows the newest
  note of the track. Notes that play together on a track share it.
* **Envelopes.** CC11 follows the envelope of each sample voice frame by
  frame. Notes of a track that sound together share CC11. Where they stand at
  different points of their envelopes, the track plays at the level of the
  game, but each note can play louder or quieter than in the game. That
  happens most where the release of a note sounds under the next note of the
  track.
* **Legato.** The driver plays the notes of a track in legato on one voice,
  and the voice keeps its envelope. CC11 keeps the level of the envelope from
  note to note. The voice also plays the sample of the new note on from the
  point that it had reached in the last one. A SoundFont player starts the
  sample of each note again. The MIDI file ends a note in legato where the next
  one starts. The release of the note then sounds for a moment under the next
  note. The driver changes the note of the voice at once.
* **Cut notes.** The game stops a note at once where another note takes its
  sample voice. The MIDI file stops it with All Sound Off where nothing else of
  its track sounds. Where another note of the track still sounds, the release
  of the cut note plays on. A song with a note that its instrument has no
  region for gets no All Sound Off. That's sequences 0, 1 and 21 of *Super
  Mario Advance 2*.
* **Echo.** The driver can add an echo to the voices of tracks with an echo
  send, but only the game's code turns it on. The conversion leaves it out.
  *Super Mario Advance 2* and *Super Mario Advance 3* turn it on in some
  levels. No track of their music has an echo send, and in those levels the
  echo reaches only the sound effects. `agbsf2` converts the effects without
  it. *A Link to the Past* never turns it on.
* **PSG.** The MIDI file changes the CC11 of a square or the noise at the start
  of a frame. The PSG envelope steps the volume at 64 Hz, and the step in the
  hardware can come up to a frame before the step in the MIDI file. Below
  100%, the wave channel scales each 4-bit step of the wave and drops the
  fraction. The SoundFont plays the wave unrounded at the share of the level.
  At 25% and 50%, the wave in the game then plays a little quieter and
  coarser.
* **Mixer character.** The driver mixes in stereo at 10512 Hz without
  interpolation, and writes 8-bit output. A SoundFont player plays the same
  samples more cleanly.
* **Doubled notes.** A MIDI channel plays a key only once. A track that plays
  a key twice at once is therefore up to 6 dB louder in the game than the one
  MIDI note that it becomes.
* **Tracks the game mutes.** A game can mute some tracks of a sequence while
  the sequence plays. `agbsf2` reads only the mutes of two routines of *Super
  Mario Advance 2*: the play-music routine and the routine for the track that
  plays while the player rides Yoshi. For another game, the MIDI file has every
  track.
* **Voices and volumes the game sets.** A game can also give the driver fewer
  sample voices, or lower the volume of the music, when it plays a sequence.
  `agbsf2` reads the settings that the play-music routine of *Super Mario
  Advance 3* makes. For another game, the MIDI file plays every sequence with
  all 7 sample voices, at the default volume of the driver.
* **Sound effects** play on their own in their conversions. Each has all 7
  sample voices of the driver. In the game, a sound effect and the music take
  voices from each other by their priorities.

### MP2K

* **Timing.** The driver plays each event at the start of the first frame that
  reaches its tick. Unless `--frame-timing` puts an event at the start of that
  frame, the MIDI file keeps it on its tick, up to a frame earlier. With
  `--frame-timing`, the MIDI file has 3600 ticks per quarter note, one for
  each 150th of a tick. Each frame then starts on a whole tick.
* **PSG tuning.** As with Konami's driver, the 11-bit frequency registers of
  the PSG put the high PSG notes of the game slightly out of tune. The table
  of the driver and its bends round down. The square and wave zones of the
  SoundFont play each key at the frequency setting that the driver gives it,
  to the nearest cent. A bend moves the pitch from there in equal temperament.
* **PSG bends.** A track that plays samples and PSG notes at once has one pitch
  bend for both. That bend moves a noise note smoothly and can take a square
  or wave note below C2. The driver steps through its noise settings and stops
  at C2.
* **PSG sweep and length.** Square channel 1 can sweep the frequency of a note
  up or down. A PSG voice can have a length, and the hardware stops the note
  after it. The SoundFont leaves both out.
* **Notes cut off.** The driver stops a note at once when it gives the channel
  of the note to another, even partway through its release. Without
  `--voice-channels`, a SoundFont player lets the note fade out instead.
  `--psg-exclusive` shortens the fade of a PSG note that the next note of its
  track cuts off. In FluidSynth, the note then fades out in 0.32 s.
* **Changes that miss older notes.** When a track changes its volume, pan or
  pitch in the tick in which it starts a note, the driver gives the change to
  the new note alone. The older notes of the track keep their volume, pan and
  pitch until the next change of the track. The MIDI file changes them all.
* **Echo.** A track can give its notes a quiet echo after their release. The
  release in the SoundFont doesn't include it.
* **The wave below 100%.** The hardware drops the fraction of each point of
  the wave pattern at 25%, 50% and 75%. The SoundFont plays the pattern
  unrounded, at the level of the rounded pattern. The wave in the game sounds
  coarser there, most of all at 25%.
* **PSG notes above velocity 127.** The velocity of a PSG note follows the
  level that the hardware gives it, up to 127. A note that would require more
  plays at 127, quieter than in the game. The quietest notes lose most. At the
  lowest level of the PSG, the driver rounds the sustain of a note up, and the
  note doesn't decay.
* **Reverb.** The reverb of the driver is a short echo of its output. The
  reverb send approximates it.
* **Changed voices.** A track can change its copy of a voice with extended
  commands, and a note can start partway into its sample. The SoundFont plays
  the voices of the voice group as they are, from the start. With
  `--psg-exclusive`, the exclusive classes follow those voices too. A track
  that moves its voice to another PSG channel can then end a note that the
  driver keeps playing.
* **Sound register writes.** A track can write to the sound registers. The
  conversion leaves these out, with a warning.
* **Mixer character.** The mixer rate of the driver is often 13379 Hz. The
  driver mixes with linear interpolation and writes 8-bit output. A SoundFont
  player plays the same samples more cleanly.
* **Camelot's synth voices.** The mixer makes its waves at its rate, 31536 Hz,
  and the samples in the SoundFont contain them at the pitch of each key. A
  pitch bend moves the cycle of duties of a pulse wave and the filter of a saw
  wave with the pitch. The game keeps them as they are. The MIDI file puts the
  key shift of a track in the pitch bend, and a key shift moves them too. The
  loop of a sample ends at the
  nearest whole cycle of its wave, up to half a cycle from where the cycle of
  the duty ends. The single cycle of the triangle wave leaves out the faint
  aliasing that the mixer adds.
* **Camelot's echo.** The echo of the mixer comes back a frame and four frames
  later, and from the right side to the left after 704 points. The reverb send
  only approximates it. In *Mario Tennis: Power Tour*, the echo comes back a
  frame and seven frames later, and from the right side to the left after 352
  points.

### Brownie Brown's driver

* **Timing.** The MIDI file puts each note on the beat that `agbsf2` works out.
  A note can be up to a frame and a half from where the game plays it.
  `--frame-timing` keeps each note on its frame instead. Where a song has no
  steady beat, as in most sound effects, its notes keep their frames.
* **The hardware's envelope.** The envelope of a PSG channel steps on the
  64 Hz clock of the sound hardware, and that clock isn't tied to the frames.
  CC11 follows a model of it a frame at a time. A step can then come up to a
  frame later than in the game, and the steps of a noise drum up to 1/128 s
  earlier or later.
* **Mixer character.** The driver mixes at 16384 Hz and writes 8-bit output.
  It plays the lower octaves by repeating each point. When it mixes all five
  voices, it interpolates only between the points that it plays twice. A
  SoundFont player plays the same samples more cleanly. In the SoundFont, the
  patterns of the wave channel have no DC offset. The square waves have one at
  duty cycles other than 50%. The *Magical Vacation* revision plays each
  sample straight through a FIFO. It scales each point to the volume, a step
  lower for most negative points. A sample that doesn't loop stops about 20
  points before its end, and those points play at the start of the next note
  on the FIFO.
* **Sound effects** play on their own in their conversions. In the game, a
  sound effect takes its PSG channel from the music while it plays.

### Krawall

* **Mixer character.** The mixer mixes at 16384 Hz and writes 8-bit output.
  Unless the game asks it to interpolate, it plays each sample point by point.
  A short looped wave then gains tones that aren't harmonics of its note.
  Those tones can come close to the harmonics in level. A SoundFont player
  interpolates and plays the same samples more cleanly. For a game that never
  asks for interpolation, such as *Digimon Racing*, FluidSynth comes closer to
  the game at the mixer rate with interpolation off:

  ```sh
  fluidsynth -ni -f nointerp.txt -r 16384 -F song.wav song.sf2 song.mid
  ```

  The file `nointerp.txt` contains the line `interp 0`. The game clips a level
  past the full scale of the mixer. The conversion plays it at full scale.
* **The game's settings.** The conversion plays each module on its own, at the
  music volume that the game sets before it plays music. *Eragon*, *Puppy
  Luv: Spa and Resort* and the games with packed music files set it from their
  options, and the conversion plays at the default of the options. *The Sims:
  Bustin' Out* plays its music at 255. There the loudest notes of a few pieces
  would require more than the full loudness of a MIDI file. The conversion plays
  it at 128, 6 dB quieter than the game, with the same balance. *The Urbz:
  Sims in the City*, *Robots* and *Shrek SuperSlam* play their music at 255
  too, and their conversions also play at 128. *Juka and the Monophonic
  Menace* fades its music in from silence at each start. The conversion fades
  in the same way, and its marked loop then starts on the second pass.
  `agbsf2` contains the volumes that the games set and the fade-in for the
  releases that its tables list, by game code. Another release, such as one
  from another region, plays at 128 without a fade. *Cocoto Kart Racer* and
  *Cocoto Platform Jumper* set a volume for each piece, and the conversion
  plays each piece at that volume. A piece that the game plays at two volumes
  converts at one of them. The level music of *Cocoto Platform Jumper*
  converts at 73, the volume after the tip of a level. The game plays it at 88
  after a lost life, 1.6 dB louder. A game that changes the volume of the
  music at other times does so outside the module.
* **More than 15 channels.** MIDI has 15 channels besides the drum channel,
  and the channels of a larger module share them (see
  [MIDI from Krawall](#midi-from-krawall)). Where two channels on one MIDI
  channel play notes at once, the pan, expression and bend of each act on the
  notes of the other. A note that starts on a MIDI channel can also change the
  release of the note before it. Some pieces of *Puppy Luv: Spa and Resort*
  have notes that overlap that way.
* **Sound effects** that the game plays on the channels of the mixer aren't
  part of the modules. The conversion leaves them out.
* **Samples in music files.** A music file can list samples that the game
  loads into RAM with its module. No music file of the supported games lists
  any. `agbsf2` leaves out a module whose file lists samples, and warns.

### Ubisoft Milan's driver

* **Timing.** The MIDI file puts each note on the beat that `agbsf2` works out.
  A note can be up to a frame and a half from where the game plays it.
  `--frame-timing` keeps each note on its frame instead.
* **Mixer character.** The mixer moves through each sample at 8184 Hz, makes
  twice as many points by putting one halfway between each pair, averages each
  point with the one before, and writes 8-bit output. A SoundFont player plays
  the same samples more cleanly and brighter: the averaging takes up to 3 dB
  off the top octave.
* **The PSG.** The conversion follows the PSG frame by frame: the envelopes,
  the length counters, the square 1 sweep, the modulation and glides of the
  instruments, and the noise. A note on an instrument with legato starts a new
  MIDI note where the game continues without a restart. Neither game has such
  an instrument. In the game, the modulation, glide and wave of a channel stay
  from one piece to the next. The conversion starts each piece from the state
  after the driver init.
* **A key outside the kit.** In the game, a note on a key that the kit of
  channel 9 has no sample for plays bytes from elsewhere in the cartridge, past
  the end of the table of the sound bank, until its note-off. The conversion
  leaves the note out, with a warning.
* **Sound effects** aren't converted. The game plays them straight from its
  samples.

### Nintendo R&D1's driver

* **What the game changes.** The conversion plays a song at its first state:
  the tempo, pitch and volume that its file gives. *WarioWare, Inc.* speeds
  up the music of a stage as the stage goes on. The scripts of *Rhythm
  Tengoku* change the speed, the pitch and the volume, and in some of its
  games they silence groups of tracks.
* **The echo.** The mixer of the driver feeds its output back after 117 ms,
  and every sample note has an echo. Those notes send the level of the echo to
  CC91, and the reverb of the player stands in for the echo.
* **Random pitches.** CC82 and CC83 give each note a random pitch or key in
  the game. The conversion plays notes at their written pitch.
* **Inverted sides.** In the game, CC75 inverts the right side of a voice. The
  voice then sounds hollow on headphones and cancels in mono. The conversion
  plays the voice as usual.

### AlphaDream's driver

* **What the game changes.** The game fades its music in and out, and plays it
  at half volume in some scenes. In one cutscene it changes the tempo of song
  29. Sound effects take a track from songs 7 and 40 while they play. The
  conversion plays each song as the game starts it, at full volume and at the
  tempo of the driver.
* **Pitch.** The mixer moves in a straight line in frequency between
  semitones. The MIDI file's bends follow a curve. They differ by up to 2
  cents.

### Logik State's driver

* **Mixer character.** The mixer mixes at 21024 Hz in mono and writes 8-bit
  output. Nine of the games with `GBAMOD30` modules mix at 10512 Hz. The mixer
  plays each sample point by point, without interpolation. A SoundFont player
  interpolates and plays the same samples more cleanly. Played by FluidSynth
  2.6.2, five songs each of *Aero the Acro-Bat* and *Zapper* are up to 24 dB
  below what mGBA 0.11.0 plays in the bands from 8 kHz up.
  FluidSynth rendered them with its reverb and chorus on. It applies 0.4 of
  the attenuation of a zone, and it plays every note 1.5 dB louder than the
  SoundFont specifies (see [FluidSynth](#fluidsynth)). The mixer also cuts the
  output of each channel to 8 bits before it adds the channels. A quiet note
  loses detail there, and the conversion plays it at its level. In the builds
  for `GBAMOD30` modules, the cut rounds down. A channel whose sound is smaller
  than one step of the output then still moves the output by a step. The game
  plays a faint noise where the conversion is almost silent. In song 1 of
  *Totally Spies! 2: Undercover*, that noise fills the rests, about 23 dB under
  the loudest passages of the song.
* **The game's settings.** The conversion plays each song at the music volume
  that the game sets when it starts. *Aero the Acro-Bat* sets 48 of 64,
  *Zapper* 32 of 64, the default of its options, and *Pinball Challenge
  Deluxe* 63 of 64. The driver takes the difference between that volume and
  64 off the volume of each channel, and a channel at that difference or less
  is silent. At 48, *Aero the Acro-Bat* plays all but 18 of its 51,211 notes.
  At 32, *Zapper* plays 68,719 of its 125,285 notes, and the others stay
  silent in the MIDI files. `--music-volume 64` converts at the full volume.
  The conversion doesn't follow the fades of the game, or a change of the
  volume in its options. The builds for `GBAMOD30` modules multiply
  by the volume of the music and by a master volume. The conversion plays at
  the two volumes that each game sets when it starts its first songs, and
  `--info` gives them. Both can be above 64. For these games, `--music-volume`
  changes only the level of the whole song, and takes up to 256. The level of
  a channel stops at the full scale of a sample. At the highest volumes, the
  loudest channels therefore play quieter than in the game.
* **The sound's switch.** *Aero the Acro-Bat* switches the sound off for a
  moment at its start, and *Pinball Challenge Deluxe* does so when a table
  starts. In mGBA, that leaves the sample channels of the console at half
  volume, and the conversion plays at that level. *Pinball Challenge Deluxe*
  plays its title music before the switch, at twice the level of its files.
  *Medal of Honor: Infiltrator* switches the sound off for a frame in its menus,
  and its conversion plays at the half level too. The music of its title and
  menus plays before that, at twice the level of its files. In *Alex Rider:
  Stormbreaker*, *Alien Hominid*, *Family Feud* and *Need for Speed: Porsche
  Unleashed*, only the pause menu switches the sound off. Their conversions play
  at the full level.
* **Slides in *Pinball Challenge Deluxe*.** This build slides the pitch of a
  note as a step through its sample, and a slide down can take the step to 0
  or below. The sample then stands still or plays backwards. The conversion
  plays such a note at level 0 from there. That happens to 12 notes of the
  pieces of the game, for 2.9 s in all. The driver also keeps the last output
  of a sample after its end, as a constant level that no listener hears. The
  conversion ends the note there.
* **Overflow.** The sum in the mixer wraps where it passes the range of a
  byte. The conversion plays those places without the wrap.
* **The rate in *Need for Speed: Underground 2*.** The game starts the driver
  again at 21024 Hz for one song and at 10512 Hz for the others. The
  conversion models every song at 10512 Hz. The rate changes the character of
  the mixer and the rounding of the length of a tick, by a fraction of a
  percent.

### Webfoot Technologies' driver

* **Mixer character.** The mixer mixes at 13,981 Hz in *The Legacy of Goku* and
  at 15,978 Hz in the later games, in stereo, and writes 8-bit output. It plays
  each sample point by point, without interpolation. A SoundFont player
  interpolates and plays the same samples more cleanly. Played by FluidSynth
  2.6.2, 26 songs of the four games are within 1 dB of what mGBA 0.11.0 plays
  in 94 to 96% of their 50 ms windows. The bands from 5 kHz up are up to 31 dB
  below the game in mGBA. FluidSynth rendered them with its reverb and chorus
  on. It applies 0.4 of the attenuation of a zone, and it plays every note
  1.5 dB louder than the SoundFont specifies (see [FluidSynth](#fluidsynth)).
  The mixer also clips the sum of the channels to a byte, and the 2002 build
  wraps the sum of each side at 16 bits. The conversion plays those places
  without the clip or the wrap.
* **The game's settings.** The conversion plays each song at the music volume
  that the game starts with: 4 of 8, and 3 of 8 in *GT Transformation*. At 4,
  a channel in the centre at volume 3 or less is silent, and at 3, one at
  volume 5 or less. `--music-volume 8` converts at the full volume. The
  conversion doesn't follow a change of the volume in the options of the game,
  or the half volume of one screen of *GT Transformation*.
* **Ping-pong loops in *The Legacy of Goku*.** At the start of a ping-pong
  loop, the 2002 build turns to a place below the start. It can play several
  hundred points from before the loop. The conversion plays such a loop as a
  reflection, at the pitch of the voice's step.

### Frontline Studios' MOD player

* **Mixer character.** The mixer mixes at 18,157 Hz in mono and writes 8-bit
  output. It plays each sample point by point, without interpolation, and maps
  the sum of the channels through a table to 8 bits. A SoundFont player
  interpolates and plays the same samples more cleanly. Played by FluidSynth
  2.6.2, 15 songs of the three games are within 1 dB of what mGBA 0.11.0 plays
  in 94% of the 50 ms windows of *Hardcore Pool* and *Games Explosion!*, and in
  78% of those of *Garfield*. *Garfield* plays its music at a lower master
  volume, and its quiet passages lose more to the 8-bit output. FluidSynth
  rendered them with its reverb and chorus on. It applies 0.4 of the
  attenuation of a zone, and it plays every note 1.5 dB louder than the
  SoundFont specifies (see [FluidSynth](#fluidsynth)).
* **Loops.** The mixer checks the end of a sample once every two samples. It
  starts each pass of a loop again without the part of a point that its step
  had passed. By the check that restarts the loop, the position can be up to
  two steps past the end of the loop. A short loop therefore plays flat of its
  step, by up to 25 cents in these games. Once the loop of a note first
  starts again, the conversion bends the note to the pitch at which the loop
  plays. The bend comes up to a frame after the restart. As at the start of a
  note, a difference under 3 cents stays. Within a pass, the GBA plays points
  up to about one step past the end of the loop: the second point of the last
  pair that it checks. The loop in the SoundFont leaves them out.
* **Levels between rows.** A volume slide changes the volume of the channel at
  each frame, and the player sets the level from it at the next row. The
  conversion follows the level, and the slide moves in steps of a row, as on
  the GBA.

### Frontline Studios' XM-like player

* **Mixer character.** The mixer mixes at 20,068 Hz in mono and writes 8-bit
  output. It plays each sample point by point, without interpolation. A
  SoundFont player interpolates and plays the same samples more cleanly, with
  less sound near the top of the mixer's band and none of its images above
  10 kHz. Played by FluidSynth 2.6.2, the ten songs of the two games are within
  1 dB of what mGBA 0.11.0 plays in 95% of the 50 ms windows of *Chicken Shoot*
  and in 93% of those of *Chicken Shoot 2*.
  FluidSynth rendered them with its reverb and chorus on. It applies 0.4 of
  the attenuation of a zone, and it plays every note 1.5 dB louder than the
  SoundFont specifies (see [FluidSynth](#fluidsynth)).
* **Tempo changes.** Song 3 of *Chicken Shoot 2* changes the BPM at every row.
  The MIDI file follows each change, but the render of it in FluidSynth drifts
  late: by about 14 ms after 28 s.

### Engine Software's driver

* **Mixer character.** The mixer mixes at 13,379 Hz in mono and writes 8-bit
  output. It plays each sample point by point, without interpolation, and
  clips the sum of the channels. Every song but 0, 5 and 6 reaches the clip at
  times. A SoundFont player interpolates and plays the same samples more
  cleanly, with less sound near the top of the mixer's band and none of its
  images above 6.7 kHz. Played by FluidSynth 2.6.2, five songs of *The Santa
  Clause 3* are within 1 dB of what mGBA 0.11.0 plays in 93.9% of the 50 ms
  windows. FluidSynth rendered them with its reverb and chorus on. It applies
  0.4 of the attenuation of a zone, and it plays every note 1.5 dB louder than
  the SoundFont specifies (see [FluidSynth](#fluidsynth)).
* **Envelopes.** The sixth instrument of the bank (`Sample 06`) has a volume
  envelope. It falls from 64 to 4 over 5 ticks, rises to 31 and stays there
  until the note is released, then falls to 0. Songs 3 and 4 play it. Every
  instrument fades out after its note is released. The MIDI file plays the
  envelope and the fade-out in steps of a tick, as the driver does. The
  SoundFont alone plays each instrument at a steady level (see [SoundFont from
  Engine Software's driver](#soundfont-from-engine-softwares-driver)).
* **Fade-in.** The conversion starts the ramp of the music volume a frame
  later than the game does. In a song's first 53 frames, the music volume is
  therefore one step of 4/256 below the game's. Songs 5 and 6 start at 210
  of 256 at once and aren't affected.

### FluidSynth

The SoundFont output follows the specification, apart from the attenuation of
the MP2K PSG drums that play on one side alone (see
[SoundFont from MP2K](#soundfont-from-mp2k)). FluidSynth departs from the
specification in a way that affects the conversions of every driver:

* **Level.** FluidSynth runs the SoundFont low-pass filter on every note,
  notes whose filter is open included. The filter divides the level by the
  square root of its Q, and FluidSynth converts the default resonance of 0 dB
  to a Q of 0.707. Every note therefore plays 1.5 dB louder than the SoundFont
  specifies. A gain of 0.84 in FluidSynth gives the specified level.

Another departure affects the MP2K conversions with `--voice-channels`:

* **All Sound Off.** With `--voice-channels`, an All Sound Off (CC120) stops
  each cut-off note on the tick on which the game cuts it off. FluidSynth
  ignores the All Sound Off where the next note of the channel starts on the
  same tick with the same key, and the cut-off note then fades out over its
  release. With `--psg-exclusive` as well, FluidSynth also ignores it where a
  PSG note with a slow release cuts off another. The cut-off note then fades
  out in 0.32 s. A player that follows the MIDI specification stops the note
  at once. agbsf2 keeps the All Sound Off where the game cuts the note off.

### BASSMIDI

BASSMIDI 2.4.16 applies only some of the modulators in a SoundFont. It
ignores the modulators from key pressure and the pitch wheel:

* **Mutes.** BASSMIDI silences the notes that the MIDI files of Rare's driver
  mute with key pressure. It follows the General MIDI 2 Controller Destination
  Setting message at the start of those files, not the modulator in the
  SoundFont.
* **Fixed pitch.** Samples that a game plays at a fixed pitch follow the pitch
  wheel.

With `--psg-exclusive`, BASSMIDI ends a note of a class in about 4 ms. The PSG
notes whose release is quicker than the fade in FluidSynth get no class. In
BASSMIDI, those notes fade out over their release where the game cuts them
off.

## Internals

`docs/konami.md`, `docs/rare.md`, `docs/quintet.md`, `docs/rd1.md`,
`docs/rd2.md`, `docs/alphadream.md`, `docs/mp2k.md`, `docs/brownie.md`,
`docs/krawall.md`, `docs/ubimilan.md`, `docs/logikstate.md`,
`docs/webfoot.md`, `docs/frontline_mod.md`, `docs/frontline_xm.md` and
`docs/engine_software.md` document the drivers and their data formats in full, including how the tool
locates them. The code of the drivers comes from agblib. A folder for each
driver in `src/` connects it to the interface in `src/music.h`.

## License

`agbsf2` is released under the MIT License (see `LICENSE`). The libraries that
it builds on have these licenses:

* **agblib** (MIT License) contains the ROM and GSF reader and all fifteen
  sound drivers, with their conversions. Its notice is in
  `externals/agblib/LICENSE`.
* **ntdlib** (MIT License) is the library that agblib builds on. Its notice is
  in `externals/ntdlib/LICENSE`. Its DEFLATE decoder is adapted from Mark
  Adler's puff, under the zlib license. The notice of puff is in
  `externals/ntdlib/src/common/inflate.cpp`.
* **seqlib** (MIT License) contains the MIDI and SoundFont code. Its notice is
  in `externals/seqlib/LICENSE`.
* **xsflib** (MIT License) contains the PSF container reader, the file and
  console code and the parsing of numbers. Its notice is in
  `externals/xsflib/LICENSE`.

`THIRD_PARTY_NOTICES` contains all these notices too. Game data is not covered
by these licenses.
