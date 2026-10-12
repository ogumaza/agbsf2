// SPDX-License-Identifier: MIT

#include "rare/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rare/sequencer.h"
#include "rare/song.h"

namespace supergbamidi::rare
{
namespace
{

// Maximum conversion duration: one hour, measured in frames.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The MIDI channel that General MIDI players keep for drums. They look for its programs in bank kDrumBank.
constexpr int kDrumChannel = 9;

// A semitone in the model's pitch offsets, which are 32.32 fixed point.
constexpr int64_t kSemitone = int64_t(1) << 32;

// Maximum number of commands DumpSong() lists per track.
constexpr int kMaxListedCommands = 200000;

std::string TwoDigits(int n)
{
    char b[16];
    std::snprintf(b, sizeof b, "%02d", n);

    return b;
}

// The stretch of a tune that the conversion covers: its loop, and the tick at which the MIDI file ends.
struct Plan
{
    bool loops = false;
    uint64_t loop_start = 0; // the latest of the looping tracks' loop starts and the other tracks' last commands
    uint64_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the tracks' loops
    uint64_t end = 0;
};

// Plans the conversion: looping tunes play `loops` passes; other tunes run until the last track ends. Tracks can have
// different loop points and lengths. The overall loop starts once all looping tracks have entered their loops and all
// other tracks have played their last command other than a delay or end. It ends when all looping tracks return to
// their loop starts, subject to the length limit in LoopLength().
Plan PlanTune(const Rom& rom, const DriverInfo& info, const TuneHeader& header, int loops)
{
    Plan plan;
    uint64_t longest_end = 0;
    uint64_t last_command = 0;
    std::vector<uint64_t> lengths;
    for (uint32_t address : header.tracks)
    {
        const TrackLayout layout = ScanTrack(rom, address, info.format);
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
        plan.loop_start = std::max(plan.loop_start, last_command);
        plan.loop_end = plan.loop_start + length;
        plan.end = plan.loop_start + uint64_t(loops) * length;
    }
    else
    {
        plan.end = longest_end;
    }

    return plan;
}

// A clock that converts frame start times to ticks, following the tempo.
class TickClock
{
public:
    explicit TickClock(uint32_t ticks_per_quarter) : per_quarter_(ticks_per_quarter)
    {
    }

    // Returns the tick at which frame `frame` starts.
    uint64_t FrameTick(uint32_t frame) const
    {
        const int64_t elapsed = int64_t(frame) * kFrameTime - time_;
        if (tempo_ == 0 || elapsed <= 0)
        {
            return tick_;
        }

        return tick_ + uint64_t(elapsed) * per_quarter_ / (uint64_t(tempo_) * 256);
    }

    // Returns true once a tempo has been set.
    bool Known() const
    {
        return tempo_ != 0;
    }

    // Returns the time of `tick`, in 1/256 microseconds at the driver's speed, at the tempo now set.
    int64_t TickTime(uint64_t tick) const
    {
        return time_ + int64_t((tick - std::min(tick, tick_)) * tempo_ * 256 / per_quarter_);
    }

    // Starts a tempo of `tempo` microseconds a quarter note at `tick`.
    void SetTempo(uint64_t tick, uint32_t tempo)
    {
        if (tempo_ != 0 && tick > tick_)
        {
            time_ += int64_t((tick - tick_) * tempo_ * 256 / per_quarter_);
        }

        tick_ = std::max(tick_, tick);
        tempo_ = tempo;
    }

private:
    uint32_t per_quarter_;
    uint64_t tick_ = 0; // the start of the tempo's segment
    int64_t time_ = 0;  // its time, in 1/256 microseconds
    uint32_t tempo_ = 0;
};

// A channel pitch-offset change and its tick.
struct PitchChange
{
    uint64_t tick;
    int channel;
    int64_t offset;
    uint32_t frame;
};

// A change in whether the mixer leaves out every voice of a key on a channel, at the tick of the frame it happens in.
struct MuteChange
{
    uint64_t tick;
    int channel;
    int key;
    bool muted;
    uint32_t frame;
};

// Results of running a tune through the model: track actions, channel pitch changes, mute changes and warnings.
struct Simulation
{
    std::vector<Action> actions;
    std::vector<PitchChange> pitches;
    std::vector<MuteChange> mutes;
    std::vector<uint64_t> frame_starts; // with frame timing, the tick each frame starts at
    std::vector<std::string> warnings;
    std::optional<uint64_t> cut_off;                       // the tick where the model gave up, at the frame limit
    std::array<EnvelopeSettings, kChannels> end_envelopes; // each channel's settings from controllers 20-23 at the end
};

// Returns the tempo changes in `actions` as (tick, tempo), in the order the model runs them. The driver keeps the tempo
// it set last. A change can run after one with a later tick when two tracks change the tempo within about a frame.
// Such a change starts at that later tick, as in TickClock.
std::vector<std::pair<uint64_t, uint32_t>> TempoChanges(const std::vector<Action>& actions)
{
    std::vector<std::pair<uint64_t, uint32_t>> tempos;
    uint64_t at = 0;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kTempo)
        {
            at = std::max(at, a.tick);
            tempos.emplace_back(at, a.value);
        }
    }

    return tempos;
}

// Returns the tick at which each frame from 0 to `frames` starts, as TickClock gives it, following the tempo changes in
// `actions` in the order of their ticks. The tempo track can play a tick a frame later than other tracks, so a tempo
// change can come in a frame that starts after its tick.
std::vector<uint64_t> FrameStarts(const std::vector<Action>& actions, uint32_t per_quarter, uint32_t frames)
{
    const std::vector<std::pair<uint64_t, uint32_t>> tempos = TempoChanges(actions);
    TickClock clock(per_quarter);
    std::vector<uint64_t> starts;
    size_t next = 0;
    for (uint32_t f = 0; f <= frames; f++)
    {
        while (next < tempos.size() &&
               (!clock.Known() || clock.TickTime(tempos[next].first) <= int64_t(f) * kFrameTime))
        {
            clock.SetTempo(tempos[next].first, tempos[next].second);
            next++;
        }
        starts.push_back(clock.FrameTick(f));
    }

    return starts;
}

// Returns the tick at which the first frame that reaches `tick` starts.
uint64_t OnFrame(const std::vector<uint64_t>& starts, uint64_t tick)
{
    const auto it = std::lower_bound(starts.begin(), starts.end(), tick);
    return it == starts.end() ? starts.back() : *it;
}

// Returns the tick at which the frame that plays `tick` starts: the last frame that starts at or before it, as on every
// track but the tempo track. `starts` holds what FrameStarts() gives for `actions`.
uint64_t PlayingFrame(const std::vector<Action>& actions, uint32_t per_quarter, const std::vector<uint64_t>& starts,
                      uint64_t tick)
{
    // The frame that plays the tick is the one its time falls in, with the tempo changes up to it, as FrameStarts()
    // follows them.
    TickClock clock(per_quarter);
    for (const auto& [at, tempo] : TempoChanges(actions))
    {
        if (clock.Known() && at > tick)
        {
            break;
        }

        clock.SetTempo(at, tempo);
    }

    const uint64_t frame = uint64_t(clock.TickTime(tick) / kFrameTime);
    return starts[size_t(std::min<uint64_t>(frame, starts.size() - 1))];
}

// Runs the model until the plan's end or until the driver has stopped the tune, for an hour at most. With
// `frame_timing`, each action but a tempo change, and each pitch and mute change, goes at the start of the frame the
// driver plays it in. Frame timing also leaves out the note ons at or after the end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, const TuneHeader& header, const Plan& plan,
                    bool frame_timing)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    TickClock clock(header.ticks_per_quarter);
    std::array<int64_t, kChannels> last = {};

    // For each key on each channel (channel * 128 + key): whether it's muted, and in the current frame, -1 without
    // voices, 0 if the mixer leaves out every voice that plays it and 1 if it takes any of them.
    std::vector<bool> muted(kChannels * 128, false);
    std::vector<int8_t> heard(kChannels * 128, -1);
    std::vector<int> keys; // the keys that the frame's voices play, in slot order
    uint32_t frames = 0;
    bool finished = false; // the tune reached the plan's end, or the driver stopped it
    for (uint32_t f = 0; f < kMaxFrames; f++)
    {
        // A pitch change that a bend or program change makes, without vibrato, belongs at that command's tick. Vibrato
        // changes the pitch at the start of each frame.
        const uint64_t frame_tick = clock.FrameTick(f);
        std::array<int64_t, kChannels> command_tick;
        command_tick.fill(-1);
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kTempo)
            {
                clock.SetTempo(a.tick, a.value);
            }
            if (a.kind == Action::kBend || a.kind == Action::kProgram)
            {
                command_tick[a.channel] = int64_t(a.tick);
            }

            sim.actions.push_back(a);
        }

        for (int ch = 0; ch < kChannels; ch++)
        {
            const int64_t offset = seq.PitchOffset(ch);
            if (offset == last[size_t(ch)])
            {
                continue;
            }

            last[size_t(ch)] = offset;
            const bool vibrato = seq.Channel(ch).modulation != 0;
            const int64_t at = command_tick[size_t(ch)];
            sim.pitches.push_back({!vibrato && at >= 0 ? uint64_t(at) : frame_tick, ch, offset, f});
        }

        // A key is muted from a frame in which the mixer leaves out every voice that plays it, at the voice limit or
        // at a level of 0, and plays again from one in which the mixer takes any of them. A frame without voices of
        // the key changes nothing.
        for (int i = 0; i < seq.SlotCount(); i++)
        {
            const Slot& s = seq.GetSlot(i);
            if (s.mix == MixResult::kIdle)
            {
                continue;
            }

            const int key = s.channel * 128 + (s.key & 0x7F);
            if (heard[size_t(key)] < 0)
            {
                keys.push_back(key);
            }
            heard[size_t(key)] = std::max<int8_t>(heard[size_t(key)], s.mix == MixResult::kMixed ? 1 : 0);
        }
        for (int key : keys)
        {
            const bool mute = heard[size_t(key)] == 0;
            if (mute != muted[size_t(key)])
            {
                muted[size_t(key)] = mute;
                sim.mutes.push_back({frame_tick, key / 128, key % 128, mute, f});
            }
            heard[size_t(key)] = -1;
        }
        keys.clear();

        // Stop once every track has read up to the end, and the frames have reached it. A track counts the ticks of a
        // delay as it starts it, so its count reaches the end before the frames do.
        frames = f + 1;
        bool done = clock.Known() ? clock.FrameTick(f + 1) >= plan.end : true;
        for (int t = 0; t < seq.TrackCount() && done; t++)
        {
            done = seq.TrackDone(t) || seq.TrackTick(t) >= plan.end;
        }
        if (done || seq.Ended())
        {
            finished = true;
            break;
        }
    }

    if (!finished && frames == kMaxFrames)
    {
        sim.warnings.push_back("the tune was cut off after an hour");
        sim.cut_off = clock.FrameTick(frames);
    }
    sim.warnings.insert(sim.warnings.end(), seq.Warnings().begin(), seq.Warnings().end());

    for (int ch = 0; ch < kChannels; ch++)
    {
        sim.end_envelopes[size_t(ch)] = seq.Channel(ch).envelope;
    }

    if (frame_timing)
    {
        // The notes at or after the end, which start the loop's next pass, can come in the frame that plays the end,
        // whose start is before it. They aren't converted, as without frame timing, and neither are the envelope
        // settings that their phases take, which would otherwise go to the note before them in their slot.
        const uint64_t end = sim.cut_off ? std::min(plan.end, *sim.cut_off) : plan.end;
        std::vector<bool> slot_past_end(size_t(seq.SlotCount()), false);
        std::vector<Action> kept;
        for (const Action& a : sim.actions)
        {
            if (a.kind == Action::kNoteOn)
            {
                slot_past_end[size_t(a.slot)] = a.tick >= end;
            }

            const bool note_past_end = (a.kind == Action::kNoteOn || a.kind == Action::kNoteDropped) && a.tick >= end;
            if (!note_past_end && !(a.kind == Action::kEnvelope && slot_past_end[size_t(a.slot)]))
            {
                kept.push_back(a);
            }
        }
        sim.actions = std::move(kept);

        sim.frame_starts = FrameStarts(sim.actions, header.ticks_per_quarter, frames);
        for (Action& a : sim.actions)
        {
            if (a.kind != Action::kTempo)
            {
                a.tick = sim.frame_starts[std::min(a.frame, frames)];
            }
        }
        for (PitchChange& p : sim.pitches)
        {
            p.tick = sim.frame_starts[std::min(p.frame, frames)];
        }
        for (MuteChange& m : sim.mutes)
        {
            m.tick = sim.frame_starts[std::min(m.frame, frames)];
        }
    }

    return sim;
}

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int channel = 0;
    int key = 0;
    int velocity = 0;
    int level = 0; // the velocity + 1 of each of the driver's voices that play it, added up
    int program = -1;
    int bank = 0; // the bank of its preset
    uint32_t instrument = 0;
    EnvelopeSettings envelope; // the settings from controllers 20-23 that its envelope's phases took
    uint64_t on = 0;
    uint64_t off = 0;
    uint32_t frame = 0; // the frame the driver starts it on
    bool open = true;
    int voices = 1;               // the driver's voices that play it
    bool before_programs = false; // it plays with the program from before its track's program changes on its tick
};

// Works out the MIDI file's notes from the model's note ons and offs. A MIDI channel can't play one key twice at once,
// so where the driver's channel does, the older note ends when the newer one starts, unless they start on the same
// frame. A note ends where the driver releases its slot, where a later note takes its slot, or at the end of the
// conversion.
std::vector<Note> CollectNotes(const Simulation& sim, const Plan& plan, uint16_t track_mask, int slot_count)
{
    std::vector<Note> notes;
    std::array<std::array<int, 128>, kChannels> owner;
    for (auto& keys : owner)
    {
        keys.fill(-1);
    }
    std::array<std::array<int, 128>, kChannels> latest = owner; // each key's last note, which may have ended
    std::vector<int> slot_note(size_t(slot_count), -1), slot_level(size_t(slot_count), 0);
    std::vector<int> slot_last(size_t(slot_count), -1); // the note each slot played last, which its release is part of

    // A track can play a note just before it gives the channel its first program, at the same tick. The game plays the
    // note with whatever instrument the channel had from the tune before, so the MIDI file gives it that program.
    std::map<std::pair<int, uint64_t>, const Action*> first_program;
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            first_program.emplace(std::make_pair(int(a.channel), a.tick), &a);
        }
    }

    auto close = [&](int index, uint64_t tick)
    {
        // A note lasts at least a tick, so that its note off can't come before its note on.
        Note& n = notes[size_t(index)];
        if (n.open)
        {
            n.open = false;
            n.off = std::max(tick, n.on + 1);
            owner[size_t(n.channel)][size_t(n.key)] = -1;
        }
    };

    // Ends a slot's voice, ending the MIDI note when its last voice ends. A voice that ends in the note's first frame
    // sounds for at most two frames, so the note keeps the combined velocity of the remaining voices.
    auto release = [&](int slot, uint64_t tick, uint32_t frame)
    {
        const int index = slot_note[size_t(slot)];
        slot_note[size_t(slot)] = -1;
        if (index < 0)
        {
            return;
        }

        Note& n = notes[size_t(index)];
        if (--n.voices == 0)
        {
            close(index, tick);
        }
        else if (frame == n.frame)
        {
            n.level -= slot_level[size_t(slot)];
            n.velocity = std::min(127, n.level - 1);
        }
    };

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            release(a.slot, a.tick, a.frame);
            slot_last[size_t(a.slot)] = -1;

            const Action* program = &a;
            if (!a.playable && a.instrument == 0)
            {
                const auto found = first_program.find(std::make_pair(int(a.channel), a.tick));
                program = found != first_program.end() ? found->second : nullptr;
            }
            if ((!a.playable && program == &a) || !program || !(track_mask >> a.track & 1) || a.tick >= plan.end)
            {
                continue;
            }

            // A key played twice on the same frame is one MIDI note, as loud as the two voices together, since the
            // driver starts both on the frame.
            int& held = owner[a.channel][a.a & 0x7F];
            slot_level[size_t(a.slot)] = a.b + 1;
            if (held >= 0 && notes[size_t(held)].frame == a.frame)
            {
                Note& n = notes[size_t(held)];
                n.level += a.b + 1;
                n.velocity = std::min(127, n.level - 1);
                n.voices++;
                slot_note[size_t(a.slot)] = held;
                slot_last[size_t(a.slot)] = held;
                continue;
            }

            // A player takes a tick's events track by track. So where the older note is in a later track, it ends a
            // tick before the newer one starts, or else its note off would end the newer note too. If the older note
            // started a tick before, the newer one starts a tick late instead.
            uint64_t on = a.tick;
            if (held >= 0 && notes[size_t(held)].track > a.track)
            {
                on = std::max(on, notes[size_t(held)].on + 2);
                close(held, on - 1);
            }
            else if (held >= 0)
            {
                close(held, on);
            }

            // A note lasts at least a tick, even one that the driver releases in the frame it starts in, whose release
            // frame timing puts on its start's tick. So where the key's last note ends after this one's start, this one
            // starts as it ends, or a tick later if that note is in a later track, as above.
            int& previous = latest[a.channel][a.a & 0x7F];
            if (previous >= 0 && notes[size_t(previous)].off > on)
            {
                on = notes[size_t(previous)].off + (notes[size_t(previous)].track > a.track ? 1 : 0);
            }

            Note n;
            n.track = a.track;
            n.channel = a.channel;
            n.key = a.a & 0x7F;
            n.velocity = a.b;
            n.level = a.b + 1;
            n.program = program == &a ? a.program : program->a;
            n.instrument = program->instrument;
            n.on = on;
            n.frame = a.frame;
            notes.push_back(n);
            held = int(notes.size()) - 1;
            previous = held;
            slot_note[size_t(a.slot)] = held;
            slot_last[size_t(a.slot)] = held;
        }
        else if (a.kind == Action::kEnvelope && slot_last[size_t(a.slot)] >= 0)
        {
            // Each phase takes its own settings, which may not be those the channel had when the note started.
            EnvelopeSettings& e = notes[size_t(slot_last[size_t(a.slot)])].envelope;
            const EnvelopeSettings& took = a.envelope;
            e.attack = took.attack < 0x80 ? took.attack : e.attack;
            e.decay = took.decay < 0x80 ? took.decay : e.decay;
            e.sustain = took.sustain < 0x80 ? took.sustain : e.sustain;
            e.release = took.release < 0x80 ? took.release : e.release;
        }
        else if (a.kind == Action::kNoteOff)
        {
            // The release of an older note with the same key, which the MIDI file has ended already, leaves the newer
            // one playing. A note off that releases nothing ends a note that has ended by itself in the driver.
            const int held = owner[a.channel][a.a & 0x7F];
            if (held >= 0 && a.slot < 0)
            {
                close(held, std::min(a.tick, plan.end));
            }
            else if (held >= 0 && slot_note[size_t(a.slot)] == held)
            {
                release(a.slot, std::min(a.tick, plan.end), a.frame);
            }
        }
    }

    // The MIDI file releases the notes still playing at the end. Each takes the release that its channel's settings
    // give there. The driver's release would take the same.
    for (size_t i = 0; i < notes.size(); i++)
    {
        Note& n = notes[i];
        if (n.open && n.envelope.release >= 0x80)
        {
            n.envelope.release = sim.end_envelopes[size_t(n.channel)].release;
        }
        close(int(i), plan.end);
    }

    return notes;
}

// Returns the key pressures that mute the notes that the mixer leaves out, on the channels that `notes` play: 127 from
// the frame in which the mixer leaves out every voice of a key on a channel, and 0 from the frame in which it takes one
// again. A player that jumps back to the loop's start keeps the pressures that the loop's end left. So a key that has
// another pressure at the loop's end gets the pressure from the loop's start again there, unless it changes there.
std::vector<MuteChange> KeyPressures(const Simulation& sim, const Plan& plan, const std::vector<Note>& notes)
{
    // The channels that the notes play, and the first note on of each key on each channel in each frame.
    std::array<bool, kChannels> plays = {};
    std::map<std::tuple<int, int, uint32_t>, uint64_t> first_on;
    for (const Note& n : notes)
    {
        plays[size_t(n.channel)] = true;
        const auto [found, added] = first_on.emplace(std::make_tuple(n.channel, n.key, n.frame), n.on);
        found->second = std::min(found->second, n.on);
    }

    // A note that a track plays a frame late starts before its frame does. A pressure from the frame in which a note of
    // its key starts therefore goes no later than the note on. The note then starts with it. Each key's pressures keep
    // their order.
    std::vector<MuteChange> pressures;
    std::vector<uint64_t> last(kChannels * 128, 0);
    for (MuteChange m : sim.mutes)
    {
        const auto found = first_on.find(std::make_tuple(m.channel, m.key, m.frame));
        uint64_t& previous = last[size_t(m.channel * 128 + m.key)];
        m.tick = std::max(previous, found != first_on.end() ? std::min(m.tick, found->second) : m.tick);
        if (m.tick < plan.end && plays[size_t(m.channel)])
        {
            previous = m.tick;
            pressures.push_back(m);
        }
    }

    if (!plan.loops)
    {
        return pressures;
    }

    // Each key's pressure at the loop's start, before its end and at its end, and whether it changes at the start.
    struct LoopPressure
    {
        bool start = false, before_end = false, end = false, changes_at_start = false;
    };
    std::map<std::pair<int, int>, LoopPressure> keys;
    for (const MuteChange& m : pressures)
    {
        LoopPressure& p = keys[{m.channel, m.key}];
        p.start = m.tick <= plan.loop_start ? m.muted : p.start;
        p.before_end = m.tick < plan.loop_end ? m.muted : p.before_end;
        p.end = m.tick <= plan.loop_end ? m.muted : p.end;
        p.changes_at_start = p.changes_at_start || m.tick == plan.loop_start;
    }

    for (const auto& [key, p] : keys)
    {
        if (!p.changes_at_start && (p.before_end != p.start || p.end != p.start))
        {
            pressures.push_back({plan.loop_start, key.first, key.second, p.start, 0});
        }
    }

    return pressures;
}

// Returns the pitch bend range of each channel, in semitones: wide enough for its largest pitch offset, and its
// instruments' bend range, so that a bend without vibrato keeps its value.
std::array<int, kChannels> BendRanges(const Rom& rom, const Simulation& sim)
{
    std::array<int, kChannels> range = {};
    for (const PitchChange& p : sim.pitches)
    {
        const int64_t size = p.offset < 0 ? -p.offset : p.offset;
        range[size_t(p.channel)] = std::max(range[size_t(p.channel)], int((size + kSemitone - 1) / kSemitone));
    }

    // The bends' instrument ranges count for channels with any pitch change.
    std::array<uint32_t, kChannels> instrument = {};
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            instrument[a.channel] = a.instrument;
        }
        if (a.kind == Action::kBend && range[a.channel] > 0)
        {
            Instrument inst;
            if (ReadInstrument(rom, instrument[a.channel], inst))
            {
                range[a.channel] = std::max(range[a.channel], std::clamp(int(inst.bend_range), 0, 127));
            }
        }
    }

    for (int& r : range)
    {
        r = std::min(r, 127);
    }

    return range;
}

// Returns the MIDI pitch bend value for a pitch offset, with a bend range of `range` semitones, at least 1.
int BendValue(int64_t offset, int range)
{
    const int64_t scaled = offset * 8192;
    const int64_t unit = int64_t(range) * kSemitone;
    const int64_t steps = scaled >= 0 ? (scaled + unit / 2) / unit : -((-scaled + unit / 2) / unit);
    return int(std::clamp<int64_t>(8192 + steps, 0, 16383));
}

// Returns the elapsed seconds at `tick`, using the GBA's frame rate and the tempo changes in `actions`.
double TickSeconds(const std::vector<Action>& actions, uint32_t per_quarter, uint64_t tick)
{
    // Add up the duration of each tempo segment that starts before `tick`.
    double seconds = 0;
    uint64_t at = 0;
    uint32_t tempo = 500000;
    for (const auto& [change, value] : TempoChanges(actions))
    {
        if (change >= tick)
        {
            break;
        }

        seconds += double(change - at) * tempo / per_quarter / 1e6;
        at = change;
        tempo = value;
    }

    // The final segment runs from the last tempo change to `tick`.
    seconds += double(tick - at) * tempo / per_quarter / 1e6;

    return seconds * kTempoScale;
}

// Fills in a summary's length, loop and tempo.
void Summarize(const Simulation& sim, const Plan& plan, uint32_t per_quarter, SongSummary& sum)
{
    sum.seconds = TickSeconds(sim.actions, per_quarter, plan.end);
    if (plan.loops)
    {
        sum.loop_start = TickSeconds(sim.actions, per_quarter, plan.loop_start);
        sum.loop_end = TickSeconds(sim.actions, per_quarter, plan.loop_end);
    }

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kTempo && a.value)
        {
            sum.bpm = 60000000.0 / (a.value * kTempoScale);
            break;
        }
    }
}

// Writes a channel's settings at the start: the driver's volume (unless the tune sets one at the start), no pan,
// effects or expression, and the bend range.
void WriteChannelSetup(MidiTrack& mt, int ch, int range, int bank, bool volume_at_start)
{
    if (bank)
    {
        mt.Bank(0, ch, bank);
    }
    if (!volume_at_start)
    {
        mt.Control(0, ch, cc::kVolume, 127);
    }
    mt.Control(0, ch, cc::kPan, 64);
    mt.Control(0, ch, cc::kExpression, 127);
    mt.Control(0, ch, cc::kReverb, 0);
    mt.Control(0, ch, cc::kChorus, 0);
    if (range > 0)
    {
        mt.BendRange(0, ch, range);
        mt.PitchBend(0, ch, 8192);
    }
}

// Returns the instrument of the preset at `bank` and `program` in `file`, or -1 if there's none.
int PresetInstrument(const Sf2File& file, int bank, int program)
{
    for (const sf2::Preset& p : file.presets)
    {
        if (p.bank == bank && p.program == program)
        {
            return supergbamidi::PresetInstrument(p);
        }
    }

    return -1;
}

// Adds a preset for each program the notes play, in each note's bank. A note on the drum channel without envelope
// settings gets a copy in the drum bank too. In a shared SoundFont, another set of instruments can have the program
// there already, and `clashes` gets the program. Returns the programs whose instruments have nothing to play.
std::set<int> AddPresets(SoundfontBuilder& sf, const std::vector<Note>& notes, std::set<int>& clashes)
{
    std::set<int> missing;
    for (const Note& n : notes)
    {
        const int instrument = sf.InstrumentFor(n.instrument, n.envelope);
        if (instrument < 0)
        {
            missing.insert(n.program);
            continue;
        }

        sf.AddPreset(n.bank, n.program, instrument);
        if (n.channel == kDrumChannel && n.envelope.None())
        {
            const int drum = PresetInstrument(sf.File(), kDrumBank, n.program);
            if (drum < 0)
            {
                sf.AddPreset(kDrumBank, n.program, instrument);
            }
            else if (drum != instrument)
            {
                clashes.insert(n.program);
            }
        }
    }

    return missing;
}

// A program change, with the bank it chooses, and the track of the MIDI file it goes in, given by the tune's track
// number.
struct ProgramChange
{
    uint64_t tick;
    int track;
    int channel;
    int program;
    int bank;
};

// Preserves the driver's program for each note despite differences in event ordering. A player takes a tick's events
// track by track, and within a track, program changes before note-ons, where the driver runs each track's commands of a
// frame in order. So a program change on a note's channel and tick, after the note in its track or on an earlier track
// in the file, would give the note the wrong program. Such a note moves before its track's program changes on its tick,
// or gets a program change of its own. So does a note whose preset is in another bank than the program change before it
// chooses, for the envelope settings it plays with.
void MatchPrograms(std::vector<Note>& notes, std::vector<ProgramChange>& programs)
{
    auto in_file_order = [](const ProgramChange& a, const ProgramChange& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : a.track < b.track;
    };
    std::stable_sort(programs.begin(), programs.end(), in_file_order);

    std::vector<size_t> order(notes.size());
    for (size_t i = 0; i < order.size(); i++)
    {
        order[i] = i;
    }
    auto note_order = [&](size_t a, size_t b)
    {
        return notes[a].on != notes[b].on ? notes[a].on < notes[b].on : notes[a].track < notes[b].track;
    };
    std::stable_sort(order.begin(), order.end(), note_order);

    for (size_t i : order)
    {
        // The bank and program the file gives the note, and those it would give it before its track's changes on its
        // tick.
        Note& n = notes[i];
        std::pair<int, int> before = {-1, -1}, after = {-1, -1};
        for (const ProgramChange& p : programs)
        {
            if (p.tick > n.on || (p.tick == n.on && p.track > n.track))
            {
                break;
            }
            if (p.channel != n.channel)
            {
                continue;
            }

            after = {p.bank, p.program};
            if (p.tick < n.on || p.track < n.track)
            {
                before = after;
            }
        }

        const std::pair<int, int> preset = {n.bank, n.program};
        if (n.program < 0 || preset == after)
        {
            continue;
        }
        if (preset == before)
        {
            n.before_programs = true;
            continue;
        }

        const ProgramChange change = {n.on, n.track, n.channel, n.program, n.bank};
        programs.insert(std::upper_bound(programs.begin(), programs.end(), change, in_file_order), change);
    }
}

// Writes the MIDI file: a conductor track, then a track for each of the tune's tracks that plays notes.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const TuneHeader& header,
               const Simulation& sim, const Plan& plan, const std::vector<Note>& notes,
               const std::vector<MuteChange>& pressures, const std::array<int, kChannels>& range, int bank,
               std::string& error)
{
    MidiFile midi(uint16_t(header.ticks_per_quarter));
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    if (plan.loops)
    {
        conductor.Meta(uint32_t(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(uint32_t(plan.loop_end), 0x06, "loopEnd");
    }
    conductor.SetEnd(uint32_t(plan.end));

    // A track for each of the tune's tracks with notes, in order. A channel's events go in the first one that plays it.
    std::vector<bool> plays(header.track_count, false);
    std::array<int, kChannels> first = {};
    first.fill(int(header.track_count));
    for (const Note& n : notes)
    {
        plays[size_t(n.track)] = true;
        first[size_t(n.channel)] = std::min(first[size_t(n.channel)], n.track);
    }

    std::vector<MidiTrack*> track(header.track_count, nullptr);
    for (uint32_t t = 0; t < header.track_count; t++)
    {
        if (plays[t])
        {
            track[t] = &midi.AddTrack();
            track[t]->Name("Track " + std::to_string(t));
            track[t]->SetEnd(uint32_t(plan.end));
        }
    }

    std::array<MidiTrack*, kChannels> home = {};
    for (int ch = 0; ch < kChannels; ch++)
    {
        home[size_t(ch)] = first[size_t(ch)] < int(header.track_count) ? track[size_t(first[size_t(ch)])] : nullptr;
    }

    // The tempo goes in the conductor track, at the GBA's speed, on the ticks that TempoChanges() gives. Program
    // changes and volume go in the track that gave them, if it's in the file, or else in the channel's track.
    std::array<bool, kChannels> volume_at_start = {};
    std::vector<ProgramChange> programs;
    uint64_t tempo_tick = 0;
    for (const Action& a : sim.actions)
    {
        if (a.tick > plan.end || (a.tick == plan.end && a.kind != Action::kTempo))
        {
            continue;
        }

        const int destination = track[a.track] ? a.track : first[a.channel];
        MidiTrack* mt = destination < int(header.track_count) ? track[size_t(destination)] : nullptr;
        switch (a.kind)
        {
        case Action::kTempo:
            tempo_tick = std::max(tempo_tick, a.tick);
            conductor.Tempo(uint32_t(tempo_tick), uint32_t(std::lround(a.value * kTempoScale)));
            break;

        case Action::kProgram:
            if (mt)
            {
                programs.push_back({a.tick, destination, a.channel, a.a, bank});
            }
            break;

        case Action::kVolume:
            if (mt)
            {
                mt->Control(uint32_t(a.tick), a.channel, cc::kVolume, a.a);
                volume_at_start[a.channel] = volume_at_start[a.channel] || a.tick == 0;
            }
            break;

        default:
            break;
        }
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (home[size_t(ch)])
        {
            WriteChannelSetup(*home[size_t(ch)], ch, range[size_t(ch)], bank, volume_at_start[size_t(ch)]);
        }
    }

    // A program change gives its bank first where the channel's bank changes, to that of the presets for the envelope
    // settings that the notes after it play with, or back to the tune's.
    std::vector<Note> ordered = notes;
    MatchPrograms(ordered, programs);
    std::array<int, kChannels> channel_bank;
    channel_bank.fill(bank);
    for (const ProgramChange& p : programs)
    {
        if (p.bank != channel_bank[size_t(p.channel)])
        {
            track[size_t(p.track)]->Bank(uint32_t(p.tick), p.channel, p.bank);
            channel_bank[size_t(p.channel)] = p.bank;
        }
        track[size_t(p.track)]->Program(uint32_t(p.tick), p.channel, p.program);
    }

    // A player that jumps back to the loop's start keeps the bends that the loop's end left, as the game does until the
    // loop's first bend or program change on the channel sets its pitch. So each channel's bend is written again there,
    // unless it changes there or before, or the loop starts with the tune.
    std::array<uint64_t, kChannels> loop_set;
    loop_set.fill(plan.end);
    for (const Action& a : sim.actions)
    {
        if ((a.kind == Action::kBend || a.kind == Action::kProgram) && a.tick >= plan.loop_start)
        {
            loop_set[size_t(a.channel)] = std::min(loop_set[size_t(a.channel)], a.tick);
        }
    }

    std::array<int64_t, kChannels> loop_offset = {};
    for (const PitchChange& p : sim.pitches)
    {
        if (p.tick < plan.loop_start)
        {
            loop_offset[size_t(p.channel)] = p.offset;
        }
        else if (p.tick <= loop_set[size_t(p.channel)])
        {
            loop_set[size_t(p.channel)] = plan.end;
        }

        if (home[size_t(p.channel)] && range[size_t(p.channel)] > 0 && p.tick < plan.end)
        {
            home[size_t(p.channel)]->PitchBend(uint32_t(p.tick), p.channel,
                                               BendValue(p.offset, range[size_t(p.channel)]));
        }
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (plan.loops && plan.loop_start > 0 && loop_set[size_t(ch)] < plan.end && home[size_t(ch)] &&
            range[size_t(ch)] > 0)
        {
            home[size_t(ch)]->PitchBend(uint32_t(loop_set[size_t(ch)]), ch,
                                        BendValue(loop_offset[size_t(ch)], range[size_t(ch)]));
        }
    }

    // The key pressures go in the channel's track, before the note ons at their ticks.
    for (const MuteChange& m : pressures)
    {
        home[size_t(m.channel)]->KeyPressure(uint32_t(m.tick), m.channel, m.key, m.muted ? 127 : 0);
    }

    for (const Note& n : ordered)
    {
        MidiTrack& mt = *track[size_t(n.track)];
        mt.NoteOn(uint32_t(n.on), n.channel, n.key, n.velocity, n.before_programs);
        mt.NoteOff(uint32_t(n.off), n.channel, n.key);
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a tune, writing its files if `write` is set.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, SoundfontBuilder* shared,
                bool write)
{
    SongSummary sum;
    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), header))
    {
        sum.warnings.push_back("its header can't be read");
        return sum;
    }

    Plan plan = PlanTune(rom, info, header, opt.loops);
    const Simulation sim = Simulate(rom, info, song, header, plan, opt.frame_timing);

    // A tune that the model gave up on ends where it stopped.
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    Summarize(sim, plan, header.ticks_per_quarter, sum);

    // With frame timing, the loop's start and end go at the start of the frames that play them, as their events do, so
    // that each pass's first events fall on the same side of the marker. The end goes at the start of the first frame
    // that reaches it, after the frame that plays it.
    if (opt.frame_timing)
    {
        plan.loop_start = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_start);
        plan.loop_end = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_end);
        plan.end = OnFrame(sim.frame_starts, plan.end);
    }

    sum.warnings = sim.warnings;
    std::vector<Note> notes = CollectNotes(sim, plan, opt.track_mask, kChannels * info.slots_per_channel);

    std::set<int> tracks;
    for (const Note& n : notes)
    {
        tracks.insert(n.track);
    }

    // The notes that the driver drops, of the tracks that the conversion keeps.
    int dropped = 0;
    for (const Action& a : sim.actions)
    {
        dropped += a.kind == Action::kNoteDropped && a.tick < plan.end && (opt.track_mask >> a.track & 1) ? 1 : 0;
    }
    if (dropped)
    {
        sum.warnings.push_back(
            std::to_string(dropped) + " note" + (dropped == 1 ? "" : "s") +
            " found no available slot on their channel; omitted from the MIDI file to match the game");
    }

    sum.tracks = int(tracks.size());
    sum.silent = notes.empty();
    if (sum.silent || !write)
    {
        sum.ok = true;
        return sum;
    }

    // The SoundFont's presets, in the song's file or the shared one. Those of notes that play with envelope settings go
    // in banks of their own.
    SoundfontBuilder own(rom, info);
    SoundfontBuilder& sf = shared ? *shared : own;
    for (Note& n : notes)
    {
        n.bank = sf.BankFor(opt.bank, n.envelope);
    }

    // The SoundFont mutes a note while its key's pressure is high, in a tune that the mixer leaves voices out of.
    const std::vector<MuteChange> pressures = KeyPressures(sim, plan, notes);
    if (std::any_of(pressures.begin(), pressures.end(), [](const MuteChange& m) { return m.muted; }))
    {
        sf.AddMutes();
    }

    std::set<int> clashes;
    for (int program : AddPresets(sf, notes, clashes))
    {
        sum.warnings.push_back("program " + std::to_string(program) + "'s instrument has no samples to play");
    }
    for (int program : clashes)
    {
        sum.warnings.push_back("channel 10's program " + std::to_string(program) +
                               " is another set of instruments' in the drum bank, which some players use there");
    }

    const std::string title = rom.Title() + " #" + TwoDigits(song);
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) tune %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(header.address), kProgramName);
    const std::string stem =
        Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + TwoDigits(song)));

    std::string error;
    sum.midi_path = stem + ".mid";
    if (!WriteMidi(sum.midi_path, title, about, header, sim, plan, notes, pressures, BendRanges(rom, sim), opt.bank,
                   error))
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
    TuneHeader h;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), h))
    {
        error = "invalid tune";
        return false;
    }

    std::string text;
    char line[256];
    std::snprintf(line, sizeof line,
                  "; %s (%s) tune %d\n; header 0x%08X: %u tracks, %u ticks a quarter note, program map 0x%08X, "
                  "instruments 0x%08X\n; columns: address, tick, bytes, command\n",
                  rom.Title().c_str(), rom.GameCode().c_str(), song, unsigned(h.address), unsigned(h.track_count),
                  unsigned(h.ticks_per_quarter), unsigned(h.program_map), unsigned(h.instruments));
    text += line;

    // Each track's commands, up to its end or the end of its first loop.
    for (uint32_t t = 0; t < h.track_count; t++)
    {
        std::snprintf(line, sizeof line, "\n; ---- track %u at 0x%08X ----\n", unsigned(t), unsigned(h.tracks[t]));
        text += line;

        uint32_t address = h.tracks[t];
        uint64_t tick = 0;
        bool loop_started = false;
        for (int n = 0; n < kMaxListedCommands; n++)
        {
            Event e;
            if (!DecodeEvent(rom, address, info.format, e))
            {
                std::snprintf(line, sizeof line, "0x%08X %8llu  %02X  (unknown command; the track stops here)\n",
                              unsigned(address), static_cast<unsigned long long>(tick), rom.U8(address));
                text += line;
                break;
            }

            std::string bytes;
            for (uint32_t i = 0; i < e.size; i++)
            {
                char b[4];
                std::snprintf(b, sizeof b, "%02X ", rom.U8(address + i));
                bytes += b;
            }

            // The driver ignores a loop's end before its start.
            const bool control = e.command == kCmdControl;
            const bool loop_end = control && e.a == kCtrlLoopEnd && loop_started;
            const char* const note =
                control && e.a == kCtrlLoopEnd && !loop_started ? " (no loop to end; ignored)" : "";
            std::snprintf(line, sizeof line, "0x%08X %8llu  %-15s %s%s\n", unsigned(address),
                          static_cast<unsigned long long>(tick), bytes.c_str(), DescribeEvent(e).c_str(), note);
            text += line;

            address += e.size;
            loop_started = loop_started || (control && e.a == kCtrlLoopStart);
            if (e.command == kCmdDelay1 || e.command == kCmdDelay2 || e.command == kCmdDelay3)
            {
                tick += e.value;
            }
            if (e.command == kCmdEnd || loop_end)
            {
                break;
            }
        }
    }

    std::vector<uint8_t> data(text.begin(), text.end());
    return WriteFile(path, data, error);
}

} // namespace supergbamidi::rare
