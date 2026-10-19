// SPDX-License-Identifier: MIT

#include "brownie/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "beat_grid.h"
#include "beat_midi.h"
#include "brownie/sequencer.h"
#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "psg.h"
#include "sf2.h"

namespace agbsf2::brownie
{
namespace
{

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// Returns the MIDI track name of channel `c` of revision `r`: its name with a capital, such as "Effect square 1".
std::string TrackName(Revision r, int c)
{
    std::string name = ChannelName(r, c);
    name[0] = char(name[0] - 'a' + 'A');

    return name;
}

// The state the model leaves after a frame, for the conversion.
struct FrameData
{
    // Returns the sound register at `address`, a halfword.
    uint16_t Register(uint32_t address) const
    {
        return xsflib::LoadLe16(&registers[address - kIoNr10]);
    }

    std::array<uint8_t, 0x40> registers = {}; // the sound registers from SOUND1CNT_L
    uint8_t restarted = 0;                    // bit h: PSG channel h started again in the frame
    std::array<Voice, kVoiceCount> voices = {};
    bool mixing = false;                     // the mixer ran in the frame
    bool all_voices = false;                 // the mixer mixed all 5 voices, not just voices 0 and 1
    std::array<Fifo, kFifoCount> fifos = {}; // the Magical Vacation revision's, at the frame's start
    std::array<FifoVoice, kFifoCount> fifo_voices = {};
    std::array<uint8_t, kChannelCount> flags = {};
};

// The model's output for the stretch of a song that the plan covers.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<Event> events;
    FramePlan plan;
    std::vector<uint32_t> hints; // frames on which the tempo may change: a channel switches lengths, or loops
    std::vector<std::string> warnings;
};

// The settings that shape what a channel's notes play: their volume, detune and envelope, a PSG channel's NRx1 and
// sweep, the sample set or sample, the wave and the pan. A sample channel's record counts its envelope's progress where
// a PSG channel keeps NRx1 and the sweep, so those are left out there.
using Settings = std::array<uint32_t, 10>;

// Returns the settings of channel `c`, whose record is `ch`.
Settings SettingsOf(const Channel& ch, int c)
{
    const bool psg = c < kFirstSampleChannel;
    return {ch.volume,  ch.detune,     ch.envelope, psg ? ch.duty : 0u, psg ? ch.sweep : 0u,
            ch.samples, ch.sample_end, ch.wave,     ch.pan_on,          ch.pan_off};
}

// A channel's event other than a loop, with the channel's settings after it, which a note plays with.
struct Played
{
    uint32_t frame = 0;
    Event::Kind kind = Event::kNote;
    uint8_t key = 0;
    Settings settings = {};
};

// Returns true if a channel's events, `played`, are the same in the `length` frames from frame `a` as in those from
// frame `b`: the same kinds of event on the same frames of each, and notes with the same keys and settings.
bool SamePass(const std::vector<Played>& played, uint32_t a, uint32_t b, uint32_t length)
{
    const auto from = [&](uint32_t frame)
    {
        return std::lower_bound(played.begin(), played.end(), frame,
                                [](const Played& e, uint32_t f) { return e.frame < f; });
    };

    auto i = from(a);
    auto j = from(b);
    const auto i_end = from(a + length);
    const auto j_end = from(b + length);
    for (; i != i_end && j != j_end; i++, j++)
    {
        if (i->frame - a != j->frame - b || i->kind != j->kind ||
            (i->kind == Event::kNote && (i->key != j->key || i->settings != j->settings)))
        {
            return false;
        }
    }

    return i == i_end && j == j_end;
}

// Follows each channel of a song through its loop, and plans the stretch of the song that the conversion covers. A
// channel ends at FF, or when E2 turns it off.
class LoopTracker
{
public:
    // Starts to follow the channels in `list`.
    explicit LoopTracker(const std::vector<SongChannel>& list);

    // Notes the channels that frame `f` ended, from the state `d` that the frame left.
    void AddFrame(uint32_t f, const FrameData& d);

    // Takes in event `e`, with `ch`, the record of its channel after the event's frame.
    void AddEvent(const Event& e, const Channel& ch);

    // Plans the loop once every channel of the song has been through its loop twice or has ended. Until then, each call
    // picks the loop channel again: the channel whose loop is the longest so far. MakePlan() takes the last pick for a
    // song that the model cut off. Does nothing once the loop is planned.
    void PlanLoop();

    // Returns true once the loop is planned and the song has played it `loops` times by frame `f`.
    bool Done(uint32_t f, int loops) const;

    // Returns the plan for a song that the model ran for `frames` frames, with its loop played `loops` times.
    FramePlan MakePlan(uint32_t frames, int loops) const;

private:
    // Returns true if every channel of the song has been through its loop twice or has ended. Two passes show where
    // the loop starts.
    bool Ready() const;

    // Returns the frame that comes as far after the end of the loop channel's `n`th pass as the song's loop starts
    // after the channel's loop point.
    uint32_t PassEnd(size_t n) const;

    std::array<bool, kChannelCount> in_song_ = {};
    std::array<int64_t, kChannelCount> loop_point_;           // each channel's loop point, or -1
    std::array<int64_t, kChannelCount> end_;                  // the frame each channel ended in, or -1
    std::array<std::vector<uint32_t>, kChannelCount> passes_; // the ends of each channel's passes through its loop
    std::array<std::vector<Played>, kChannelCount> played_;   // each channel's events other than its loops
    std::array<bool, kChannelCount> checked_ = {};            // each channel's first two passes have been compared
    bool planned_ = false;
    int loop_channel_ = -1; // the channel with the longest loop, or -1
    size_t times_ = 1;      // the passes the loop channel makes through its loop in each of the song's
    uint32_t run_on_ = 0;   // the frames the model runs on for after the loop channel's last pass
};

LoopTracker::LoopTracker(const std::vector<SongChannel>& list)
{
    loop_point_.fill(-1);
    end_.fill(-1);
    for (const SongChannel& c : list)
    {
        in_song_[size_t(c.channel)] = true;
    }
}

void LoopTracker::AddFrame(uint32_t f, const FrameData& d)
{
    for (size_t c = 0; c < kChannelCount; c++)
    {
        if (in_song_[c] && end_[c] < 0 && f > 0 && !(d.flags[c] & kFlagOn))
        {
            end_[c] = f;
        }
    }
}

void LoopTracker::AddEvent(const Event& e, const Channel& ch)
{
    if (e.kind != Event::kLoop)
    {
        played_[e.channel].push_back({e.frame, e.kind, e.key, SettingsOf(ch, e.channel)});
        return;
    }

    std::vector<uint32_t>& p = passes_[e.channel];
    if (p.empty())
    {
        loop_point_[e.channel] = e.first;
    }
    p.push_back(e.frame);

    // A channel comes back to the place it loops from with the settings that the end of its loop leaves. Those can
    // differ from the settings that it first read the place with. If its first pass then plays differently from its
    // second, its loop starts with its second pass.
    if (p.size() == 2 && !checked_[e.channel])
    {
        checked_[e.channel] = true;
        const uint32_t first = uint32_t(loop_point_[e.channel]);
        if (!SamePass(played_[e.channel], first, p[0], p[0] - first))
        {
            loop_point_[e.channel] = p[0];
            p.erase(p.begin());
        }
    }
}

void LoopTracker::PlanLoop()
{
    if (planned_)
    {
        return;
    }

    // The loop channel is the one whose loop is the longest so far.
    loop_channel_ = -1;
    uint64_t longest = 0;
    std::vector<uint64_t> lengths;
    for (size_t c = 0; c < kChannelCount; c++)
    {
        const uint64_t length = passes_[c].empty() ? 0 : passes_[c][0] - uint64_t(loop_point_[c]);
        if (length > 0)
        {
            lengths.push_back(length);
        }
        if (length > longest)
        {
            longest = length;
            loop_channel_ = int(c);
        }
    }

    // Once the loop is known, the song continues for the required number of passes. Channels may have different loop
    // points and lengths. The overall loop starts once all looping channels have entered their loops and the others
    // have ended. LoopLength() works out how long the channels take to return to their loop starts, subject to its
    // length limit: `times_` passes of the longest channel loop.
    planned_ = Ready();
    if (!planned_ || loop_channel_ < 0)
    {
        return;
    }

    // The song runs on as much longer as the loop starts after the loop channel's loop point.
    times_ = size_t(LoopLength(lengths) / longest);
    uint64_t latest = uint64_t(loop_point_[size_t(loop_channel_)]);
    for (size_t c = 0; c < kChannelCount; c++)
    {
        latest = std::max<uint64_t>(latest, !passes_[c].empty() ? uint64_t(loop_point_[c])
                                            : end_[c] >= 0      ? uint64_t(end_[c])
                                                                : 0);
    }
    run_on_ = uint32_t(latest - uint64_t(loop_point_[size_t(loop_channel_)]));
}

bool LoopTracker::Done(uint32_t f, int loops) const
{
    if (!planned_ || loop_channel_ < 0)
    {
        return false;
    }

    const size_t last = size_t(loops) * times_;
    return passes_[size_t(loop_channel_)].size() >= last && f >= PassEnd(last);
}

FramePlan LoopTracker::MakePlan(uint32_t frames, int loops) const
{
    // The loop starts where every looping channel has started its loop and every other channel has ended. It ends as
    // far after the end of the loop channel's `times_`th pass. The song ends as far after its last pass, or where the
    // last channel ends if the song doesn't loop. A song that the model gave up on ends where it stopped.
    FramePlan plan;
    if (loop_channel_ >= 0)
    {
        const std::vector<uint32_t>& p = passes_[size_t(loop_channel_)];
        const uint32_t first = uint32_t(loop_point_[size_t(loop_channel_)]);
        plan.loops = true;
        plan.loop_start = first + run_on_;
        plan.loop_end = p.size() >= times_ ? PassEnd(times_) : plan.loop_start + uint32_t(times_) * (p[0] - first);
        const size_t last = size_t(loops) * times_;
        plan.end = p.size() >= last ? std::min(PassEnd(last), frames) : frames;
    }
    else
    {
        for (size_t c = 0; c < kChannelCount; c++)
        {
            plan.end = std::max(plan.end, end_[c] >= 0 ? uint32_t(end_[c]) : 0u);
        }
    }

    if (plan.end == 0 || !planned_)
    {
        plan.end = frames;
    }

    return plan;
}

bool LoopTracker::Ready() const
{
    for (size_t c = 0; c < kChannelCount; c++)
    {
        if (in_song_[c] && !checked_[c] && end_[c] < 0)
        {
            return false;
        }
    }

    return true;
}

uint32_t LoopTracker::PassEnd(size_t n) const
{
    return passes_[size_t(loop_channel_)][n - 1] + run_on_;
}

// Returns the state that the model left after a frame.
FrameData CaptureFrame(const Sequencer& seq)
{
    FrameData d;
    d.registers = seq.Registers();
    d.restarted = seq.Restarted();
    d.voices = seq.VoicesBeforeMix();
    d.mixing = seq.Mixing();
    d.all_voices = seq.GetGlobals()[kMixerMode] != 0;
    d.fifos = seq.FifosBeforeInterrupts();
    d.fifo_voices = seq.FifoVoicesBeforeInterrupts();
    for (int c = 0; c < kChannelCount; c++)
    {
        d.flags[size_t(c)] = seq.GetChannel(c).flags;
    }

    return d;
}

// Runs the model until every channel of the song has been through its loop twice or ended, and the song has played its
// loop `loops` times. `list` is the song's list of channels.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, const std::vector<SongChannel>& list, int loops)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    LoopTracker tracker(list);

    bool finished = false; // the song played what the plan requires
    std::array<uint8_t, kChannelCount> length_sets = {};
    for (uint32_t f = 0; f < kMaxFrames && (f == 0 || seq.Playing()); f++)
    {
        seq.Step();
        sim.frames.push_back(CaptureFrame(seq));
        tracker.AddFrame(f, sim.frames.back());

        // A channel that switches lengths may change the tempo.
        for (int c = 0; c < kChannelCount; c++)
        {
            if (seq.LengthSet(c) != length_sets[size_t(c)])
            {
                length_sets[size_t(c)] = seq.LengthSet(c);
                sim.hints.push_back(f);
            }
        }

        // Each pass through a loop starts again on the beat, whatever the tempo was at its end.
        for (const Event& e : seq.Events())
        {
            sim.events.push_back(e);
            if (e.kind == Event::kLoop)
            {
                sim.hints.push_back(f);
            }
            tracker.AddEvent(e, seq.GetChannel(e.channel));
        }

        // The loop is planned after the frame's events. The check for the song's end comes after the plan.
        tracker.PlanLoop();
        if (tracker.Done(f, loops))
        {
            finished = true;
            break;
        }
    }

    const uint32_t frames = uint32_t(sim.frames.size());
    sim.plan = tracker.MakePlan(frames, loops);
    sim.warnings = seq.Warnings();
    if (!finished && seq.Playing() && frames == kMaxFrames)
    {
        sim.warnings.push_back("the song was cut off after an hour");
    }

    return sim;
}

// A change that the frequency sweep makes to a square's pitch within a frame.
struct SweepStep
{
    double at = 0;   // its position in the frame, from 0 to 1
    double bend = 0; // semitones from the note's key
};

// A channel's sound in one frame.
struct Sound
{
    bool sounding = false;
    double left = 0, right = 0; // levels, with a full-scale FIFO sample at 128
    double bend = 0;            // semitones from the note's key
    int shape = -1;             // the duty or the wave shape that the frame plays
    uint32_t frame = 0;         // the frame it's in
    std::vector<SweepStep> sweeps;
};

// A note as the MIDI file plays it.
struct Note
{
    int key = 60;
    int program = 0;
    uint32_t on = 0; // frames
    uint32_t off = 0;
    std::vector<Sound> sounds; // one for each of its frames
    NoiseSound noise;          // a noise note's drum
};

// Returns the registers of PSG channel `h` after frame `d`, for its model.
PsgRegisters PsgRegistersOf(const FrameData& d, int h)
{
    return {d.Register(kIoNr10), d.Register(kIoControlRegisters[h]), d.Register(kIoFrequencyRegisters[h]),
            d.Register(kIoNr30), (d.restarted & (1 << h)) != 0};
}

// Returns the wave shape that the wave channel plays after a frame. The driver writes the wave RAM's bank 0 and plays
// it, in the 32-step mode.
WaveShape WaveShapeOf(const FrameData& d)
{
    WaveShape shape;
    for (size_t i = kIoWaveRam - kIoNr10; i < kIoSoundEnd - kIoNr10; i++)
    {
        shape.steps.push_back(uint8_t(d.registers[i] >> 4));
        shape.steps.push_back(uint8_t(d.registers[i] & 15));
    }

    return shape;
}

// Converts a song's channels into the MIDI file's notes.
class NoteMaker
{
public:
    NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim, int base, InstrumentSet& instruments)
        : rom_(rom), info_(info), sim_(sim), base_(base), instruments_(instruments)
    {
    }

    // Returns channel `c`'s notes, and adds the instruments they play.
    std::vector<Note> Make(int c)
    {
        // Each note starts a stretch of the channel, which lasts until its next note or its end. A rest doesn't end it:
        // ED turns a PSG channel off, or mutes the wave channel, and EC silences a voice, which the channel's sound
        // shows, but a sample channel's ED and a PSG channel's EC leave the channel's sound playing.
        std::vector<size_t> starts;
        for (size_t i = 0; i < sim_.events.size(); i++)
        {
            const Event& e = sim_.events[i];
            if (e.channel == c && (e.kind == Event::kNote || e.kind == Event::kEnd))
            {
                starts.push_back(i);
            }
        }

        // A PSG channel's sound comes from a model of the channel.
        std::vector<Note> notes;
        const ChannelKind kind = KindOf(info_.revision, c);
        std::optional<PsgFeed> psg;
        if (c < kFirstSampleChannel)
        {
            psg.emplace(c & 3, [this, c](uint32_t f) { return PsgRegistersOf(sim_.frames[f], c & 3); });
        }

        const uint32_t end = sim_.plan.end;
        for (size_t i = 0; i < starts.size(); i++)
        {
            const Event& e = sim_.events[starts[i]];
            const uint32_t first = e.frame;
            const uint32_t last =
                i + 1 < starts.size() ? sim_.events[starts[i + 1]].frame : uint32_t(sim_.frames.size());
            const uint32_t stop = std::min(last, end);
            if (e.kind != Event::kNote || first >= stop)
            {
                continue;
            }

            // The key, and for a PSG note the frequency setting it stands for, or for a FIFO's sample note the timer
            // reload value, without the detune.
            int key = base_ + e.key;
            int key_setting = 0;
            switch (kind)
            {
            case ChannelKind::kSquare:
            case ChannelKind::kWave:
                key_setting = rom_.U16(info_.frequency_table + 2 * uint32_t(e.key)) & 0x7FF;
                break;
            case ChannelKind::kNoise:
                key = base_;
                break;
            case ChannelKind::kMixerSample:
                key = base_ + SampleKey(e.key);
                break;
            case ChannelKind::kFifoSample:
                key_setting = rom_.U16(info_.rate_table + 2 * uint32_t(kRateSteps * e.key));
                break;
            }

            // The stretch's frames, split into the runs in which the channel sounds.
            Note note;
            bool open = false;
            for (uint32_t f = first; f < last && f < sim_.frames.size(); f++)
            {
                const FrameData& d = sim_.frames[f];
                Sound s;
                if (psg)
                {
                    s = PsgSound(c, psg->Volume(f), psg->Model(), d, key_setting);
                }
                else
                {
                    s = kind == ChannelKind::kFifoSample ? FifoSound(c, d, key_setting) : SampleSound(c, d);
                }
                s.frame = f;
                if (!s.sounding || f >= stop)
                {
                    if (open)
                    {
                        note.off = f;
                        Finish(c, e, key_setting, note, notes);
                        open = false;
                    }
                    if (f >= stop)
                    {
                        break;
                    }
                    continue;
                }

                if (!open)
                {
                    note = Note();
                    note.key = key;
                    note.on = f;
                    open = true;
                }
                note.sounds.push_back(s);
                if (kind == ChannelKind::kNoise)
                {
                    note.noise.AddFrame(d.Register(kIoControlRegisters[kNoiseChannel]),
                                        d.Register(kIoFrequencyRegisters[kNoiseChannel]),
                                        (d.restarted & (1 << kNoiseChannel)) != 0);
                }
            }

            if (open)
            {
                note.off = std::min(last, end);
                Finish(c, e, key_setting, note, notes);
            }
        }

        return notes;
    }

private:
    // Returns a PSG channel's sound in frame `d`. The PSG model `model` has taken the frame in. `volume` is the volume
    // that its Frame() returned. A music channel is silent while a sound effect has its PSG channel, and the wave
    // channel while NR32 mutes it.
    Sound PsgSound(int c, int volume, const PsgModel& model, const FrameData& d, int key_setting)
    {
        Sound s;
        const int h = c & 3;
        if (volume < 0 || (c < kFirstEffectChannel && (d.flags[size_t(c)] & kFlagEffect)))
        {
            return s;
        }

        const PsgScale scale = PsgScaleOf(d.Register(kIoSoundcntL), d.Register(kIoSoundcntH));
        const int nr51 = d.Register(kIoSoundcntL) >> 8;
        double level = 0;
        switch (h)
        {
        case kWaveChannel:
            {
                // NR32 scales the wave. ED's rest and some envelope steps leave it at 0%. The wave is then silent,
                // whatever frequency the rest writes.
                const double share = WaveShare(d.Register(kIoControlRegisters[kWaveChannel]));
                if (share == 0)
                {
                    return s;
                }

                level = 2 * kPsgLevelPerVolume * 15 * share;
                s.shape = instruments_.Shape(WaveShapeOf(d));
                break;
            }

        case kNoiseChannel:
            // The drum's sample has the noise channel's volume in it.
            level = kPsgLevelPerVolume * 15;
            break;

        default:
            level = kPsgLevelPerVolume * volume;
            s.shape = (d.Register(kIoControlRegisters[h]) >> 6) & 3;
            break;
        }

        s.sounding = true;
        s.left = (nr51 & (0x10 << h)) ? level * scale.left : 0;
        s.right = (nr51 & (0x01 << h)) ? level * scale.right : 0;

        if (h != kNoiseChannel && key_setting)
        {
            s.bend = PsgSemitones(key_setting, model.Frequency());
            for (const auto& [at, frequency] : model.Sweeps())
            {
                s.sweeps.push_back({at, PsgSemitones(key_setting, frequency)});
            }
        }

        return s;
    }

    // Returns a sample channel's sound in a frame: its voice's levels, while the mixer mixes it and it hasn't reached
    // the end of a sample that doesn't loop.
    static Sound SampleSound(int c, const FrameData& d)
    {
        Sound s;
        const int v = c - kFirstSampleChannel;
        const Voice& voice = d.voices[size_t(v)];
        if (!d.mixing || (v >= kMode0Voices && !d.all_voices) || (voice.point == voice.end && voice.loop == voice.end))
        {
            return s;
        }

        s.sounding = true;
        s.right = voice.right * kFifoLevel / 256.0;
        s.left = voice.left * kFifoLevel / 256.0;

        return s;
    }

    // Returns a sample channel's sound in a frame in the Magical Vacation revision: the FIFO's volume on both sides,
    // while the FIFO's timer runs and the channel's note started it, and the bend from the rate of `key_setting`, the
    // timer reload value of the note's key, to the timer's.
    static Sound FifoSound(int c, const FrameData& d, int key_setting)
    {
        Sound s;
        const Fifo& f = d.fifos[size_t(c & 1)];
        if (!f.running || f.owner != c)
        {
            return s;
        }

        s.sounding = true;
        s.left = s.right = (d.fifo_voices[size_t(c & 1)].volume + 1.0) * kFifoLevel / 256;
        s.bend = 12 * std::log2(double(0x10000 - key_setting) / double(0x10000 - f.reload));

        return s;
    }

    // Gives a finished note its instrument and keeps it. A FIFO's sample note plays at `key_setting`, the timer reload
    // value of its key without the detune.
    void Finish(int c, const Event& e, int key_setting, Note& note, std::vector<Note>& notes)
    {
        if (note.sounds.empty() || note.on >= note.off)
        {
            return;
        }

        Instrument inst;
        const ChannelKind kind = KindOf(info_.revision, c);
        switch (kind)
        {
        case ChannelKind::kFifoSample:
            {
                // The FIFO's sample when the note started, and the root key and cents that play the sample at the
                // timer's rate for the note's key without the detune.
                const FifoVoice& v = sim_.frames[e.frame].fifo_voices[size_t(c & 1)];
                const GameSample sample = {v.point - 4, v.end, v.loop};
                const double cents = 1200 * std::log2(double(kSecondCycles) / (0x10000 - key_setting) / info_.mix_rate);
                const int root = note.key - int(std::lround(cents / 100));
                inst.kind = Instrument::kSamples;
                inst.set = sample.start;
                instruments_.AddSampleKey(inst.set, note.key, sample, root,
                                          int(std::lround(cents - 100.0 * (note.key - root))));
                break;
            }

        case ChannelKind::kMixerSample:
            {
                // The voice's sample when the note started, and the key that plays the sample at the mixer's rate.
                const FrameData& d = sim_.frames[e.frame];
                const Voice& v = d.voices[size_t(c - kFirstSampleChannel)];
                const int key = SampleKey(e.key);
                inst.kind = Instrument::kSamples;
                inst.set = e.samples;
                const int root = note.key + kOctaveKeys * (kSampleOctaves - 1 - SampleOctave(key));
                instruments_.AddSampleKey(e.samples, note.key, {v.point, v.end, v.loop}, root);
                break;
            }

        case ChannelKind::kNoise:
            {
                note.noise.Trim();
                const auto [kit, key] = instruments_.Drum(note.noise);
                inst.kind = Instrument::kNoise;
                inst.first = kit;
                note.key = key;
                break;
            }

        case ChannelKind::kSquare:
        case ChannelKind::kWave:
            {
                // The shape at the start, and the first one it switches to.
                inst.kind = kind == ChannelKind::kWave ? Instrument::kWave : Instrument::kSquare;
                inst.first = note.sounds[0].shape;
                inst.second = inst.first;
                for (size_t i = 1; i < note.sounds.size(); i++)
                {
                    if (note.sounds[i].shape != inst.first)
                    {
                        inst.frames = int(i);
                        inst.second = note.sounds[i].shape;
                        break;
                    }
                }

                // A wave pattern of 32 steps plays an octave lower than a square, and one that repeats sounds higher.
                if (inst.kind == Instrument::kWave)
                {
                    note.key += WaveKeyOffset(instruments_.ShapeAt(inst.first)) - 12;
                }
                break;
            }
        }

        note.program = instruments_.Program(inst);
        notes.push_back(note);
    }

    const Rom& rom_;
    const DriverInfo& info_;
    const Simulation& sim_;
    const int base_;
    InstrumentSet& instruments_;
};

// Returns the pitch bend range that covers the largest bend of a channel's notes `notes`, or 0 if they don't bend.
int BendRange(const std::vector<Note>& notes)
{
    double widest = 0;
    for (const Note& n : notes)
    {
        for (const Sound& s : n.sounds)
        {
            widest = std::max(widest, std::fabs(s.bend));
            for (const SweepStep& step : s.sweeps)
            {
                widest = std::max(widest, std::fabs(step.bend));
            }
        }
    }

    return BendRangeFor(widest, kSmallBend);
}

// Writes channel `c`'s notes `notes` on its MIDI channel, with its program and controllers where they change.
// `loop_start` is the tick of the loop's start, or 0. `tick` gives the tick of a frame on the channel.
void WriteChannel(MidiTrack& mt, int c, const std::vector<Note>& notes, uint32_t loop_start,
                  const std::function<uint32_t(double)>& tick)
{
    const int ch = MelodicChannel(c);
    const int range = BendRange(notes);
    ChannelWriter writer(mt, ch, notes[0].program, NumberedPreset(notes[0].program), range, loop_start);
    for (const Note& n : notes)
    {
        const uint32_t on = tick(n.on);
        const uint32_t off = tick(n.off);
        if (on >= off)
        {
            continue;
        }

        writer.SetProgram(on, n.program, NumberedPreset(n.program));

        // Each frame's levels and bend.
        for (const Sound& s : n.sounds)
        {
            if (s.frame >= n.off)
            {
                break;
            }

            const uint32_t at = tick(s.frame);
            writer.SetLevels(at, s.left, s.right);
            if (!range)
            {
                continue;
            }

            // The frame's bend, then the sweep's steps within the frame.
            writer.SetBend(at, s.bend);
            for (const SweepStep& step : s.sweeps)
            {
                const uint32_t step_at = tick(s.frame + step.at);
                writer.Reach(step_at);
                if (s.frame + step.at < n.off)
                {
                    writer.SetBend(step_at, step.bend);
                }
            }
        }

        writer.Note(on, off, std::clamp(n.key, 0, 127));
    }
}

// Writes the MIDI file: a conductor track with the tempo map, then a track for each channel that plays notes. Each
// event goes on the song's beat, or with `frame_timing`, on the frame the driver plays it in.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, Revision revision,
               const Simulation& sim, const BeatGrid& grid, bool frame_timing,
               const std::array<std::vector<Note>, kChannelCount>& notes, std::string& error)
{
    const BeatTicks ticks(grid, frame_timing);
    const FramePlan& plan = sim.plan;
    const uint32_t end = ticks.Song(plan.end);
    MidiFile midi(kBeatQuarterTicks);
    WriteBeatConductor(midi.AddTrack(), title, about, ticks, plan, end);

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the song.
    const uint32_t loop_start = plan.loops ? ticks.Marker(plan.loop_start) : 0;

    for (int c = 0; c < kChannelCount; c++)
    {
        const std::vector<Note>& list = notes[size_t(c)];
        if (list.empty())
        {
            continue;
        }

        // The channel's track. Its events go no later than the song's end.
        const auto tick = [&](double frame)
        {
            return ticks.Channel(c, frame, end);
        };
        MidiTrack& mt = midi.AddTrack();
        mt.Name(TrackName(revision, c));
        mt.SetEnd(end);
        WriteChannel(mt, c, list, loop_start, tick);
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, writing its files if `write` is set.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write)
{
    SongReport report;
    std::vector<SongChannel> list;
    if (!ReadSong(rom, info, song, list))
    {
        report.warnings.push_back("its list of channels can't be read");
        return report;
    }

    const Simulation sim = Simulate(rom, info, song, list, settings.loops);
    report.warnings = sim.warnings;

    const Tuning tuning = PsgTuning(rom, info);
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    NoteMaker maker(rom, info, sim, tuning.base, instruments);
    std::array<std::vector<Note>, kChannelCount> notes;
    for (int c = 0; c < kChannelCount; c++)
    {
        if (settings.track_mask & (1 << c))
        {
            notes[size_t(c)] = maker.Make(c);
            report.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
    }

    // The song's beat, from the frames on which every channel's notes, rests and holds start, whichever channels the
    // conversion includes, so that conversions of different channels line up.
    std::vector<std::vector<uint32_t>> starts(kChannelCount);
    for (const Event& e : sim.events)
    {
        if (e.kind != Event::kLoop && e.frame <= sim.plan.end)
        {
            starts[e.channel].push_back(e.frame);
        }
    }
    const BeatGrid grid(starts, sim.hints);

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
    const std::string number = SongNumber(song, info.song_count);
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) sound %d, channels listed at 0x%08X, converted by %s",
                  rom.Title().c_str(), rom.GameCode().c_str(), song, unsigned(SongAddress(rom, info, song)),
                  kProgramName);
    const std::string stem = SongStem(settings, number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, info.revision, sim, grid, settings.frame_timing, notes, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        SoundfontBuilder sf(rom, uint32_t(info.mix_rate));
        for (const std::string& w : own.Build(sf, tuning.cents))
        {
            report.warnings.push_back(w);
        }

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
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

} // namespace agbsf2::brownie
