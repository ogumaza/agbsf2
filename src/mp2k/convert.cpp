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

// The driver's ticks: 24 to a quarter note, and 150 of a frame's tempo counter to a tick.
constexpr int kTicksPerQuarter = 24;
constexpr double kTickCounter = 150;

// The tempo before a song sets one, in the driver's units: quarter notes a minute at 60 frames a second.
constexpr int kDefaultTempo = 150;

// The reverb level whose feedback comes closest to the echo of Camelot's mixer: 53/128 of each side's output from four
// frames before.
constexpr int kCamelotEcho = 53;

// The MIDI channel that General MIDI players keep for drums, and the bank they look in for its programs.
constexpr int kDrumChannel = 9;
constexpr int kDrumBank = 128;

// Maximum number of commands DumpSong() lists per track.
constexpr int kMaxListedCommands = 200000;

// The stretch of a song that the conversion covers: its loop, and the tick at which the MIDI file ends.
struct Plan
{
    bool loops = false;
    uint64_t loop_start = 0; // the latest of the looping tracks' loop starts and the other tracks' last commands
    uint64_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the tracks' loops
    uint64_t end = 0;
};

// Plans the conversion: looping songs play `loops` passes; other songs run until the last track ends. Tracks can have
// different loop points and lengths. The overall loop starts once all looping tracks have entered their loops and all
// other tracks have played their last command and released their notes. It ends when all looping tracks return to their
// loop starts, subject to the length limit in LoopLength(). Only tracks that fit in the music player count. With
// `second`, the loop starts at the song's second pass through it, and the song plays the first pass before it.
Plan PlanSong(const Rom& rom, const SongHeader& header, int track_count, int loops, bool second)
{
    Plan plan;
    uint64_t longest_end = 0;
    uint64_t last_command = 0;
    std::vector<uint64_t> lengths;
    for (int t = 0; t < track_count; t++)
    {
        const TrackLayout layout = ScanTrack(rom, header.tracks[size_t(t)]);
        if (layout.loops && layout.loop_end > layout.loop_start)
        {
            plan.loops = true;
            plan.loop_start = std::max(plan.loop_start, layout.loop_start);
            lengths.push_back(layout.loop_end - layout.loop_start);
        }
        else if (!layout.loops)
        {
            longest_end = std::max(longest_end, layout.end);
            last_command = std::max(last_command, layout.last_command);
        }
    }

    if (plan.loops)
    {
        const uint64_t length = LoopLength(lengths);
        plan.loop_start = std::max(plan.loop_start, last_command) + (second ? length : 0);
        plan.loop_end = plan.loop_start + length;
        plan.end = plan.loop_start + uint64_t(loops) * length;
    }
    else
    {
        plan.end = longest_end;
    }

    return plan;
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
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, const Plan& plan)
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

// Works out the MIDI file's notes from the model's note ons, releases and ends. Each track plays on a MIDI channel of
// its own. A MIDI channel can't play one key twice at once, so where a track does, the older note ends when the newer
// one starts, unless they start on the same tick, when they make one louder note. A note ends where the driver releases
// it, where its sound stops, where a later note takes its channel, or at the end of the conversion. A note whose
// envelope never gets above 0 is silent in the game and is left out. So are a sample voice without an attack, and a PSG
// note so quiet that its level rounds to 0.
Notes CollectNotes(const Rom& rom, const Simulation& sim, const Plan& plan, uint16_t track_mask)
{
    Notes result;
    std::vector<Note>& notes = result.notes;
    std::array<std::array<int, 128>, 16> owner;
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
        // A note lasts at least a tick, so that its note off can't come before its note on.
        Note& n = notes[size_t(index)];
        if (n.open)
        {
            n.open = false;
            n.off = std::max(std::min(tick, plan.end), n.on + 1);
            owner[size_t(n.track)][size_t(n.key)] = -1;
        }
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
        if (a.kind == Action::kNoteDropped && a.tick < plan.end && (track_mask >> a.track & 1))
        {
            result.dropped++;
        }
        else if (a.kind == Action::kNoteOn)
        {
            held[size_t(a.channel)] = -1;
            last[size_t(a.channel)] = -1;
            if (a.tick >= plan.end || !(track_mask >> a.track & 1) || a.program < 0 || a.b == 0)
            {
                continue;
            }

            int& same_key = owner[a.track][a.a];
            if (same_key >= 0 && notes[size_t(same_key)].on == a.tick)
            {
                Note& n = notes[size_t(same_key)];
                n.level += a.b;
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

            Note n;
            n.track = a.track;
            n.key = a.a;
            n.velocity = a.b;
            n.level = a.b;
            n.program = a.program;
            Voice voice;
            ReadVoice(rom, a.voice, voice);
            n.psg = voice.PsgChannel();
            n.play_key = a.value;
            n.on = a.tick;
            notes.push_back(n);
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
                Note& n = notes[size_t(index)];
                if (n.open)
                {
                    owner[size_t(n.track)][size_t(n.key)] = -1;
                }

                n.open = false;
                n.removed = true;
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

    notes.erase(std::remove_if(notes.begin(), notes.end(), [](const Note& n) { return n.removed || !n.audible; }),
                notes.end());

    return result;
}

// Works out the MIDI file's notes when each of the driver's sound channels has a MIDI channel: one for each note a
// channel plays. A note ends where the driver releases it, or where its sound stops if that comes first, and its sound
// stops where the channel's does: where it fades out, or where a later note takes the channel. A note whose sound stops
// on the tick it started is left out, as are those that never sound.
Notes CollectVoiceNotes(const Rom& rom, const Simulation& sim, const Plan& plan, uint16_t track_mask)
{
    Notes result;
    std::vector<Note>& notes = result.notes;

    // Each channel's note until its sound stops.
    std::array<int, kChannelCount> current;
    current.fill(-1);

    auto release = [&](Note& n, uint64_t tick)
    {
        if (n.open)
        {
            n.open = false;
            n.off = std::max(std::min(tick, plan.end), n.on + 1);
        }
    };

    auto stop = [&](int channel, uint64_t tick, bool sounded)
    {
        Note& n = notes[size_t(current[size_t(channel)])];
        n.removed = n.removed || !sounded || tick <= n.on;
        release(n, tick);
        n.stop = tick < plan.end ? tick : 0;
        current[size_t(channel)] = -1;
    };

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kNoteDropped && a.tick < plan.end && (track_mask >> a.track & 1))
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
            if (a.tick >= plan.end || !(track_mask >> a.track & 1) || a.program < 0 || a.b == 0)
            {
                continue;
            }

            Note n;
            n.track = a.track;
            n.key = a.a;
            n.velocity = a.b;
            n.level = a.b;
            n.program = a.program;
            Voice voice;
            ReadVoice(rom, a.voice, voice);
            n.psg = voice.PsgChannel();
            n.play_key = a.value;
            n.on = a.tick;
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
            release(notes[size_t(current[size_t(a.channel)])], a.tick);
        }
        else if (a.kind == Action::kNoteEnd)
        {
            stop(a.channel, a.tick, a.sounded);
        }
    }

    for (Note& n : notes)
    {
        release(n, plan.end);
    }

    notes.erase(std::remove_if(notes.begin(), notes.end(), [](const Note& n) { return n.removed || !n.audible; }),
                notes.end());

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
    std::vector<TempoChange> map = {{0, kTickCounter / kDefaultTempo}};

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
        const uint64_t blend = a.tick + uint64_t(progress / int(kTickCounter));
        const int part = progress % int(kTickCounter);
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
bool TemposRepeat(const Simulation& sim, const Plan& plan)
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
uint64_t LoopFrom(const Plan& plan, const FrameGrid* frames)
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
            conductor.Tempo(uint32_t(tick), QuarterMicros(kTickCounter / tempo));
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

// Fills in a summary's length, loop and tempo.
void Summarize(const std::vector<TempoChange>& map, const Plan& plan, SongSummary& sum)
{
    sum.seconds = TickSeconds(map, plan.end);
    if (plan.loops)
    {
        sum.loop_start = TickSeconds(map, plan.loop_start);
        sum.loop_end = TickSeconds(map, plan.loop_end);
    }

    sum.bpm = 60e6 / QuarterMicros(map.front().frames);
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
std::vector<PitchPoint> PitchPoints(const Simulation& sim, const std::vector<Note>& notes, const Plan& plan,
                                    uint64_t loop_from)
{
    std::vector<PitchPoint> points;
    for (int t = 0; t < 16; t++)
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
std::array<int, 16> BendRanges(const std::vector<PitchPoint>& points)
{
    std::array<int, 16> range = {};
    for (const PitchPoint& p : points)
    {
        const int size = p.pitch < 0 ? -p.pitch : p.pitch;
        range[size_t(p.track)] = std::max(range[size_t(p.track)], std::min((size + 255) / 256, 127));
    }

    return range;
}

// Returns the MIDI pitch bend value for a pitch offset in 256ths of a semitone, with a bend range of `range` semitones,
// at least 1.
int BendValue(int offset, int range)
{
    const int64_t scaled = int64_t(offset) * 8192;
    const int64_t unit = int64_t(range) * 256;
    const int64_t steps = scaled >= 0 ? (scaled + unit / 2) / unit : -((-scaled + unit / 2) / unit);
    return int(std::clamp<int64_t>(8192 + steps, 0, 16383));
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
std::array<int, 16> AssignChannels(const std::vector<Note>& notes, bool shared)
{
    std::array<int, 16> channel;
    for (int t = 0; t < 16; t++)
    {
        channel[size_t(t)] = t;
    }

    std::array<bool, 16> plays = {};
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
    }
    if (!shared || !plays[kDrumChannel])
    {
        return channel;
    }

    for (int c = kDrumChannel + 1; c < 16; c++)
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
std::array<int, 16> AssignVoiceChannels(std::vector<Note>& notes)
{
    std::array<bool, kChannelCount> plays = {};
    for (const Note& n : notes)
    {
        plays[size_t(n.sound_channel)] = true;
    }

    std::array<int, kChannelCount> midi = {};
    std::array<int, 16> sound;
    sound.fill(-1);
    int next = 0;
    for (int c = 0; c < kChannelCount; c++)
    {
        if (plays[size_t(c)])
        {
            const int ch = next < 15 ? (next < kDrumChannel ? next : next + 1) : kDrumChannel;
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
    std::map<int, PresetSlot> slots; // in a shared SoundFont, the preset of each program that plays
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

        PresetSlot slot = {0, n.program};
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
std::vector<Action> LoopEndSettings(const Simulation& sim, const Plan& plan)
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
        if (before != start.end() && before->second->tick < plan.loop_start && before->second->a == a->a)
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
                        const std::vector<TempoChange>& map, const Plan& plan)
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

// Writes the MIDI file: a conductor track, then a track for each of the song's tracks that plays notes, on the MIDI
// channel that `channel` gives it. With a shared SoundFont, each program change selects the bank and program of the
// preset that `presets` gives for it.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
               const std::vector<TempoChange>& map, const Plan& plan, std::vector<Note> notes,
               const std::array<int, 16>& channel, const Presets* presets, int reverb, const FrameGrid* frames,
               std::string& error)
{
    MidiFile midi(kTicksPerQuarter);
    MidiTrack& conductor = AddConductor(midi, title, about, frames ? std::vector<TempoChange>() : map, plan);

    std::array<bool, 16> plays = {};
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
    }

    std::array<MidiTrack*, 16> track = {};
    for (int t = 0; t < 16; t++)
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

        MidiTrack* mt = a.track < 16 ? track[a.track] : nullptr;
        const uint32_t tick = uint32_t(a.tick);
        switch (a.kind)
        {
        case Action::kVoice:
            if (mt)
            {
                programs.push_back({a.tick, a.track, a.a, 0});
            }
            break;

        case Action::kVolume:
            if (mt)
            {
                mt->Control(tick, channel[a.track], cc::kVolume, a.a);
            }
            break;

        case Action::kPan:
            if (mt)
            {
                mt->Control(tick, channel[a.track], cc::kPan, a.a);
            }
            break;

        default:
            break;
        }
    }

    const std::vector<PitchPoint> points = PitchPoints(sim, notes, plan, LoopFrom(plan, frames));
    const std::array<int, 16> range = BendRanges(points);
    for (const PitchPoint& p : points)
    {
        if (track[size_t(p.track)] && range[size_t(p.track)] > 0)
        {
            track[size_t(p.track)]->PitchBend(uint32_t(p.tick), channel[size_t(p.track)],
                                              BendValue(p.pitch, range[size_t(p.track)]));
        }
    }

    MatchPrograms(notes, programs);
    for (const ProgramChange& p : programs)
    {
        MidiTrack& mt = *track[size_t(p.track)];
        const int ch = channel[size_t(p.track)];
        if (!presets)
        {
            mt.Program(uint32_t(p.tick), ch, p.program, p.step);
            continue;
        }

        // A track left on the drum channel plays the copies in the drum bank, which players find with bank 0.
        const auto slot = presets->slots.find(p.program);
        const PresetSlot s = slot != presets->slots.end() ? slot->second : PresetSlot{0, p.program};
        mt.Bank(uint32_t(p.tick), ch, ch == kDrumChannel ? 0 : s.bank, p.step);
        mt.Program(uint32_t(p.tick), ch, s.program, p.step);
    }

    for (int t = 0; t < 16; t++)
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

    return WriteMidiFile(midi, conductor, frames, path, error);
}

// Each track's volume, pan and pitch offset from each tick on, as the model reports them.
struct TrackSettings
{
    std::array<std::map<uint64_t, int>, 16> volume, pan, pitch;
};

// Returns the tracks' settings up to the end of the conversion.
TrackSettings SettingsOf(const Simulation& sim, const Plan& plan)
{
    TrackSettings settings;
    for (const Action& a : sim.actions)
    {
        if (a.tick >= plan.end || a.track >= 16)
        {
            continue;
        }

        if (a.kind == Action::kVolume)
        {
            settings.volume[a.track][a.tick] = a.a;
        }
        else if (a.kind == Action::kPan)
        {
            settings.pan[a.track][a.tick] = a.a;
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
                       const Presets* presets, const Plan& plan, uint64_t loop_from, int reverb)
{
    // The channel's settings, written only where they change, or where they're unknown, and its pitch offsets, which
    // wait for the bend range.
    PresetSlot preset = {-1, -1};
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

        // The note's preset: in a shared SoundFont, the bank and program that hold it, though a channel left on the
        // drum channel plays the copies in the drum bank, which players find with bank 0.
        PresetSlot slot = {0, n.program};
        if (presets)
        {
            const auto found = presets->slots.find(n.program);
            slot = found != presets->slots.end() ? found->second : slot;
            slot.bank = ch == kDrumChannel ? 0 : slot.bank;
        }
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
        mt.PitchBend(uint32_t(tick), ch, BendValue(value, std::max(range, 1)));
    }
}

// Writes the MIDI file with a MIDI channel for each of the driver's sound channels that plays notes: a conductor track,
// then a track for each of those channels, named after it, in the order of the MIDI channels that `sound` gives them.
bool WriteVoiceMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
                    const std::vector<TempoChange>& map, const Plan& plan, const std::vector<Note>& notes,
                    const std::array<int, 16>& sound, const Presets* presets, int reverb, const FrameGrid* frames,
                    std::string& error)
{
    MidiFile midi(kTicksPerQuarter);
    MidiTrack& conductor = AddConductor(midi, title, about, frames ? std::vector<TempoChange>() : map, plan);

    const TrackSettings settings = SettingsOf(sim, plan);
    const uint64_t loop_from = LoopFrom(plan, frames);
    for (int ch = 0; ch < 16; ch++)
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

    return WriteMidiFile(midi, conductor, frames, path, error);
}

// Converts a song, writing its files if `write` is set.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, SoundfontBuilder* shared,
                bool write)
{
    SongSummary sum;
    SongHeader header;
    if (!ReadSongHeader(rom, SongAddress(rom, info, song), header) || header.track_count == 0)
    {
        sum.warnings.push_back("its header can't be read");
        return sum;
    }

    const int track_count = std::min(header.track_count, SongPlayer(rom, info, song).track_count);
    // The marked loop starts at the song's second pass if its first plays at other tempos, as when a track sets the
    // tempo on the loop's first tick before the loop starts, or the loop's end leaves another tempo. A song that the
    // model gave up on before its second pass ended keeps its loop.
    Plan plan = PlanSong(rom, header, track_count, opt.loops, false);
    if (plan.loops)
    {
        const Plan two = PlanSong(rom, header, track_count, 2, false);
        const Simulation check = Simulate(rom, info, song, two);
        if ((!check.cut_off || *check.cut_off >= two.end) && !TemposRepeat(check, two))
        {
            plan = PlanSong(rom, header, track_count, opt.loops, true);
        }
    }
    const Simulation sim = Simulate(rom, info, song, plan);

    // A song that the model gave up on ends where it stopped.
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    sum.warnings = sim.warnings;
    Notes collected = opt.voice_channels ? CollectVoiceNotes(rom, sim, plan, opt.track_mask)
                                         : CollectNotes(rom, sim, plan, opt.track_mask);
    const std::vector<TempoChange> map = TempoMap(sim.actions);
    Summarize(map, plan, sum);

    std::set<int> tracks;
    for (const Note& n : collected.notes)
    {
        tracks.insert(n.track);
    }

    const auto changed = [&](const Action& a)
    {
        return a.kind == Action::kNoteOn && a.modified && a.tick < plan.end && (opt.track_mask >> a.track & 1);
    };
    if (std::any_of(sim.actions.begin(), sim.actions.end(), changed))
    {
        sum.warnings.push_back(
            "a track changes its voice with extended commands, or starts its notes partway into their samples, "
            "which the SoundFont doesn't follow");
    }
    if (collected.dropped)
    {
        sum.warnings.push_back(std::to_string(collected.dropped) + " note" + (collected.dropped == 1 ? "" : "s") +
                               " had no available sound channel; omitted from the MIDI file to match the game");
    }

    sum.tracks = int(tracks.size());
    sum.silent = collected.notes.empty();
    if (sum.silent || !write)
    {
        sum.ok = true;
        return sum;
    }

    // The MIDI channels: one for each of the driver's sound channels that plays, or one for each track.
    std::array<int, 16> channel = {};
    std::array<int, 16> sound = {};
    if (opt.voice_channels)
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
        sum.warnings.push_back("voice " + std::to_string(program) + " has no samples to play");
    }
    if (presets.drum_clash)
    {
        sum.warnings.push_back(
            "all 16 MIDI channels play, so some notes stay on the drum channel, and another song has one of their "
            "programs in the drum bank");
    }
    if (presets.full)
    {
        sum.warnings.push_back("the shared SoundFont has no free preset left for some of its voices");
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
    const std::string stem = Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + number));

    std::string error;
    sum.midi_path = stem + ".mid";
    const Presets* shared_presets = shared ? &presets : nullptr;
    const FrameGrid frames = opt.frame_timing ? MakeFrameGrid(sim) : FrameGrid();
    const FrameGrid* framed = opt.frame_timing ? &frames : nullptr;
    const bool written = opt.voice_channels
                             ? WriteVoiceMidi(sum.midi_path, title, about, sim, map, plan, collected.notes, sound,
                                              shared_presets, reverb, framed, error)
                             : WriteMidi(sum.midi_path, title, about, sim, map, plan, collected.notes, channel,
                                         shared_presets, reverb, framed, error);
    if (!written)
    {
        sum.warnings.push_back(error);
        return sum;
    }

    if (!shared)
    {
        Sf2File& file = sf.File();
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
                        SoundfontBuilder* shared)
{
    return Run(rom, info, song, opt, shared, true);
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    SongHeader h;
    if (!ReadSongHeader(rom, SongAddress(rom, info, song), h) || h.track_count == 0)
    {
        error = "invalid song";
        return false;
    }

    std::string text;
    char line[256];
    std::snprintf(line, sizeof line,
                  "; %s (%s) song %d\n; header 0x%08X: %d tracks, priority %u, reverb 0x%02X, voice group 0x%08X\n"
                  "; columns: address, tick, bytes, command. Each track is listed in the order it plays, up to its "
                  "end or the end of its first loop.\n",
                  rom.Title().c_str(), rom.GameCode().c_str(), song, unsigned(h.address), h.track_count,
                  unsigned(h.priority), unsigned(h.reverb), unsigned(h.voices));
    text += line;

    for (int t = 0; t < h.track_count; t++)
    {
        std::snprintf(line, sizeof line, "\n; ---- track %d at 0x%08X ----\n", t, unsigned(h.tracks[size_t(t)]));
        text += line;

        // Follow the track as the driver plays it, through its patterns, repeats and jumps.
        uint32_t address = h.tracks[size_t(t)];
        uint8_t running = 0;
        uint64_t tick = 0;
        std::array<uint32_t, 3> stack = {};
        int level = 0, repeats = 0;
        std::set<std::tuple<uint32_t, int, uint32_t>> played;
        for (int n = 0; n < kMaxListedCommands; n++)
        {
            Event e;
            if (!DecodeEvent(rom, address, running, e))
            {
                std::snprintf(line, sizeof line, "0x%08X %8llu  %02X  (unknown command; the track stops here)\n",
                              unsigned(address), static_cast<unsigned long long>(tick), rom.U8(address));
                text += line;
                break;
            }

            played.insert(std::make_tuple(address, level, level ? stack[size_t(level - 1)] : 0));
            std::string bytes;
            for (uint32_t i = 0; i < e.size && i < 8; i++)
            {
                char b[4];
                std::snprintf(b, sizeof b, "%02X ", rom.U8(address + i));
                bytes += b;
            }
            std::snprintf(line, sizeof line, "0x%08X %8llu  %-24s %s%s\n", unsigned(address),
                          static_cast<unsigned long long>(tick), bytes.c_str(),
                          std::string(size_t(2 * level), ' ').c_str(), DescribeEvent(e).c_str());
            text += line;

            running = RunningStatus(e, running);
            address += e.size;
            const uint8_t c = e.command;
            if (c >= kCmdWait && c <= 0xB0)
            {
                tick += uint64_t(ClockLength(c - kCmdWait));
            }
            else if (c == kCmdXcmd && e.arg[0] == kXcmdWait)
            {
                tick += uint64_t(e.arg[1] | (e.arg[2] << 8));
            }
            else if (c == kCmdPatt && level < 3)
            {
                stack[size_t(level++)] = address;
                address = e.target;
            }
            else if (c == kCmdPend && level > 0)
            {
                address = stack[size_t(--level)];
            }
            else if (c == kCmdRept && e.arg[0] && ++repeats < e.arg[0])
            {
                address = e.target;
            }
            else if (c == kCmdRept && e.arg[0])
            {
                repeats = 0;
            }
            else if (c == kCmdGoto || c == kCmdRept)
            {
                if (played.count(std::make_tuple(e.target, level, level ? stack[size_t(level - 1)] : 0)))
                {
                    break;
                }

                address = e.target;
            }
            else if (EndsTrack(c) || c == kCmdPatt || (c == kCmdXcmd && (e.arg[0] == 0 || e.arg[0] == 3)))
            {
                break;
            }
        }
    }

    std::vector<uint8_t> data(text.begin(), text.end());
    return WriteFile(path, data, error);
}

} // namespace agbsf2::mp2k
