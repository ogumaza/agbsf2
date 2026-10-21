// SPDX-License-Identifier: MIT

// Song conversion for Ubisoft Milan's driver. The model plays a piece frame by frame, and its notes go on the song's
// beat, which BeatGrid works out from the frames they start and end on, since the driver counts frames.

#include "ubimilan/convert.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "beat_grid.h"
#include "beat_midi.h"
#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rom.h"
#include "sf2.h"
#include "ubimilan/driver.h"
#include "ubimilan/sequencer.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{
namespace
{

// The sequence channels that play notes, in the order of the MIDI file's tracks: the PSG's three, then channel 9.
constexpr std::array<int, 4> kChannels = {0, 1, 2, kKitChannel};

// The MIDI channel of each of those channels. Channel 9 plays on MIDI channel 11. Players treat notes on MIDI channel
// 10 as drums, and some ignore their note-offs or their samples' loops. The kit's notes require both.
constexpr std::array<int, kChannels.size()> kMidiChannels = {0, 1, 2, 10};

// The notes of each of those channels.
using ChannelNotes = std::array<std::vector<Note>, kChannels.size()>;

// What the model played, up to the conversion's end.
struct Simulation
{
    std::vector<Note> notes;
    std::vector<uint32_t> sequence_starts;
    FramePlan plan;
    std::vector<std::string> warnings;
};

// Plays a song for the conversion. A piece that loops plays its list of sequences `loops` times from its start. From
// LoopStart::kSecondPass, it plays them `loops` times from its second pass, after the first pass. A piece that doesn't
// loop plays until its track stops and no voice plays. Notes that still sound at the end end with it.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    const bool looping = seq.Piece().Loops();
    const size_t count = seq.Piece().sequences.size();
    const bool second = start_pass == LoopStart::kSecondPass;
    const size_t passes = size_t(std::max(loops, 1)) + (second ? 1 : 0);
    bool done = false;
    while (!done && seq.Frame() < kMaxFrames)
    {
        seq.Step();
        done = looping ? seq.SequenceStarts().size() > passes * count : !seq.Playing();
    }

    sim.sequence_starts = seq.SequenceStarts();
    sim.warnings = seq.Warnings();
    if (looping && done)
    {
        sim.plan.loops = true;
        sim.plan.loop_start = sim.sequence_starts[second ? count : 0];
        sim.plan.loop_end = sim.sequence_starts[(second ? 2 : 1) * count];
        sim.plan.end = sim.sequence_starts[passes * count];
    }
    else
    {
        sim.plan.end = std::max<uint32_t>(seq.Frame(), 1);
    }
    if (!done)
    {
        sim.warnings.push_back("it was cut off after an hour");
    }

    for (Note n : seq.Notes())
    {
        if (n.on >= sim.plan.end)
        {
            continue;
        }

        if (!n.ended || n.off > sim.plan.end)
        {
            n.ended = true;
            n.off = sim.plan.end;
        }
        sim.notes.push_back(n);
    }

    return sim;
}

// Returns true if the notes of a looping song's second pass start as those of its first, in `sim`'s first two passes:
// the same keys at the same frames from the pass's start, with the same velocity, kit offset or instrument, and sample.
// The kit's offset is the setting that a pass can leave to the next.
bool FirstPassRepeats(const Simulation& sim)
{
    if (!sim.plan.loops)
    {
        return true;
    }

    PassComparison passes(sim.plan.loop_start, sim.plan.loop_end - sim.plan.loop_start);
    for (const Note& n : sim.notes)
    {
        passes.Add(n.on, n.channel, n.key, {double(n.velocity), double(n.program), double(n.sample)});
    }

    return passes.Alike();
}

// Plays a song for the conversion, with `loops` passes of its loop. A piece that loops whose first pass plays unlike
// the next marks its second pass.
Simulation SimulateConversion(const Rom& rom, const DriverInfo& info, int song, int loops)
{
    Simulation sim = Simulate(rom, info, song, loops, LoopStart::kFirstPass);
    if (sim.plan.loops && !FirstPassRepeats(Simulate(rom, info, song, 2, LoopStart::kFirstPass)))
    {
        sim = Simulate(rom, info, song, loops, LoopStart::kSecondPass);
    }

    return sim;
}

// Returns the place of a sequence channel in kChannels, or -1 if it plays no notes.
int ChannelIndex(int channel)
{
    for (size_t i = 0; i < kChannels.size(); i++)
    {
        if (kChannels[i] == channel)
        {
            return int(i);
        }
    }

    return -1;
}

// Returns a note's MIDI velocity, or 0 if the driver plays it silently. A kit note's velocity scales its sample, 64 to
// the whole scale. A square's or the noise's starting volume scales the PSG, and an envelope that rises from 0 is taken
// at full volume. The wave's volume code plays it at 100%, 50% or 25%. SquareRootValue() turns the level into the
// velocity.
int Velocity(const Note& n)
{
    double level = 0;
    if (PlaysKit(n.channel, n.key))
    {
        level = n.velocity / 64.0;
    }
    else if (n.channel == kWaveChannel)
    {
        static const double kWaveLevels[4] = {0, 1, 0.5, 0.25};
        level = kWaveLevels[(n.envelope >> 5) & 3];
    }
    else
    {
        const int volume = n.envelope >> 4;
        const bool rises = (n.envelope & 8) && (n.envelope & 7);
        level = (volume ? volume : rises ? 15 : 0) / 15.0;
    }

    return level > 0 ? SquareRootValue(level, 1, 1) : 0;
}

// Returns the notes that sound on the sequence channels whose bits `track_mask` sets, and adds their instruments to
// `instruments`. Adds a warning to `warnings` if a PSG note has an envelope. The conversion leaves envelopes out.
ChannelNotes ChooseNotes(const Simulation& sim, uint32_t track_mask, InstrumentSet& instruments,
                         std::vector<std::string>& warnings)
{
    ChannelNotes notes;
    bool envelopes = false;
    for (const Note& n : sim.notes)
    {
        const int c = ChannelIndex(n.channel);
        if (c < 0 || !(track_mask & (1 << n.channel)) || !Velocity(n))
        {
            continue;
        }

        notes[size_t(c)].push_back(n);
        if (PlaysKit(n.channel, n.key))
        {
            instruments.AddKitKey(n.program, n.key, n.sample);
        }
        else if (PlaysNoise(n.channel, n.key))
        {
            instruments.AddNoiseKey(n.program, n.key, n.control);
        }
        else if (n.channel == kWaveChannel)
        {
            instruments.AddWave(n.program);
        }
        else
        {
            instruments.AddSquare(n.channel, n.program, n.duty);
        }
        envelopes = envelopes || (n.channel != kWaveChannel && (n.envelope & 7));
    }
    if (envelopes)
    {
        warnings.push_back("some PSG notes have an envelope, which the conversion leaves out");
    }

    return notes;
}

// Returns the song's beat. It comes from the frames on which every channel's notes start and end, including the
// channels that the conversion leaves out. Conversions of different channels then line up. Each channel's first event
// is a rest at the piece's start. The files then start where the piece starts, even when it starts with silence.
BeatGrid SongGrid(const Simulation& sim)
{
    std::vector<std::vector<uint32_t>> starts(kChannels.size());
    for (const Note& n : sim.notes)
    {
        const int c = ChannelIndex(n.channel);
        if (c >= 0 && Velocity(n))
        {
            if (starts[size_t(c)].empty())
            {
                starts[size_t(c)].push_back(0);
            }
            starts[size_t(c)].push_back(n.on);
            starts[size_t(c)].push_back(n.off);
        }
    }
    for (std::vector<uint32_t>& frames : starts)
    {
        std::sort(frames.begin(), frames.end());
    }

    std::vector<uint32_t> hints = sim.sequence_starts;
    hints.erase(std::remove_if(hints.begin(), hints.end(), [&](uint32_t f) { return f >= sim.plan.end; }), hints.end());

    return BeatGrid(starts, hints);
}

// Writes to `mt` the track of the channel at place `c` in kChannels. The channel plays `notes`, at least one. The
// track holds its name, its end at tick `end`, and the notes at the ticks that `ticks` gives them. The channel plays
// on its MIDI channel in kMidiChannels, in the bank that PresetBank() gives it. At tick 0, the track sets the first
// note's bank and program. It sets CC7 and CC11 to 127, CC10 to 64, and CC91 and CC93 to 0. The notes' velocities
// hold their levels. The mixer is mono and adds no reverb or chorus. The conversion writes no bends. The track
// therefore sets no bend range. Unless the loop starts with the song, the channel gives its bank and program again
// at its first note from tick `loop_start`.
void WriteChannel(MidiTrack& mt, size_t c, const std::vector<Note>& notes, const BeatTicks& ticks, uint32_t loop_start,
                  uint32_t end)
{
    mt.Name(ChannelName(kChannels[c]));
    mt.SetEnd(end);
    const int preset_bank = PresetBank(kChannels[c]);
    const int midi_channel = kMidiChannels[c];
    int bank = preset_bank;
    int program = notes.front().program;
    mt.Bank(0, midi_channel, bank);
    mt.Program(0, midi_channel, program);
    mt.Control(0, midi_channel, cc::kVolume, 127);
    mt.Control(0, midi_channel, cc::kPan, 64);
    mt.Control(0, midi_channel, cc::kExpression, 127);
    mt.Control(0, midi_channel, cc::kReverb, 0);
    mt.Control(0, midi_channel, cc::kChorus, 0);
    bool restated = loop_start == 0;
    for (const Note& n : notes)
    {
        const uint32_t on = ticks.Channel(int(c), n.on, end);
        const uint32_t off = std::max(on + 1, ticks.Channel(int(c), n.off, end));
        if (!restated && on >= loop_start)
        {
            bank = program = -1;
            restated = true;
        }
        if (bank != preset_bank)
        {
            mt.Bank(on, midi_channel, preset_bank);
            bank = preset_bank;
        }
        if (program != n.program)
        {
            mt.Program(on, midi_channel, n.program);
            program = n.program;
        }
        mt.NoteOn(on, midi_channel, n.key, Velocity(n));
        mt.NoteOff(off, midi_channel, n.key);
    }
}

// Writes the MIDI file: a conductor track with the tempo map, then a track for each channel that plays notes. Each
// event goes on the song's beat, or with `frame_timing`, on the frame the driver plays it in.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const FramePlan& plan,
               const BeatGrid& grid, bool frame_timing, const ChannelNotes& notes, std::string& error)
{
    const BeatTicks ticks(grid, frame_timing);
    const uint32_t end = ticks.Song(plan.end);
    MidiFile midi(kBeatQuarterTicks);
    WriteBeatConductor(midi.AddTrack(), title, about, ticks, plan, end);

    const uint32_t loop_start = plan.loops ? ticks.Marker(plan.loop_start) : 0;
    for (size_t c = 0; c < notes.size(); c++)
    {
        if (!notes[c].empty())
        {
            WriteChannel(midi.AddTrack(), c, notes[c], ticks, loop_start, end);
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, or with `write` false, works out what the conversion would be.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write)
{
    SongReport report;
    MusicPiece piece;
    if (!ReadMusic(rom, info, song, piece))
    {
        report.warnings.push_back("its list of sequences can't be read");
        return report;
    }

    // The model's notes, those that the files hold and their instruments, and the song's beat. The simulation's
    // warnings come first.
    const Simulation sim = SimulateConversion(rom, info, song, settings.loops);
    report.warnings = sim.warnings;
    InstrumentSet own;
    const ChannelNotes notes = ChooseNotes(sim, settings.track_mask, shared ? *shared : own, report.warnings);
    const BeatGrid grid = SongGrid(sim);

    // The tracks that play notes, the length, the loop and the first tempo.
    for (const std::vector<Note>& list : notes)
    {
        report.tracks += list.empty() ? 0 : 1;
    }
    report.seconds = FrameSeconds(sim.plan.end);
    if (sim.plan.loops)
    {
        report.loop_start = FrameSeconds(sim.plan.loop_start);
        report.loop_end = FrameSeconds(sim.plan.loop_end);
    }
    report.bpm = 60 / FrameSeconds(grid.Tempos().front().frames);

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description.
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) music %d, resource %d at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, info.songs[size_t(song)], unsigned(piece.address), kProgramName);
    const std::string stem = SongStem(settings, number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, sim.plan, grid, settings.frame_timing, notes, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        Sf2File file;
        for (const std::string& w : own.Build(rom, info, file))
        {
            report.warnings.push_back(w);
        }
        file.SetTitle(title);
        report.sf2_path = stem + ".sf2";
        if (!file.Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return report;
        }
    }

    report.ok = true;

    return report;
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

} // namespace agbsf2::ubimilan
