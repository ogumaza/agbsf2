// SPDX-License-Identifier: MIT

// Song conversion for AlphaDream's driver. The model plays a song frame by frame, and its notes go on the song's beat,
// which BeatGrid works out from the frames they start and end on.

#include "alphadream/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "alphadream/driver.h"
#include "alphadream/sequencer.h"
#include "alphadream/soundfont.h"
#include "beat_grid.h"
#include "beat_midi.h"
#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::alphadream
{
namespace
{

// The longest that a conversion plays: an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// What the model played, up to the conversion's end.
struct Simulation
{
    std::vector<Note> notes;
    std::vector<Note> held; // every note from its start to the frame on which its track lets it go, for the beat
    std::vector<Change> changes;
    FramePlan plan;
    std::vector<std::string> warnings;
};

// Returns a looping song's period: of the periods between the tracks' first two gotos, the one that a whole number of
// passes of the most tracks fill. Of two that the same number of tracks fill, the shorter. The tracks that fill it
// repeat seamlessly in a looping player. A track that doesn't jumps at each repeat.
uint32_t LoopPeriod(const Sequencer& seq)
{
    std::vector<uint32_t> periods;
    for (const std::vector<uint32_t>& frames : seq.GotoFrames())
    {
        if (frames.size() >= 2 && frames[1] > frames[0])
        {
            periods.push_back(frames[1] - frames[0]);
        }
    }
    uint32_t best = seq.LoopFrame(1) - seq.LoopFrame(0);
    std::ptrdiff_t most = 0;
    for (uint32_t p : periods)
    {
        const std::ptrdiff_t filled =
            std::count_if(periods.begin(), periods.end(), [p](uint32_t q) { return p % q == 0; });
        if (filled > most || (filled == most && p < best))
        {
            best = p;
            most = filled;
        }
    }
    return best;
}

// Plays a song for the conversion. A song whose tracks go back by a goto loops: its loop starts on the frame on which
// the last of them first goes back, and lasts for LoopPeriod(). The commands after a goto run on the same frame. The
// conversion plays `loops` passes of the loop. A song that doesn't loop plays until every track has ended and no
// sample sounds. Notes that still sound at the end end with it.
Simulation Simulate(const Rom& rom, const DriverInfo& info, const Song& song, int loops)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    const int passes = std::max(loops, 1);
    bool done = false;
    while (!done && seq.Frame() < kMaxFrames)
    {
        seq.Step();
        done = seq.Looped(passes + 1) || !seq.Playing();
    }

    if (seq.Looped(passes + 1))
    {
        sim.plan.loops = true;
        sim.plan.loop_start = seq.LoopFrame(0);
        sim.plan.loop_end = sim.plan.loop_start + LoopPeriod(seq);
        sim.plan.end = sim.plan.loop_start + uint32_t(passes) * (sim.plan.loop_end - sim.plan.loop_start);
        // A track with a shorter period than the loop's can go back for the last time before the loop's last pass
        // ends.
        while (seq.Frame() < sim.plan.end && seq.Frame() < kMaxFrames)
        {
            seq.Step();
        }
    }
    else
    {
        sim.plan.end = std::max<uint32_t>(seq.Frame(), 1);
    }
    sim.warnings = seq.Warnings();
    if (!done)
    {
        sim.warnings.push_back("it was cut off after an hour");
    }

    for (Note n : seq.Notes())
    {
        if (n.on < sim.plan.end && n.released != n.on)
        {
            Note h = n;
            h.released = std::min(n.released, sim.plan.end);
            sim.held.push_back(h);
        }
        // A note that another note replaces on the frame it starts never reaches the mixer, and neither does one that
        // its channel stops on that frame. Each track's first frame starts and releases such a note.
        if (n.on >= sim.plan.end || (n.ended && n.off == n.on))
        {
            continue;
        }
        if (!n.ended || n.off > sim.plan.end)
        {
            n.ended = true;
            n.off = sim.plan.end;
        }
        n.off = std::max(n.off, n.on + 1);
        sim.notes.push_back(n);
    }
    for (const Change& c : seq.Changes())
    {
        if (c.frame < sim.plan.end)
        {
            sim.changes.push_back(c);
        }
    }
    return sim;
}

// The notes and changes of each track.
struct TrackPlay
{
    std::vector<Note> notes;
    std::vector<Change> changes;
};
using Tracks = std::array<TrackPlay, kTracks>;

// Returns the notes and changes of the tracks whose bits `track_mask` sets, and adds their instruments to
// `instruments`.
Tracks ChooseTracks(const Simulation& sim, uint32_t track_mask, InstrumentSet& instruments)
{
    Tracks tracks;
    for (const Note& n : sim.notes)
    {
        if (track_mask & (1u << n.track))
        {
            tracks[size_t(n.track)].notes.push_back(n);
            instruments.Add(TrackKind(n.track), n.instrument, n.key, n.region);
        }
    }
    for (const Change& c : sim.changes)
    {
        if (track_mask & (1u << c.track))
        {
            tracks[size_t(c.track)].changes.push_back(c);
        }
    }
    return tracks;
}

// Returns the song's beat, from the frames on which the tracks start and release their notes.
BeatGrid SongGrid(const Simulation& sim)
{
    std::vector<std::vector<uint32_t>> starts(kTracks);
    for (const Note& n : sim.held)
    {
        std::vector<uint32_t>& frames = starts[size_t(n.track)];
        if (frames.empty())
        {
            frames.push_back(0);
        }
        frames.push_back(n.on);
        frames.push_back(n.released);
    }
    for (std::vector<uint32_t>& frames : starts)
    {
        std::sort(frames.begin(), frames.end());
    }
    std::vector<uint32_t> hints;
    if (sim.plan.loops)
    {
        hints = {sim.plan.loop_start, sim.plan.loop_end};
    }
    return BeatGrid(starts, hints);
}

// Returns the name of a track's MIDI track.
std::string TrackName(int track)
{
    static const char* const kPsgNames[] = {"Square 1", "Square 2", "Wave", "Noise"};
    return track < kSampleTracks ? "Track " + std::to_string(track) : kPsgNames[track - kSampleTracks];
}

// Writes a track's notes, their pitches and their levels. The MIDI channel is the track's, without the drum channel.
// The levels give each note's pan and expression, and the pitches its bend.
void WriteTrack(MidiTrack& mt, int track, const TrackPlay& play, const BeatTicks& ticks, uint32_t loop_start,
                uint32_t end)
{
    mt.Name(TrackName(track));
    mt.SetEnd(end);
    double widest = 0;
    for (const Change& c : play.changes)
    {
        widest = std::max(widest, std::abs(c.pitch));
    }
    const int range = BendRangeFor(widest, kSmallBend);
    const Kind kind = TrackKind(track);
    const int first = play.notes.front().instrument;
    ChannelWriter writer(mt, MelodicChannel(track), first, NumberedPreset(PresetNumber(kind, first)), range,
                         loop_start);
    size_t next = 0;
    for (size_t i = 0; i < play.notes.size(); i++)
    {
        const Note& n = play.notes[i];
        const uint32_t on = ticks.Channel(track, n.on, end);
        const uint32_t off = std::max(on + 1, ticks.Channel(track, n.off, end));
        writer.SetProgram(on, n.instrument, NumberedPreset(PresetNumber(kind, n.instrument)));
        const uint32_t until = i + 1 < play.notes.size() ? play.notes[i + 1].on : UINT32_MAX;
        while (next < play.changes.size() && play.changes[next].frame <= n.on)
        {
            const Change& c = play.changes[next++];
            writer.SetLevels(on, c.left, c.right);
            writer.SetBend(on, c.pitch);
        }
        writer.Note(on, off, n.key);
        while (next < play.changes.size() && play.changes[next].frame < until)
        {
            const Change& c = play.changes[next++];
            const uint32_t at = std::min(ticks.Channel(track, c.frame, end), off);
            writer.SetLevels(at, c.left, c.right);
            writer.SetBend(at, c.pitch);
        }
    }
}

bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const FramePlan& plan,
               const BeatGrid& grid, bool frame_timing, const Tracks& tracks, std::string& error)
{
    const BeatTicks ticks(grid, frame_timing);
    const uint32_t end = ticks.Song(plan.end);
    MidiFile midi(kBeatQuarterTicks);
    WriteBeatConductor(midi.AddTrack(), title, about, ticks, plan, end);
    const uint32_t loop_start = plan.loops ? ticks.Marker(plan.loop_start) : 0;
    for (int t = 0; t < kTracks; t++)
    {
        if (!tracks[size_t(t)].notes.empty())
        {
            WriteTrack(midi.AddTrack(), t, tracks[size_t(t)], ticks, loop_start, end);
        }
    }
    return WriteFile(path, midi.Encode(), error);
}

SongReport Run(const Rom& rom, const DriverInfo& info, int number, const ConvertSettings& settings,
               InstrumentSet* shared, bool write)
{
    SongReport report;
    Song song;
    if (!ReadSong(rom, info, number, song))
    {
        report.warnings.push_back("it plays no track");
        return report;
    }
    report.address = song.address;

    const Simulation sim = Simulate(rom, info, song, settings.loops);
    report.warnings = sim.warnings;
    InstrumentSet own;
    const Tracks tracks = ChooseTracks(sim, settings.track_mask, shared ? *shared : own);
    for (const TrackPlay& t : tracks)
    {
        report.tracks += t.notes.empty() ? 0 : 1;
    }
    report.seconds = FrameSeconds(sim.plan.end);
    if (sim.plan.loops)
    {
        report.loop_start = FrameSeconds(sim.plan.loop_start);
        report.loop_end = FrameSeconds(sim.plan.loop_end);
    }
    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        if (!report.silent)
        {
            report.bpm = 60 / FrameSeconds(SongGrid(sim).Tempos().front().frames);
        }
        return report;
    }

    const BeatGrid grid = SongGrid(sim);
    report.bpm = 60 / FrameSeconds(grid.Tempos().front().frames);
    const std::string file_number = SongNumber(number, info.song_count);
    const std::string title = rom.Title() + " #" + file_number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) song %d at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), number, unsigned(song.address), kProgramName);
    const std::string stem = SongStem(settings, file_number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, sim.plan, grid, settings.frame_timing, tracks, error))
    {
        report.warnings.push_back(error);
        return report;
    }
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

// Returns a listing of a track's commands, from its start to its end or its first goto back. In B85A and A84J, a note's
// third byte is the track's volume, and F1 is a note's length. In B85A, FA sets the channel's priority.
std::string DumpTrack(const Rom& rom, Version version, uint32_t data)
{
    std::string out;
    char line[96];
    uint16_t at = 0;
    bool rests = false;
    const auto next = [&]()
    {
        return rom.U8(data + at++);
    };
    for (int guard = 0; guard < 65536; guard++)
    {
        const uint16_t start = at;
        const uint8_t b = next();
        std::snprintf(line, sizeof line, "  +%04x  ", start);
        out += line;
        if (b == 0xE0 || b == 0xE3)
        {
            const int depth = next();
            const int period = next();
            std::snprintf(line, sizeof line, "%s LFO depth %d period %d delay %d\n", b == 0xE0 ? "pitch" : "level",
                          depth, period, next());
        }
        else if (b == 0xE1 || b == 0xE2 || b == 0xE4 || b == 0xE5)
        {
            std::snprintf(line, sizeof line, "%s LFO %s\n", b < 0xE3 ? "pitch" : "level",
                          b == 0xE1 || b == 0xE4 ? "on" : "off");
        }
        else if (b >= 0xE8 && b <= 0xEF)
        {
            const int times = next();
            const int lo = next();
            std::snprintf(line, sizeof line, "%s %d, counter %d, to +%04x\n", b < 0xEC ? "loop" : "break", times, b & 3,
                          unsigned(uint16_t(at + 1 + (lo | (rom.U8(data + at) << 8)))));
            at++;
        }
        else if (b == 0xFA && version == Version::kHamSports)
        {
            std::snprintf(line, sizeof line, "priority %d\n", next());
        }
        else if ((b >= 0xF0 && b <= 0xF6 && (b != 0xF1 || version == Version::kSuperstarSaga)) || b == 0xF9)
        {
            static const char* const kNames[] = {"instrument", "volume", "pan", "fine tune",
                                                 "bend range", "bend",   "rest"};
            std::snprintf(line, sizeof line, "%s %d\n", b == 0xF9 ? "tempo" : kNames[b - 0xF0], next());
        }
        else if (b == 0xF7)
        {
            std::snprintf(line, sizeof line, "release\n");
        }
        else if (b == 0xF8)
        {
            const int lo = next();
            const uint16_t to = uint16_t(at + 1 + (lo | (rom.U8(data + at) << 8)));
            at++;
            std::snprintf(line, sizeof line, "goto +%04x\n", unsigned(to));
            out += line;
            if (to <= start)
            {
                break;
            }
            continue;
        }
        else if (b == 0xFF)
        {
            out += "end\n";
            break;
        }
        else
        {
            const uint8_t k = next();
            rests = rests || (b && (k & 0x80));
            char volume[24] = "";
            if (version != Version::kSuperstarSaga)
            {
                std::snprintf(volume, sizeof volume, " volume %d", next());
            }
            if (b == 0)
            {
                std::snprintf(line, sizeof line, "tie key %d%s\n", k & 0x7F, volume);
                out += line;
                if (k & 0x80)
                {
                    std::snprintf(line, sizeof line, "  +%04x  wait %d\n", unsigned(at), next());
                    out += line;
                }
                continue;
            }
            std::snprintf(line, sizeof line, "note key %d length %d%s\n", k & 0x7F, b, volume);
            out += line;
            if (rests)
            {
                std::snprintf(line, sizeof line, "  +%04x  rest %d\n", unsigned(at), next());
                out += line;
            }
            continue;
        }
        out += line;
    }
    return out;
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

bool DumpSong(const Rom& rom, const DriverInfo& info, int number, const std::string& path, std::string& error)
{
    Song song;
    if (!ReadSong(rom, info, number, song))
    {
        error = "song " + std::to_string(number) + " plays no track";
        return false;
    }
    std::string text = "song " + std::to_string(number) + " at " + Hex(song.address) + "\n";
    for (int t = 0; t < kTracks; t++)
    {
        if (song.mask & (1 << t))
        {
            text += "track " + std::to_string(t) + " (" + TrackName(t) + ") at " + Hex(song.tracks[size_t(t)]) + "\n";
            text += DumpTrack(rom, info.version, song.tracks[size_t(t)]);
        }
    }
    return WriteTextFile(path, text, error);
}

} // namespace agbsf2::alphadream
