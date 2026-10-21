// SPDX-License-Identifier: MIT

#include "rare/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rare/driver.h"
#include "rare/sequencer.h"
#include "rare/song.h"
#include "rare/soundfont.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rare
{
namespace
{

// Maximum conversion duration: one hour, measured in frames.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// Maximum number of commands DumpSong() lists per track.
constexpr int kMaxListedCommands = 200000;

// Plans the conversion of a tune from its tracks (see PlanSong()).
SongPlan PlanTune(const Rom& rom, const DriverInfo& info, const TuneHeader& header, int loops)
{
    std::vector<TrackLayout> layouts;
    for (uint32_t address : header.tracks)
    {
        layouts.push_back(ScanTrack(rom, address, info.format));
    }

    return PlanSong(layouts, loops, LoopStart::kFirstPass);
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
// it set last. A change can run after one with a later tick when two tracks change the tempo within about a frame. Such
// a change starts at that later tick, as in TickClock.
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

// Follows which keys the mixer leaves out, frame by frame. A key is muted from a frame in which the mixer leaves out
// every voice that plays it, at the voice limit or at a level of 0. The key plays again from a frame in which the mixer
// takes any of them. A frame without voices of the key changes nothing.
class MuteTracker
{
public:
    // Adds the mute changes of `frame` to `changes`, at `tick`. `seq` has just run the frame. The keys come in the
    // order of the slots that first play them in the frame.
    void Update(const Sequencer& seq, uint32_t frame, uint64_t tick, std::vector<MuteChange>& changes);

private:
    // The keys of every channel, as channel * 128 + key.
    static constexpr size_t kKeys = kChannels * 128;

    std::vector<bool> muted_ = std::vector<bool>(kKeys, false);

    // In the current frame, -1 for a key without voices, 0 where the mixer leaves out every voice that plays the key,
    // and 1 where the mixer takes any of them.
    std::vector<int8_t> heard_ = std::vector<int8_t>(kKeys, -1);

    std::vector<int> keys_; // the keys that the frame's voices play, in slot order
};

void MuteTracker::Update(const Sequencer& seq, uint32_t frame, uint64_t tick, std::vector<MuteChange>& changes)
{
    for (int i = 0; i < seq.SlotCount(); i++)
    {
        const Slot& s = seq.GetSlot(i);
        if (s.mix == MixResult::kIdle)
        {
            continue;
        }

        const int key = s.channel * 128 + (s.key & 0x7F);
        if (heard_[size_t(key)] < 0)
        {
            keys_.push_back(key);
        }
        heard_[size_t(key)] = std::max<int8_t>(heard_[size_t(key)], s.mix == MixResult::kMixed ? 1 : 0);
    }

    for (int key : keys_)
    {
        const bool mute = heard_[size_t(key)] == 0;
        if (mute != muted_[size_t(key)])
        {
            muted_[size_t(key)] = mute;
            changes.push_back({tick, key / 128, key % 128, mute, frame});
        }
        heard_[size_t(key)] = -1;
    }
    keys_.clear();
}

// Moves each action but a tempo change, and each pitch and mute change, to the start of the frame that the driver plays
// it in. `frames` is the number of frames that the model ran, with `slot_count` note slots. Also leaves out the note
// ons at or after the end.
void PutOnFrames(Simulation& sim, const SongPlan& plan, uint32_t per_quarter, uint32_t frames, int slot_count)
{
    // The notes at or after the end start the loop's next pass. They can come in the frame that plays the end, a frame
    // that starts before the end. Like the default timing, frame timing leaves them out. It also leaves out the
    // envelope settings that their phases take. The conversion would otherwise give those settings to the note before
    // them in their slot.
    const uint64_t end = sim.cut_off ? std::min(plan.end, *sim.cut_off) : plan.end;
    std::vector<bool> slot_past_end(size_t(slot_count), false);
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

    sim.frame_starts = FrameStarts(sim.actions, per_quarter, frames);
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

// Runs the model until the plan's end or until the driver has stopped the tune, for an hour at most. With
// `frame_timing`, each action but a tempo change, and each pitch and mute change, goes at the start of the frame the
// driver plays it in. Frame timing also leaves out the note ons at or after the end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, const TuneHeader& header, const SongPlan& plan,
                    bool frame_timing)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    TickClock clock(header.ticks_per_quarter);
    std::array<int64_t, kChannels> last = {};
    MuteTracker mute_tracker;
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

        mute_tracker.Update(seq, f, frame_tick, sim.mutes);

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
        PutOnFrames(sim, plan, header.ticks_per_quarter, frames, seq.SlotCount());
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

// Works out the MIDI file's notes from the model's note ons and offs. A MIDI channel can't play one key twice at once.
// Where the driver's channel does, the older note ends when the newer one starts, unless they start on the same frame.
// A note ends where the driver releases its slot, where a later note takes its slot, or at the end of the conversion.
class NoteCollector
{
public:
    // Collects the notes of the tracks in `track_mask` from `sim`. `slot_count` is the number of the model's note
    // slots. `sim` and `plan` have to outlive the collector.
    NoteCollector(const Simulation& sim, const SongPlan& plan, uint32_t track_mask, int slot_count);

    // Returns the notes in the order of the model's note ons. Call it once.
    std::vector<Note> Collect();

private:
    // Starts the MIDI note of a note on. A note on of a key that started a note on the same frame adds a voice to that
    // note instead.
    void NoteOn(const Action& a);

    // Ends the note that a note off releases.
    void NoteOff(const Action& a);

    // Gives the note that a slot played last the settings that a phase of the slot's envelope took.
    void TakeSettings(const Action& a);

    // Returns the action that gives a note on its program: the note on itself for a note that the model plays. A note
    // before its channel's first program change takes the change at its tick, if there's one. Returns null for any
    // other note that the model doesn't play.
    const Action* ProgramFor(const Action& a) const;

    // Ends note `index` at `tick`, unless it has ended already.
    void Close(int index, uint64_t tick);

    // Ends a slot's voice, and the note when its last voice ends.
    void Release(int slot, uint64_t tick, uint32_t frame);

    const Simulation& sim_;
    const SongPlan& plan_;
    uint32_t track_mask_;
    std::vector<Note> notes_;
    std::array<std::array<int, 128>, kChannels> owner_;  // each key's playing note, or -1
    std::array<std::array<int, 128>, kChannels> latest_; // each key's last note, ended or not, or -1
    std::vector<int> slot_note_;                         // the note of each slot's voice, or -1
    std::vector<int> slot_level_;                        // the velocity + 1 of each slot's voice

    // The note that each slot played last, or -1. The release of the slot's voice is part of the note.
    std::vector<int> slot_last_;

    // The first program change on each channel at each tick.
    std::map<std::pair<int, uint64_t>, const Action*> first_program_;
};

NoteCollector::NoteCollector(const Simulation& sim, const SongPlan& plan, uint32_t track_mask, int slot_count)
    : sim_(sim),
      plan_(plan),
      track_mask_(track_mask),
      slot_note_(size_t(slot_count), -1),
      slot_level_(size_t(slot_count), 0),
      slot_last_(size_t(slot_count), -1)
{
    for (auto& keys : owner_)
    {
        keys.fill(-1);
    }
    latest_ = owner_;

    // A track can play a note just before it gives the channel its first program, at the same tick. The game plays the
    // note with whatever instrument the channel had from the tune before. The MIDI file gives the note the program that
    // the channel gets at its tick.
    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kProgram)
        {
            first_program_.emplace(std::make_pair(int(a.channel), a.tick), &a);
        }
    }
}

std::vector<Note> NoteCollector::Collect()
{
    for (const Action& a : sim_.actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            NoteOn(a);
        }
        else if (a.kind == Action::kEnvelope)
        {
            TakeSettings(a);
        }
        else if (a.kind == Action::kNoteOff)
        {
            NoteOff(a);
        }
    }

    // The MIDI file releases the notes still playing at the end. Each takes the release that its channel's settings
    // give there. The driver's release would take the same.
    for (size_t i = 0; i < notes_.size(); i++)
    {
        Note& n = notes_[i];
        if (n.open && n.envelope.release >= 0x80)
        {
            n.envelope.release = sim_.end_envelopes[size_t(n.channel)].release;
        }
        Close(int(i), plan_.end);
    }

    return std::move(notes_);
}

void NoteCollector::NoteOn(const Action& a)
{
    Release(a.slot, a.tick, a.frame);
    slot_last_[size_t(a.slot)] = -1;

    const Action* program = ProgramFor(a);
    if (!program || !(track_mask_ >> a.track & 1) || a.tick >= plan_.end)
    {
        return;
    }

    // A key played twice on the same frame is one MIDI note, as loud as the two voices together. The driver starts both
    // on the frame.
    int& held = owner_[a.channel][a.a & 0x7F];
    slot_level_[size_t(a.slot)] = a.b + 1;
    if (held >= 0 && notes_[size_t(held)].frame == a.frame)
    {
        Note& n = notes_[size_t(held)];
        n.level += a.b + 1;
        n.velocity = std::min(127, n.level - 1);
        n.voices++;
        slot_note_[size_t(a.slot)] = held;
        slot_last_[size_t(a.slot)] = held;
        return;
    }

    // A player takes a tick's events track by track. So where the older note is in a later track, it ends a tick before
    // the newer one starts. Otherwise its note off would end the newer note too. If the older note started a tick
    // before, the newer one starts a tick late instead.
    uint64_t on = a.tick;
    if (held >= 0 && notes_[size_t(held)].track > a.track)
    {
        on = std::max(on, notes_[size_t(held)].on + 2);
        Close(held, on - 1);
    }
    else if (held >= 0)
    {
        Close(held, on);
    }

    // A note lasts at least a tick, even one that the driver releases in the frame it starts in. Frame timing puts such
    // a release on the note's start tick. So where the key's last note ends after this one's start, this one starts as
    // that one ends. It starts a tick later if that note is in a later track, for the reason above.
    int& previous = latest_[a.channel][a.a & 0x7F];
    if (previous >= 0 && notes_[size_t(previous)].off > on)
    {
        on = notes_[size_t(previous)].off + (notes_[size_t(previous)].track > a.track ? 1 : 0);
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
    notes_.push_back(n);
    held = int(notes_.size()) - 1;
    previous = held;
    slot_note_[size_t(a.slot)] = held;
    slot_last_[size_t(a.slot)] = held;
}

void NoteCollector::NoteOff(const Action& a)
{
    // Where a note off releases an older note of the key, the MIDI file has ended that note already. The newer note
    // goes on playing. A note off that releases nothing ends a note that has ended by itself in the driver.
    const int held = owner_[a.channel][a.a & 0x7F];
    if (held >= 0 && a.slot < 0)
    {
        Close(held, std::min(a.tick, plan_.end));
    }
    else if (held >= 0 && slot_note_[size_t(a.slot)] == held)
    {
        Release(a.slot, std::min(a.tick, plan_.end), a.frame);
    }
}

void NoteCollector::TakeSettings(const Action& a)
{
    // Each phase takes the settings that the channel has when the phase starts. Those may differ from the settings at
    // the note's start.
    const int index = slot_last_[size_t(a.slot)];
    if (index >= 0)
    {
        notes_[size_t(index)].envelope.Merge(a.envelope);
    }
}

const Action* NoteCollector::ProgramFor(const Action& a) const
{
    if (a.playable)
    {
        return &a;
    }
    if (a.instrument != 0)
    {
        return nullptr;
    }

    const auto found = first_program_.find(std::make_pair(int(a.channel), a.tick));
    return found != first_program_.end() ? found->second : nullptr;
}

void NoteCollector::Close(int index, uint64_t tick)
{
    // A note lasts at least a tick. Its note off then can't come before its note on.
    Note& n = notes_[size_t(index)];
    if (n.open)
    {
        n.open = false;
        n.off = std::max(tick, n.on + 1);
        owner_[size_t(n.channel)][size_t(n.key)] = -1;
    }
}

void NoteCollector::Release(int slot, uint64_t tick, uint32_t frame)
{
    const int index = slot_note_[size_t(slot)];
    slot_note_[size_t(slot)] = -1;
    if (index < 0)
    {
        return;
    }

    // A voice that ends in the note's first frame sounds for two frames at most. The note then keeps the combined
    // velocity of the remaining voices.
    Note& n = notes_[size_t(index)];
    if (--n.voices == 0)
    {
        Close(index, tick);
    }
    else if (frame == n.frame)
    {
        n.level -= slot_level_[size_t(slot)];
        n.velocity = std::min(127, n.level - 1);
    }
}

// Returns the key pressures that mute the notes that the mixer leaves out, on the channels that `notes` play: 127 from
// the frame in which the mixer leaves out every voice of a key on a channel, and 0 from the frame in which it takes one
// again. A player that jumps back to the loop's start keeps the pressures that the loop's end left. So a key that has
// another pressure at the loop's end gets the pressure from the loop's start again there, unless it changes there.
std::vector<MuteChange> KeyPressures(const Simulation& sim, const SongPlan& plan, const std::vector<Note>& notes)
{
    // The channels that the notes play, and the first note on of each key on each channel in each frame.
    std::array<bool, kChannels> plays = {};
    std::map<std::tuple<int, int, uint32_t>, uint64_t> first_on;
    for (const Note& n : notes)
    {
        plays[size_t(n.channel)] = true;
        uint64_t& on = first_on.try_emplace(std::make_tuple(n.channel, n.key, n.frame), n.on).first->second;
        on = std::min(on, n.on);
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

// Returns pressure 0 at the start for each key that `notes` play before `pressures` give the key a pressure. A player
// that keeps key pressures from one file to the next would otherwise play the file with the pressures that the file
// before left. With a SoundFont that mutes notes, a key left at 127 would mute the file's notes on it.
std::vector<MuteChange> StartPressures(const std::vector<Note>& notes, const std::vector<MuteChange>& pressures)
{
    // Each key's first note on.
    std::map<std::pair<int, int>, uint64_t> first_on;
    for (const Note& n : notes)
    {
        uint64_t& on = first_on.try_emplace({n.channel, n.key}, n.on).first->second;
        on = std::min(on, n.on);
    }

    // A pressure at or before a key's first note on comes before that note. Such a key gets no pressure at the start.
    for (const MuteChange& m : pressures)
    {
        const auto found = first_on.find({m.channel, m.key});
        if (found != first_on.end() && m.tick <= found->second)
        {
            first_on.erase(found);
        }
    }

    std::vector<MuteChange> start;
    for (const auto& [key, on] : first_on)
    {
        start.push_back({0, key.first, key.second, false, 0});
    }

    return start;
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

// Fills in a report's length, loop and tempo.
void Summarize(const Simulation& sim, const SongPlan& plan, uint32_t per_quarter, SongReport& report)
{
    report.seconds = TickSeconds(sim.actions, per_quarter, plan.end);
    if (plan.loops)
    {
        report.loop_start = TickSeconds(sim.actions, per_quarter, plan.loop_start);
        report.loop_end = TickSeconds(sim.actions, per_quarter, plan.loop_end);
    }

    for (const Action& a : sim.actions)
    {
        if (a.kind == Action::kTempo && a.value)
        {
            report.bpm = 60000000.0 / (a.value * kTempoScale);
            break;
        }
    }
}

// Returns the General MIDI 2 Controller Destination Setting message (MMA CA-022) for MIDI channel `ch`: a universal
// real time message (7F) to every device (7F), of the Controller Destination Setting (09) for polyphonic key pressure
// (02), on the channel, that sets amplitude control (02) to a range of 0. A key's notes then play at their full level
// at pressure 0 and fall silent at 127.
std::vector<uint8_t> KeyPressureMuteMessage(int ch)
{
    return {0xF0, 0x7F, 0x7F, 0x09, 0x02, uint8_t(ch), 0x02, 0x00, 0xF7};
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

// The programs whose presets AddPresets() couldn't add as the notes play them.
struct PresetProblems
{
    std::set<int> missing; // the programs whose instruments have nothing to play
    std::set<int> clashes; // the drum channel's programs that another set of instruments has in the drum bank
};

// Adds a preset for each program the notes play, in each note's bank. A note on the drum channel without envelope
// settings gets a copy in the drum bank too. In a shared SoundFont, another set of instruments can have the program
// there already.
PresetProblems AddPresets(SoundfontBuilder& sf, const std::vector<Note>& notes)
{
    PresetProblems problems;
    for (const Note& n : notes)
    {
        const int instrument = sf.InstrumentFor(n.instrument, n.envelope);
        if (instrument < 0)
        {
            problems.missing.insert(n.program);
            continue;
        }

        sf.AddPreset(n.bank, n.program, instrument);
        if (n.channel == kDrumChannel && n.envelope.None())
        {
            const sf2::Preset* preset = sf.FindPreset(kDrumBank, n.program);
            const int drum = preset ? PresetInstrument(*preset) : -1;
            if (drum < 0)
            {
                sf.AddPreset(kDrumBank, n.program, instrument);
            }
            else if (drum != instrument)
            {
                problems.clashes.insert(n.program);
            }
        }
    }

    return problems;
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

// Writes `programs`. `track` holds the MIDI file's track for each of the tune's tracks. Each channel starts in bank
// `bank`.
void WritePrograms(const std::vector<ProgramChange>& programs, const std::vector<MidiTrack*>& track, int bank)
{
    // A program change gives its bank first where the channel's bank changes, to that of the presets for the envelope
    // settings that the notes after it play with, or back to the tune's.
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
}

// Writes the pitch bends of each channel that has a track in `home` and a bend range in `range`.
void WriteBends(const Simulation& sim, const SongPlan& plan, const std::array<MidiTrack*, kChannels>& home,
                const std::array<int, kChannels>& range)
{
    // A player that jumps back to the loop's start keeps the bends that the loop's end left. The game keeps them too,
    // until the loop's first bend or program change on the channel sets its pitch. So each channel's bend is written
    // again at the loop's start. That's left out where the bend changes there or before, or where the loop starts with
    // the tune.
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
                                               FixedBendValue(p.offset, kSemitone, range[size_t(p.channel)]));
        }
    }

    for (int ch = 0; ch < kChannels; ch++)
    {
        if (plan.loops && plan.loop_start > 0 && loop_set[size_t(ch)] < plan.end && home[size_t(ch)] &&
            range[size_t(ch)] > 0)
        {
            home[size_t(ch)]->PitchBend(uint32_t(loop_set[size_t(ch)]), ch,
                                        FixedBendValue(loop_offset[size_t(ch)], kSemitone, range[size_t(ch)]));
        }
    }
}

// Writes the MIDI file: a conductor track, then a track for each of the tune's tracks that plays notes.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const TuneHeader& header,
               const Simulation& sim, const SongPlan& plan, const std::vector<Note>& notes,
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
                mt->Control(uint32_t(a.tick), a.channel, cc::kVolume, SquareRootValue(a.a, 127, 0));
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

    std::vector<Note> ordered = notes;
    MatchPrograms(ordered, programs);
    WritePrograms(programs, track, bank);
    WriteBends(sim, plan, home, range);

    // A player that ignores the SoundFont's mute modulator can follow the General MIDI 2 Controller Destination Setting
    // message instead. Each channel that mutes a key gets the message at the start, before the channel's key pressures.
    std::array<bool, kChannels> mutes = {};
    for (const MuteChange& m : pressures)
    {
        mutes[size_t(m.channel)] = mutes[size_t(m.channel)] || m.muted;
    }
    for (int ch = 0; ch < kChannels; ch++)
    {
        if (mutes[size_t(ch)])
        {
            home[size_t(ch)]->SysEx(0, KeyPressureMuteMessage(ch));
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
        mt.NoteOn(uint32_t(n.on), n.channel, n.key, SquareRootValue(n.velocity, 127, 1), n.before_programs);
        mt.NoteOff(uint32_t(n.off), n.channel, n.key);
    }

    return WriteFile(path, midi.Encode(), error);
}

// Returns the number of notes that the driver drops before the plan's end, in the tracks of `track_mask`.
int DroppedNotes(const Simulation& sim, const SongPlan& plan, uint32_t track_mask)
{
    int dropped = 0;
    for (const Action& a : sim.actions)
    {
        dropped += a.kind == Action::kNoteDropped && a.tick < plan.end && (track_mask >> a.track & 1) ? 1 : 0;
    }

    return dropped;
}

// Writes the files of tune `song` after Run() has converted it: the MIDI file, and the SoundFont unless the tune uses
// `shared`. With `shared`, the tune's presets go in bank `bank` there. Adds the warnings to `report`. Sets `report.ok`
// once the files are written.
void WriteTune(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, int bank, const TuneHeader& header, const SongPlan& plan,
               const Simulation& sim, std::vector<Note>& notes, SongReport& report)
{
    // The SoundFont's presets, in the song's file or the shared one. Those of notes that play with envelope settings go
    // in banks of their own.
    SoundfontBuilder own(rom, info);
    SoundfontBuilder& sf = shared ? *shared : own;
    for (Note& n : notes)
    {
        n.bank = sf.BankFor(bank, n.envelope);
    }

    // The SoundFont mutes a note while its key's pressure is high, in a tune that the mixer leaves voices out of.
    std::vector<MuteChange> pressures = KeyPressures(sim, plan, notes);
    const bool mutes = std::any_of(pressures.begin(), pressures.end(), [](const MuteChange& m) { return m.muted; });
    if (mutes)
    {
        sf.AddMutes();
    }

    // A file that a SoundFont with mutes plays starts with pressure 0 for each key that plays before the tune gives it
    // a pressure. The shared SoundFont can gain the mutes from a later tune. Every file that shares it therefore starts
    // that way too. These pressures come before the tune's pressures at tick 0.
    if (mutes || shared)
    {
        const std::vector<MuteChange> start = StartPressures(notes, pressures);
        pressures.insert(pressures.begin(), start.begin(), start.end());
    }

    const PresetProblems problems = AddPresets(sf, notes);
    for (int program : problems.missing)
    {
        report.warnings.push_back("program " + std::to_string(program) + "'s instrument has no samples to play");
    }
    for (int program : problems.clashes)
    {
        report.warnings.push_back("channel 10's program " + std::to_string(program) +
                                  " is another set of instruments' in the drum bank, which some players use there");
    }

    // The tune's number has two digits, or more from 100 on, as Music::FileNumber() gives it.
    const std::string number = SongNumber(song, 0);
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) tune %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(header.address), kProgramName);
    const std::string stem = SongStem(settings, number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, header, sim, plan, notes, pressures, BendRanges(rom, sim), bank,
                   error))
    {
        report.warnings.push_back(error);
        return;
    }

    if (!shared)
    {
        Sf2File& file = sf.File();
        file.SetTitle(title);
        report.sf2_path = stem + ".sf2";
        if (!file.Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return;
        }
    }

    report.ok = true;
}

// Converts a tune, writing its files if `write` is set. With `shared`, the tune's presets go in bank `bank` there.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, int bank, bool write)
{
    SongReport report;
    TuneHeader header;
    if (!ReadTuneHeader(rom, TuneAddress(rom, info, song), header))
    {
        report.warnings.push_back("its header can't be read");
        return report;
    }

    SongPlan plan = PlanTune(rom, info, header, settings.loops);
    const Simulation sim = Simulate(rom, info, song, header, plan, settings.frame_timing);

    // A tune that the model gave up on ends where it stopped.
    if (sim.cut_off)
    {
        plan.end = std::min(plan.end, *sim.cut_off);
    }

    Summarize(sim, plan, header.ticks_per_quarter, report);

    // With frame timing, the loop's start and end go at the start of the frames that play them, as their events do, so
    // that each pass's first events fall on the same side of the marker. The end goes at the start of the first frame
    // that reaches it, after the frame that plays it.
    if (settings.frame_timing)
    {
        plan.loop_start = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_start);
        plan.loop_end = PlayingFrame(sim.actions, header.ticks_per_quarter, sim.frame_starts, plan.loop_end);
        plan.end = OnFrame(sim.frame_starts, plan.end);
    }

    report.warnings = sim.warnings;
    std::vector<Note> notes =
        NoteCollector(sim, plan, settings.track_mask, kChannels * info.slots_per_channel).Collect();

    std::set<int> tracks;
    for (const Note& n : notes)
    {
        tracks.insert(n.track);
    }

    const int dropped = DroppedNotes(sim, plan, settings.track_mask);
    if (dropped)
    {
        report.warnings.push_back(
            std::to_string(dropped) + " note" + (dropped == 1 ? "" : "s") +
            " found no available slot on their channel; omitted from the MIDI file to match the game");
    }

    report.tracks = int(tracks.size());
    report.silent = notes.empty();
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    WriteTune(rom, info, song, settings, shared, bank, header, plan, sim, notes, report);

    return report;
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, 0, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared, int bank)
{
    return Run(rom, info, song, settings, shared, bank, true);
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

            const std::string bytes = HexBytes(rom, address, e.size);

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
            if (IsDelay(e.command))
            {
                tick += e.value;
            }
            if (e.command == kCmdEnd || loop_end)
            {
                break;
            }
        }
    }

    return WriteTextFile(path, text, error);
}

} // namespace agbsf2::rare
