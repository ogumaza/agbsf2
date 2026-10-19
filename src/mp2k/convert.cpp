// SPDX-License-Identifier: MIT

#include "mp2k/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "files.h"
#include "midi.h"
#include "mp2k/sequencer.h"
#include "mp2k/song.h"
#include "music.h"
#include "program.h"

namespace agbsf2::mp2k
{
namespace
{

// Maximum conversion duration: one hour, measured in frames.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The frames that the model runs past the end, for notes whose attack takes a while to sound.
constexpr uint32_t kAttackFrames = 16;

// The driver's ticks to a quarter note.
constexpr int kTicksPerQuarter = 24;

// The reverb level whose feedback comes closest to the echo of Camelot's mixer: 53/128 of each side's output from four
// frames before.
constexpr int kCamelotEcho = 53;

// The channels of a MIDI file.
constexpr int kMidiChannels = 16;

// Returns the layouts of a song's first `count` tracks.
std::vector<TrackLayout> ScanTracks(const Rom& rom, const SongHeader& header, int count)
{
    std::vector<TrackLayout> layouts;
    for (int t = 0; t < count; t++)
    {
        layouts.push_back(ScanTrack(rom, header.tracks[size_t(t)]));
    }

    return layouts;
}

// Results of running a song through the model: actions, warnings, and the tick where it gave up, if it did.
struct Simulation
{
    std::vector<Action> actions;
    std::vector<uint64_t> tick_ends; // the player's tick count after each frame
    std::vector<std::string> warnings;
    std::optional<uint64_t> cut_off; // the tick where the model gave up, at the frame limit
};

// Runs the model until it has played the plan's last tick, or until every track has ended.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, const SongPlan& plan)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    if (plan.loops && plan.loop_start > 0)
    {
        seq.RestateFrom(plan.loop_start);
    }

    uint32_t frames = 0;
    bool finished = false; // the song reached the plan's end, or its tracks ended
    for (uint32_t f = 0; f < kMaxFrames; f++)
    {
        const std::vector<Action>& actions = seq.Step();
        sim.actions.insert(sim.actions.end(), actions.begin(), actions.end());
        sim.tick_ends.push_back(seq.Tick());
        frames = f + 1;
        if (seq.Tick() > plan.end || seq.TracksEnded())
        {
            finished = true;
            break;
        }
    }

    if (!finished && frames == kMaxFrames)
    {
        sim.warnings.push_back("the song was cut off after an hour");
        sim.cut_off = seq.Tick();
    }
    sim.warnings.insert(sim.warnings.end(), seq.Warnings().begin(), seq.Warnings().end());

    // A PSG note's attack can take 8 frames to raise its level above 0. So the model runs on past the end, and keeps
    // the notes that become audible there. It keeps the note ons too. They tell a channel's next note apart.
    for (uint32_t f = 0; f < kAttackFrames && !sim.cut_off; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn || a.kind == Action::kAudible)
            {
                sim.actions.push_back(a);
            }
        }
    }

    return sim;
}

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int key = 0;
    int velocity = 0;
    int level = 0;  // the velocities of the driver's channels that play it, added up
    int voices = 1; // the driver's channels that play it
    int program = -1;
    int psg = 0;      // the PSG channel its voice plays on, or 0 for a sample
    int play_key = 0; // the key the driver plays it at: its own, or a drum kit voice's
    uint64_t on = 0;
    uint64_t off = 0;
    bool open = true;
    bool audible = false;         // it got louder than silence in the game
    bool removed = false;         // the driver stopped it before it started
    bool before_programs = false; // it plays with the program from before its track's program changes on its tick
    int step = 0;                 // its step at its tick: it plays after the program change of that step
    int midi_channel = 0;         // the MIDI channel it plays on
    int sound_channel = -1;       // with a MIDI channel for each sound channel, the driver's channel that plays it
    uint64_t stop = 0;            // with a MIDI channel for each sound channel, where the driver stops its sound, or 0
};

// The MIDI file's notes, and the number of notes the game didn't play for want of a channel.
struct Notes
{
    std::vector<Note> notes;
    int dropped = 0;
};

// Returns true if the model dropped a note that the MIDI file would have had, for want of a channel.
bool CountsDropped(const Action& a, const SongPlan& plan, uint32_t track_mask)
{
    return a.kind == Action::kNoteDropped && a.tick < plan.end && (track_mask >> a.track & 1);
}

// Returns true if the note that note-on `a` starts goes in the MIDI file: it starts before the end, on a chosen track,
// with a voice and a velocity above 0.
bool KeepsNote(const Action& a, const SongPlan& plan, uint32_t track_mask)
{
    return a.tick < plan.end && (track_mask >> a.track & 1) && a.program >= 0 && a.velocity != 0;
}

// Returns the note that note-on `a` starts.
Note NoteFrom(const Rom& rom, const Action& a)
{
    Note n;
    n.track = a.track;
    n.key = a.key;
    n.velocity = a.velocity;
    n.level = a.velocity;
    n.program = a.program;
    Voice voice;
    ReadVoice(rom, a.voice, voice);
    n.psg = voice.PsgChannel();
    n.play_key = a.play_key;
    n.on = a.tick;

    return n;
}

// Ends an open note at `tick`, or at the end of the conversion if that comes first. A note lasts at least a tick, so
// that its note off can't come before its note on.
void CloseNote(Note& n, uint64_t tick, const SongPlan& plan)
{
    if (n.open)
    {
        n.open = false;
        n.off = std::max(std::min(tick, plan.end), n.on + 1);
    }
}

// Removes the notes that the driver stopped before they started, and those that never got louder than silence.
void RemoveSilent(std::vector<Note>& notes)
{
    notes.erase(std::remove_if(notes.begin(), notes.end(), [](const Note& n) { return n.removed || !n.audible; }),
                notes.end());
}

// Works out the MIDI file's notes from the model's note ons, releases and ends. Each track plays on a MIDI channel of
// its own. A MIDI channel can't play one key twice at once, so where a track does, the older note ends when the newer
// one starts, unless they start on the same tick, when they make one louder note. A note ends where the driver releases
// it, where its sound stops, where a later note takes its channel, or at the end of the conversion. A note whose
// envelope never gets above 0 is silent in the game and is left out. So are a sample voice without an attack, and a PSG
// note so quiet that its level rounds to 0.
Notes CollectNotes(const Rom& rom, const Simulation& sim, const SongPlan& plan, uint32_t track_mask)
{
    Notes result;
    std::vector<Note>& notes = result.notes;
    std::array<std::array<int, 128>, kMaxTracks> owner;
    for (auto& keys : owner)
    {
        keys.fill(-1);
    }

    // Each channel's note while the driver holds it, and the last note it started.
    std::array<int, kChannelCount> held = {};
    std::array<int, kChannelCount> last = {};
    held.fill(-1);
    last.fill(-1);

    auto close = [&](int index, uint64_t tick)
    {
        Note& n = notes[size_t(index)];
        if (n.open)
        {
            owner[size_t(n.track)][size_t(n.key)] = -1;
        }
        CloseNote(n, tick, plan);
    };

    // Lets go of a channel's part in its note, which ends with its last channel.
    auto let_go = [&](int channel, uint64_t tick)
    {
        const int index = held[size_t(channel)];
        held[size_t(channel)] = -1;
        if (index >= 0 && --notes[size_t(index)].voices == 0)
        {
            close(index, tick);
        }
    };

    for (const Action& a : sim.actions)
    {
        if (CountsDropped(a, plan, track_mask))
        {
            result.dropped++;
        }
        else if (a.kind == Action::kNoteOn)
        {
            held[size_t(a.channel)] = -1;
            last[size_t(a.channel)] = -1;
            if (!KeepsNote(a, plan, track_mask))
            {
                continue;
            }

            int& same_key = owner[a.track][a.key];
            if (same_key >= 0 && notes[size_t(same_key)].on == a.tick)
            {
                Note& n = notes[size_t(same_key)];
                n.level += a.velocity;
                n.velocity = std::min(127, n.level);
                n.voices++;
                held[size_t(a.channel)] = same_key;
                last[size_t(a.channel)] = same_key;
                continue;
            }
            if (same_key >= 0)
            {
                close(same_key, a.tick);
            }

            notes.push_back(NoteFrom(rom, a));
            same_key = int(notes.size()) - 1;
            held[size_t(a.channel)] = same_key;
            last[size_t(a.channel)] = same_key;
        }
        else if (a.kind == Action::kAudible)
        {
            const int index = held[size_t(a.channel)] >= 0 ? held[size_t(a.channel)] : last[size_t(a.channel)];
            if (index >= 0)
            {
                notes[size_t(index)].audible = true;
            }
        }
        else if (a.kind == Action::kRelease && a.channel >= 0)
        {
            let_go(a.channel, a.tick);
        }
        else if (a.kind == Action::kNoteEnd && a.channel >= 0)
        {
            // A note the mixer never started didn't sound, unless another channel plays it too. A channel the driver
            // has released has already left the note's count of voices.
            const int index = last[size_t(a.channel)];
            const int counted = held[size_t(a.channel)] == index ? 1 : 0;
            if (!a.sounded && index >= 0 && notes[size_t(index)].voices <= counted)
            {
                close(index, a.tick);
                notes[size_t(index)].removed = true;
                held[size_t(a.channel)] = -1;
            }
            else
            {
                let_go(a.channel, a.tick);
            }

            last[size_t(a.channel)] = -1;
        }
    }

    for (size_t i = 0; i < notes.size(); i++)
    {
        close(int(i), plan.end);
    }

    RemoveSilent(notes);

    return result;
}

// Works out the MIDI file's notes when each of the driver's sound channels has a MIDI channel: one for each note a
// channel plays. A note ends where the driver releases it, or where its sound stops if that comes first, and its sound
// stops where the channel's does: where it fades out, or where a later note takes the channel. A note whose sound stops
// on the tick it started is left out, as are those that never sound.
Notes CollectVoiceNotes(const Rom& rom, const Simulation& sim, const SongPlan& plan, uint32_t track_mask)
{
    Notes result;
    std::vector<Note>& notes = result.notes;

    // Each channel's note until its sound stops.
    std::array<int, kChannelCount> current;
    current.fill(-1);

    auto stop = [&](int channel, uint64_t tick, bool sounded)
    {
        Note& n = notes[size_t(current[size_t(channel)])];
        n.removed = n.removed || !sounded || tick <= n.on;
        CloseNote(n, tick, plan);
        n.stop = tick < plan.end ? tick : 0;
        current[size_t(channel)] = -1;
    };

    for (const Action& a : sim.actions)
    {
        if (CountsDropped(a, plan, track_mask))
        {
            result.dropped++;
        }
        else if (a.channel < 0)
        {
            continue;
        }
        else if (a.kind == Action::kNoteOn)
        {
            if (current[size_t(a.channel)] >= 0)
            {
                stop(a.channel, a.tick, true);
            }
            if (!KeepsNote(a, plan, track_mask))
            {
                continue;
            }

            Note n = NoteFrom(rom, a);
            n.sound_channel = a.channel;
            notes.push_back(n);
            current[size_t(a.channel)] = int(notes.size()) - 1;
        }
        else if (current[size_t(a.channel)] < 0)
        {
            continue;
        }
        else if (a.kind == Action::kAudible)
        {
            notes[size_t(current[size_t(a.channel)])].audible = true;
        }
        else if (a.kind == Action::kRelease)
        {
            CloseNote(notes[size_t(current[size_t(a.channel)])], a.tick, plan);
        }
        else if (a.kind == Action::kNoteEnd)
        {
            stop(a.channel, a.tick, a.sounded);
        }
    }

    for (Note& n : notes)
    {
        CloseNote(n, plan.end, plan);
    }

    RemoveSilent(notes);

    return result;
}

// The time each tick takes from a tick on, in frames, until the next change.
struct TempoChange
{
    uint64_t tick;
    double frames;
};

// Returns the time each tick takes from each tick on, as the driver plays the song. Each frame adds the tempo to the
// tick counter before running any ticks. When a song changes tempo, progress towards the next tick has already been
// counted at the old tempo. The next tick combines that progress with the remainder at the new tempo. Any ticks still
// due in the current frame run first, at the old tempo.
std::vector<TempoChange> TempoMap(const std::vector<Action>& actions)
{
    std::vector<TempoChange> map = {{0, double(kTickCounter) / kDefaultTempo}};

    auto set = [&](uint64_t tick, double frames)
    {
        while (!map.empty() && map.back().tick >= tick)
        {
            map.pop_back();
        }

        map.push_back({tick, frames});
    };

    for (const Action& a : actions)
    {
        if (a.kind != Action::kTempo)
        {
            continue;
        }

        const double old_tempo = std::max(a.old_tempo, 1);
        const double tempo = std::max(a.value, 1);
        const int progress = std::max(a.counter, 0);
        const uint64_t blend = a.tick + uint64_t(progress / kTickCounter);
        const int part = progress % kTickCounter;
        if (part)
        {
            set(blend, part / old_tempo + (kTickCounter - part) / tempo);
            set(blend + 1, kTickCounter / tempo);
        }
        else
        {
            set(blend, kTickCounter / tempo);
        }
    }

    return map;
}

// Returns true if the song's second pass through its loop plays at the tempos of its first: the same tempo where each
// starts, and the same changes at the same ticks from the pass's start. A tempo that the loop's end leaves carries into
// the next pass, unlike the one that the song's start gives the first, and a player that loops on the markers can't
// repeat that. The driver's tempos are compared rather than the MIDI file's, whose tick of each change also holds part
// of the old tempo, as much as the driver had counted at it. `sim` has to hold two passes of the loop.
bool TemposRepeat(const Simulation& sim, const SongPlan& plan)
{
    // The tempo after each tick that changes it.
    std::map<uint64_t, int> tempos;
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kTempo)
        {
            tempos[a.tick] = a.value;
        }
    }

    const auto tempo_at = [&](uint64_t tick)
    {
        const auto after = tempos.upper_bound(tick);
        return after == tempos.begin() ? kDefaultTempo : std::prev(after)->second;
    };

    const uint64_t length = plan.loop_end - plan.loop_start;
    PassComparison passes(plan.loop_start, length);
    passes.Add(plan.loop_start, 0, 0, {double(tempo_at(plan.loop_start))});
    passes.Add(plan.loop_end, 0, 0, {double(tempo_at(plan.loop_end))});
    for (const auto& [tick, tempo] : tempos)
    {
        if (tick != plan.loop_start && tick != plan.loop_end)
        {
            passes.Add(tick, 0, 0, {double(tempo)});
        }
    }

    return passes.Alike();
}

// Plans the conversion of a song from the layouts of the tracks that its music player plays (see PlanSong()). The
// marked loop starts at the song's second pass if its first pass plays at other tempos. That happens when a track sets
// the tempo on the loop's first tick before the loop starts, or when the loop's end leaves another tempo. A song that
// the model gave up on before its second pass ended keeps its loop.
SongPlan PlanConversion(const Rom& rom, const DriverInfo& info, int song, const std::vector<TrackLayout>& layouts,
                        int loops)
{
    const SongPlan plan = PlanSong(layouts, loops, false);
    if (!plan.loops)
    {
        return plan;
    }

    const SongPlan two = PlanSong(layouts, 2, false);
    const Simulation check = Simulate(rom, info, song, two);
    if ((!check.cut_off || *check.cut_off >= two.end) && !TemposRepeat(check, two))
    {
        return PlanSong(layouts, loops, true);
    }

    return plan;
}

// Returns the microseconds a quarter note lasts when a tick takes `frames` frames, at the GBA's frame rate.
uint32_t QuarterMicros(double frames)
{
    const double micros = 1e6 * kTicksPerQuarter * frames / kFrameRate;
    return uint32_t(std::clamp(std::lround(micros), 1L, 0xFFFFFFL));
}

// The MIDI file's ticks with frame timing, which puts each event at the start of the frame the driver plays it in. A
// tick is a unit of the tempo, so a quarter note has 24 × 150 of them, and each frame lasts the tempo that it adds to
// the driver's counter, and starts on a whole tick.
struct FrameGrid
{
    std::vector<uint64_t> tick_ends;              // the player's tick count after each frame
    std::vector<uint64_t> starts;                 // the MIDI tick each frame starts at, and the one after the last
    std::vector<std::pair<uint64_t, int>> tempos; // the MIDI tick of each frame that changes the tempo, and the tempo
};

constexpr uint16_t kFrameQuarterTicks = uint16_t(kTicksPerQuarter * kTickCounter);

// The MIDI file's ticks for each of the driver's, which put 960 to the quarter note. FluidSynth rounds its position to
// a whole tick at each tempo change, so at the driver's 24 to the quarter note it lost up to half a tick at each one,
// and rushed a song's ritardandos and accelerandos.
constexpr uint32_t kMidiTicksPerTick = 40;

// Returns the frame grid of a song. A frame adds the tempo that was set before it started; a tempo of 0 counts as 1, so
// that each frame lasts a tick.
FrameGrid MakeFrameGrid(const Simulation& sim)
{
    FrameGrid grid;
    grid.tick_ends = sim.tick_ends;
    grid.starts.push_back(0);
    int tempo = kDefaultTempo;
    size_t next = 0;
    for (uint32_t f = 0; f < sim.tick_ends.size(); f++)
    {
        for (; next < sim.actions.size() && sim.actions[next].frame < f; next++)
        {
            if (sim.actions[next].kind == Action::kTempo)
            {
                tempo = std::max(sim.actions[next].value, 1);
            }
        }

        if (grid.tempos.empty() || grid.tempos.back().second != tempo)
        {
            grid.tempos.push_back({grid.starts.back(), tempo});
        }
        grid.starts.push_back(grid.starts.back() + uint64_t(tempo));
    }

    return grid;
}

// Returns the MIDI tick of the start of the frame that the driver plays song tick `tick` in.
uint32_t FrameTick(const FrameGrid& grid, uint64_t tick)
{
    const auto it = std::upper_bound(grid.tick_ends.begin(), grid.tick_ends.end(), tick);
    const size_t f = size_t(it - grid.tick_ends.begin());
    return uint32_t(grid.starts[std::min(f, grid.starts.size() - 1)]);
}

// Returns the first song tick whose events go on the loop's start marker: the loop's start, or with frame timing, the
// first tick of the frame that plays it.
uint64_t LoopFrom(const SongPlan& plan, const FrameGrid* frames)
{
    if (!frames)
    {
        return plan.loop_start;
    }

    const auto played = std::upper_bound(frames->tick_ends.begin(), frames->tick_ends.end(), plan.loop_start);
    return played == frames->tick_ends.begin() ? 0 : *std::prev(played);
}

// Writes the MIDI file. With frame timing, the events go at the start of the frames the driver plays them in, and the
// conductor track, which has no tempos yet, gets the frames' tempos. Otherwise each of the driver's ticks becomes
// kMidiTicksPerTick of the file's.
bool WriteMidiFile(MidiFile& midi, MidiTrack& conductor, const FrameGrid* frames, const std::string& path,
                   std::string& error)
{
    if (frames)
    {
        const auto place = [&](uint32_t tick)
        {
            return FrameTick(*frames, tick);
        };
        midi.Retime(place);
        for (const auto& [tick, tempo] : frames->tempos)
        {
            conductor.Tempo(uint32_t(tick), QuarterMicros(double(kTickCounter) / tempo));
        }
        midi.SetDivision(kFrameQuarterTicks);
    }
    else
    {
        midi.Retime([](uint32_t tick) { return tick * kMidiTicksPerTick; });
        midi.SetDivision(uint16_t(kTicksPerQuarter * kMidiTicksPerTick));
    }

    return WriteFile(path, midi.Encode(), error);
}

// Returns the elapsed seconds at `tick`, at the GBA's frame rate.
double TickSeconds(const std::vector<TempoChange>& map, uint64_t tick)
{
    double frames = 0;
    for (size_t i = 0; i < map.size() && map[i].tick < tick; i++)
    {
        const uint64_t end = i + 1 < map.size() ? std::min(map[i + 1].tick, tick) : tick;
        frames += double(end - map[i].tick) * map[i].frames;
    }

    return frames / kFrameRate;
}

// Fills in a report's length, loop and tempo.
void Summarize(const std::vector<TempoChange>& map, const SongPlan& plan, SongReport& report)
{
    report.seconds = TickSeconds(map, plan.end);
    if (plan.loops)
    {
        report.loop_start = TickSeconds(map, plan.loop_start);
        report.loop_end = TickSeconds(map, plan.loop_end);
    }

    report.bpm = 60e6 / QuarterMicros(map.front().frames);
}

// A pitch offset the MIDI file gives a track, in 256ths of a semitone, from a tick on.
struct PitchPoint
{
    uint64_t tick;
    int track;
    int pitch;
};

// Returns the rate in Hz of the noise generator's steps at a noise setting: 524288 Hz divided by the ratio (0 counts as
// 0.5) and by 2 to the power of the shift + 1.
double NoiseClock(uint32_t setting)
{
    const uint32_t ratio = setting & 7;
    return 524288.0 / (ratio ? double(ratio) : 0.5) / double(uint32_t(2) << (setting >> 4));
}

// Returns the pitch offset that makes the SoundFont play a PSG note the way the driver plays it with the track's pitch
// offset `pitch`. The driver plays a noise note at the noise setting of its key and the offset's whole semitones, which
// steps through the noise settings 4 to an octave. It plays a square or wave note whose key and offset come to less
// than 36 at key 36, without the offset's fraction, and the SoundFont's zones play keys below 36 at key 36's pitch.
int PsgPitch(const Note& n, int pitch)
{
    const int key = n.play_key + (pitch >> 8);
    if (n.psg == 4)
    {
        const double ratio =
            NoiseClock(KeyToPsgFrequency(4, std::max(key, 0), 0)) / NoiseClock(KeyToPsgFrequency(4, n.play_key, 0));
        return int(std::lround(256.0 * 12.0 * std::log2(ratio)));
    }

    const int zone_key = std::max(n.play_key, 36);
    if (key <= 35)
    {
        return (36 - zone_key) * 256;
    }

    return pitch - (zone_key - n.play_key) * 256;
}

// Builds each track's MIDI pitch offsets from the model's pitch changes. While all of a track's notes are PSG notes,
// the offset follows the pitch that the driver plays the newest of them at (see PsgPitch()), through that note's
// release while no other note plays. A looping player retains the offset from the loop's end, as the game does until
// the loop sets the track's pitch, which the model then reports (see Sequencer::RestateFrom()), or starts a PSG note
// whose offset depends on its key, so the offset is written there even if it hasn't changed. `loop_from` is the first
// tick on the loop-start marker.
std::vector<PitchPoint> PitchPoints(const Simulation& sim, const std::vector<Note>& notes, const SongPlan& plan,
                                    uint64_t loop_from)
{
    std::vector<PitchPoint> points;
    for (int t = 0; t < kMaxTracks; t++)
    {
        // The ticks at which the track's pitch, or the notes it plays, change.
        std::map<uint64_t, int> raw;
        std::set<uint64_t> ticks;
        for (const Action& a : sim.actions)
        {
            if (a.kind == Action::kPitch && a.track == t && a.tick < plan.end)
            {
                raw[a.tick] = a.value;
                ticks.insert(a.tick);
            }
        }

        std::vector<const Note*> mine;
        for (const Note& n : notes)
        {
            if (n.track == t)
            {
                mine.push_back(&n);
                ticks.insert(n.on);
                ticks.insert(n.off);
            }
        }
        std::stable_sort(mine.begin(), mine.end(), [](const Note* a, const Note* b) { return a->on < b->on; });

        // Sweep the ticks, keeping the notes that play at each, and the newest of the last ones that played if they
        // were all PSG notes, whose release goes on at the pitch the driver plays it at until another note starts.
        std::vector<const Note*> playing;
        const Note* tail = nullptr;
        size_t next = 0;
        int pitch = 0;
        int last = 0x7FFFFFFF;
        bool before_loop = plan.loops && plan.loop_start > 0;
        for (uint64_t tick : ticks)
        {
            if (tick >= plan.end && tick > 0)
            {
                break;
            }

            const auto found = raw.find(tick);
            pitch = found != raw.end() ? found->second : pitch;

            while (next < mine.size() && mine[next]->on <= tick)
            {
                playing.push_back(mine[next++]);
            }
            playing.erase(std::remove_if(playing.begin(), playing.end(), [&](const Note* n) { return n->off <= tick; }),
                          playing.end());

            const Note* newest = nullptr;
            bool all_psg = true;
            for (const Note* n : playing)
            {
                all_psg = all_psg && n->psg != 0;
                newest = !newest || n->on >= newest->on ? n : newest;
            }

            if (newest)
            {
                tail = all_psg ? newest : nullptr;
            }

            const int value = tail ? PsgPitch(*tail, pitch) : pitch;
            const bool sets = found != raw.end() || (tail && tail->on == tick && value != pitch);
            if (before_loop && tick >= loop_from && sets)
            {
                before_loop = false;
                last = 0x7FFFFFFF;
            }

            if (value != last)
            {
                points.push_back({tick, t, value});
                last = value;
            }
        }
    }

    return points;
}

// Returns the pitch bend range of each track, in semitones: wide enough for its largest pitch offset.
std::array<int, kMaxTracks> BendRanges(const std::vector<PitchPoint>& points)
{
    std::array<int, kMaxTracks> range = {};
    for (const PitchPoint& p : points)
    {
        const int size = p.pitch < 0 ? -p.pitch : p.pitch;
        range[size_t(p.track)] = std::max(range[size_t(p.track)], std::min((size + 255) / 256, 127));
    }

    return range;
}

// Writes a channel's settings at the start: the song's reverb, no chorus, full expression, and the bend range.
void WriteChannelSetup(MidiTrack& mt, int ch, int range, int reverb)
{
    mt.Control(0, ch, cc::kExpression, 127);
    mt.Control(0, ch, cc::kReverb, reverb);
    mt.Control(0, ch, cc::kChorus, 0);
    if (range > 0)
    {
        mt.BendRange(0, ch, range);
    }
}

// Returns the MIDI channel of each track: the channel with its number. In a shared SoundFont, the drum bank can't hold
// every song's presets, so track 9 moves off the drum channel, to the first channel above it that no track plays on, or
// else the last one below it.
std::array<int, kMaxTracks> AssignChannels(const std::vector<Note>& notes, bool shared)
{
    std::array<int, kMaxTracks> channel;
    for (int t = 0; t < kMaxTracks; t++)
    {
        channel[size_t(t)] = t;
    }

    std::array<bool, kMaxTracks> plays = {};
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
    }
    if (!shared || !plays[kDrumChannel])
    {
        return channel;
    }

    for (int c = kDrumChannel + 1; c < kMidiChannels; c++)
    {
        if (!plays[size_t(c)])
        {
            channel[kDrumChannel] = c;
            return channel;
        }
    }
    for (int c = kDrumChannel - 1; c >= 0; c--)
    {
        if (!plays[size_t(c)])
        {
            channel[kDrumChannel] = c;
            return channel;
        }
    }

    return channel;
}

// Gives each of the driver's sound channels that plays a note a MIDI channel, in order, leaving the drum channel until
// last. Returns the sound channel on each MIDI channel, or -1 for none.
std::array<int, kMidiChannels> AssignVoiceChannels(std::vector<Note>& notes)
{
    std::array<bool, kChannelCount> plays = {};
    for (const Note& n : notes)
    {
        plays[size_t(n.sound_channel)] = true;
    }

    std::array<int, kChannelCount> midi = {};
    std::array<int, kMidiChannels> sound;
    sound.fill(-1);
    int next = 0;
    for (int c = 0; c < kChannelCount; c++)
    {
        if (plays[size_t(c)])
        {
            const int ch = next < kMidiChannels - 1 ? MelodicChannel(next) : kDrumChannel;
            midi[size_t(c)] = ch;
            sound[size_t(ch)] = c;
            next++;
        }
    }

    for (Note& n : notes)
    {
        n.midi_channel = midi[size_t(n.sound_channel)];
    }

    return sound;
}

// Returns the name of one of the driver's sound channels: DirectSound 1 to 12, then Square 1, Square 2, Wave and Noise.
std::string SoundChannelName(int channel)
{
    static const char* const kPsgNames[] = {"Square 1", "Square 2", "Wave", "Noise"};
    if (channel < kFirstPsgChannel)
    {
        return "DirectSound " + std::to_string(channel + 1);
    }

    return kPsgNames[channel - kFirstPsgChannel];
}

// A song's presets.
struct Presets
{
    std::map<int, BankProgram> slots; // in a shared SoundFont, the preset of each program that plays
    std::set<int> missing;           // the programs whose voices have nothing to play
    bool drum_clash = false;         // the drum bank has another song's preset for a program on the drum channel
    bool full = false;               // a shared SoundFont had no free preset for a program
};

// Adds a preset for each program the notes play. In a song's separate SoundFont, it's in bank 0, and in the drum bank
// too for the notes on the drum channel. In a shared SoundFont, SoundfontBuilder::SharedPreset() places it, and the
// notes left on the drum channel get a copy in the drum bank under the same program, unless another song has the
// program there.
Presets AddPresets(SoundfontBuilder& sf, const SongHeader& header, const std::vector<Note>& notes, bool shared)
{
    Presets presets;
    for (const Note& n : notes)
    {
        const int instrument = sf.InstrumentFor(header.voices + 12 * uint32_t(n.program), n.key);
        if (instrument < 0)
        {
            presets.missing.insert(n.program);
            continue;
        }

        BankProgram slot = {0, n.program};
        if (shared)
        {
            slot = sf.SharedPreset(n.program, instrument);
            if (slot.bank < 0)
            {
                presets.full = true;
                continue;
            }

            presets.slots[n.program] = slot;
        }
        else
        {
            sf.AddPreset(0, n.program, instrument);
        }

        if (n.midi_channel == kDrumChannel && !sf.AddPreset(kDrumBank, slot.program, instrument))
        {
            presets.drum_clash = true;
        }
    }

    return presets;
}

// Returns the bank and program that select the preset of `program` on MIDI channel `channel`. Without a shared
// SoundFont, `presets` is null and the bank is 0. In a shared SoundFont, they're the slot that holds the preset. A
// channel left on the drum channel gets bank 0. Players look for its programs in the drum bank. AddPresets() puts
// copies of the presets there.
BankProgram PresetFor(const Presets* presets, int program, int channel)
{
    BankProgram slot = {0, program};
    if (presets)
    {
        const auto found = presets->slots.find(program);
        slot = found != presets->slots.end() ? found->second : slot;
        slot.bank = channel == kDrumChannel ? 0 : slot.bank;
    }

    return slot;
}

// A program change on a track.
struct ProgramChange
{
    uint64_t tick;
    int track;
    int program;
    int step; // its place among its track's program changes at its tick
};

// Makes the MIDI file play each note with the program that the driver plays it with. A player takes program changes
// before note ons on the same tick. The driver runs a track's commands in order. So a note whose program comes from
// before its track's changes of voice on its tick goes before the program changes. A note whose program one of them
// sets goes at that change's step, after it and before the next. Any other note gets a program change of its own, after
// the tick's others.
void MatchPrograms(std::vector<Note>& notes, std::vector<ProgramChange>& programs)
{
    auto in_order = [](const ProgramChange& a, const ProgramChange& b)
    {
        return std::tie(a.track, a.tick) < std::tie(b.track, b.tick);
    };
    std::stable_sort(programs.begin(), programs.end(), in_order);

    for (Note& n : notes)
    {
        // The track's changes at the note's tick, and its program before them.
        const ProgramChange at = {n.on, n.track, 0, 0};
        const auto first = std::lower_bound(programs.begin(), programs.end(), at, in_order);
        const auto last = std::upper_bound(first, programs.end(), at, in_order);
        const bool track_before = first != programs.begin() && std::prev(first)->track == n.track;
        const int before = track_before ? std::prev(first)->program : -1;

        const auto sets = [&n](const ProgramChange& p)
        {
            return p.program == n.program;
        };
        if (first != last && sets(*std::prev(last)))
        {
            n.step = int(last - first) - 1;
        }
        else if (n.program == before)
        {
            n.before_programs = first != last;
        }
        else if (const auto change = std::find_if(first, last, sets); change != last)
        {
            n.step = int(change - first);
        }
        else
        {
            n.step = int(last - first);
            programs.insert(last, {n.on, n.track, n.program, 0});
        }
    }

    for (size_t i = 0; i < programs.size(); i++)
    {
        programs[i].step = i > 0 && !in_order(programs[i - 1], programs[i]) ? programs[i - 1].step + 1 : 0;
    }
}

// Returns copies of the voice, volume and pan settings in the loop-end tick, moved to the loop start, where they match
// settings inherited by the marked pass. The next pass begins on the loop-end tick, but a looping player jumps back
// before playing those events. Repeating the settings at the start restores the values that would otherwise be missed.
std::vector<Action> LoopEndSettings(const Simulation& sim, const SongPlan& plan)
{
    // Each track's last voice, volume and pan up to the loop's start, and the ones it sets in the loop end's tick.
    std::map<std::pair<int, int>, const Action*> start, end;
    for (const Action& a : sim.actions)
    {
        const bool setting = a.kind == Action::kVoice || a.kind == Action::kVolume || a.kind == Action::kPan;
        if (setting && a.tick <= plan.loop_start)
        {
            start[{a.track, a.kind}] = &a;
        }
        if (setting && a.tick == plan.loop_end)
        {
            end[{a.track, a.kind}] = &a;
        }
    }

    std::vector<Action> settings;
    for (const auto& [key, a] : end)
    {
        const auto before = start.find(key);
        if (before != start.end() && before->second->tick < plan.loop_start && before->second->value == a->value)
        {
            settings.push_back(*a);
            settings.back().tick = plan.loop_start;
        }
    }

    return settings;
}

// Adds the conductor track: the title, the loop markers and the tempo, at the GBA's speed, since the driver's tempo is
// for 60 frames a second, and returns it.
MidiTrack& AddConductor(MidiFile& midi, const std::string& title, const std::string& about,
                        const std::vector<TempoChange>& map, const SongPlan& plan)
{
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    if (plan.loops)
    {
        conductor.Meta(uint32_t(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(uint32_t(plan.loop_end), 0x06, "loopEnd");
    }
    conductor.SetEnd(uint32_t(plan.end));

    for (const TempoChange& change : map)
    {
        if (change.tick <= plan.end)
        {
            conductor.Tempo(uint32_t(change.tick), QuarterMicros(change.frames));
        }
    }

    return conductor;
}

// Adds a MIDI track for each of the song's tracks that plays notes. Each one plays on the MIDI channel that `channel`
// gives it. With a shared SoundFont, each program change selects the bank and program of the preset that `presets`
// gives for it.
void AddSongTracks(MidiFile& midi, const Simulation& sim, const SongPlan& plan, std::vector<Note> notes,
                   const std::array<int, kMaxTracks>& channel, const Presets* presets, int reverb, uint64_t loop_from)
{
    std::array<bool, kMaxTracks> plays = {};
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
    }

    std::array<MidiTrack*, kMaxTracks> track = {};
    for (int t = 0; t < kMaxTracks; t++)
    {
        if (plays[size_t(t)])
        {
            track[size_t(t)] = &midi.AddTrack();
            track[size_t(t)]->Name("Track " + std::to_string(t));
            track[size_t(t)]->SetEnd(uint32_t(plan.end));
        }
    }

    // The model's actions, and the settings that the loop's end gives the next pass where the marked pass has them.
    std::vector<Action> actions = sim.actions;
    const std::vector<Action> loop_end = LoopEndSettings(sim, plan);
    actions.insert(actions.end(), loop_end.begin(), loop_end.end());

    std::vector<ProgramChange> programs;
    for (const Action& a : actions)
    {
        if (a.tick >= plan.end)
        {
            continue;
        }

        MidiTrack* mt = a.track < kMaxTracks ? track[a.track] : nullptr;
        const uint32_t tick = uint32_t(a.tick);
        switch (a.kind)
        {
        case Action::kVoice:
            if (mt)
            {
                programs.push_back({a.tick, a.track, a.value, 0});
            }
            break;

        case Action::kVolume:
            if (mt)
            {
                mt->Control(tick, channel[a.track], cc::kVolume, a.value);
            }
            break;

        case Action::kPan:
            if (mt)
            {
                mt->Control(tick, channel[a.track], cc::kPan, a.value);
            }
            break;

        default:
            break;
        }
    }

    const std::vector<PitchPoint> points = PitchPoints(sim, notes, plan, loop_from);
    const std::array<int, kMaxTracks> range = BendRanges(points);
    for (const PitchPoint& p : points)
    {
        if (track[size_t(p.track)] && range[size_t(p.track)] > 0)
        {
            track[size_t(p.track)]->PitchBend(uint32_t(p.tick), channel[size_t(p.track)],
                                              FixedBendValue(p.pitch, 256, range[size_t(p.track)]));
        }
    }

    MatchPrograms(notes, programs);
    for (const ProgramChange& p : programs)
    {
        MidiTrack& mt = *track[size_t(p.track)];
        const int ch = channel[size_t(p.track)];
        const BankProgram slot = PresetFor(presets, p.program, ch);
        if (presets)
        {
            mt.Bank(uint32_t(p.tick), ch, slot.bank, p.step);
        }
        mt.Program(uint32_t(p.tick), ch, slot.program, p.step);
    }

    for (int t = 0; t < kMaxTracks; t++)
    {
        if (track[size_t(t)])
        {
            WriteChannelSetup(*track[size_t(t)], channel[size_t(t)], range[size_t(t)], reverb);
        }
    }

    for (const Note& n : notes)
    {
        MidiTrack& mt = *track[size_t(n.track)];
        const int ch = channel[size_t(n.track)];
        mt.NoteOn(uint32_t(n.on), ch, n.key, n.velocity, n.before_programs, n.step);
        mt.NoteOff(uint32_t(n.off), ch, n.key);
    }
}

// Each track's volume, pan and pitch offset from each tick on, as the model reports them.
struct TrackSettings
{
    std::array<std::map<uint64_t, int>, kMaxTracks> volume, pan, pitch;
};

// Returns the tracks' settings up to the end of the conversion.
TrackSettings SettingsOf(const Simulation& sim, const SongPlan& plan)
{
    TrackSettings settings;
    for (const Action& a : sim.actions)
    {
        if (a.tick >= plan.end || a.track >= kMaxTracks)
        {
            continue;
        }

        if (a.kind == Action::kVolume)
        {
            settings.volume[a.track][a.tick] = a.value;
        }
        else if (a.kind == Action::kPan)
        {
            settings.pan[a.track][a.tick] = a.value;
        }
        else if (a.kind == Action::kPitch)
        {
            settings.pitch[a.track][a.tick] = a.value;
        }
    }

    return settings;
}

// Returns a track's setting at a tick, from its changes, or `fallback` before the track sets it.
int SettingAt(const std::map<uint64_t, int>& changes, uint64_t tick, int fallback)
{
    const auto next = changes.upper_bound(tick);
    return next == changes.begin() ? fallback : std::prev(next)->second;
}

// Writes the notes of one of the driver's sound channels, in order, on MIDI channel `ch`. Before each note, the channel
// takes the note's program and its track's volume, pan and pitch, which it then follows until the sound channel's sound
// stops, where an All Sound Off cuts off the note's release. With a shared SoundFont, each program change selects the
// bank and program of the preset that `presets` gives for it.
void WriteVoiceChannel(MidiTrack& mt, int ch, const std::vector<const Note*>& notes, const TrackSettings& settings,
                       const Presets* presets, const SongPlan& plan, uint64_t loop_from, int reverb)
{
    // The channel's settings, written only where they change, or where they're unknown, and its pitch offsets, which
    // wait for the bend range.
    BankProgram preset = {-1, -1};
    std::optional<int> volume, pan;
    std::optional<int> pitch = 0;
    std::vector<std::pair<uint64_t, int>> offsets;

    auto set_volume = [&](uint64_t tick, int value)
    {
        if (volume != value)
        {
            mt.Control(uint32_t(tick), ch, cc::kVolume, value);
            volume = value;
        }
    };

    auto set_pan = [&](uint64_t tick, int value)
    {
        if (pan != value)
        {
            mt.Control(uint32_t(tick), ch, cc::kPan, value);
            pan = value;
        }
    };

    auto set_pitch = [&](uint64_t tick, int value)
    {
        if (pitch != value)
        {
            offsets.push_back({tick, value});
            pitch = value;
        }
    };

    for (size_t i = 0; i < notes.size(); i++)
    {
        const Note& n = *notes[i];

        // A player that jumps back to the loop's start keeps the settings that the loop's end left, so the channel's
        // first note from the loop's start marker, whose first tick is `loop_from`, sets them all again.
        if (plan.loops && n.on >= loop_from && (i == 0 || notes[i - 1]->on < loop_from))
        {
            preset = {-1, -1};
            volume.reset();
            pan.reset();
            pitch.reset();
        }

        // The note's preset, set where it changes.
        const BankProgram slot = PresetFor(presets, n.program, ch);
        if (slot.bank != preset.bank || slot.program != preset.program)
        {
            if (presets)
            {
                mt.Bank(uint32_t(n.on), ch, slot.bank);
            }
            mt.Program(uint32_t(n.on), ch, slot.program);
            preset = slot;
        }

        // The pitch offset that plays the note as the driver does, which differs from the track's for a PSG note (see
        // PsgPitch()).
        auto played = [&](int offset)
        {
            return n.psg ? PsgPitch(n, offset) : offset;
        };

        // The track's settings when the note starts.
        const std::map<uint64_t, int>& track_volume = settings.volume[size_t(n.track)];
        const std::map<uint64_t, int>& track_pan = settings.pan[size_t(n.track)];
        const std::map<uint64_t, int>& track_pitch = settings.pitch[size_t(n.track)];
        set_volume(n.on, SettingAt(track_volume, n.on, 127));
        set_pan(n.on, SettingAt(track_pan, n.on, 64));
        set_pitch(n.on, played(SettingAt(track_pitch, n.on, 0)));

        // Their changes until the channel's sound stops, or until the channel's next note if the sound outlasts the
        // conversion.
        const uint64_t until = n.stop ? n.stop : i + 1 < notes.size() ? notes[i + 1]->on : plan.end;
        for (auto c = track_volume.upper_bound(n.on); c != track_volume.end() && c->first < until; ++c)
        {
            set_volume(c->first, c->second);
        }
        for (auto c = track_pan.upper_bound(n.on); c != track_pan.end() && c->first < until; ++c)
        {
            set_pan(c->first, c->second);
        }
        for (auto c = track_pitch.upper_bound(n.on); c != track_pitch.end() && c->first < until; ++c)
        {
            set_pitch(c->first, played(c->second));
        }

        mt.NoteOn(uint32_t(n.on), ch, n.key, n.velocity);
        mt.NoteOff(uint32_t(n.off), ch, n.key);
        if (n.stop)
        {
            mt.Control(uint32_t(n.stop), ch, cc::kAllSoundOff, 0);
        }
    }

    // The bend range is wide enough for the channel's largest pitch offset.
    int range = 0;
    for (const auto& [tick, value] : offsets)
    {
        range = std::max(range, std::min((std::abs(value) + 255) / 256, 127));
    }

    WriteChannelSetup(mt, ch, range, reverb);
    for (const auto& [tick, value] : offsets)
    {
        mt.PitchBend(uint32_t(tick), ch, FixedBendValue(value, 256, std::max(range, 1)));
    }
}

// Adds a MIDI track for each of the driver's sound channels that plays notes, in the order of the MIDI channels that
// `sound` gives them. Each track is named after its sound channel.
void AddVoiceTracks(MidiFile& midi, const Simulation& sim, const SongPlan& plan, const std::vector<Note>& notes,
                    const std::array<int, kMidiChannels>& sound, const Presets* presets, int reverb, uint64_t loop_from)
{
    const TrackSettings settings = SettingsOf(sim, plan);
    for (int ch = 0; ch < kMidiChannels; ch++)
    {
        if (sound[size_t(ch)] < 0)
        {
            continue;
        }

        std::vector<const Note*> mine;
        for (const Note& n : notes)
        {
            if (n.midi_channel == ch)
            {
                mine.push_back(&n);
            }
        }
        std::stable_sort(mine.begin(), mine.end(), [](const Note* a, const Note* b) { return a->on < b->on; });

        MidiTrack& mt = midi.AddTrack();
        mt.Name(SoundChannelName(sound[size_t(ch)]));
        mt.SetEnd(uint32_t(plan.end));
        WriteVoiceChannel(mt, ch, mine, settings, presets, plan, loop_from, reverb);
    }
}

// Converts a song, writing its files if `write` is set.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write)
{
    SongReport report;
    SongHeader header;
    if (!ReadSong(rom, info, song, header))
    {
        report.warnings.push_back("its header can't be read");
        return report;
    }

    const std::vector<TrackLayout> layouts = ScanTracks(rom, header, PlayedTracks(rom, info, song, header));
    SongPlan plan = PlanConversion(rom, info, song, layouts, settings.loops);
    const Simulation sim = Simulate(rom, info, song, plan);

    // A song that the model gave up on ends where it stopped.
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    report.warnings = sim.warnings;
    Notes collected = settings.voice_channels ? CollectVoiceNotes(rom, sim, plan, settings.track_mask)
                                              : CollectNotes(rom, sim, plan, settings.track_mask);
    const std::vector<TempoChange> map = TempoMap(sim.actions);
    Summarize(map, plan, report);

    std::set<int> tracks;
    for (const Note& n : collected.notes)
    {
        tracks.insert(n.track);
    }

    const auto changed = [&](const Action& a)
    {
        return a.kind == Action::kNoteOn && a.modified && a.tick < plan.end && (settings.track_mask >> a.track & 1);
    };
    if (std::any_of(sim.actions.begin(), sim.actions.end(), changed))
    {
        report.warnings.push_back(
            "a track changes its voice with extended commands, or starts its notes partway into their samples, "
            "which the SoundFont doesn't follow");
    }
    if (collected.dropped)
    {
        report.warnings.push_back(std::to_string(collected.dropped) + " note" + (collected.dropped == 1 ? "" : "s") +
                                  " had no available sound channel; omitted from the MIDI file to match the game");
    }

    report.tracks = int(tracks.size());
    report.silent = collected.notes.empty();
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The MIDI channels: one for each of the driver's sound channels that plays, or one for each track.
    std::array<int, kMaxTracks> channel = {};
    std::array<int, kMidiChannels> sound = {};
    if (settings.voice_channels)
    {
        sound = AssignVoiceChannels(collected.notes);
    }
    else
    {
        channel = AssignChannels(collected.notes, shared != nullptr);
        for (Note& n : collected.notes)
        {
            n.midi_channel = channel[size_t(n.track)];
        }
    }

    // The SoundFont's presets, in the song's file or the shared one.
    SoundfontBuilder own(rom, info);
    SoundfontBuilder& sf = shared ? *shared : own;
    const Presets presets = AddPresets(sf, header, collected.notes, shared != nullptr);
    for (int program : presets.missing)
    {
        report.warnings.push_back("voice " + std::to_string(program) + " has no samples to play");
    }
    if (presets.drum_clash)
    {
        report.warnings.push_back(
            "all 16 MIDI channels play, so some notes stay on the drum channel, and another song has one of their "
            "programs in the drum bank");
    }
    if (presets.full)
    {
        report.warnings.push_back("the shared SoundFont has no free preset left for some of its voices");
    }

    // The song's reverb, if it sets one, or else the driver's. Camelot's mixer has an echo of its own instead, the same
    // in every song, which feeds back about as much as the driver's reverb at its level.
    int reverb = header.reverb & 0x80 ? header.reverb & 0x7F : info.reverb;
    if (info.camelot_mixer)
    {
        reverb = kCamelotEcho;
    }

    // The files' names and titles, and the MIDI file's description.
    const std::string number = SongNumber(song, info.song_count);
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) song %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(header.address), kProgramName);
    const std::string stem = SongStem(settings, number);

    // The MIDI file: a conductor track, then a track for each of the song's tracks or each of the driver's sound
    // channels.
    const Presets* shared_presets = shared ? &presets : nullptr;
    const FrameGrid frames = settings.frame_timing ? MakeFrameGrid(sim) : FrameGrid();
    const FrameGrid* framed = settings.frame_timing ? &frames : nullptr;
    MidiFile midi(kTicksPerQuarter);
    MidiTrack& conductor = AddConductor(midi, title, about, framed ? std::vector<TempoChange>() : map, plan);

    const uint64_t loop_from = LoopFrom(plan, framed);
    if (settings.voice_channels)
    {
        AddVoiceTracks(midi, sim, plan, collected.notes, sound, shared_presets, reverb, loop_from);
    }
    else
    {
        AddSongTracks(midi, sim, plan, collected.notes, channel, shared_presets, reverb, loop_from);
    }

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidiFile(midi, conductor, framed, report.midi_path, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    if (!shared)
    {
        Sf2File& file = sf.File();
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
                       SoundfontBuilder* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

} // namespace agbsf2::mp2k
