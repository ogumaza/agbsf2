// SPDX-License-Identifier: MIT

#include "brownie/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"

#include "beat_grid.h"
#include "brownie/sequencer.h"
#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "sf2.h"

namespace agbsf2::brownie
{
namespace
{

// The MIDI file's ticks in a quarter note: several to a frame at the tempos that songs have. The driver plays
// everything at the start of a frame, but square 1's frequency sweep changes the pitch about twice a frame.
constexpr uint16_t kQuarterTicks = 480;

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

constexpr int kVelocity = 127; // every note's velocity, as CC11 carries the loudness

// Each channel's MIDI track name, and those of the Magical Vacation revision's sample channels.
constexpr const char* kChannelNames[kChannelCount] = {
    "Square 1",     "Square 2",  "Wave",      "Noise",     "Effect square 1", "Effect square 2", "Effect wave",
    "Effect noise", "Samples 1", "Samples 2", "Samples 3", "Samples 4",       "Samples 5"};
constexpr const char* kVacationSampleNames[4] = {"Samples 1", "Samples 2", "Effect samples 1", "Effect samples 2"};

// Square 1, the PSG channel with the frequency sweep.
constexpr int kSquare1 = 0;

// The noise kits' first key, and the drums in each kit.
constexpr int kFirstDrumKey = 36;
constexpr int kKitDrums = 92;

// The widest pitch bend range the MIDI files set, in semitones.
constexpr int kMaxBendRange = 96;

// The keys of each octave of a sample set: the mixer plays the first 12 keys' samples at an eighth of its rate, the
// next 12 at a quarter and the 12 after them at half, and the rest at its rate.
constexpr int kOctaveKeys = 12;
constexpr int kSampleOctaves = 4;

// Returns the seconds that `frames` frames take.
double FrameSeconds(double frames)
{
    return frames * double(kFrameCycles) / double(kSecondCycles);
}

// Returns the MIDI channel of driver channel `c`: the channels in order, without the drum channel.
int MidiChannel(int c)
{
    return c < 9 ? c : c + 1;
}

// The state the model leaves after a frame, for the conversion.
struct FrameData
{
    // Returns the sound register at `address`, a halfword.
    uint16_t Register(uint32_t address) const
    {
        const size_t at = address - 0x04000060;
        return uint16_t(registers[at] | (registers[at + 1] << 8));
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

// The stretch of a song that the conversion covers, in frames.
struct Plan
{
    bool loops = false;
    uint32_t loop_start = 0; // the latest of the looping channels' loop points and the other channels' ends
    uint32_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the channels' loops
    uint32_t end = 0;
};

// The model's output for the stretch of a song that the plan covers.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<Event> events;
    Plan plan;
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

// Runs the model until every channel of the song has been through its loop twice or ended, and the song has played its
// loop `loops` times.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    std::vector<SongChannel> list;
    ReadSong(rom, info, song, list);

    // Each channel's loop point and the ends of its passes through its loop, and the frame it ended in, if it did. A
    // channel ends at FF, or when E2 turns it off.
    std::array<int64_t, kChannelCount> loop_point, end;
    loop_point.fill(-1);
    end.fill(-1);
    std::array<std::vector<uint32_t>, kChannelCount> passes;
    std::array<bool, kChannelCount> in_song = {};
    for (const SongChannel& c : list)
    {
        in_song[size_t(c.channel)] = true;
    }

    // Each channel's events other than its loops, and whether its first two passes through its loop have been compared.
    std::array<std::vector<Played>, kChannelCount> played;
    std::array<bool, kChannelCount> checked = {};

    int loop_channel = -1;
    size_t times = 1; // the passes the loop channel makes through its loop in each of the song's
    bool planned = false;
    bool finished = false; // the song played what the plan requires
    uint32_t run_on = 0;   // the frames the model runs on for after the loop channel's last pass
    std::array<uint8_t, kChannelCount> length_sets = {};
    for (uint32_t f = 0; f < kMaxFrames && (f == 0 || seq.Playing()); f++)
    {
        seq.Step();
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
            if (seq.LengthSet(c) != length_sets[size_t(c)])
            {
                length_sets[size_t(c)] = seq.LengthSet(c);
                sim.hints.push_back(f);
            }
            if (in_song[size_t(c)] && end[size_t(c)] < 0 && f > 0 && !(d.flags[size_t(c)] & kFlagOn))
            {
                end[size_t(c)] = f;
            }
        }
        sim.frames.push_back(d);

        for (const Event& e : seq.Events())
        {
            sim.events.push_back(e);
            if (e.kind != Event::kLoop)
            {
                played[e.channel].push_back({e.frame, e.kind, e.key, SettingsOf(seq.GetChannel(e.channel), e.channel)});
                continue;
            }

            // Each pass through a loop starts again on the beat, whatever the tempo was at its end.
            sim.hints.push_back(f);
            std::vector<uint32_t>& p = passes[e.channel];
            if (p.empty())
            {
                loop_point[e.channel] = e.first;
            }
            p.push_back(f);

            // A channel comes back to the place it loops from with the settings that the end of its loop leaves, which
            // can differ from those it first read the place with. Its first pass then plays differently from the
            // others, which play the same, so its loop starts with its second pass.
            if (p.size() == 2 && !checked[e.channel])
            {
                checked[e.channel] = true;
                const uint32_t first = uint32_t(loop_point[e.channel]);
                if (!SamePass(played[e.channel], first, p[0], p[0] - first))
                {
                    loop_point[e.channel] = p[0];
                    p.erase(p.begin());
                }
            }
        }

        // Once every channel has been through its loop twice, which shows where its loop starts, or has ended, the loop
        // is known. The song then continues for the required number of passes. Channels may have different loop points
        // and lengths. The overall loop starts once all looping channels have entered their loops and the others have
        // ended. LoopLength() finds how long it takes them to return to their loop starts, subject to its length limit:
        // `times` passes of the longest channel loop.
        if (!planned)
        {
            planned = true;
            loop_channel = -1;
            uint64_t longest = 0;
            std::vector<uint64_t> lengths;
            for (size_t c = 0; c < kChannelCount; c++)
            {
                planned = planned && (!in_song[c] || checked[c] || end[c] >= 0);
                const uint64_t length = passes[c].empty() ? 0 : passes[c][0] - uint64_t(loop_point[c]);
                if (length > 0)
                {
                    lengths.push_back(length);
                }
                if (length > longest)
                {
                    longest = length;
                    loop_channel = int(c);
                }
            }

            // The song runs on as much longer as the loop starts after the loop channel's loop point.
            if (planned && loop_channel >= 0)
            {
                times = size_t(LoopLength(lengths) / longest);
                uint64_t latest = uint64_t(loop_point[size_t(loop_channel)]);
                for (size_t c = 0; c < kChannelCount; c++)
                {
                    latest = std::max<uint64_t>(latest, !passes[c].empty() ? uint64_t(loop_point[c])
                                                        : end[c] >= 0      ? uint64_t(end[c])
                                                                           : 0);
                }
                run_on = uint32_t(latest - uint64_t(loop_point[size_t(loop_channel)]));
            }
        }
        if (planned && loop_channel >= 0 && passes[size_t(loop_channel)].size() >= size_t(loops) * times &&
            f >= passes[size_t(loop_channel)][size_t(loops) * times - 1] + run_on)
        {
            finished = true;
            break;
        }
    }

    // The loop starts where every looping channel has started its loop and every other channel has ended, and ends as
    // far after the end of the loop channel's `times`th pass. The song ends as far after its last pass, or where the
    // last channel ends if the song doesn't loop. A song that the model gave up on ends where it stopped.
    Plan& plan = sim.plan;
    const uint32_t frames = uint32_t(sim.frames.size());
    if (loop_channel >= 0)
    {
        const std::vector<uint32_t>& p = passes[size_t(loop_channel)];
        const uint32_t first = uint32_t(loop_point[size_t(loop_channel)]);
        plan.loops = true;
        plan.loop_start = first + run_on;
        plan.loop_end = p.size() >= times ? p[times - 1] + run_on : plan.loop_start + uint32_t(times) * (p[0] - first);
        const size_t last = size_t(loops) * times;
        plan.end = p.size() >= last ? std::min(p[last - 1] + run_on, frames) : frames;
    }
    else
    {
        for (size_t c = 0; c < kChannelCount; c++)
        {
            plan.end = std::max(plan.end, end[c] >= 0 ? uint32_t(end[c]) : 0u);
        }
    }

    if (plan.end == 0 || !planned)
    {
        plan.end = frames;
    }

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

// The PSG's level on each side: the master volume and the PSG's share of the mix.
struct PsgScale
{
    double left = 1, right = 1;
};

PsgScale PsgScaleOf(const FrameData& d)
{
    static constexpr double kMix[4] = {0.25, 0.5, 1.0, 1.0};
    const double mix = kMix[d.Register(0x04000082) & 3];
    const int nr50 = d.Register(0x04000080) & 0xFF;
    return {mix * ((nr50 >> 4 & 7) + 1) / 8.0, mix * ((nr50 & 7) + 1) / 8.0};
}

// A model of a PSG channel's on state, volume, length counter and frequency sweep, from frame to frame.
class PsgModel
{
public:
    explicit PsgModel(int channel) : channel_(channel)
    {
    }

    // Takes in the registers after a frame. Returns the channel's volume at the frame's start (0-15), or -1 if it's
    // off.
    int Frame(const FrameData& d)
    {
        // NR10 sets the sweep as it goes.
        const uint16_t nr10 = d.Register(0x04000060);
        sweep_time_ = (nr10 >> 4) & 7;
        sweep_shift_ = nr10 & 7;
        sweep_down_ = (nr10 & 8) != 0;

        // A restart starts the channel again, and a frequency written without one plays until the sweep's next step.
        const uint16_t envelope = d.Register(kEnvelope[channel_]);
        const uint16_t control = d.Register(kControl[channel_]);
        if (d.restarted & (1 << channel_))
        {
            Restart(envelope, control);
        }
        else if ((control & 0x7FF) != written_)
        {
            frequency_ = control & 0x7FF;
        }
        written_ = control & 0x7FF;

        // A channel whose DAC is off is off at once.
        if (channel_ == kWaveChannel ? !(d.Register(0x04000070) & 0x80) : !(envelope & 0xF800))
        {
            on_ = false;
        }

        const int volume = on_ ? volume_ : -1;
        start_frequency_ = frequency_;

        // Move the frame sequencer on by a frame: the length counter at 256 Hz, the envelope at 64 Hz and the sweep at
        // 128 Hz. Each change the sweep makes is kept with how far into the frame it comes.
        constexpr uint64_t kStepCycles = kSecondCycles / 512;
        const uint64_t before = cycles_;
        sweeps_.clear();
        cycles_ += kFrameCycles;
        for (uint64_t k = 1; cycles_ >= kStepCycles; k++)
        {
            cycles_ -= kStepCycles;
            const int was = frequency_;
            Tick();
            if (on_ && frequency_ != was)
            {
                sweeps_.emplace_back(double(k * kStepCycles - before) / double(kFrameCycles), frequency_);
            }
        }

        return volume;
    }

    // Returns the frequency setting the channel plays at the start of the last frame.
    int Frequency() const
    {
        return start_frequency_;
    }

    // Returns the changes the sweep made to the frequency in the last frame, as (position in the frame, frequency).
    const std::vector<std::pair<double, int>>& Sweeps() const
    {
        return sweeps_;
    }

private:
    static constexpr uint32_t kEnvelope[4] = {0x04000062, 0x04000068, 0x04000072, 0x04000078}; // NRx1 and NRx2
    static constexpr uint32_t kControl[4] = {0x04000064, 0x0400006C, 0x04000074, 0x0400007C};  // NRx3 and NRx4

    void Restart(uint16_t envelope, uint16_t control)
    {
        on_ = true;
        volume_ = channel_ == kWaveChannel ? 15 : envelope >> 12;
        up_ = (envelope & 0x0800) != 0;
        step_ = channel_ == kWaveChannel ? 0 : (envelope >> 8) & 7;
        envelope_ticks_ = 0;
        length_on_ = (control & 0x4000) != 0;
        length_ = channel_ == kWaveChannel ? 256 - (envelope & 0xFF) : 64 - (envelope & 63);
        frequency_ = control & 0x7FF;

        // The sweep starts from the new frequency, and with a shift, a sweep up that would at once go past the top
        // turns the channel off.
        shadow_ = frequency_;
        sweeping_ = channel_ == kSquare1 && (sweep_time_ || sweep_shift_);
        sweep_ticks_ = 0;
        if (sweeping_ && sweep_shift_ && SweepNext() > 0x7FF)
        {
            on_ = false;
        }
    }

    // Returns the frequency that the sweep's next step works out from its copy of the frequency.
    int SweepNext() const
    {
        const int change = shadow_ >> sweep_shift_;
        return sweep_down_ ? shadow_ - change : shadow_ + change;
    }

    // Runs one step of the 512 Hz frame sequencer.
    void Tick()
    {
        sequencer_step_ = (sequencer_step_ + 1) & 7;
        if (sequencer_step_ % 2 == 0 && length_on_ && on_ && --length_ <= 0)
        {
            on_ = false;
        }

        if (sequencer_step_ == 7 && step_ && ++envelope_ticks_ >= step_)
        {
            envelope_ticks_ = 0;
            volume_ = up_ ? std::min(volume_ + 1, 15) : std::max(volume_ - 1, 0);
        }

        // A sweep step past the top turns the channel off; otherwise, with a shift, the new frequency plays, and the
        // step after it is checked at once.
        if ((sequencer_step_ == 2 || sequencer_step_ == 6) && sweeping_ && sweep_time_ && ++sweep_ticks_ >= sweep_time_)
        {
            sweep_ticks_ = 0;
            const int next = SweepNext();
            if (next > 0x7FF)
            {
                on_ = false;
            }
            else if (sweep_shift_)
            {
                shadow_ = next;
                frequency_ = next;
                on_ = on_ && SweepNext() <= 0x7FF;
            }
        }
    }

    int channel_;
    bool on_ = false;
    int volume_ = 0;
    bool up_ = false;
    int step_ = 0;
    int envelope_ticks_ = 0;
    bool length_on_ = false;
    int length_ = 0;
    int frequency_ = 0; // the frequency setting that plays
    int written_ = 0;   // the frequency setting last written
    int shadow_ = 0;    // the sweep's copy of the frequency setting
    bool sweeping_ = false;
    int sweep_time_ = 0;
    int sweep_shift_ = 0;
    bool sweep_down_ = false;
    int sweep_ticks_ = 0;
    uint64_t cycles_ = 0;
    int sequencer_step_ = 0;
    int start_frequency_ = 0;
    std::vector<std::pair<double, int>> sweeps_;
};

// Returns the wave shape that the wave channel plays after a frame. The driver writes the wave RAM's bank 0 and plays
// it, in the 32-step mode.
WaveShape WaveShapeOf(const FrameData& d)
{
    WaveShape shape;
    for (size_t i = 0x30; i < 0x40; i++)
    {
        shape.steps.push_back(uint8_t(d.registers[i] >> 4));
        shape.steps.push_back(uint8_t(d.registers[i] & 15));
    }

    return shape;
}

// Returns the octave of a sample note's key, 0-3, which the mixer plays at an eighth of its rate up to its full rate.
int SampleOctave(int key)
{
    return std::min(key / kOctaveKeys, kSampleOctaves - 1);
}

// Returns the key that a sample note plays: keys from 64 play key 47.
int SampleKey(int key)
{
    return (key & 0xC0) ? 47 : key;
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

        std::vector<Note> notes;
        const bool psg = c < kFirstSampleChannel;
        PsgModel model(c & 3);
        uint32_t fed = 0; // the frames the PSG model has taken in

        auto feed = [&](uint32_t until)
        {
            for (; psg && fed < until && fed < sim_.frames.size(); fed++)
            {
                model.Frame(sim_.frames[fed]);
            }
        };

        const uint32_t end = sim_.plan.end;
        for (size_t i = 0; i < starts.size(); i++)
        {
            const Event& e = sim_.events[starts[i]];
            const uint32_t first = e.frame;
            const uint32_t last =
                i + 1 < starts.size() ? sim_.events[starts[i + 1]].frame : uint32_t(sim_.frames.size());
            const uint32_t stop = std::min(last, end);
            feed(first);
            if (e.kind != Event::kNote || first >= stop)
            {
                continue;
            }

            // The key, and for a PSG note the frequency setting it stands for, or for a sample note in the Magical
            // Vacation revision the timer reload value, without the detune.
            int base = 0;
            const bool fifo = !psg && info_.revision == Revision::kMagicalVacation;
            int key = base_ + (psg && (c & 3) == kNoiseChannel ? 0 : psg || fifo ? e.key : SampleKey(e.key));
            if (psg && (c & 3) != kNoiseChannel)
            {
                base = rom_.U16(info_.frequency_table + 2 * uint32_t(e.key)) & 0x7FF;
            }
            else if (fifo)
            {
                base = rom_.U16(info_.rate_table + 2 * uint32_t(kRateSteps * e.key));
            }

            // The stretch's frames, split into the runs in which the channel sounds.
            Note note;
            bool open = false;
            for (uint32_t f = first; f < last && f < sim_.frames.size(); f++)
            {
                const FrameData& d = sim_.frames[f];
                Sound s = psg ? PsgSound(c, model, d, base) : fifo ? FifoSound(c, d, base) : SampleSound(c, d);
                s.frame = f;
                fed = psg ? f + 1 : fed;
                if (!s.sounding || f >= stop)
                {
                    if (open)
                    {
                        note.off = f;
                        Finish(c, e, note, notes);
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
                AddNoiseFrame(c, d, note.noise);
            }

            if (open)
            {
                note.off = std::min(last, end);
                Finish(c, e, note, notes);
            }
        }

        return notes;
    }

private:
    // Returns a PSG channel's sound in a frame. A music channel is silent while a sound effect has its PSG channel, and
    // the wave channel while NR32 mutes it.
    Sound PsgSound(int c, PsgModel& model, const FrameData& d, int base)
    {
        Sound s;
        const int h = c & 3;
        const int volume = model.Frame(d);
        if (volume < 0 || (c < kFirstEffectChannel && (d.flags[size_t(c)] & kFlagEffect)))
        {
            return s;
        }

        const PsgScale scale = PsgScaleOf(d);
        const int nr51 = d.Register(0x04000080) >> 8;
        double level = 0;
        switch (h)
        {
        case kWaveChannel:
            {
                // NR32 scales the wave: 100%, 50% or 25%, or 75% with bit 15. At 0%, which ED's rest and some envelope
                // steps leave, the wave is silent, whatever frequency the rest writes.
                static constexpr double kCodes[4] = {0, 1, 0.5, 0.25};
                const uint16_t nr32 = d.Register(0x04000072);
                if (!(nr32 & 0xE000))
                {
                    return s;
                }

                level = 2 * kPsgLevelPerVolume * 15 * ((nr32 & 0x8000) ? 0.75 : kCodes[(nr32 >> 13) & 3]);
                s.shape = instruments_.Shape(WaveShapeOf(d));
                break;
            }

        case kNoiseChannel:
            // The drum's sample has the noise channel's volume in it.
            level = kPsgLevelPerVolume * 15;
            break;

        default:
            level = kPsgLevelPerVolume * volume;
            s.shape = (d.Register(h == kSquare1 ? 0x04000062 : 0x04000068) >> 6) & 3;
            break;
        }

        s.sounding = true;
        s.left = (nr51 & (0x10 << h)) ? level * scale.left : 0;
        s.right = (nr51 & (0x01 << h)) ? level * scale.right : 0;

        if (h != kNoiseChannel && base)
        {
            s.bend = Bend(base, model.Frequency());
            for (const auto& [at, frequency] : model.Sweeps())
            {
                s.sweeps.push_back({at, Bend(base, frequency)});
            }
        }

        return s;
    }

    // Returns the semitones from frequency setting `base` up to `frequency`.
    static double Bend(int base, int frequency)
    {
        return 12 * std::log2(double(2048 - base) / double(2048 - std::min(frequency, 2047)));
    }

    // Returns a sample channel's sound in a frame: its voice's levels, while the mixer mixes it and it hasn't reached
    // the end of a sample that doesn't loop.
    static Sound SampleSound(int c, const FrameData& d)
    {
        Sound s;
        const int v = c - kFirstSampleChannel;
        const Voice& voice = d.voices[size_t(v)];
        if (!d.mixing || (v >= 2 && !d.all_voices) || (voice.point == voice.end && voice.loop == voice.end))
        {
            return s;
        }

        s.sounding = true;
        s.right = voice.right * kFifoLevel / 256.0;
        s.left = voice.left * kFifoLevel / 256.0;

        return s;
    }

    // Returns a sample channel's sound in a frame in the Magical Vacation revision: the FIFO's volume on both sides,
    // while the FIFO's timer runs and the channel's note started it, and the bend from the rate of `base`, the timer
    // reload value of the note's key, to the timer's.
    static Sound FifoSound(int c, const FrameData& d, int base)
    {
        Sound s;
        const Fifo& f = d.fifos[size_t(c & 1)];
        if (!f.running || f.owner != c)
        {
            return s;
        }

        s.sounding = true;
        s.left = s.right = (d.fifo_voices[size_t(c & 1)].volume + 1.0) * kFifoLevel / 256;
        s.bend = 12 * std::log2(double(0x10000 - base) / double(0x10000 - f.reload));

        return s;
    }

    // Adds a frame of a noise note to its drum: the registers the frame leaves, with a restart at the note's start.
    static void AddNoiseFrame(int c, const FrameData& d, NoiseSound& sound)
    {
        if ((c & 3) != kNoiseChannel || c >= kFirstSampleChannel)
        {
            return;
        }

        const bool restart = sound.frames.empty() || (d.restarted & (1 << kNoiseChannel));
        sound.frames.emplace_back(d.Register(0x04000078),
                                  uint16_t((d.Register(0x0400007C) & 0x7FFF) | (restart ? 0x8000 : 0)));
    }

    // Gives a finished note its instrument and keeps it.
    void Finish(int c, const Event& e, Note& note, std::vector<Note>& notes)
    {
        if (note.sounds.empty() || note.on >= note.off)
        {
            return;
        }

        Instrument inst;
        if (c >= kFirstSampleChannel && info_.revision == Revision::kMagicalVacation)
        {
            // The FIFO's sample, as the note started it, and the root key and cents that play it at the timer's rate
            // for the note's key, without the detune.
            const FifoVoice& v = sim_.frames[e.frame].fifo_voices[size_t(c & 1)];
            const GameSample sample = {v.point - 4, v.end, v.loop};
            const uint16_t reload = rom_.U16(info_.rate_table + 2 * uint32_t(kRateSteps * e.key));
            const double cents = 1200 * std::log2(double(kSecondCycles) / (0x10000 - reload) / info_.mix_rate);
            const int root = note.key - int(std::lround(cents / 100));
            inst.kind = Instrument::kSamples;
            inst.set = sample.start;
            instruments_.AddSampleKey(inst.set, note.key, sample, root,
                                      int(std::lround(cents - 100.0 * (note.key - root))));
        }
        else if (c >= kFirstSampleChannel)
        {
            // The voice's sample, as the note started it, and the key that plays it at the mixer's rate.
            const FrameData& d = sim_.frames[e.frame];
            const Voice& v = d.voices[size_t(c - kFirstSampleChannel)];
            const int key = SampleKey(e.key);
            inst.kind = Instrument::kSamples;
            inst.set = e.samples;
            const int root = note.key + kOctaveKeys * (kSampleOctaves - 1 - SampleOctave(key));
            instruments_.AddSampleKey(e.samples, note.key, {v.point, v.end, v.loop}, root);
        }
        else if ((c & 3) == kNoiseChannel)
        {
            // The drum ends with the last frame that changes the registers; the channel plays on from there by itself.
            std::vector<std::pair<uint16_t, uint16_t>>& frames = note.noise.frames;
            while (frames.size() > 1 &&
                   frames.back() == std::make_pair(frames[frames.size() - 2].first,
                                                   uint16_t(frames[frames.size() - 2].second & 0x7FFF)))
            {
                frames.pop_back();
            }

            const auto [kit, key] = instruments_.Drum(note.noise);
            inst.kind = Instrument::kNoise;
            inst.first = kit;
            note.key = key;
        }
        else
        {
            // The shape at the start, and the first one it switches to.
            inst.kind = (c & 3) == kWaveChannel ? Instrument::kWave : Instrument::kSquare;
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
                note.key += SoundfontBuilder::WaveKeyOffset(instruments_.ShapeAt(inst.first)) - 12;
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

// Returns a zone that plays `sample` on keys `lo` to `hi`, looping it if `loop` is set, tuned by `cents`.
sf2::Zone SampleZone(int sample, bool loop, int lo, int hi, int cents)
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, lo, hi));
    if (cents)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kFineTune, cents));
    }
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, loop ? 1 : 0));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

    return z;
}

// Returns the SF2 time in timecents for `frames` frames.
int FrameTimecents(int frames)
{
    return sf2::ToTimecents(FrameSeconds(frames) * 1000.0);
}

// Returns the zones of an instrument that plays sample `first` for `frames` frames of each note, and then `second`.
std::vector<sf2::Zone> SwitchZones(int first, int frames, int second, int cents)
{
    if (frames == 0)
    {
        return {SampleZone(first, true, 0, 127, cents)};
    }

    // The first sample holds for the frames and then falls silent; the second waits for them.
    const int time = FrameTimecents(frames);
    sf2::Zone a = SampleZone(first, true, 0, 127, cents);
    a.generators.insert(a.generators.begin() + 1, {sf2::Generator::Value(sf2::kHoldVolEnv, time),
                                                   sf2::Generator::Value(sf2::kDecayVolEnv, -12000),
                                                   sf2::Generator::Value(sf2::kSustainVolEnv, 1440)});
    sf2::Zone b = SampleZone(second, true, 0, 127, cents);
    b.generators.insert(b.generators.begin() + 1, sf2::Generator::Value(sf2::kDelayVolEnv, time));

    return {a, b};
}

// Returns the pitch bend value for `bend` semitones with a bend range of `range`.
int BendValue(double bend, int range)
{
    return std::clamp(8192 + int(std::lround(bend / range * 8192)), 0, 16383);
}

// Returns the MIDI tick of a place `quarters` quarter notes into the song.
uint32_t QuarterTick(double quarters)
{
    return uint32_t(std::lround(std::max(0.0, quarters) * kQuarterTicks));
}

// Writes the MIDI file: a conductor track with the tempo map, then a track for each channel that plays notes. Each
// event goes on the song's beat, or with `frame_timing`, on the frame the driver plays it in.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, Revision revision,
               const Simulation& sim, const BeatGrid& grid, bool frame_timing,
               const std::array<std::vector<Note>, kChannelCount>& notes, std::string& error)
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

    const Plan& plan = sim.plan;
    const uint32_t end = song_tick(plan.end);

    // Returns the tick of `frame` on channel `c`, up to the song's end.
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

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the song.
    const uint32_t loop_start = plan.loops ? loop_tick(plan.loop_start) : 0;

    for (int c = 0; c < kChannelCount; c++)
    {
        const std::vector<Note>& list = notes[size_t(c)];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        const int ch = MidiChannel(c);
        const bool vacation = revision == Revision::kMagicalVacation && c >= kFirstSampleChannel;
        mt.Name(vacation ? kVacationSampleNames[c - kFirstSampleChannel] : kChannelNames[c]);
        mt.SetEnd(end);

        // The pitch bend range covers the channel's largest bend.
        double widest = 0;
        for (const Note& n : list)
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
        const int range = widest > 0.005 ? std::clamp(int(std::ceil(widest - 1e-9)), 2, kMaxBendRange) : 0;

        int program = list[0].program;
        mt.Bank(0, ch, program / 128);
        mt.Program(0, ch, program % 128);
        mt.Control(0, ch, cc::kVolume, 127);
        mt.Control(0, ch, cc::kPan, 64);
        mt.Control(0, ch, cc::kExpression, 0);
        mt.Control(0, ch, cc::kReverb, 0);
        mt.Control(0, ch, cc::kChorus, 0);
        if (range)
        {
            mt.BendRange(0, ch, range);
            mt.PitchBend(0, ch, 8192);
        }

        // A player that jumps back to the loop's start keeps the settings that the loop's end left, so they're all
        // written again from the channel's first event there.
        int cc10 = 64, cc11 = 0, bend = 8192;
        bool before_loop = loop_start > 0;
        const auto forget = [&](uint32_t at)
        {
            if (before_loop && at >= loop_start)
            {
                before_loop = false;
                program = -1;
                cc10 = cc11 = bend = -1;
            }
        };

        for (const Note& n : list)
        {
            const uint32_t on = tick(c, n.on);
            const uint32_t off = tick(c, n.off);
            if (on >= off)
            {
                continue;
            }

            forget(on);
            if (n.program != program)
            {
                if (program < 0 || n.program / 128 != program / 128)
                {
                    mt.Bank(on, ch, n.program / 128);
                }

                program = n.program;
                mt.Program(on, ch, program % 128);
            }

            // Each frame's levels and bend.
            for (const Sound& s : n.sounds)
            {
                if (s.frame >= n.off)
                {
                    break;
                }

                const uint32_t at = tick(c, s.frame);
                forget(at);
                int n10 = cc10, n11 = cc11;
                LevelsToControllers(s.left, s.right, n10, n11);
                if (n10 != cc10)
                {
                    mt.Control(at, ch, cc::kPan, cc10 = n10);
                }
                if (n11 != cc11)
                {
                    mt.Control(at, ch, cc::kExpression, cc11 = n11);
                }
                if (!range)
                {
                    continue;
                }

                // The frame's bend, then the sweep's steps within the frame.
                const int v = BendValue(s.bend, range);
                if (v != bend)
                {
                    mt.PitchBend(at, ch, bend = v);
                }
                for (const SweepStep& step : s.sweeps)
                {
                    const uint32_t step_at = tick(c, s.frame + step.at);
                    forget(step_at);
                    const int w = BendValue(step.bend, range);
                    if (s.frame + step.at < n.off && w != bend)
                    {
                        mt.PitchBend(step_at, ch, bend = w);
                    }
                }
            }

            mt.NoteOn(on, ch, std::clamp(n.key, 0, 127), kVelocity);
            mt.NoteOff(off, ch, std::clamp(n.key, 0, 127));
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, writing its files if `write` is set.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, InstrumentSet* shared,
                bool write)
{
    SongSummary sum;
    std::vector<SongChannel> list;
    if (!ReadSong(rom, info, song, list))
    {
        sum.warnings.push_back("its list of channels can't be read");
        return sum;
    }

    const Simulation sim = Simulate(rom, info, song, opt.loops);
    sum.warnings = sim.warnings;

    int base = 0;
    const int cents = PsgTuning(rom, info, base);
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    NoteMaker maker(rom, info, sim, base, instruments);
    std::array<std::vector<Note>, kChannelCount> notes;
    for (int c = 0; c < kChannelCount; c++)
    {
        if (opt.track_mask & (1 << c))
        {
            notes[size_t(c)] = maker.Make(c);
            sum.tracks += notes[size_t(c)].empty() ? 0 : 1;
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
    const std::string number = SongNumber(song, info.song_count);
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) sound %d, channels listed at 0x%08X, converted by %s",
                  rom.Title().c_str(), rom.GameCode().c_str(), song,
                  unsigned(rom.U32(info.song_table + 4 * uint32_t(song))), kProgramName);
    const std::string stem = Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + number));

    std::string error;
    sum.midi_path = stem + ".mid";
    if (!WriteMidi(sum.midi_path, title, about, info.revision, sim, grid, opt.frame_timing, notes, error))
    {
        sum.warnings.push_back(error);
        return sum;
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        SoundfontBuilder sf(rom, uint32_t(info.mix_rate));
        for (const std::string& w : own.Build(sf, cents))
        {
            sum.warnings.push_back(w);
        }

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

int InstrumentSet::Program(const Instrument& inst)
{
    auto it = programs_.find(inst);
    if (it == programs_.end())
    {
        it = programs_.emplace(inst, int(programs_.size())).first;
    }

    return it->second;
}

int InstrumentSet::Shape(const WaveShape& shape)
{
    auto it = shapes_.find(shape);
    if (it != shapes_.end())
    {
        return it->second;
    }

    const int index = int(shape_list_.size());
    shape_list_.push_back(shape);
    shapes_[shape] = index;

    return index;
}

std::pair<int, int> InstrumentSet::Drum(const NoiseSound& sound)
{
    auto it = drums_.find(sound);
    if (it == drums_.end())
    {
        it = drums_.emplace(sound, int(drums_.size())).first;
    }

    return {it->second / kKitDrums, kFirstDrumKey + it->second % kKitDrums};
}

void InstrumentSet::AddSampleKey(uint32_t set, int key, const GameSample& sample, int root, int cents)
{
    sample_keys_[set].emplace(key, SampleKey{sample, root, cents});
}

std::vector<std::string> InstrumentSet::Build(SoundfontBuilder& sf, int cents) const
{
    // The instruments in program order, so that the samples come in the order the songs first play them.
    std::vector<const Instrument*> order(programs_.size());
    for (const auto& [inst, program] : programs_)
    {
        order[size_t(program)] = &inst;
    }

    std::vector<std::string> warnings;
    for (size_t program = 0; program < order.size(); program++)
    {
        const Instrument& inst = *order[program];
        std::vector<sf2::Zone> zones;
        std::string name;
        char text[64];
        switch (inst.kind)
        {
        case Instrument::kSquare:
            {
                // The first sample is made before the second, so that their numbers don't depend on the order in which
                // the compiler evaluates a call's arguments.
                const int first = sf.SquareSample(inst.first);
                const int second = sf.SquareSample(inst.second);
                zones = SwitchZones(first, inst.frames, second, cents);
                name = std::string("Square ") + kDutyNames[inst.first & 3];
                if (inst.frames)
                {
                    name += std::string(" to ") + kDutyNames[inst.second & 3];
                }
                break;
            }

        case Instrument::kWave:
            {
                // As for a square, the first sample is made before the second.
                const int first_sample = sf.WaveSample(ShapeAt(inst.first));
                const int second_sample = sf.WaveSample(ShapeAt(inst.second));
                zones = SwitchZones(first_sample, inst.frames, second_sample, cents);

                // The note's key allows for how often the first shape repeats, so the second shape plays from the
                // first's root key, at the same rate.
                const int first = SoundfontBuilder::WaveKeyOffset(ShapeAt(inst.first));
                const int second = SoundfontBuilder::WaveKeyOffset(ShapeAt(inst.second));
                if (zones.size() == 2 && first != second)
                {
                    zones[1].generators.insert(zones[1].generators.end() - 1,
                                               sf2::Generator::Value(sf2::kOverridingRootKey, 60 + first));
                }

                std::snprintf(text, sizeof text, inst.frames ? "Wave %d to %d" : "Wave %d", inst.first, inst.second);
                name = text;
                break;
            }

        case Instrument::kNoise:
            for (const auto& [sound, index] : drums_)
            {
                if (index / kKitDrums != inst.first)
                {
                    continue;
                }

                const int key = kFirstDrumKey + index % kKitDrums;
                const int sample = sf.NoiseSample(sound);
                sf2::Zone z;
                z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, key, key));
                z.generators.push_back(sf2::Generator::Value(sf2::kOverridingRootKey, key));
                z.generators.push_back(
                    sf2::Generator::Value(sf2::kSampleModes, Looped(sf.File().samples[size_t(sample)]) ? 1 : 0));
                z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));
                zones.push_back(z);
            }
            std::snprintf(text, sizeof text, inst.first ? "Noise drums %d" : "Noise drums", inst.first + 1);
            name = text;
            break;

        default:
            {
                // A zone for each key a song plays, which plays its semitone's sample at the key's octave, or in the
                // Magical Vacation revision the sample at the key's rate.
                const auto it = sample_keys_.find(inst.set);
                if (it != sample_keys_.end())
                {
                    for (const auto& [key, entry] : it->second)
                    {
                        const int index = sf.GameSampleIndex(entry.sample);
                        if (index < 0)
                        {
                            warnings.push_back("a sample note's sample isn't in the ROM");
                            continue;
                        }

                        sf2::Zone z =
                            SampleZone(index, Looped(sf.File().samples[size_t(index)]), key, key, entry.cents);
                        z.generators.insert(z.generators.end() - 1,
                                            sf2::Generator::Value(sf2::kOverridingRootKey, entry.root));
                        zones.push_back(z);
                    }
                }

                std::snprintf(text, sizeof text, "Samples %08X", unsigned(inst.set));
                name = text;
                break;
            }
        }

        sf.AddPreset(name, int(program) / 128, int(program) % 128, sf.AddInstrument(name, std::move(zones)));
    }

    return warnings;
}

SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt)
{
    InstrumentSet instruments;
    return Run(rom, info, song, opt, &instruments, false);
}

SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        InstrumentSet* shared)
{
    return Run(rom, info, song, opt, shared, true);
}

int PsgTuning(const Rom& rom, const DriverInfo& info, int& base)
{
    // Each key's pitch less its number, in cents above MIDI key 0.
    std::vector<int> offsets;
    for (uint32_t k = 0; k < 48; k++)
    {
        const int setting = rom.U16(info.frequency_table + 2 * k);
        if (setting > 0 && setting < 2048)
        {
            const double hz = 131072.0 / (2048 - setting);
            offsets.push_back(int(std::lround(100 * (69 + 12 * std::log2(hz / 440)) - 100.0 * k)));
        }
    }

    if (offsets.empty())
    {
        base = 36;
        return 0;
    }

    std::sort(offsets.begin(), offsets.end());
    const int median = offsets[offsets.size() / 2];
    base = (median + 50) / 100;

    return median - 100 * base;
}

} // namespace agbsf2::brownie
