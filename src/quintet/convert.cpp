// SPDX-License-Identifier: MIT

#include "quintet/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"

#include "files.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "quintet/sequencer.h"
#include "sf2.h"
#include "song_banks.h"

namespace agbsf2::quintet
{
namespace
{

// The MIDI file's resolution: 37 ticks for each of the driver's, so that a frame at the driver's tempo T is exactly T
// ticks long.
constexpr uint32_t kSongTick = 37;
constexpr uint32_t kTicksPerQuarter = 96 * kSongTick;

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The MIDI file's tempo when the model plays no frames.
constexpr int kDefaultTempo = 90;

// The slowest tempo a MIDI file can hold at this resolution: at 3 or less, a quarter note lasts more than the 2^24
// microseconds a tempo event can give it.
constexpr int kMinMidiTempo = 4;

constexpr int kVelocity = 127; // every note's velocity, as CC11 carries the loudness

// Each channel's MIDI track name.
constexpr const char* kChannelNames[kChannels] = {"Square 1", "Square 2", "Wave", "Noise", "PCM A", "PCM B"};

// The key of the first drum in the noise channel's kit.
constexpr int kFirstDrumKey = 36;

// The MIDI key of a square channel's pitch table index 0 (C2). The wave channel plays a pattern of 32 steps an octave
// lower, and one of 64 steps two octaves lower.
constexpr int kSquareBaseKey = 36;

// A PCM note's pitch table index that plays a sample at its own rate, and the key that does in the SoundFont.
constexpr int kPcmUnityIndex = 24;
constexpr int kPcmRootKey = 60;

// The widest pitch bend range the MIDI files set, in semitones.
constexpr int kMaxBendRange = 96;

// The bits of SOUNDCNT_H for each FIFO: its volume (100% when set), and its right and left outputs.
constexpr uint16_t kFifoVolume[2] = {0x0004, 0x0008};
constexpr uint16_t kFifoRight[2] = {0x0100, 0x1000};
constexpr uint16_t kFifoLeft[2] = {0x0200, 0x2000};

std::string TwoDigits(int n)
{
    char b[16];
    std::snprintf(b, sizeof b, "%02d", n);

    return b;
}

// Returns the seconds that `frames` frames take.
double FrameSeconds(double frames)
{
    return frames * double(kFrameCycles) / double(kSecondCycles);
}

// The state the model leaves after a frame, for the conversion.
struct FrameData
{
    Hardware hardware;
    std::array<Fifo, 2> fifos;
    std::array<uint16_t, kChannels> tempo = {};
};

// The stretch of a song that the conversion covers, in MIDI ticks.
struct Plan
{
    bool loops = false;
    uint32_t loop_start = 0; // the latest of the looping channels' loop points and the others' ends, or a loop later
    uint32_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the channels' loops
    uint32_t next_end = 0;   // as far again: the end of the song's next pass through its loop
    uint32_t end = 0;
};

// The model's output for the stretch of a song that the plan covers, and its timing in the MIDI file.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<Event> events;
    std::vector<uint32_t> frame_ticks; // the MIDI tick each frame starts at, and the one after the last frame
    std::vector<uint32_t> event_ticks; // each event's MIDI tick
    Plan plan;
    std::vector<std::string> warnings;
};

// Returns the MIDI file's tempo in a frame: the first channel's tempo T, in which the frame lasts T ticks.
int MidiTempo(const FrameData& d)
{
    return std::max<int>(d.tempo[0], kMinMidiTempo);
}

// Returns each event's MIDI tick. The driver times a channel's notes by counting its ticks from the last time it reset
// the count: at the song's start, at a loop and at a tempo change. It plays each note at the start of the frame that
// its count reaches. So the first note after a reset starts its frame, and the notes after it keep to their ticks, 37
// MIDI ticks each at the first channel's tempo, within the frames the driver plays them in, or with `frame_timing`, at
// the start of those frames.
std::vector<uint32_t> EventTicks(const Simulation& sim, bool frame_timing)
{
    std::vector<uint32_t> ticks(sim.events.size());
    std::array<bool, kChannels> reset;
    reset.fill(true);
    std::array<uint32_t, kChannels> base = {};
    std::array<uint64_t, kChannels> base_tick = {};
    std::array<double, kChannels> scale = {};
    for (size_t i = 0; i < sim.events.size(); i++)
    {
        const Event& e = sim.events[i];
        const size_t c = e.channel;
        const uint32_t start = sim.frame_ticks[e.frame];
        const uint32_t next = sim.frame_ticks[e.frame + 1];
        if (frame_timing)
        {
            ticks[i] = start;
            continue;
        }

        // A loop resets the channel's count, and so does a tempo: on the first channel, every channel's. A loop point
        // read before the first note after a reset belongs to that note's frame.
        if (e.kind == Event::kTempo || e.kind == Event::kLoop)
        {
            if (e.kind == Event::kTempo && c == 0)
            {
                reset.fill(true);
            }

            reset[c] = true;
            ticks[i] = start;
            continue;
        }
        if (reset[c] && e.kind == Event::kLoopPoint)
        {
            ticks[i] = start;
            continue;
        }

        if (reset[c])
        {
            reset[c] = false;
            base[c] = start;
            base_tick[c] = e.tick;
            const FrameData& d = sim.frames[e.frame];
            scale[c] = double(kSongTick) * MidiTempo(d) / std::max<uint16_t>(d.tempo[c], 1);
        }

        const double tick = base[c] + double(e.tick - base_tick[c]) * scale[c];
        ticks[i] = uint32_t(std::clamp<double>(std::round(tick), start, next - 1));
    }

    return ticks;
}

// Runs the model until every channel has looped, ended or stopped at its loop point, and the song has played its loop
// `loops` times, and works out the MIDI file's timing. With `second`, the loop starts at the song's second pass through
// it, and the song plays the first pass before it.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, bool frame_timing, bool second)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    sim.frame_ticks.push_back(0);

    // Each channel's loop point, the ends of its passes through its loop, and where it ends or stops at its loop point,
    // as indexes into the events.
    std::array<int, kChannels> loop_point, end;
    loop_point.fill(-1);
    end.fill(-1);
    std::array<std::vector<int>, kChannels> passes;
    int loop_channel = -1;
    size_t times = 1; // the passes the loop channel makes through its loop in each of the song's
    bool planned = false;
    bool finished = false; // the song played what the plan requires
    uint32_t run_on = 0;   // the MIDI ticks the model runs on for after the loop channel's last pass
    for (uint32_t f = 0; f < kMaxFrames && !seq.Ended(); f++)
    {
        seq.Step();
        FrameData d;
        d.hardware = seq.GetHardware();
        d.fifos = {seq.GetFifo(0), seq.GetFifo(1)};
        for (int c = 0; c < kChannels; c++)
        {
            d.tempo[size_t(c)] = seq.GetChannel(c).tempo;
        }
        sim.frames.push_back(d);

        // The MIDI file's tempo map follows the first channel, which keeps every frame the same length.
        sim.frame_ticks.push_back(sim.frame_ticks.back() + uint32_t(MidiTempo(d)));

        for (const Event& e : seq.Events())
        {
            const size_t c = e.channel;
            const int index = int(sim.events.size());
            sim.events.push_back(e);
            if (e.kind == Event::kLoopPoint && passes[c].empty())
            {
                loop_point[c] = index;
            }
            else if (e.kind == Event::kLoop)
            {
                passes[c].push_back(index);
            }
            else if (e.kind == Event::kEnd || e.kind == Event::kHold)
            {
                end[c] = index;
            }
        }

        // Once every channel has looped, ended or stopped at its loop point, the loop is known. The song then continues
        // for the required number of passes, and with `second`, one more before them. Channels may have different loop
        // points and lengths. The overall loop starts once all looping channels have entered their loops and the others
        // have ended or stopped. LoopLength() finds how long it takes them to return to their loop starts, subject to
        // its length limit: `times` passes of the longest channel loop.
        if (!planned)
        {
            planned = true;
            loop_channel = -1;
            uint64_t longest = 0;
            std::vector<uint64_t> lengths;
            for (size_t c = 0; c < kChannels; c++)
            {
                planned = planned && (!passes[c].empty() || end[c] >= 0);
                const uint64_t length =
                    !passes[c].empty() && loop_point[c] >= 0
                        ? sim.events[size_t(passes[c][0])].tick - sim.events[size_t(loop_point[c])].tick
                        : 0;
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

            // The song runs on as much longer as the loop starts after the loop channel's loop point: for at least as
            // many MIDI ticks as the frames from that loop point to the latest took, whatever the tempo then, or to the
            // end of the frame if they're in the same one.
            if (planned && loop_channel >= 0)
            {
                times = size_t(LoopLength(lengths) / longest);
                const uint32_t first = sim.events[size_t(loop_point[size_t(loop_channel)])].frame;
                uint32_t latest = first;
                for (size_t c = 0; c < kChannels; c++)
                {
                    if (!passes[c].empty() && loop_point[c] >= 0)
                    {
                        latest = std::max(latest, sim.events[size_t(loop_point[c])].frame);
                    }
                    else if (end[c] >= 0)
                    {
                        latest = std::max(latest, sim.events[size_t(end[c])].frame);
                    }
                }
                run_on = latest > first ? sim.frame_ticks[latest + 1] - sim.frame_ticks[first] : 0;
            }
        }
        if (planned && loop_channel < 0)
        {
            finished = true;
            break;
        }
        const size_t needed = (size_t(loops) + (second ? 1 : 0)) * times; // the loop channel's passes in the song
        if (planned && passes[size_t(loop_channel)].size() >= needed)
        {
            const int last = passes[size_t(loop_channel)][needed - 1];
            if (sim.frame_ticks[f] >= sim.frame_ticks[sim.events[size_t(last)].frame] + run_on)
            {
                finished = true;
                break;
            }
        }
    }

    sim.event_ticks = EventTicks(sim, frame_timing);

    // The loop starts where every looping channel has started its loop and every other channel has ended or stopped,
    // that far after the loop channel's loop point, and ends as far after the end of the loop channel's `times`th pass.
    // With `second`, it starts and ends a loop later, as far after the ends of its `times`th and twice `times`th
    // passes. The song's next pass through the loop ends a loop after it. The song ends as far after its last pass, or
    // where the last channel ends or stops if the song doesn't loop. A song that the model gave up on ends where it
    // stopped.
    Plan& plan = sim.plan;
    if (loop_channel >= 0)
    {
        const size_t c = size_t(loop_channel);
        const std::vector<int>& p = passes[c];
        uint32_t start = sim.event_ticks[size_t(loop_point[c])];
        for (size_t k = 0; k < kChannels; k++)
        {
            if (!passes[k].empty() && loop_point[k] >= 0)
            {
                start = std::max(start, sim.event_ticks[size_t(loop_point[k])]);
            }
            else if (end[k] >= 0)
            {
                start = std::max(start, sim.event_ticks[size_t(end[k])]);
            }
        }

        const uint32_t first_pass = sim.event_ticks[size_t(p[0])] - sim.event_ticks[size_t(loop_point[c])];
        const uint32_t delay = start - sim.event_ticks[size_t(loop_point[c])];

        // Where the loop channel's `n`th pass ends, moved as far on as the loop starts after its loop point: the loop
        // start for 0, and if the model stopped before that pass ended, where it would at the first pass's length.
        const auto pass_end = [&](size_t n)
        {
            return n == 0          ? start
                   : p.size() >= n ? sim.event_ticks[size_t(p[n - 1])] + delay
                                   : start + uint32_t(n) * first_pass;
        };

        const size_t skip = second ? times : 0; // the loop channel's passes before the loop
        const size_t last = size_t(loops) * times + skip;
        plan.loops = true;
        plan.loop_start = pass_end(skip);
        plan.loop_end = pass_end(skip + times);
        plan.next_end = pass_end(skip + 2 * times);
        plan.end = p.size() >= last ? std::min(pass_end(last), sim.frame_ticks.back()) : sim.frame_ticks.back();
    }
    else
    {
        for (size_t c = 0; c < kChannels; c++)
        {
            if (end[c] >= 0)
            {
                plan.end = std::max(plan.end, sim.event_ticks[size_t(end[c])]);
            }
        }
    }

    if (plan.end == 0)
    {
        plan.end = sim.frame_ticks.back();
    }

    sim.warnings = seq.Warnings();
    if (!finished && !seq.Ended() && sim.frames.size() == kMaxFrames)
    {
        sim.warnings.push_back("the song was cut off after an hour");
    }

    return sim;
}

// A change that the frequency sweep makes to a square's pitch within a frame.
struct SweepStep
{
    double at = 0;     // its position in the frame, from 0 to 1
    uint32_t tick = 0; // its MIDI tick
    double bend = 0;   // semitones from the note's key
};

// A channel's sound in one frame.
struct Sound
{
    bool sounding = false;
    double left = 0, right = 0; // levels, with a full-scale FIFO sample at 128
    double bend = 0;            // semitones from the note's key
    int shape = -1;             // the duty or the wave shape that the frame plays
    uint32_t tick = 0;          // the MIDI tick its changes go at
    std::vector<SweepStep> sweeps;
};

// The SoundFont instrument a note plays.
struct Instrument
{
    enum Kind : uint8_t
    {
        kSquare,
        kWave,
        kNoiseKit,
        kPcm,
    };

    bool operator<(const Instrument& o) const
    {
        return std::tie(kind, first, frames, second, pcm) < std::tie(o.kind, o.first, o.frames, o.second, o.pcm);
    }

    Kind kind = kSquare;
    int first = 0;  // the duty or the wave shape at a note's start
    int frames = 0; // the frames before it switches, or 0
    int second = 0; // the duty or the wave shape it switches to
    PcmSound pcm;
};

// A note as the MIDI file plays it.
struct Note
{
    int key = 60;
    int program = 0;
    uint32_t on = 0; // MIDI ticks
    uint32_t off = 0;
    std::vector<Sound> sounds; // one for each of its frames
    NoiseSound noise;          // a noise note's drum
    PcmSound pcm;              // a PCM note's sample
};

// The PSG's level on each side: the master volume and the PSG's share of the mix.
struct PsgScale
{
    double left = 1, right = 1;
};

PsgScale PsgScaleOf(const Hardware& hw)
{
    static constexpr double kMix[4] = {0.25, 0.5, 1.0, 1.0};
    const double mix = kMix[hw.registers[11] & 3];
    const int nr50 = hw.registers[10] & 0xFF;
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
    int Frame(const Hardware& hw)
    {
        // NR10 sets the sweep as it goes.
        sweep_time_ = (hw.registers[0] >> 4) & 7;
        sweep_shift_ = hw.registers[0] & 7;
        sweep_down_ = (hw.registers[0] & 8) != 0;

        // A restart starts the channel again, and a frequency written without one plays until the sweep's next step.
        const uint16_t envelope = hw.registers[size_t(kEnvelope[channel_])];
        const uint16_t control = hw.registers[size_t(kControl[channel_])];
        if (hw.restarted & (1 << channel_))
        {
            Restart(envelope, control);
        }
        else if ((control & 0x7FF) != written_)
        {
            frequency_ = control & 0x7FF;
        }
        written_ = control & 0x7FF;

        // A channel whose DAC is off is off at once.
        if (channel_ == kWave ? !(hw.registers[5] & 0x80) : !(envelope & 0xF800))
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
    static constexpr int kEnvelope[4] = {1, 3, 6, 8}; // NRx1/NRx2, or NR31/NR32
    static constexpr int kControl[4] = {2, 4, 7, 9};  // NRx3/NRx4

    void Restart(uint16_t envelope, uint16_t control)
    {
        on_ = true;
        volume_ = channel_ == kWave ? 15 : envelope >> 12;
        up_ = (envelope & 0x0800) != 0;
        step_ = channel_ == kWave ? 0 : (envelope >> 8) & 7;
        envelope_ticks_ = 0;
        length_on_ = (control & 0x4000) != 0;
        length_ = channel_ == kWave ? 256 - (envelope & 0xFF) : 64 - (envelope & 63);
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

// Returns the wave shape that the wave channel plays after a frame: the bank NR30 selects, or both banks starting with
// it in the 64-step mode.
WaveShape WaveShapeOf(const Hardware& hw)
{
    const uint16_t nr30 = hw.registers[5];
    const size_t bank = (nr30 >> 6) & 1;
    WaveShape shape;
    for (size_t b = 0; b < ((nr30 & 0x20) ? 2u : 1u); b++)
    {
        for (uint8_t v : hw.wave_ram[(bank + b) & 1])
        {
            shape.steps.push_back(uint8_t(v >> 4));
            shape.steps.push_back(uint8_t(v & 15));
        }
    }

    return shape;
}

// Converts a song's channels into the MIDI file's notes.
class NoteMaker
{
public:
    NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim)
        : rom_(rom), info_(info), sim_(sim), end_(sim.plan.end)
    {
    }

    // Returns channel `c`'s notes, and adds the instruments they play.
    std::vector<Note> Make(int c)
    {
        // Each note or "play again" starts a stretch of the channel, which lasts until its next note, rest or end.
        std::vector<size_t> starts;
        for (size_t i = 0; i < sim_.events.size(); i++)
        {
            const Event& e = sim_.events[i];
            if (e.channel == c &&
                (e.kind == Event::kNote || e.kind == Event::kRepeat || e.kind == Event::kRest || e.kind == Event::kEnd))
            {
                starts.push_back(i);
            }
        }

        std::vector<Note> notes;
        PsgModel psg(c < kPcmA ? c : 0);
        uint32_t fed = 0; // the frames the PSG model has taken in

        auto feed = [&](uint32_t until)
        {
            for (; c < kPcmA && fed < until && fed < sim_.frames.size(); fed++)
            {
                psg.Frame(sim_.frames[fed].hardware);
            }
        };

        int key = 60;
        int base = 0; // the frequency setting or PCM pitch that the key stands for
        for (size_t i = 0; i < starts.size(); i++)
        {
            const Event& e = sim_.events[starts[i]];
            const uint32_t first = e.frame;
            const uint32_t last =
                i + 1 < starts.size() ? sim_.events[starts[i + 1]].frame : uint32_t(sim_.frames.size());
            const uint32_t on = sim_.event_ticks[starts[i]];
            const uint32_t stop = std::min(i + 1 < starts.size() ? sim_.event_ticks[starts[i + 1]] : end_, end_);
            feed(first);
            if (e.kind == Event::kRest || e.kind == Event::kEnd || on >= stop)
            {
                continue;
            }

            if (e.kind == Event::kNote)
            {
                key = KeyOf(c, e.index, base);
            }
            else if (c >= kPcmA)
            {
                key = kPcmRootKey;
                base = 0;
            }

            // The stretch's frames, split into the runs in which the channel sounds. Each frame's changes go as far
            // into it as the stretch's start is into its first frame, and stay before the next stretch.
            Note note;
            bool open = false;
            for (uint32_t f = first; f < last && f < sim_.frames.size(); f++)
            {
                const FrameData& d = sim_.frames[f];
                const uint32_t into = on - sim_.frame_ticks[first];
                const uint32_t tick = std::min({sim_.frame_ticks[f] + into, sim_.frame_ticks[f + 1] - 1, stop - 1});
                Sound s = c < kPcmA ? PsgSound(c, psg, d, base) : FifoSound(c, d, base);
                s.tick = tick;
                fed = c < kPcmA ? f + 1 : fed;

                // The sweep's steps go as far into the frame's ticks as they come into the frame, before the next
                // frame's changes.
                const uint32_t length = sim_.frame_ticks[f + 1] - sim_.frame_ticks[f];
                const uint32_t last_tick = std::min(sim_.frame_ticks[f + 1] + into, stop) - 1;
                for (SweepStep& step : s.sweeps)
                {
                    step.tick = std::min(tick + uint32_t(std::lround(step.at * length)), last_tick);
                }

                if (!s.sounding)
                {
                    if (open)
                    {
                        note.off = tick;
                        Finish(c, note, notes);
                        open = false;
                    }
                    continue;
                }

                if (!open)
                {
                    note = Note();
                    note.key = key;
                    note.on = tick;
                    note.pcm = PcmSoundOf(d.fifos[size_t(c >= kPcmA ? c - kPcmA : 0)]);
                    open = note.on < stop;
                }
                if (open)
                {
                    note.sounds.push_back(s);
                    AddNoiseFrame(c, d.hardware, note.noise);
                }
            }

            if (open)
            {
                note.off = stop;
                Finish(c, note, notes);
            }
        }

        return notes;
    }

    // Returns the instruments the notes play, with their programs.
    const std::map<Instrument, int>& Programs() const
    {
        return programs_;
    }

    // Returns the noise kit's drums, with their keys.
    const std::map<NoiseSound, int>& Drums() const
    {
        return drums_;
    }

    // Returns wave shape `index`.
    const WaveShape& Shape(int index) const
    {
        return shape_list_[size_t(index)];
    }

private:
    // Returns a note's MIDI key, and sets `base` to the pitch it stands for.
    int KeyOf(int c, int index, int& base) const
    {
        if (c >= kPcmA)
        {
            base = int16_t(rom_.U16(info_.pcm_pitch_table + uint32_t(index) * 2));
            return kPcmRootKey + index - kPcmUnityIndex;
        }

        base = rom_.U16(info_.frequency_table + uint32_t(index) * 2) & 0x7FF;

        return kSquareBaseKey + index;
    }

    // Returns a PSG channel's sound in a frame.
    Sound PsgSound(int c, PsgModel& psg, const FrameData& d, int base)
    {
        Sound s;
        const Hardware& hw = d.hardware;
        const int volume = psg.Frame(hw);
        if (volume < 0)
        {
            return s;
        }

        const PsgScale scale = PsgScaleOf(hw);
        const int nr51 = hw.registers[10] >> 8;
        double level = 0;
        switch (c)
        {
        case kWave:
            {
                // NR32 scales the wave: 100%, 50% or 25%, or 75% with bit 15.
                static constexpr double kCodes[4] = {0, 1, 0.5, 0.25};
                const uint16_t nr32 = hw.registers[6];
                level = 2 * kPsgLevelPerVolume * 15 * ((nr32 & 0x8000) ? 0.75 : kCodes[(nr32 >> 13) & 3]);
                s.shape = ShapeIndex(WaveShapeOf(hw));
                break;
            }

        case kNoise:
            // The drum's sample has the noise channel's volume in it.
            level = kPsgLevelPerVolume * 15;
            break;

        default:
            level = kPsgLevelPerVolume * volume;
            s.shape = (hw.registers[size_t(c == kSquare1 ? 1 : 3)] >> 6) & 3;
            break;
        }

        s.sounding = true;
        s.left = (nr51 & (0x10 << c)) ? level * scale.left : 0;
        s.right = (nr51 & (0x01 << c)) ? level * scale.right : 0;

        if (c != kNoise && base)
        {
            s.bend = Bend(base, psg.Frequency());
            for (const auto& [at, frequency] : psg.Sweeps())
            {
                s.sweeps.push_back({at, 0, Bend(base, frequency)});
            }
        }

        return s;
    }

    // Returns the semitones from frequency setting `base` up to `frequency`.
    static double Bend(int base, int frequency)
    {
        return 12 * std::log2(double(2048 - base) / double(2048 - std::min(frequency, 2047)));
    }

    // Returns a PCM channel's sound in a frame.
    Sound FifoSound(int c, const FrameData& d, int base) const
    {
        Sound s;
        const int f = c - kPcmA;
        const Fifo& fifo = d.fifos[size_t(f)];
        if (fifo.position == ~0u || !fifo.base_rate || !fifo.rate)
        {
            return s;
        }

        const uint16_t h = d.hardware.registers[11];
        const double level = (h & kFifoVolume[f]) ? kFifoLevel : kFifoLevel / 2;
        s.sounding = true;
        s.left = (h & kFifoLeft[f]) ? level : 0;
        s.right = (h & kFifoRight[f]) ? level : 0;

        // The key plays at the rate that the note's pitch stands for, which may differ from the rate it plays at by a
        // detune or an effect.
        const double nominal = fifo.base_rate + double(fifo.base_rate) * base / 1000.0;
        s.bend = 12 * std::log2(double(fifo.rate) / nominal);

        return s;
    }

    // Returns what a FIFO plays from its last start, at the rate it plays at then: the frames until the driver stops it
    // or loops it, and the frames of each pass through the loop, as the bytes the FIFO plays in them. A FIFO that
    // started from its loop plays passes through the loop from the start.
    static PcmSound PcmSoundOf(const Fifo& fifo)
    {
        PcmSound p;
        p.start = fifo.start;
        p.from = fifo.data - fifo.start;
        p.looped = fifo.loop != 0;
        p.loop = p.looped ? fifo.loop - fifo.start : 0;
        p.rate = fifo.base_rate;

        const double bytes = double(kFrameCycles) / std::max<uint32_t>(SampleCycles(fifo.rate), 1);
        const bool in_loop = p.looped && fifo.data == fifo.loop;
        const uint32_t first = in_loop ? 0 : FifoFrames(p.from * 60, fifo.rate, fifo.end);
        const uint32_t pass = FifoFrames(fifo.loop_position + fifo.rate, fifo.rate, fifo.end) + 1;
        p.first = uint32_t(std::lround(first * bytes));
        p.loop_length = p.looped ? uint32_t(std::lround(pass * bytes)) : 0;
        p.from = in_loop ? p.loop : p.from;

        return p;
    }

    // Returns the index of a wave shape, numbering the shapes in order of first use.
    int ShapeIndex(const WaveShape& shape)
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

    // Adds a frame of a noise note to its drum: the registers the frame leaves, with a restart at the note's start.
    static void AddNoiseFrame(int c, const Hardware& hw, NoiseSound& sound)
    {
        if (c != kNoise)
        {
            return;
        }

        const bool restart = sound.frames.empty() || (hw.restarted & (1 << kNoise));
        sound.frames.emplace_back(hw.registers[8], uint16_t((hw.registers[9] & 0x7FFF) | (restart ? 0x8000 : 0)));
    }

    // Gives a finished note its instrument and keeps it.
    void Finish(int c, Note& note, std::vector<Note>& notes)
    {
        if (note.sounds.empty() || note.on >= note.off)
        {
            return;
        }

        Instrument inst;
        if (c == kNoise)
        {
            // The drum ends with the last frame that changes the registers; the channel plays on from there by itself.
            std::vector<std::pair<uint16_t, uint16_t>>& frames = note.noise.frames;
            while (frames.size() > 1 &&
                   frames.back() == std::make_pair(frames[frames.size() - 2].first,
                                                   uint16_t(frames[frames.size() - 2].second & 0x7FFF)))
            {
                frames.pop_back();
            }

            inst.kind = Instrument::kNoiseKit;
            auto it = drums_.find(note.noise);
            if (it == drums_.end())
            {
                it = drums_.emplace(note.noise, kFirstDrumKey + int(drums_.size())).first;
            }
            note.key = it->second;
        }
        else if (c >= kPcmA)
        {
            inst.kind = Instrument::kPcm;
            inst.pcm = note.pcm;
        }
        else
        {
            // The shape at the start, and the first one it switches to.
            inst.kind = c == kWave ? Instrument::kWave : Instrument::kSquare;
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

            // A wave pattern of 64 steps plays an octave lower than one of 32, and one that repeats sounds higher.
            if (c == kWave)
            {
                const WaveShape& shape = Shape(inst.first);
                note.key += SoundfontBuilder::WaveKeyOffset(shape) - (shape.steps.size() > 32 ? 24 : 12);
            }
        }

        auto it = programs_.find(inst);
        if (it == programs_.end())
        {
            it = programs_.emplace(inst, int(programs_.size())).first;
        }

        note.program = it->second;
        notes.push_back(note);
    }

    const Rom& rom_;
    const DriverInfo& info_;
    const Simulation& sim_;
    const uint32_t end_;
    std::map<Instrument, int> programs_;
    std::map<NoiseSound, int> drums_;
    std::map<WaveShape, int> shapes_;
    std::vector<WaveShape> shape_list_;
};

// Returns true if each channel's second pass through its loop plays the same notes as its first: the same keys and
// programs at the same ticks from the pass's start, with the same levels, bend and tempo. Every note has the same
// velocity. The notes' ends aren't compared: the first pass can be a fraction of a frame longer than the next, as the
// driver drops the part of a frame it has counted when it loops, and the sound hardware's clocks, which aren't tied to
// the frames, can end a note a frame earlier or later from pass to pass. `sim` has to hold three passes of the song's
// loop, so that every channel's second pass is complete.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim);
    for (int c = 0; c < kChannels; c++)
    {
        // Where the channel's first pass starts, at its loop point, and where it and the second pass end.
        std::vector<uint32_t> bounds;
        for (size_t i = 0; i < sim.events.size() && bounds.size() < 3; i++)
        {
            const Event& e = sim.events[i];
            if (e.channel == c && e.kind == Event::kLoopPoint && bounds.size() < 2)
            {
                bounds.assign(1, sim.event_ticks[i]);
            }
            else if (e.channel == c && e.kind == Event::kLoop && !bounds.empty())
            {
                bounds.push_back(sim.event_ticks[i]);
            }
        }
        if (bounds.size() < 3)
        {
            continue;
        }

        std::array<std::vector<std::tuple<uint32_t, int, int, int, int, double, int>>, 2> passes;
        for (const Note& n : maker.Make(c))
        {
            for (size_t k = 0; k < 2; k++)
            {
                if (n.on >= bounds[k] && n.on < bounds[k + 1])
                {
                    const auto frame = std::upper_bound(sim.frame_ticks.begin(), sim.frame_ticks.end(), n.on) - 1;
                    int cc10 = 64, cc11 = 0;
                    LevelsToControllers(n.sounds[0].left, n.sounds[0].right, cc10, cc11);
                    passes[k].emplace_back(n.on - bounds[k], n.key, n.program, cc10, cc11, n.sounds[0].bend,
                                           MidiTempo(sim.frames[size_t(frame - sim.frame_ticks.begin())]));
                }
            }
        }
        if (passes[0] != passes[1])
        {
            return false;
        }
    }

    return true;
}

// Returns true if each channel starts the same number of notes in the marked loop and the next pass. When a channel
// loops, the driver discards its fractional frame, which differs between channels, so a channel's later passes can
// start up to a frame earlier relative to the loop channel than its first did. The copy of a note at the loop's start
// can then fall before the end marker, causing a looping player to play it twice.
bool SeamRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim);
    const Plan& plan = sim.plan;
    for (int c = 0; c < kChannels; c++)
    {
        const std::vector<Note> notes = maker.Make(c);
        const auto starts = [&](uint32_t from, uint32_t to)
        {
            return std::count_if(notes.begin(), notes.end(), [&](const Note& n) { return n.on >= from && n.on < to; });
        };

        if (starts(plan.loop_start, plan.loop_end) != starts(plan.loop_end, plan.next_end))
        {
            return false;
        }
    }

    return true;
}

// Returns the MIDI file's tempo changes before the end, as (MIDI tick, tempo).
std::vector<std::pair<uint32_t, int>> TempoMap(const Simulation& sim)
{
    std::vector<std::pair<uint32_t, int>> tempos;
    for (size_t f = 0; f < sim.frames.size() && (f == 0 || sim.frame_ticks[f] < sim.plan.end); f++)
    {
        const int tempo = MidiTempo(sim.frames[f]);
        if (tempos.empty() || tempos.back().second != tempo)
        {
            tempos.emplace_back(sim.frame_ticks[f], tempo);
        }
    }

    if (tempos.empty())
    {
        tempos.emplace_back(0, kDefaultTempo);
    }

    return tempos;
}

// Returns the seconds from the start to MIDI tick `tick`.
double TickSeconds(const Simulation& sim, uint32_t tick)
{
    const std::vector<uint32_t>& starts = sim.frame_ticks;
    const size_t f = size_t(std::upper_bound(starts.begin(), starts.end(), tick) - starts.begin()) - 1;
    const double into = f + 1 < starts.size() ? double(tick - starts[f]) / double(starts[f + 1] - starts[f]) : 0.0;
    return FrameSeconds(double(f) + into);
}

// Returns the microseconds of a quarter note at the driver's tempo `tempo`.
uint32_t QuarterMicros(int tempo)
{
    constexpr uint64_t kNumerator = uint64_t(kTicksPerQuarter) * kFrameCycles * 1000000;
    const uint64_t denominator = uint64_t(tempo) * kSecondCycles;
    return uint32_t((kNumerator + denominator / 2) / denominator);
}

// Returns the SF2 time in timecents for `frames` frames.
int FrameTimecents(int frames)
{
    return sf2::ToTimecents(FrameSeconds(frames) * 1000.0);
}

// Returns a zone that plays `sample` on every key, looping it if `loop` is set.
sf2::Zone SampleZone(int sample, bool loop)
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, loop ? 1 : 0));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

    return z;
}

// Returns the zones of an instrument that plays sample `first` for `frames` frames of each note, and then `second`.
std::vector<sf2::Zone> SwitchZones(int first, int frames, int second)
{
    if (frames == 0)
    {
        return {SampleZone(first, true)};
    }

    // The first sample holds for the frames and then falls silent; the second waits for them.
    const int time = FrameTimecents(frames);
    sf2::Zone a = SampleZone(first, true);
    a.generators.insert(a.generators.begin() + 1, {sf2::Generator::Value(sf2::kHoldVolEnv, time),
                                                   sf2::Generator::Value(sf2::kDecayVolEnv, -12000),
                                                   sf2::Generator::Value(sf2::kSustainVolEnv, 1440)});
    sf2::Zone b = SampleZone(second, true);
    b.generators.insert(b.generators.begin() + 1, sf2::Generator::Value(sf2::kDelayVolEnv, time));

    return {a, b};
}

// Adds the SoundFont presets of a song's instruments, each in the bank and program of `slots` that its program number
// picks. Returns the warnings for instruments with nothing to play.
std::vector<std::string> AddPresets(SoundfontBuilder& sf, const NoteMaker& maker, const std::vector<BankProgram>& slots)
{
    std::vector<std::string> warnings;
    for (const auto& [inst, program] : maker.Programs())
    {
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
                zones = SwitchZones(first, inst.frames, second);
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
                const int first_sample = sf.WaveSample(maker.Shape(inst.first));
                const int second_sample = sf.WaveSample(maker.Shape(inst.second));
                zones = SwitchZones(first_sample, inst.frames, second_sample);

                // The note's key allows for how often the first shape repeats, so the second shape plays from the
                // first's root key, at the same rate.
                const int first = SoundfontBuilder::WaveKeyOffset(maker.Shape(inst.first));
                const int second = SoundfontBuilder::WaveKeyOffset(maker.Shape(inst.second));
                if (zones.size() == 2 && first != second)
                {
                    zones[1].generators.insert(zones[1].generators.end() - 1,
                                               sf2::Generator::Value(sf2::kOverridingRootKey, 60 + first));
                }

                std::snprintf(text, sizeof text, inst.frames ? "Wave %d to %d" : "Wave %d", inst.first, inst.second);
                name = text;
                break;
            }

        case Instrument::kNoiseKit:
            for (const auto& [sound, key] : maker.Drums())
            {
                const int sample = sf.NoiseSample(sound);
                sf2::Zone z;
                z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, key, key));
                z.generators.push_back(sf2::Generator::Value(sf2::kOverridingRootKey, key));
                z.generators.push_back(
                    sf2::Generator::Value(sf2::kSampleModes, Looped(sf.File().samples[size_t(sample)]) ? 1 : 0));
                z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));
                zones.push_back(z);
            }
            name = "Noise drums";
            break;

        default:
            {
                const int sample = sf.PcmSample(inst.pcm);
                if (sample >= 0)
                {
                    zones.push_back(SampleZone(sample, inst.pcm.looped));
                }
                else
                {
                    warnings.push_back("a PCM note's sample has nothing to play");
                }

                std::snprintf(text, sizeof text, "PCM %08X", unsigned(inst.pcm.start));
                name = text;
                break;
            }
        }

        const BankProgram& slot = slots[size_t(program)];
        sf.AddPreset(name, slot.bank, slot.program, sf.AddInstrument(name, std::move(zones)));
    }

    return warnings;
}

// Returns the pitch bend value for `bend` semitones with a bend range of `range`.
int BendValue(double bend, int range)
{
    return std::clamp(8192 + int(std::lround(bend / range * 8192)), 0, 16383);
}

// Writes the MIDI file: a conductor track, then a track for each channel that plays notes. `slots` gives each program
// number its preset's bank and program.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
               const std::array<std::vector<Note>, kChannels>& notes, const std::vector<BankProgram>& slots,
               std::string& error)
{
    const Plan& plan = sim.plan;
    const uint32_t end = plan.end;
    MidiFile midi(kTicksPerQuarter);
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const auto& [tick, tempo] : TempoMap(sim))
    {
        conductor.Tempo(tick, QuarterMicros(tempo));
    }
    if (plan.loops)
    {
        conductor.Meta(plan.loop_start, 0x06, "loopStart");
        conductor.Meta(plan.loop_end, 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    for (int c = 0; c < kChannels; c++)
    {
        const std::vector<Note>& list = notes[size_t(c)];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name(kChannelNames[c]);
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
        const int range = widest > 0 ? std::clamp(int(std::ceil(widest - 1e-9)), 2, kMaxBendRange) : 0;

        int program = list[0].program;
        mt.Bank(0, c, slots[size_t(program)].bank);
        mt.Program(0, c, slots[size_t(program)].program);
        mt.Control(0, c, cc::kVolume, 127);
        mt.Control(0, c, cc::kPan, 64);
        mt.Control(0, c, cc::kExpression, 0);
        mt.Control(0, c, cc::kReverb, 0);
        mt.Control(0, c, cc::kChorus, 0);
        if (range)
        {
            mt.BendRange(0, c, range);
            mt.PitchBend(0, c, 8192);
        }

        // A player that jumps back to the loop's start keeps the settings that the loop's end left, so they're all
        // written again from the channel's first event there, unless the loop starts with the song.
        int cc10 = 64, cc11 = 0, bend = 8192;
        bool before_loop = plan.loops && plan.loop_start > 0;
        const auto forget = [&](uint32_t tick)
        {
            if (before_loop && tick >= plan.loop_start)
            {
                before_loop = false;
                program = -1;
                cc10 = cc11 = bend = -1;
            }
        };

        for (const Note& n : list)
        {
            forget(n.on);
            if (n.program != program)
            {
                if (program < 0 || slots[size_t(n.program)].bank != slots[size_t(program)].bank)
                {
                    mt.Bank(n.on, c, slots[size_t(n.program)].bank);
                }

                program = n.program;
                mt.Program(n.on, c, slots[size_t(program)].program);
            }

            // Each frame's levels and bend.
            for (const Sound& s : n.sounds)
            {
                const uint32_t tick = s.tick;
                if (tick >= n.off)
                {
                    break;
                }

                forget(tick);
                int n10 = cc10, n11 = cc11;
                LevelsToControllers(s.left, s.right, n10, n11);
                if (n10 != cc10)
                {
                    mt.Control(tick, c, cc::kPan, cc10 = n10);
                }
                if (n11 != cc11)
                {
                    mt.Control(tick, c, cc::kExpression, cc11 = n11);
                }
                if (!range)
                {
                    continue;
                }

                // The frame's bend, then the sweep's steps within the frame.
                const int v = BendValue(s.bend, range);
                if (v != bend)
                {
                    mt.PitchBend(tick, c, bend = v);
                }
                for (const SweepStep& step : s.sweeps)
                {
                    forget(step.tick);
                    const int w = BendValue(step.bend, range);
                    if (step.tick < n.off && w != bend)
                    {
                        mt.PitchBend(step.tick, c, bend = w);
                    }
                }
            }

            mt.NoteOn(n.on, c, std::clamp(n.key, 0, 127), kVelocity);
            mt.NoteOff(n.off, c, std::clamp(n.key, 0, 127));
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, writing its files if `write` is set.
SongSummary Run(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt, SoundfontBuilder* shared,
                bool write)
{
    SongSummary sum;
    if (song < 0 || song >= int(info.song_addresses.size()))
    {
        sum.warnings.push_back("its header can't be read");
        return sum;
    }

    // Start the marked loop on the second pass if it differs from the first. The intro may leave a different octave or
    // other setting from the end of the loop. A channel may continue changing on later passes; the marker still starts
    // at the second. Compare notes on the beat, as in the default MIDI output, so `frame_timing` moves the same loops
    // rather than also moving those that differ only through rounding to frames. Also move the marker if a note at the
    // loop's start would play twice, under either timing mode.
    Simulation sim = Simulate(rom, info, song, opt.loops, opt.frame_timing, false);
    if (sim.plan.loops)
    {
        const Simulation beat = Simulate(rom, info, song, 3, false, false);
        if (!FirstPassRepeats(rom, info, beat) || !SeamRepeats(rom, info, beat) ||
            !SeamRepeats(rom, info, Simulate(rom, info, song, 3, true, false)))
        {
            sim = Simulate(rom, info, song, opt.loops, opt.frame_timing, true);
        }
    }
    sum.warnings = sim.warnings;

    NoteMaker maker(rom, info, sim);
    std::array<std::vector<Note>, kChannels> notes;
    for (int c = 0; c < kChannels; c++)
    {
        if (opt.track_mask & (1 << c))
        {
            notes[size_t(c)] = maker.Make(c);
            sum.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
    }

    sum.seconds = TickSeconds(sim, sim.plan.end);
    if (sim.plan.loops)
    {
        sum.loop_start = TickSeconds(sim, sim.plan.loop_start);
        sum.loop_end = TickSeconds(sim, sim.plan.loop_end);
    }
    sum.bpm = 60.0 / FrameSeconds(double(kTicksPerQuarter) / TempoMap(sim).front().second);

    sum.silent = sum.tracks == 0;
    if (sum.silent || !write)
    {
        sum.ok = true;
        return sum;
    }

    // The SoundFont's presets, in the song's file or the shared one, in the banks and programs that it gives the song.
    SoundfontBuilder own(rom);
    SoundfontBuilder& sf = shared ? *shared : own;
    const int count = int(maker.Programs().size());
    const std::vector<BankProgram> slots = sf.Banks().Place(opt.bank, count);
    if (int(slots.size()) < count)
    {
        sum.warnings.push_back("needs " + std::to_string(count) +
                               " presets, more than the SoundFont's banks have free" +
                               (shared ? "; convert the song without --single-sf2" : ""));
        return sum;
    }
    for (const std::string& w : AddPresets(sf, maker, slots))
    {
        sum.warnings.push_back(w);
    }

    const std::string title = rom.Title() + " #" + TwoDigits(song);
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) song %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(info.song_addresses[size_t(song)]), kProgramName);
    const std::string stem =
        Utf8(PathFromUtf8(opt.out_dir) / PathFromUtf8(SafeFileName(opt.base_name) + "_" + TwoDigits(song)));

    std::string error;
    sum.midi_path = stem + ".mid";
    if (!WriteMidi(sum.midi_path, title, about, sim, notes, slots, error))
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

} // namespace agbsf2::quintet
