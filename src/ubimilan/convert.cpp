// SPDX-License-Identifier: MIT

// Song conversion for Ubisoft Milan's driver. The model plays a piece frame by frame, and its notes go on the song's
// beat, which BeatGrid works out from the frames they start and end on, since the driver counts frames.

#include "ubimilan/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "beat_grid.h"
#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "sf2.h"
#include "ubimilan/sequencer.h"

namespace agbsf2::ubimilan
{
namespace
{

constexpr uint16_t kQuarterTicks = 480;

// The longest that a song plays: an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The sequence channels that play notes, in the order of the MIDI file's tracks, and the tracks' names: the PSG's
// three, then channel 9.
constexpr std::array<int, 4> kChannels = {0, 1, 2, kKitChannel};
const char* const kChannelNames[4] = {"Square 1", "Square 2", "Wave", "Kit"};

// The MIDI channel of each of those channels. Channel 9 plays on MIDI channel 11. Players treat notes on MIDI channel
// 10 as drums, and some ignore their note-offs or their samples' loops. The kit's notes require both.
constexpr std::array<int, 4> kMidiChannels = {0, 1, 2, 10};

// Returns the seconds that `frames` frames last.
double FrameSeconds(double frames)
{
    return frames * double(kFrameCycles) / double(kSecondCycles);
}

// The conversion's length and loop, in frames.
struct Plan
{
    bool loops = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t end = 0;
};

// What the model played, up to the conversion's end.
struct Simulation
{
    std::vector<Note> notes;
    std::vector<uint32_t> segment_starts;
    Plan plan;
    std::vector<std::string> warnings;
};

// Plays a song for the conversion. A piece that loops plays its list of sequences `loops` times from its start, or with
// `second`, from its second pass, which then plays the first once before it; one that doesn't plays until its track
// stops and no voice plays. Notes that still sound at the end end with it.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, bool second)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    MusicPiece piece;
    ReadMusic(rom, info, song, piece);
    const bool looping = piece.flags & 1;
    const size_t count = piece.segments.size();
    const size_t passes = size_t(std::max(loops, 1)) + (second ? 1 : 0);
    bool done = false;
    while (!done && seq.Frame() < kMaxFrames)
    {
        seq.Step();
        done = looping ? seq.SegmentStarts().size() > passes * count : !seq.Playing();
    }

    sim.segment_starts = seq.SegmentStarts();
    sim.warnings = seq.Warnings();
    if (looping && done)
    {
        sim.plan.loops = true;
        sim.plan.loop_start = sim.segment_starts[second ? count : 0];
        sim.plan.loop_end = sim.segment_starts[(second ? 2 : 1) * count];
        sim.plan.end = sim.segment_starts[passes * count];
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
// the same keys at the same frames from the pass's start, with the same velocity, kit offset or instrument, and
// sample. The kit's offset is the setting that a pass can leave to the next.
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
// the whole scale. A square's or the noise's starting volume scales the PSG, and an envelope that rises from 0 is
// taken at full volume. The wave's volume code plays it at 100%, 50% or 25%.
int Velocity(const Note& n)
{
    double level = 0;
    if (n.channel == kKitChannel && n.key > kLastNoiseKey)
    {
        level = n.velocity / 64.0;
    }
    else if (n.channel == 2)
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

    return level > 0 ? std::clamp(int(std::lround(level * 127)), 1, 127) : 0;
}

// Returns the MIDI tick of a place `quarters` quarter notes into the song.
uint32_t QuarterTick(double quarters)
{
    return uint32_t(std::lround(std::max(0.0, quarters) * kQuarterTicks));
}

// Writes the MIDI file: a conductor track with the tempo map, then a track for each channel that plays notes. Each
// event goes on the song's beat, or with `frame_timing`, on the frame the driver plays it in. Each channel plays on its
// MIDI channel in kMidiChannels and in the bank numbered after it.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Plan& plan,
               const BeatGrid& grid, bool frame_timing, const std::array<std::vector<Note>, 4>& notes,
               std::string& error)
{
    // The song's end is on the beat.
    const auto song_tick = [&](double frame)
    {
        return QuarterTick(frame_timing ? grid.Quarters(frame) : grid.Snap(frame));
    };

    // A loop marker comes no later than any channel's events from its frame on.
    const auto loop_tick = [&](double frame)
    {
        return QuarterTick(frame_timing ? grid.Quarters(frame) : grid.Marker(frame));
    };

    const uint32_t end = song_tick(plan.end);
    const auto tick = [&](int c, double frame)
    {
        return std::min(end, QuarterTick(frame_timing ? grid.Quarters(frame) : grid.Quarters(c, frame)));
    };

    MidiFile midi(kQuarterTicks);
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const BeatGrid::Tempo& t : grid.Tempos())
    {
        conductor.Tempo(QuarterTick(t.quarter), uint32_t(std::lround(FrameSeconds(t.frames) * 1e6)));
    }

    // The beat's tempo at the loop's start is given again after the marker. The loop's end can have another, which a
    // player that keeps its tempo when it jumps back would otherwise play the loop's start at.
    if (plan.loops)
    {
        conductor.Meta(loop_tick(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(loop_tick(plan.loop_end), 0x06, "loopEnd");
        conductor.RepeatTempo(loop_tick(plan.loop_start));
    }
    conductor.SetEnd(end);

    // From the loop's start, each channel gives its bank and program again, unless the loop starts with the song.
    const uint32_t loop_start = plan.loops ? loop_tick(plan.loop_start) : 0;
    for (size_t c = 0; c < notes.size(); c++)
    {
        if (notes[c].empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name(kChannelNames[c]);
        mt.SetEnd(end);
        const int ch = kChannels[c];
        const int midi_channel = kMidiChannels[c];
        int bank = -1;
        int program = -1;
        bool restated = loop_start == 0;
        for (const Note& n : notes[c])
        {
            const uint32_t on = tick(int(c), n.on);
            const uint32_t off = std::max(on + 1, tick(int(c), n.off));
            if (!restated && on >= loop_start)
            {
                bank = program = -1;
                restated = true;
            }
            if (bank != ch)
            {
                mt.Bank(on, midi_channel, ch);
                bank = ch;
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

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, or with `write` false, works out what the conversion would be.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, InstrumentSet* shared,
                bool write)
{
    SongSummary sum;
    MusicPiece piece;
    if (!ReadMusic(rom, info, song, piece))
    {
        sum.warnings.push_back("its list of sequences can't be read");
        return sum;
    }

    // A piece that loops whose first pass plays unlike the next marks its second pass.
    Simulation sim = Simulate(rom, info, song, opt.loops, false);
    if (sim.plan.loops && !FirstPassRepeats(Simulate(rom, info, song, 2, false)))
    {
        sim = Simulate(rom, info, song, opt.loops, true);
    }
    sum.warnings = sim.warnings;

    // The notes that sound on the chosen channels, and their instruments.
    std::array<std::vector<Note>, 4> notes;
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    bool envelopes = false;
    for (const Note& n : sim.notes)
    {
        const int c = ChannelIndex(n.channel);
        if (c < 0 || !(opt.track_mask & (1 << n.channel)) || !Velocity(n))
        {
            continue;
        }

        notes[size_t(c)].push_back(n);
        if (n.channel == kKitChannel)
        {
            if (n.key > kLastNoiseKey)
            {
                instruments.AddKitKey(n.program, n.key, n.sample);
            }
            else
            {
                instruments.AddNoiseKey(n.program, n.key, uint8_t(n.control));
            }
        }
        else
        {
            instruments.AddPsg(n.channel, n.program, n.channel == 2 ? n.control : n.duty);
        }
        envelopes = envelopes || (n.channel != 2 && (n.envelope & 7));
    }
    if (envelopes)
    {
        sum.warnings.push_back("some PSG notes have an envelope, which the conversion leaves out");
    }

    // The song's beat, from the frames on which every channel's notes start and end, whichever channels the
    // conversion includes, so that conversions of different channels line up. Each channel's first event is the
    // piece's start, as a rest, so that the files start with the piece, whatever silence comes before the first note.
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
    std::vector<uint32_t> hints = sim.segment_starts;
    hints.erase(std::remove_if(hints.begin(), hints.end(), [&](uint32_t f) { return f >= sim.plan.end; }), hints.end());
    const BeatGrid grid(starts, hints);

    for (const std::vector<Note>& list : notes)
    {
        sum.tracks += list.empty() ? 0 : 1;
    }
    sum.seconds = FrameSeconds(sim.plan.end);
    if (sim.plan.loops)
    {
        sum.loop_start = FrameSeconds(sim.plan.loop_start);
        sum.loop_end = FrameSeconds(sim.plan.loop_end);
    }
    sum.bpm = 60 / FrameSeconds(grid.Tempos().front().frames);

    sum.silent = sum.tracks == 0;
    if (sum.silent || !write)
    {
        sum.ok = true;
        return sum;
    }

    // The files' names and titles, and the MIDI file's description.
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) music %d, resource %d at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, info.songs[size_t(song)], unsigned(piece.address), kProgramName);
    const std::string stem = Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + number));

    std::string error;
    sum.midi_path = stem + ".mid";
    if (!WriteMidi(sum.midi_path, title, about, sim.plan, grid, opt.frame_timing, notes, error))
    {
        sum.warnings.push_back(error);
        return sum;
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        Sf2File file;
        for (const std::string& w : own.Build(rom, info, file))
        {
            sum.warnings.push_back(w);
        }
        file.name = title;
        file.comment = "Instruments for " + title + ", extracted by " + kProgramName;
        sum.sf2_path = stem + ".sf2";
        if (!file.Write(sum.sf2_path, error))
        {
            sum.warnings.push_back(error);
            return sum;
        }
    }

    sum.ok = true;

    return sum;
}

} // namespace

SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt)
{
    return Run(rom, info, song, opt, nullptr, false);
}

SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        InstrumentSet* shared)
{
    return Run(rom, info, song, opt, shared, true);
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    MusicPiece piece;
    if (!ReadMusic(rom, info, song, piece))
    {
        error = "its list of sequences can't be read";
        return false;
    }

    // Each sequence's commands, with the frame each plays on from the sequence's start: a countdown of n puts the next
    // command n + 1 frames later, or on the same frame if it's 0. The piece's first sequence starts before the first
    // frame's step, so its first countdown puts its first command n frames in.
    std::string out;
    char line[160];
    std::snprintf(line, sizeof line, "music %d: resource %d at 0x%08X, flags %d (%s)\n", song, info.songs[size_t(song)],
                  unsigned(piece.address), piece.flags, piece.flags & 1 ? "loops" : "plays once");
    out += line;
    for (size_t i = 0; i < piece.segments.size(); i++)
    {
        const int id = piece.segments[i];
        const uint32_t address = ResourceAddress(rom, info, id);
        std::snprintf(line, sizeof line, "\nsequence %d at 0x%08X\n", id, unsigned(address));
        out += line;
        if (!address || rom.U8(address) != kSequence)
        {
            out += "  not a sequence\n";
            continue;
        }

        uint32_t at = address + 4;
        uint32_t wait = rom.U16(at);
        at += 2;
        uint32_t frame = i == 0 ? wait : wait ? wait + 1 : 0;
        while (rom.Contains(at, 1))
        {
            const int status = rom.U8(at);
            const int kind = status & 0xF0;
            std::snprintf(line, sizeof line, "  %6u  0x%08X  ", unsigned(frame), unsigned(at));
            out += line;
            at++;
            if (status == 0xFC)
            {
                out += "FC end\n";
                break;
            }
            if (kind == 0x90 || kind == 0x80)
            {
                std::snprintf(line, sizeof line, "%02X %s channel %d key %d velocity %d\n", unsigned(status),
                              kind == 0x90 ? "note on" : "note off", status & 0xF, rom.U8(at), rom.U8(at + 1));
                at += 2;
            }
            else if (kind == 0xC0)
            {
                std::snprintf(line, sizeof line, "%02X program channel %d: %d\n", unsigned(status), status & 0xF,
                              rom.U8(at));
                at++;
            }
            else if (kind == 0xE0)
            {
                std::snprintf(line, sizeof line, "%02X nothing\n", unsigned(status));
            }
            else
            {
                std::snprintf(line, sizeof line, "%02X unknown, on which the game resets\n", unsigned(status));
                out += line;
                break;
            }
            out += line;
            wait = rom.U16(at);
            at += 2;
            frame += wait ? wait + 1 : 0;
        }
    }

    return WriteFile(path, std::vector<uint8_t>(out.begin(), out.end()), error);
}

} // namespace agbsf2::ubimilan
