# Research and validation tools

These Python scripts run a game's sound driver from a ROM you supply. Use
them to study the drivers or check a change to `agbsf2` against the
game's code. They aren't needed for conversion.

Setup (Python 3.10 or later):

```sh
python3 -m venv .venv
. .venv/bin/activate        # on Windows: .venv\Scripts\activate
pip install -r tools/requirements.txt
```

`compare_audio.py` also needs `ffmpeg`, and rendering a MIDI file for comparison
needs a SoundFont player such as `fluidsynth`.

These scripts in `tools/` serve more than one driver:

| Script | Description |
|---|---|
| `gbarom.py` | loads a `.gba` ROM or a GSF rip; used by the other scripts |
| `gbadis.py` | recursive-descent Thumb/ARM disassembler that resolves literal pools, marks code a GSF rip has zeroed out, and can read code at the address the game copies it to |
| `psg_model.py` | Game Boy APU model that renders a driver's PSG register writes to audio |
| `conversion.py` | finds a driver's MIDI files in a folder of `agbsf2`'s output, and compares a conversion on the song's beat with one made with `--frame-timing`; used by the `compare_notes.py` scripts |
| `traces.py` | runs `agbsf2 --trace` and compares that trace with the one that the emulated driver prints, line by line; used by the `compare_trace.py` scripts. The `compare_notes.py` scripts use its parser for `--songs` |

A conversion includes songs from every driver found in the game. Files from
drivers other than the first include the driver's name. The `compare_trace.py`
scripts, and the `compare_notes.py` scripts apart from Rare's and MP2K's, run
`agbsf2` with `--driver`. Each `compare_notes.py` checks only its
driver's files.

Driver-specific scripts are in `tools/konami/`, `tools/rare/`, `tools/quintet/`,
`tools/rd2/`, `tools/mp2k/`, `tools/brownie/`, `tools/krawall/` and
`tools/ubimilan/`. Run the commands below from the repository root.

## Konami's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: per-frame track output records, DirectSound mixer output, PSG register writes |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated driver, frame by frame, for every song |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the emulated driver, note by note |
| `compare_audio.py` | compares two renders: loudness envelopes, spectra and level ratio |
| `test_song.py` | writes a copy of a ROM with a test song for commands and outputs that the game's songs may not use |

### Checking agbsf2 against Konami's driver

```sh
python tools/konami/compare_trace.py rom.gba build/agbsf2              # the sequencer: every song, 12000 frames
build/agbsf2 -q -o rom rom.gba                                         # convert every song into rom/
python tools/konami/compare_notes.py rom.gba build/agbsf2 rom          # the conversion, note by note
python tools/konami/driver_emu.py rom.gba render-ds 10 3000 ref.wav    # the driver's sample mix, song 10
build/agbsf2 -q -o out -s 10 -t 4-15 rom.gba                           # the same tracks as MIDI + SF2
fluidsynth -ni -R 0 -C 0 -F mine.wav out/rom_10.sf2 out/rom_10.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

By default, converted notes follow the estimated beat and may be up to a
frame and a half from the driver's timing. `compare_notes.py` first makes a
temporary conversion with `--frame-timing`, placing each event on the
driver's frame. It checks that the original conversion has the same notes,
keys, programs, controller changes and pitch bends, in the same order on
each channel and within 1.65 frames of the temporary conversion: a frame and
a half, and the rounding of a tick. An event that gives a controller, pitch
bend or program the value it already has, as the conversions do where a loop
starts, isn't compared. The SoundFonts must be identical.

It then checks each note in the temporary conversion against the hardware
state left by the driver on the same frame: the note's start and stop,
the sample, duty cycle, wave or noise setting that plays, its pitch, its level
on each side and its echo send. If a MIDI track or an entire song is absent
from the conversion, the corresponding channel or song must also be silent in
the driver, and each track with notes has to stand for a different voice or
channel of the driver. It lists the differences and the largest pitch and
level deviations. PSG notes are written at their equal-tempered pitch, while
the driver's frequency table can only get within one step of the Game Boy's
11-bit frequency register, so that much is allowed for.

`render-ds` leaves the echo out unless you pass `--echo`, because MIDI can't
reproduce it, and fluidsynth's reverb and chorus are turned off to match.
`render-psg` covers the four PSG channels, and `--channel N` renders one of
them. `--voices` on `render-ds` renders chosen sample voices.

### Working out Konami's driver

In a GSF rip, `gsfopt` has zeroed every byte the songs never touched, which
makes the driver easy to isolate:

1. **Find the entry points.** In a GSF rip, the ripper's patch calls the
   driver's "play song" and "start song" routines. Main's IRQ table points at
   the VBlank handler, which calls the per-frame routine.
2. **Disassemble.** `gbadis.py` was run from those entry points. The per-frame
   routine's command dispatch gave the byte code, and the output stage showed
   how each track's record reaches the PSG registers and the DirectSound
   voices. The mixer was found as ARM code copied to IWRAM.
3. **Model.** The sequencer was reimplemented from that reading.

`driver_emu.py` picks the routine addresses by the ROM's game code. Those of
Yu-Gi-Oh! Ultimate Masters Edition: World Championship Tournament 2006 (`BY6J`,
`BY6E`), Shaman King: Master of Spirits 1 and 2 (`BSOE`, `B2ME`), Yu-Gi-Oh! Day
of the Duelist (`BY7E`), Yu-Gi-Oh! GX: Duel Academy (`BYGE`), Yu-Gi-Oh! World
Championship Tournament 2004 (`BYWP`), Rave Master: Special Attack Force
(`BRME`), Yu-Gi-Oh! The Eternal Duelist Soul (`AY5E`), Yu-Gi-Oh! Worldwide
Edition (`AYWE`) and Yu-Gi-Oh! Dungeon Dice Monsters (`AYDE`) are built in. For
another game, find the same routines, starting from the patterns in
`docs/konami.md`, and add them to `GAMES`. A game whose driver has the same code
as one of these usually places the routines at different addresses. Match
the code while ignoring literal pools and call targets. Read the init arguments
from the game's call to the init routine, and the RAM addresses from its literal
pools.

An entry in `GAMES` also says how many tracks and voices the driver has,
where the track output records are and where their pan bytes are, how the
mixer scales a voice's level, whether it has echo and which voices it takes
from echo bus 0, and whether a pitch change reloads the wave RAM. Those differ
in the older revisions of `BYWP`, `BRME`, `AY5E`, `AYWE` and `AYDE`, whose
output stage is part of the sequencer. `compare_trace.py` and `compare_notes.py`
then check them the same way as the others. In `AY5E` and `AYWE`, a voice has
one level for both sides, and the per-frame entry runs the mixer itself, which
fills a ring buffer as far as the DMA 1 interrupt has moved its position. The
emulator skips that call, and when it renders, it moves the position on and
runs the mixer itself.

`AYDE` has no mixer. Its DMA 1 and DMA 2 interrupt routines each mix two voices
into a FIFO, 16 samples at a time, and each FIFO plays at its timer's rate,
set by the last note started on it. When the emulator renders, it plays each
FIFO out at its timer's rate, runs the routine whenever a FIFO has 16 samples or
fewer left, and samples the two at 32768 Hz. `compare_notes.py` takes a voice's
pitch from its FIFO's timer period, and the entry says how its voice records and
sample table are laid out. Every write to a square channel restarts it,
including vibrato updates. The wave channel restarts only when it loads a new
wave. `compare_notes.py` therefore allows a PSG note to start on any frame that
writes to its channel. A note must start when a silent channel begins playing,
the wave changes or the noise restarts. DMA transfers into the sound registers,
such as `AYDE`'s wave RAM loads, count as register writes.

A game's songs may not use every command and output its driver has, so
`test_song.py` covers the others with a test song. It writes a copy of the ROM
with the test song in place of song 0. The script's docstring shows how to
check it.

## Rare's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator and records its note slots after each frame or renders the mixer's output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated driver, frame by frame, for every tune |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the emulated driver, note by note |
| `compare_audio.py` | compares two renders: loudness envelopes, spectra and level ratio |
| `test_song.py` | writes a copy of a ROM with a test song for commands and modes that the game's tunes may not use |

### Checking agbsf2 against Rare's driver

```sh
python tools/rare/compare_trace.py rom.gba build/agbsf2           # the model: every tune, 12000 frames
build/agbsf2 -q -o rom rom.gba                                    # convert every tune into rom/
python tools/rare/compare_notes.py rom.gba rom                    # the conversion, note by note
python tools/rare/driver_emu.py rom.gba render 10 3000 ref.wav    # the driver's output for tune 10
fluidsynth -ni -R 0 -C 0 -F mine.wav rom/rom_10.sf2 rom/rom_10.mid
python tools/rare/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` runs `agbsf2 --trace` and the driver side by side. After
each frame, the script compares every note slot: its state, channel, keys,
velocity, envelope phase and level, instrument, and its position in the sample
to a 2^23rd of a byte. It reports the first difference in each tune that has
one. `-j` compares several tunes at once.

`compare_notes.py` checks every note in a folder of MIDI files against what the
driver plays on the same frame: the note's start and release, its velocity, the
channel's volume, the sample that the SoundFont plays for it, and its pitch on
every frame, from the SoundFont zone's tuning and the channel's pitch bend. A
velocity or volume of v has to be written as 127 × √(v/127). The
script also checks the zone's envelope against the one that agbsf2 works out
from the instrument and the settings from controllers 20-23 (in the revision
that has them) that the driver's RAM held as each phase started. The MIDI file
keeps each event on its own tick, where the driver plays it on a frame, so it
allows a frame and a half of difference either way. It lists the differences
and the largest pitch deviation.

`driver_emu.py render` writes the driver's mono output at its own rate, and
`--channels` renders only the notes of the MIDI channels it names. The other
notes still count towards the mixer's limit of 8 voices, so the result is what
the game would play with those channels muted.

The driver's init sets the music volume to 128. In *Donkey Kong Country 3*
(`BDQE`), `driver_emu.py` sets it to 69 after it requests a tune. The game sets
the same volume for most tunes. At that volume the mixer leaves out the notes
of a channel at volume 0. The model does the same.

*Donkey Kong Country 3* plays tune 12 on channels 0-3 above water and on
channels 4-8 under water. The game holds the channels of the other arrangement
at a level of 0. `agbsf2` writes `rom_12` above water and `rom_12_underwater`
under water. `compare_notes.py` checks each file with the levels of its
arrangement. Before each frame, `driver_emu.py` sets the other arrangement's
channels to a level of 0. The script leaves out their notes.
`driver_emu.py render` plays tune 12 above water, or under water with
`--under-water`. `compare_trace.py` and `driver_emu.py trace` keep every
channel at a level of 128. So does `agbsf2 --trace`.

### Working out Rare's driver

The driver's init is easy to find in a ROM, because it copies the driver's ARM
code to IWRAM with a sequence that's the same in every revision (see
`docs/rare.md`). From there:

1. **Find the entry points.** The init stores the tune table's address and
   calls the routines that set the mix rate and the voice limit. The game calls
   the request routine with a tune number from many places, and the per-frame
   routine from one.
2. **Disassemble.** `gbadis.py` was run from those entry points, and with
   `--copy` it read the ARM code at the address the init copies it to. The
   sequencer's command table gave the byte code, and the mixer showed how each
   note's level and pitch are worked out.
3. **Model.** The sequencer, the envelopes and the mixer's choice of voices were
   reimplemented from that reading.

`driver_emu.py` picks the routine and RAM addresses by the ROM's game code.
Those of *Donkey Kong Country* (`A5NE`), *Sabre Wulf* (`AWUE`), *It's Mr.
Pants* (`BPIE`), *Banjo-Kazooie: Grunty's Revenge* (`BKZX`), *Donkey Kong
Country 2* (`B2DE`), *Banjo-Pilot* (`BAJE`) and *Donkey Kong Country 3*
(`BDQE`) are built in. For another game, find the same routines, starting from
the patterns in `docs/rare.md`, and add them to `GAMES`:

* `init`, `request` and `frame` are the init, the routine that requests a tune
  and the per-frame routine. In each of the games above, the request routine
  comes just before the init in ROM, and the per-frame routine a little after
  it.
* `tune`, `note_slots`, `fx_slots`, `buffer_flag`, `buffers` and
  `samples_per_frame` come from the per-frame routine and the mixer's literal
  pools: the tune playing, the note slots and sound effect records, the output
  buffer flag, the two words that hold the addresses of the output buffer's
  halves, and the samples a frame.
* `channel_volume`, the channels' controller 7 values, comes from the
  controller handler, and so does `envelope_settings` in a revision with
  controllers 20-23: the 4 bytes for each channel that they set.
* In a revision with a music volume, `music_volume` is the word that holds it.
  After each request, the script sets it to the volume at which `agbsf2` plays
  the game's tunes.
* In a revision with channel levels, `channel_levels` is the 16 bytes that hold
  them. `water` gives a tune that the game plays above and under water, with
  the channels of each arrangement. `docs/rare.md` describes the game's routine
  that sets the levels.
* `slots_per_channel` and `fx_count` give the note slots each channel has and
  the number of sound effect records, if they aren't 6 and 5. The table at the
  start of `docs/rare.md` gives them for each checked game.

`driver_emu.py` identifies the mixer and the note-advance routine in IWRAM by
their code, so you don't need to supply their addresses.

A game's tunes may not use every command and mode its driver has, so
`test_song.py` covers the others with a test song. It writes a copy of the ROM
with the test song in place of tune 0. The script's docstring shows how to
check it.

## Quintet's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: its writes to the sound, DMA and timer registers, each channel's position and countdown, and a render of its output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated driver's register writes, frame by frame, for every song |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against what the emulated driver plays, frame by frame |

### Checking agbsf2 against Quintet's driver

```sh
python tools/quintet/compare_trace.py rom.gba build/agbsf2            # the model: every song, 12000 frames
build/agbsf2 -q -o rom rom.gba                                        # convert every song into rom/
python tools/quintet/compare_notes.py rom.gba build/agbsf2 rom        # the conversion, frame by frame
python tools/quintet/driver_emu.py rom.gba render 10 3000 ref.wav     # the driver's output for song 10
build/agbsf2 -q -l 1 -o out -s 10 rom.gba
fluidsynth -ni -R 0 -C 0 -F mine.wav out/rom_10.sf2 out/rom_10.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` runs `agbsf2 --trace` and the driver side by side. From the
play routine's writes on, the script compares every write that the driver makes
to the sound registers, DMA 1 and 2 and timers 0 and 1, in order. It leaves out
the bytes a FIFO start writes into the FIFO. Those bytes follow from the DMA's
source. The script reports the first difference in each song that has one.
`-j` compares several songs at once.

`compare_notes.py` works out, from the driver's register writes, what each of
the 6 channels plays in each frame: whether it sounds, its level on each side,
its pitch, and its duty, wave pattern or sample. The PSG's envelope, length
counter and frequency sweep are modelled, and so is each FIFO's rate. It then
checks the MIDI file and the SoundFont against that, frame by frame: the
square and wave channels' frequency settings against the frequency table's
entry for each note's key, and the PCM channels' rates against the PCM pitch
table's, so that the tables' tuning, which the MIDI file leaves out, isn't
counted. A noise drum's sample includes the channel's volume, which is checked
frame by frame, allowing one step either way while the hardware envelope runs.
Since the MIDI file keeps each note on its own tick, up to a frame after the
driver's frame, each frame is compared with the same stretch of the note. A
note on a MIDI channel that the driver doesn't have, or one that starts after
all of the driver's channels have ended, counts as a difference.

`agbsf2` numbers the sound effects after the music file's songs. The scripts
check them in the same way. `driver_emu.py` plays a sound effect through the
game's routine for sound effects, with no music playing, and follows the sound
effect channels. `compare_notes.py` starts from the registers that the game's
sound init leaves. A sound effect plays on from them.

`driver_emu.py render` writes the driver's stereo output at 32768 Hz: the PSG
through `psg_model.py`, and each FIFO's sample at its timer's rate.
`--channels` renders only the channels it names, all four PSG channels, one of
them, or none, with any of the FIFOs.

### Working out Quintet's driver

The games keep the driver in one block of Thumb code, which their sound
routines call into. From there:

1. **Find the entry points.** The game's sound init calls the driver's init and
   gives it the game's files, its play-song routine looks up the music file
   and calls the driver's play routine with a song's address, and a short
   routine calls the per-frame routine.
2. **Disassemble.** `gbadis.py` was run from those entry points. The note
   reader's command table gave the byte code, and the output stage showed how
   the channels' register image reaches the hardware.
3. **Model.** The driver was reimplemented from that reading.

`driver_emu.py` picks the addresses by the ROM's game code. Those of *Super
Robot Taisen A* (`ASRJ`), *D* (`A6SJ`), *R* (`AJ9J`) and *J* (`B6JJ`) are
built in. For another game, add an entry to `GAMES` with:

* `sound_init`, `play` and `frame`: the game's routines that call the
  driver's init and set up its files, play a song by number, and run the
  per-frame routine. The play-song routine is the one that calls the driver's
  play routine (see `docs/quintet.md`), and the other two are near it.
* `play_effect`: the game's routine that plays a sound effect by number. It
  calls the driver's routine for sound effects.
* `channels` and `effect_channels`: the tables of the 6 music channel records'
  addresses and the 6 sound effect channel records'. The driver's play routine
  loads both.
* `songs`: the number of songs in the music file. `agbsf2 --info` gives it.
* `countdown`: the offset of the countdown in a channel's record, `0x18` in
  the A revision and `0x1A` in the J revision.

## Nintendo R&D2's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: each voice's state and the PSG register writes, frame by frame, the players and tracks in use, and a render of the mixer's output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated driver's voices and PSG register writes, frame by frame, for every sequence |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the notes the emulated driver plays |
| `test_song.py` | copies a ROM and replaces sequence 0 with a test sequence for the commands and kinds of instrument that a game's sequences may not use |

### Checking agbsf2 against Nintendo R&D2's driver

```sh
python tools/rd2/compare_trace.py rom.gba build/agbsf2            # the model: every sequence, 12000 frames
build/agbsf2 -q -o rom rom.gba                                    # convert every sequence into rom/
python tools/rd2/compare_notes.py rom.gba build/agbsf2 rom        # the conversion, note by note
python tools/rd2/test_song.py rom.gba test.gba                    # the test sequence, as sequence 0
python tools/rd2/compare_trace.py test.gba build/agbsf2 --songs 0
python tools/rd2/driver_emu.py rom.gba render 5 3000 ref.wav --tracks 1  # track 1 of sequence 5
build/agbsf2 -q -l 1 -o out -s 5 -t 1 rom.gba
fluidsynth -ni -r 10512 -F mine.wav out/rom_05.sf2 out/rom_05.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` runs `agbsf2 --trace` and the driver side by side.
After each frame it compares every field of each voice that plays, the order of
the mixer's list of sample voices, and every write to the PSG's registers. It
reports the first difference in each sequence that has one. `-j` compares
several sequences at once.

`compare_notes.py` runs the driver with hooks in its note on routine, which
give each note's track, key, velocity, voice, region and sample, and follows
each voice from frame to frame. Each note the driver plays has to be in the
MIDI file, and the other way round: on its track's channel, with the key and
velocity the conversion gives it, starting in the frame the driver starts it in
or the frame before, and ending within a frame of the driver's release or stop
of its voice. Notes that a track starts on the same key at the same time are
one MIDI note, the first's, which ends with the last of them, and a note that
its instrument has no region for, which the driver plays from its bank's table
of offsets, mustn't be in the MIDI file. Each note's SoundFont zone has to play
the driver's sample at the pitch the driver plays it at, to within a cent, and
while it's the newest note of its track, the MIDI file's pitch bend has to
follow its voice's pitch, frame by frame. A sequence named in `--songs` that has
no MIDI file mustn't play any notes.

A PSG voice's note plays at velocity 127. It ends where its PSG channel falls
silent, or where another note takes the channel. In each frame until then, its
CC11 has to give the channel's volume at the frame's start, on the sides that
NR51 gives the channel. `psg_model.py` works out the volume from the driver's
writes to the PSG's registers, with the hardware's envelopes. The wave
channel's volume is 15 times the share of its full level that NR32 sets.

`agbsf2` numbers the sound effects after the sequences. Both scripts check
them in the same way. On a track whose sample notes play above velocity 127,
the conversion divides each velocity by a scale. The track's CC11 raises its
levels by the same scale (see `README.md`). `compare_notes.py` takes the scale
from the track's loudest note. A track whose CC11 reaches 127 can have a
smaller scale. Its notes' velocities then have to fit one scale.

`driver_emu.py render` writes the mixer's stereo output at 10512 Hz: the 8-bit
samples the driver sends to the FIFOs, without the PSG. `--tracks` mutes the
player's other tracks with their mute flags. A track's output can then be
compared with a render of `agbsf2 -t` alone.

### Working out Nintendo R&D2's driver

The driver is a block of Thumb code, with three ARM routines that its init
copies to IWRAM. From there:

1. **Find the entry points.** The game's sound init calls the driver's init
   with the game's settings, and the game's VBlank handler calls the driver's
   VBlank routine and its per-frame routine. The game's play-music routine
   requests a sequence for a player, and its per-frame sound routine hands the
   requests over.
2. **Disassemble.** `gbadis.py` was run from those entry points. The track
   reader's command table gave the byte code, and the voice routines showed
   how the mixer and PSG play each voice.
3. **Model.** The driver was reimplemented from that reading.

`driver_emu.py` picks the addresses by the ROM's game code. The addresses of
three games are built in: *The Legend of Zelda: A Link to the Past & Four
Swords* (`AZLE`), *Super Mario Advance 2: Super Mario World* (`AA2E`) and
*Super Mario Advance 3: Yoshi's Island* (`A3AE`). *Super Mario Advance 2* has
an older revision of the driver. For another game, add an entry to `GAMES`
with:

* `sound_init`: the game's routine that calls the driver's init with its
  settings.
* `request` and `commit`: the driver's routines that request a sequence for a
  player and hand the requests over (see `docs/rd2.md`).
* `effect_request`, `effect_player` and `settings`: the driver's routine that
  requests a sound effect, the first of the players that the game plays its
  sound effects on, and the variable that holds the address of the game's
  settings. The script numbers the sound effects after the sequences, from the
  tables that the settings give.
* `vblank`, `frame` and `requests`: the driver's VBlank routine, its per-frame
  routine, and the per-frame routine's first step, which carries out the
  requests.
* `voices`, `active`, `tracks`, `players` and `fixed_region`: the driver's
  voices, its list of the sample voices that play, its tracks and players, and
  its region in RAM for instruments with a sample for each key.
* `outputs` and `output_index`: the output buffers, and the variable that picks
  the one the mixer writes.
* `note_on`, `note_key`, `note_voice` and `note_done`: the note on routine, and
  the points in it where it has added the transpose, found a voice, and set the
  voice's sample.
* `active_end`, `music_player`, `player_size`, `player_playing`, `voice_note`
  and `voice_velocity`, where the game's revision of the driver differs from A
  Link to the Past's: what the last voice of the mixer's list links to, the
  player the game plays its music on, the size of a player's record and the
  offset of its byte that is 1 while it plays, and the offsets of a voice's note
  and velocity. In the Super Mario Advance 2 revision, the mixer's list ends
  with a node, the music player is 0x13, a player's record is 0x44 bytes, and a
  voice keeps the velocity at 0x09 and no note.
* `play_music`, `music_table` and `music_count`, in a game whose play-music
  routine gives the driver settings along with the sequence, as *Super Mario
  Advance 3*'s does: the routine, its table of music, and the number of music
  numbers in the table. `driver_emu.py` then starts each sequence through the
  routine, as the first music number that plays it. Otherwise `play_music` is
  `None`.

A region read from a bank's table of offsets can find its sample's address
outside the GBA's memory. The emulator reads 0 there, and so does `agbsf2`.
`test_song.py` pads a ROM that ends before its free
space with 0xFF, and writes the test sequence in the form of the game's
revision.

## MP2K

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's MP2K driver under the Unicorn ARM emulator and records its sound channels after each frame, or renders the mixer's output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated driver, frame by frame, for every song |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the emulated driver, note by note |

### Checking agbsf2 against MP2K

```sh
python tools/mp2k/compare_trace.py rom.gba build/agbsf2           # the model: every song, 12000 frames
build/agbsf2 -q -o rom rom.gba                                    # convert every song into rom/
python tools/mp2k/compare_notes.py rom.gba rom                    # the conversion, note by note
python tools/mp2k/driver_emu.py rom.gba render 10 3000 ref.wav    # the driver's DirectSound output for song 10
```

`compare_trace.py` runs `agbsf2 --trace` and the driver side by side. After
each frame, the script compares every sound channel that plays: its status,
track, keys, velocity, priority, envelope level and volumes, then a DirectSound
channel's rate, sample, and position in it to a 2^23rd of a point, or a PSG
channel's frequency setting, envelope goal and counter, sustain level, outputs
and wave. It reports the first difference in each song that has one. `-j`
compares several songs at once.

`compare_notes.py` checks every note the driver starts against the MIDI file:
the note's start and release, its velocity, its track's CC10 and CC11, the
sound that the SoundFont zone plays for it, and its pitch on every frame until
its release. The pitch comes from the zone's tuning and the channel's pitch
bend. CC10 and CC11 have to come within a step of the values that the
driver's levels on each side give. A velocity of v has to be written as
127 × √(v/127). The script reads the driver's channels just
before the PSG routine runs in each frame. The notes that the tracks started in
the frame then still have their start flag. A note
whose envelope never rises above 0 doesn't count. A PSG square or wave note can
be two steps of the 11-bit frequency register from equal temperament (see
`docs/mp2k.md`), and that much is allowed for. A sample without a loop that
runs out stops its channel, which can happen between ticks, so then the MIDI
note can end as late as the first tick after it. When a track changes its pitch
in a tick in which it starts a note, the driver leaves the track's older notes
behind until its next change, while the MIDI file bends them all, so an older
note's pitch isn't compared until the driver catches it up. Track 9 plays on
the drum channel only in a song that plays on all 16 channels. Its zones then
come from bank 128. A synth voice of Camelot's
mixer has to play the SoundFont's sample for its wave, and for a pulse or saw
wave, the sample made for the key the driver plays it at, whose pitch is the
driver's rate for that key. A song named in `--songs` that has no MIDI file
mustn't play any notes. It needs a conversion made without `--voice-channels`,
so that each MIDI channel holds one track.

`driver_emu.py render` writes the DirectSound mixer's stereo output at its own
rate, with the same output on both sides for a mono mixer. It leaves the PSG
channels out. `--tracks` mixes only the notes of the tracks it names, and the
others still take up channels, so the result is what the game would play with
those tracks muted. Camelot's mixer's output has its echo in it, and a muted
track's channels go into sums of their own, since the mixer leaves out a silent
channel, which then doesn't move on through its sample.

### Working out MP2K

The driver's routines have the names of the SDK's m4a library. The patterns
that `agbsf2` finds them by are in `docs/mp2k.md`:

1. **Find the entry points.** `m4aSongNumStart` has the same code in every
   version, apart from two registers that some games' compiler swaps, and its
   literal pool gives the song table and the music player table.
   `m4aSoundMain`, which calls the driver's main routine, comes just before it,
   and `m4aSoundInit`, which sets the driver's mode, a little before that.
   `m4aSoundVSync` restarts the sound DMA each frame. Where the compiler swaps
   the registers, it's compiled from C: `push {lr}`, then loads of the address
   at `0x03007FF0` and of the structure's ID.
2. **Model.** The sequencer, the choice of channels, the envelopes and the
   mixer's progress through each sample were reimplemented from the driver's
   code.

`driver_emu.py` picks the routine addresses by the ROM's game code. Those of
*Pokémon Emerald* (`BPEE`), *The Legend of Zelda: A Link to the Past & Four
Swords* (`AZLE`), whose Four Swords half and menus use MP2K, *The Legend of
Zelda: The Minish Cap* (`BZME`), *Kingdom Hearts: Chain of Memories* (`B8CE`),
*Super Robot Taisen: Original Generation 2* (`B2RE`) and *Golden Sun: The Lost
Age* (`AGFE`) are built in. For
another game, find the same routines and tables, and add them to `GAMES`:
`init`, `song_start`, `main` and `vsync` are `m4aSoundInit`,
`m4aSongNumStart`, `m4aSoundMain` and `m4aSoundVSync`, and `song_table` and
`player_table` come from `m4aSongNumStart`'s literal pool. The emulator finds
the driver's state, its channels and the PSG routine from the structure whose
address the init stores at `0x03007FF0`.

`AGFE`'s entry runs Camelot's version under `CamelotEmulator`, which does what
the game does around the driver (see `docs/mp2k.md`): it copies the mixer to
IWRAM before the init, and each frame it runs Camelot's `m4aSoundVSync`, copies
the DMA counter, runs Camelot's `SoundMain` and then the output stage. Three
GBA behaviours need special handling in Unicorn:

* Camelot calls routines with `mov lr, rN` and the second half of a `bl` alone,
  which the ARM7 runs as a jump to LR. Unicorn's cores either take it for half
  of a Thumb-2 instruction or switch to ARM mode, so each such call in the code
  is changed to `blx lr`, which does the same.
* The mixer writes instructions into its loops, which the ARM7 runs at once.
  Unicorn runs translations of the code, so each write to the mixer's code
  drops the translations it changes, and a new translation starts just before
  the fixed-pitch loop, whose first pass would otherwise run the old code.
* The mixer has DMA3 copy a frame's points of a sample to the stack, so a
  write that starts DMA3 at once does the copy.

The output stage writes two 8-bit points for each mixed point, which add up to
a 9-bit point, at twice the mix rate; `render` averages them.

## Brownie Brown's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound driver under the Unicorn ARM emulator: its writes to the sound registers, each sound channel's record and each mixer voice's or FIFO's sample, its variables, and a render of its output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated driver, frame by frame, for every sound |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against what the emulated driver plays, frame by frame |

### Checking agbsf2 against Brownie Brown's driver

```sh
python tools/brownie/compare_trace.py rom.gba build/agbsf2 -j 4        # the model: every sound, 12000 frames
build/agbsf2 -q -o rom rom.gba                                         # convert every sound into rom/
python tools/brownie/compare_notes.py rom.gba build/agbsf2 rom         # the conversion, frame by frame
python tools/brownie/driver_emu.py rom.gba render 10 3000 ref.wav --channels 8,9,10,11,12
build/agbsf2 -q -l 1 -o out -s 10 -t 8-12 rom.gba                     # the same channels as MIDI + SF2
fluidsynth -ni -R 0 -C 0 -r 32768 -F mine.wav out/rom_010.sf2 out/rom_010.mid
python tools/konami/compare_audio.py ref.wav mine.wav
```

`compare_trace.py` runs `agbsf2 --trace` and the driver side by side. Frame by
frame, the script compares every write the driver makes to the sound registers,
in order, each sound channel's record, each mixer voice's or FIFO's sample, and
the driver's other variables. It reports the first difference in each sound
that has one. `-j` compares several sounds at once.

As with Konami's driver, the default conversion follows the estimated beat.
`compare_notes.py` makes a second conversion with `--frame-timing` and
checks that the original matches it, allowing 1.65 frames of timing
difference per event: a frame and a half, and the rounding of a tick. It
then checks the second conversion against the driver.

`compare_notes.py` works out what each channel plays in each frame: for the PSG
channels, from the driver's register writes, with the PSG's envelope, length
counter and frequency sweep modelled, and with a sound effect's channel taking
its PSG channel from the music; and for the sample channels, from the voices as
the per-frame routine leaves them, while the mixer runs and mixes them, or in
the *Magical Vacation* revision from the FIFOs' samples, while their timers
run. It then checks the MIDI file and the SoundFont against that, frame by
frame: the square and wave channels' frequency settings against the frequency
table's entry for each note's key, in a zone tuned as the table is; a sample
channel's sample, its length and loop, and the rate the mixer or the timer
plays it at; and each channel's levels. A noise drum's sample includes the
channel's volume, which is checked frame by frame, allowing one step either way
while the hardware envelope runs. A sound channel plays one note at a time, so a
MIDI note that still sounds when the next one on its channel starts counts as a
difference, and so does a note on a MIDI channel that stands for none of the
sound channels. A sound named in `--songs` that has no MIDI file mustn't play
anything.

`driver_emu.py render` writes the driver's stereo output at 32768 Hz: the PSG
through `psg_model.py`, with the hardware's envelope, length counter and sweep,
and the FIFOs' points from the frame their DMA starts in, or in the *Magical
Vacation* revision each FIFO's points at its timer's rate, on both sides.
`--channels` keeps the PSG channels it names, all four, one or none, and the
sample channels all together or not at all.

### Working out Brownie Brown's driver

The driver is ARM code, with its mixer copied to IWRAM:

1. **Find the entry points.** The game's VBlank handler calls a Thumb routine
   that calls the per-frame routine, and the IRQ table that the game copies to
   RAM points the DMA 1 and DMA 2 interrupts at the mixer. The game's other
   sound routines, which queue a sound and start the fades, are Thumb routines
   just before the driver's tables.
2. **Disassemble.** `gbadis.py` was run from the per-frame routine and the
   mixer, with an `A` before each address for ARM code, and with `--copy` it
   read the mixer's code at its address in IWRAM. The command table gave the
   byte code, and the mixer showed how the voices move through their samples.
3. **Model.** The per-frame routine, the fades, the queue and the mixer's
   progress through each sample were reimplemented from that reading.

The *Magical Vacation* revision turned up when the patterns that detection
reads were searched for in the game: all of them matched but the per-frame
routine's start, which pushes `r8` too, and the routine's loop counts 12
channels. A diff of the two revisions' per-frame routines, instruction by
instruction with the addresses masked, showed what changed, and the Thumb
routines that call the driver, just before its frequency table, led through
veneers to its init, its queue, its fade out and the two interrupt routines
that feed the FIFOs.

`driver_emu.py` picks the addresses by the ROM's game code. Those of *Sword of
Mana* (`AVSE`) and *Magical Vacation* (`AMVJ`) are built in. For another game,
add an entry to `GAMES` with:

* `channel_count` and `voice_count`: the channels that the per-frame routine
  runs, and the mixer's voices or the FIFOs.
* `init`, `request` and `frame`: the driver's init, the routine that queues a
  sound, and the per-frame routine, all in ARM.
* `mixer` and `copies`: the mixer's address in IWRAM, and the copies the game
  makes of it from the ROM, as (source, destination, size); or for the
  *Magical Vacation* revision, `interrupts`: the DMA 1 and DMA 2 interrupt
  routines, which feed FIFO A and FIFO B, and no copies.
* `channels`, `voices`, `fifos` and `variables`: the channel records, the
  voices or the FIFOs' samples, the 32 bytes of the mixer's points for the
  FIFOs, and the stretches of the driver's variables: from the queue to the
  voices, or the queue and then SOUNDCNT_L's copy and the fade.

In the *Magical Vacation* revision, the harness plays each FIFO at its timer's
rate after each frame's routine: at each overflow, the FIFO plays a point, and
if it then holds 16 or fewer, the harness runs the FIFO's interrupt routine,
which gives it the next 4 points or stops the timer. It notes the channel whose
note starts each timer from the routine's `r7`.

The emulator starts each call with `r0`-`r12` at 0, so that what `ED` writes
from `r5` is the same as in the model.

## Krawall

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's build of Krawall under the Unicorn ARM emulator: the mixer's variables, the player's record, each of the module's channels and each mixer channel, and a render of its output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated player, frame by frame, for every module |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against what the emulated player plays, tick by tick |
| `test_song.py` | writes a copy of a game with hand-made modules for the parts of the player that the game's modules may not use |

### Checking agbsf2 against Krawall

```sh
python tools/krawall/compare_trace.py rom.gba build/agbsf2 -j 4        # the model: every module, 12000 frames
build/agbsf2 -q -o rom rom.gba                                         # convert every module into rom/
python tools/krawall/compare_notes.py rom.gba build/agbsf2 rom         # the conversion, tick by tick
python tools/krawall/test_song.py rom.gba test.gba                     # modules with every effect
python tools/krawall/compare_trace.py test.gba build/agbsf2 --songs N-M        # the last 3 that --info lists
python tools/krawall/driver_emu.py rom.gba render ADDRESS 3000 ref.wav  # a module's address, from --info
```

`compare_trace.py` runs `agbsf2 --trace` and the game's code side by side.
Frame by frame, the script compares the changes to the mixer's variables (the
master volumes, the mixer's quality settings, the sound timer's step and count,
the music's volume and the count of plays), the player's record, each of the
module's channels and each mixer channel, in hex as Krawall keeps them in RAM.
It reports the first difference in each module that has one. `-j` compares
several modules at once.

`compare_notes.py` runs the game's code and reads the module channels after
each player tick, from a hook where the mixer's worker resumes. For each
active mixer channel still owned by the module channel's last note, it
records the sample, position, step and levels, plus position changes made by
`kramSetPos()`. It derives notes from that state in the same way as the
converter, then checks the MIDI and SoundFont tick by tick. Note starts and
ends must be within a millisecond on the correct MIDI channel. The preset
must match the sample data, loop and start offset. Pitch must be within 5
cents, and CC10 and CC11 within one step of the values needed for the
recorded levels.

`test_song.py` adds three modules after the game's data, which the module scan
lists after the game's modules: one with Amiga periods and every effect of the
effect column, the same patterns with S3M's fast volume slides, and one with
linear frequencies, XM instruments with envelopes, fades and vibratos, and the
volume column's effects. They play three samples of their own, which loop
forwards, back and forth or not at all, and the copy points the player's
literals at new tables of samples and instruments, so that the game's modules
play as before.

`driver_emu.py render` writes the mixer's 8-bit stereo output at the mixer's
rate. It names a module by the address of its header, which `--info` lists.
`--song N` plays song N of a module that separates songs with `+++`, in song
mode. agbsf2 converts each such song that way. The two comparisons play it
that way too.

### Working out Krawall

The player is Thumb code in ROM, and the mixer is ARM code in IWRAM:

1. **Find the entry points.** The library's version string is in the ROM, and
   the game's sound init calls `kragInit()`, `kramSetMasterVol()` and
   `kramQualityMode()`, which lead to the timers' setup and the sound timer.
   The game's routine that starts a module calls `krapPlay()` with a module
   from its table, and its VBlank handler calls the worker, `kramWorker()`,
   in IWRAM. The game's start-up code copies the library's IWRAM code and data,
   and its EWRAM data, from the ROM.
2. **Disassemble.** `gbadis.py` was run from `krapPlay()` and the player's
   tick, and with `--copy` from the worker and the mixer's routines at their
   addresses in IWRAM. The tables of effects gave each effect's routines.
3. **Model.** The player, the effects and the mixer's progress through each
   sample were reimplemented from that reading.

`driver_emu.py` picks the addresses by the ROM's game code. Those of *Digimon
Racing* (`BDGE`) are built in. For another game with the same build, add an
entry to `GAMES` with:

* `iwram` and `ewram`: the ROM's copies of the library's IWRAM code and data
  and of its EWRAM data, which the game's start-up code makes, as (source,
  size).
* `sound_init`, `play`, `worker` and `dma_address`: the game's sound init, which
  calls `kragInit()`, `kramSetMasterVol()` and `kramQualityMode()`,
  `krapPlay()`, `kramWorker()` and `getDmaAddress()`, which the harness replaces
  with a routine that hands the worker the same two buffers and a frame's
  samples each time.
* `after_tick` and `set_pos`: where the worker goes on after a tick, and
  `kramSetPos()`, for `compare_notes.py`.
* `frame_samples`, `player`, `mixer`, `master`, `timer`, `music_volume` and
  `plays`: the samples of a frame, and the addresses of the player's record,
  the mixer's channels, the master volumes, the sound timer's step and count,
  the music's volume and the count of plays.

`test_song.py` also needs, in its `GAMES`, the address of the game's table of
samples and the player's literals that give the tables of samples and
instruments. The script counts the table's samples.

## Ubisoft Milan's driver

| Script | Description |
|---|---|
| `driver_emu.py` | runs the game's sound engine under the Unicorn ARM emulator: each frame's writes to the PSG's registers, the track and each voice that the mixer plays, and a render of the mixer's output |
| `compare_trace.py` | diffs `agbsf2 --trace` against the emulated engine, frame by frame, for every piece of music |
| `compare_notes.py` | checks a conversion's MIDI files and SoundFonts against the samples and PSG notes that the emulated engine plays |

### Checking agbsf2 against Ubisoft Milan's driver

```sh
python tools/ubimilan/compare_trace.py rom.gba build/agbsf2 -j 4        # the model: every piece, 12000 frames
build/agbsf2 -q -o rom rom.gba                                          # convert every piece into rom/
python tools/ubimilan/compare_notes.py rom.gba build/agbsf2 rom         # the conversion, note by note
python tools/ubimilan/driver_emu.py rom.gba render 3 600 ref.wav        # piece 3's first 600 frames
```

`compare_trace.py` runs `agbsf2 --trace` and the game's code side by side.
Frame by frame, the script compares every write to the PSG's registers, the
track's countdown, next command and next sequence, and each voice that the
mixer plays. A key outside channel 9's kit makes the engine play bytes from a
mirror of the ROM. The model leaves those notes out. The script doesn't compare
their voices, and counts their frames instead.

`compare_notes.py` converts the ROM again with `--frame-timing`, and checks
that the conversion on the beat has the same notes within 1.65 frames and the
same SoundFonts. It then runs the game's code with hooks on the routines that
start a sample on a voice and stop a voice. Each sample that sounds has to be a
note on MIDI channel 10 on its frame, at the velocity that its volume gives,
ending where the voice stops, with a zone that plays the sample's points at the
mixer's rate. The PSG's notes that sound have to match the MIDI file's.

`driver_emu.py render` writes the mixer's 8-bit output at its rate, 32704 Hz,
on both sides.

### Working out Ubisoft Milan's driver

The engine is Thumb code in ROM, and its mixer ARM code in IWRAM:

1. **Find the entry points.** The ROM's literals of the sound and DMA registers
   led to the sound init, the DMA interrupts and the PSG's routines. The PSG's
   note routine led to the track's step, whose commands are MIDI's, and the
   track's callers to the routine that starts a piece of music and to the
   game's VBlank task. The literals of the engine's RAM led to the game's
   sound update. The game's start hands the engine its sound bank, a file of
   its archive.
2. **Disassemble.** `gbadis.py` was run from those routines, and from the mixer
   as ARM code at its address in the ROM. The sound init's float gave Timer 0's
   reload.
3. **Model.** The track, the pieces' lists, the kit's voices, the mixer's
   progress through each sample and the PSG's notes were reimplemented from that
   reading.

`driver_emu.py` picks the addresses by the ROM's game code. Those of *Tomb
Raider: The Prophecy* (`AL9P`) are built in. For another game, add an entry to
`GAMES` with:

* `file_lookup` and `bank_file`: the archive's lookup and the number of the
  file that the game's start hands the engine as its sound bank.
* `init`, `voices_init`, `start`, `step` and `update`: the engine's init, the
  routine that clears the mixer's voices, the routine that starts a piece of
  music, the track's step and the game's sound update.
* `play_sample` and `stop_voice`: the routines that start a sample on a voice
  and stop a voice. `compare_notes.py` hooks them.
* `mixer`, `mixer_size`, `mixer_ram`, `mixer_pointer` and `mixer_flag`: the
  mixer in the ROM, the word that gives its size, where the harness copies it,
  the variable that the DMA interrupts call it through, and the flag that lets
  the first interrupt run it.
* `track`, `voices` and `output`: the track, the mixer's voices and its output.
* `timer_reload`: Timer 0's reload.

The harness copies the mixer to IWRAM itself, because the game's copy goes
through its memory manager. Unicorn has no DMA, and the harness does DMA 3's
copies itself.
