// SPDX-License-Identifier: MIT

#include "psg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gba.h"
#include "sf2.h"

namespace agbsf2
{
namespace
{

// The PSG's channels that PsgModel treats apart: square 1 with its sweep, and the wave.
constexpr int kSquare1 = 0;
constexpr int kWave = 2;

// The longest noise sound rendered after its last change, in frames: about 4 seconds.
constexpr size_t kMaxNoiseFrames = 240;

// The CPU cycles between the frame sequencer's ticks of the envelope (64 Hz) and of the length counter (256 Hz).
constexpr uint64_t kEnvelopeTickCycles = kSecondCycles / 64;
constexpr uint64_t kLengthTickCycles = kSecondCycles / 256;

// The rate of the noise channel's LFSR clock at a divisor of 1 and no shift, in Hz.
constexpr uint32_t kNoiseClock = 524288;

// A model of the Game Boy's noise channel, enough to render what a driver plays on it: the LFSR, the envelope and the
// length counter. It gives a point of output at a time.
class NoiseChannel
{
public:
    // Gives `rate` points a second.
    explicit NoiseChannel(uint32_t rate) : point_cycles_(kSecondCycles / rate), clock_step_(kNoiseClock / rate)
    {
    }

    // Applies the registers: a restart if `nr43` has bit 15 set, and otherwise the new frequency settings alone.
    void Write(uint16_t nr41, uint16_t nr43)
    {
        settings_ = uint8_t(nr43);
        if (!(nr43 & 0x8000))
        {
            return;
        }

        // The envelope and the length counter count the 64 Hz and 256 Hz ticks of the frame sequencer. The sequencer
        // runs on its own, and a restart can come anywhere between two ticks. The model starts halfway between them.
        const uint8_t nr42 = uint8_t(nr41 >> 8);
        on_ = (nr42 & 0xF8) != 0;
        volume_ = nr42 >> 4;
        up_ = (nr42 & 8) != 0;
        step_ = nr42 & 7;
        envelope_cycles_ = kEnvelopeTickCycles / 2;
        length_ = 64 - (nr41 & 63);
        length_on_ = (nr43 & 0x4000) != 0;
        length_cycles_ = kLengthTickCycles / 2;
        lfsr_ = 0x7FFF;
        clock_ = 0;
    }

    // Returns the next point of output, from -1 to 1 in units of a full volume, and moves the channel on by a point.
    double Next()
    {
        const double out = on_ ? ((lfsr_ & 1) ? -1.0 : 1.0) * volume_ / 15.0 : 0.0;

        // The envelope steps at 64 Hz, and the length counter counts at 256 Hz.
        if (step_)
        {
            envelope_cycles_ += point_cycles_;
            if (envelope_cycles_ >= uint64_t(step_) * kEnvelopeTickCycles)
            {
                envelope_cycles_ = 0;
                if (up_ && volume_ < 15)
                {
                    volume_++;
                }
                else if (!up_ && volume_ > 0)
                {
                    volume_--;
                }
            }
        }
        if (length_on_)
        {
            length_cycles_ += point_cycles_;
            if (length_cycles_ >= kLengthTickCycles)
            {
                length_cycles_ = 0;
                if (--length_ <= 0)
                {
                    on_ = false;
                }
            }
        }

        // The LFSR's clock is 524288 / r / 2^(s+1) Hz. An r of 0 counts as 0.5. Each point then moves the clock on by
        // clock_step_ / ((r ? 2r : 1) << s) steps.
        const int shift = settings_ >> 4;
        const int ratio = settings_ & 7;
        if (shift < 14)
        {
            clock_ += clock_step_;
            const uint32_t den = uint32_t(ratio ? 2 * ratio : 1) << shift;
            for (; clock_ >= den; clock_ -= den)
            {
                lfsr_ = NextLfsr(lfsr_, (settings_ & 8) != 0);
            }
        }

        return out;
    }

    // Returns true if the channel can still make a sound without another restart.
    bool Sounding() const
    {
        return on_ && (volume_ > 0 || (up_ && step_));
    }

    // Returns true if its sound stays the same from here on.
    bool Steady() const
    {
        return !step_ && !length_on_;
    }

private:
    uint64_t point_cycles_; // the CPU cycles in a point
    uint32_t clock_step_;   // the steps of the LFSR's clock in a point, at a divisor of 1 and no shift
    bool on_ = false;
    int volume_ = 0;
    bool up_ = false;
    int step_ = 0;
    uint64_t envelope_cycles_ = 0;
    int length_ = 0;
    bool length_on_ = false;
    uint64_t length_cycles_ = 0;
    uint8_t settings_ = 0;
    uint32_t lfsr_ = 0x7FFF;
    uint32_t clock_ = 0;
};

} // namespace

std::string SquareName(int duty)
{
    return std::string("Square ") + kDutyNames[duty];
}

std::vector<int16_t> SquareCycle(int duty, int16_t amplitude)
{
    std::vector<int16_t> cycle;
    for (int step = 0; step < 8; step++)
    {
        const bool high = (kDutyPatterns[duty] >> (7 - step)) & 1;
        cycle.insert(cycle.end(), 8, high ? amplitude : int16_t(-amplitude));
    }

    return cycle;
}

PsgScale PsgScaleOf(uint16_t soundcnt_l, uint16_t soundcnt_h)
{
    static constexpr double kMix[4] = {0.25, 0.5, 1.0, 1.0};
    const double mix = kMix[soundcnt_h & 3];
    const int nr50 = soundcnt_l & 0xFF;
    return {mix * ((nr50 >> 4 & 7) + 1) / 8.0, mix * ((nr50 & 7) + 1) / 8.0};
}

double WaveShare(uint16_t sound3cnt_h)
{
    static constexpr double kShares[4] = {0, 1, 0.5, 0.25};
    return (sound3cnt_h & 0x8000) ? 0.75 : kShares[(sound3cnt_h >> 13) & 3];
}

PsgModel::PsgModel(int channel) : channel_(channel)
{
}

int PsgModel::Frame(const PsgRegisters& r)
{
    // NR10 sets the sweep as it goes.
    sweep_time_ = (r.nr10 >> 4) & 7;
    sweep_shift_ = r.nr10 & 7;
    sweep_down_ = (r.nr10 & 8) != 0;

    // A restart starts the channel again, and a frequency written without one plays until the sweep's next step.
    if (r.restarted)
    {
        Restart(r.envelope, r.frequency);
    }
    else if ((r.frequency & 0x7FF) != written_)
    {
        frequency_ = r.frequency & 0x7FF;
    }
    written_ = r.frequency & 0x7FF;

    // A channel whose DAC is off is off at once.
    if (channel_ == kWave ? !(r.nr30 & 0x80) : !(r.envelope & 0xF800))
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

void PsgModel::Restart(uint16_t envelope, uint16_t frequency)
{
    on_ = true;
    volume_ = channel_ == kWave ? 15 : envelope >> 12;
    up_ = (envelope & 0x0800) != 0;
    step_ = channel_ == kWave ? 0 : (envelope >> 8) & 7;
    envelope_ticks_ = 0;
    length_on_ = (frequency & 0x4000) != 0;
    length_ = channel_ == kWave ? 256 - (envelope & 0xFF) : 64 - (envelope & 63);
    frequency_ = frequency & 0x7FF;

    // The sweep starts from the new frequency. With a shift, a sweep up that would at once go past the top turns the
    // channel off.
    shadow_ = frequency_;
    sweeping_ = channel_ == kSquare1 && (sweep_time_ || sweep_shift_);
    sweep_ticks_ = 0;
    if (sweeping_ && sweep_shift_ && SweepNext() > 0x7FF)
    {
        on_ = false;
    }
}

int PsgModel::SweepNext() const
{
    const int change = shadow_ >> sweep_shift_;
    return sweep_down_ ? shadow_ - change : shadow_ + change;
}

void PsgModel::Tick()
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

    // A sweep step past the top turns the channel off. Otherwise, with a shift, the new frequency plays, and the step
    // after it is checked at once.
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

PsgFeed::PsgFeed(int channel, std::function<PsgRegisters(uint32_t)> registers)
    : registers_(std::move(registers)), model_(channel)
{
}

int PsgFeed::Volume(uint32_t f)
{
    int volume = -1;
    for (; fed_ <= f; fed_++)
    {
        volume = model_.Frame(registers_(fed_));
    }

    return volume;
}

int WaveShapes::Number(const WaveShape& shape)
{
    auto it = numbers_.find(shape);
    if (it != numbers_.end())
    {
        return it->second;
    }

    const int number = int(list_.size());
    list_.push_back(shape);
    numbers_[shape] = number;

    return number;
}

void NoiseSound::AddFrame(uint16_t nr41, uint16_t nr43, bool restarted)
{
    const bool restart = frames.empty() || restarted;
    frames.emplace_back(nr41, uint16_t((nr43 & 0x7FFF) | (restart ? 0x8000 : 0)));
}

void NoiseSound::Trim()
{
    while (frames.size() > 1 && frames.back() == std::make_pair(frames[frames.size() - 2].first,
                                                                uint16_t(frames[frames.size() - 2].second & 0x7FFF)))
    {
        frames.pop_back();
    }
}

sf2::Sample NoiseSequence(bool narrow, double level)
{
    sf2::Sample s;
    s.name = narrow ? "Noise 7-bit" : "Noise 15-bit";
    s.sample_rate = kNoiseSequenceRate;
    s.original_key = 60;
    const uint32_t points = narrow ? 127 * 8 : 32767;
    const int16_t amplitude = int16_t(std::lround(32767 * level));
    uint32_t lfsr = 0x7FFF;
    for (uint32_t i = 0; i < points; i++)
    {
        s.data.push_back((lfsr & 1) ? int16_t(-amplitude) : amplitude);
        lfsr = NextLfsr(lfsr, narrow);
    }
    s.loop_start = 0;
    s.loop_end = points;
    AddLoopGuard(s);

    return s;
}

int WaveKeyOffset(const WaveShape& shape)
{
    // A pattern whose first half matches its second half sounds an octave higher, and so on.
    int offset = 0;
    for (size_t n = shape.steps.size(); n >= 2 && n % 2 == 0; n /= 2)
    {
        bool repeats = true;
        for (size_t i = 0; i < n / 2; i++)
        {
            repeats = repeats && shape.steps[i] == shape.steps[i + n / 2];
        }
        if (!repeats)
        {
            break;
        }

        offset += 12;
    }

    return offset;
}

PsgSamples::PsgSamples(uint32_t noise_rate) : noise_rate_(noise_rate)
{
}

int PsgSamples::Square(Sf2File& file, int duty)
{
    duty &= 3;
    if (auto it = squares_.find(duty); it != squares_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    out.name = SquareName(duty);
    SetCycle(out, SquareCycle(duty, 32767));
    out.sample_rate = uint32_t(64 * kMiddleC + 0.5);
    out.original_key = 60;

    file.samples.push_back(std::move(out));
    return squares_[duty] = int(file.samples.size()) - 1;
}

int PsgSamples::Wave(Sf2File& file, const WaveShape& shape)
{
    if (auto it = waves_.find(shape); it != waves_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    char name[32];
    std::snprintf(name, sizeof name, "Wave %d", int(waves_.size()));
    out.name = name;

    // 4 points a step. The pattern is stored at half scale, without its mean: a channel at 100% plays it at level
    // 4 × 15, so that a pattern from 0 to 15 is as loud as a square at volume 15.
    double mean = 0;
    for (uint8_t v : shape.steps)
    {
        mean += v;
    }
    mean /= double(shape.steps.size());

    std::vector<int16_t> cycle;
    for (uint8_t v : shape.steps)
    {
        cycle.insert(cycle.end(), 4, int16_t((v - mean) / 15.0 * 32767 + (v >= mean ? 0.5 : -0.5)));
    }

    SetCycle(out, cycle);
    out.sample_rate = uint32_t(double(cycle.size()) * kMiddleC + 0.5);
    out.original_key = uint8_t(60 + WaveKeyOffset(shape));

    file.samples.push_back(std::move(out));
    return waves_[shape] = int(file.samples.size()) - 1;
}

int PsgSamples::Noise(Sf2File& file, const NoiseSound& sound)
{
    if (auto it = noises_.find(sound); it != noises_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    char name[32];
    std::snprintf(name, sizeof name, "Noise %d", int(noises_.size()));
    out.name = name;
    out.sample_rate = noise_rate_;
    out.original_key = 60;

    // Each of the note's frames applies the driver's settings. After the last, the channel plays on until it falls
    // silent for good or its sound stops changing.
    NoiseChannel noise(noise_rate_);
    const uint64_t point_cycles = kSecondCycles / noise_rate_;
    uint64_t cycles = 0;
    uint64_t points = 0;
    for (size_t frame = 0; frame < sound.frames.size() + kMaxNoiseFrames; frame++)
    {
        if (frame < sound.frames.size())
        {
            noise.Write(sound.frames[frame].first, sound.frames[frame].second);
        }

        cycles += kFrameCycles;
        for (; points * point_cycles < cycles; points++)
        {
            out.data.push_back(int16_t(noise.Next() * 32767));
        }

        if (frame + 1 >= sound.frames.size() && (!noise.Sounding() || noise.Steady()))
        {
            break;
        }
    }

    // A sound that goes on unchanged loops its last 2 frames, and the rest end in a few points of silence. A part of a
    // point counts as a whole one in the frames' points.
    if (noise.Sounding())
    {
        const size_t frame_points = (kFrameCycles + point_cycles - 1) / point_cycles;
        const size_t loop = out.data.size() >= 2 * frame_points ? out.data.size() - 2 * frame_points : 0;
        out.loop_start = uint32_t(loop);
        out.loop_end = uint32_t(out.data.size());
        AddLoopGuard(out);
    }
    else
    {
        out.data.insert(out.data.end(), kLoopGuard, int16_t(0));
    }

    file.samples.push_back(std::move(out));
    return noises_[sound] = int(file.samples.size()) - 1;
}

NamedZones PsgSamples::SquareInstrument(Sf2File& file, int first, int frames, int second, int cents)
{
    // The first sample is made before the second. Their numbers then don't depend on the order in which the compiler
    // evaluates a call's arguments.
    const int first_sample = Square(file, first);
    const int second_sample = Square(file, second);
    NamedZones made;
    made.zones = SwitchZones(first_sample, frames, second_sample, cents);
    made.name = SquareName(first & 3);
    if (frames)
    {
        made.name += std::string(" to ") + kDutyNames[second & 3];
    }

    return made;
}

NamedZones PsgSamples::WaveInstrument(Sf2File& file, const WaveShapes& shapes, int first, int frames, int second,
                                      int cents)
{
    // As for a square, the first sample is made before the second.
    const int first_sample = Wave(file, shapes[first]);
    const int second_sample = Wave(file, shapes[second]);
    NamedZones made;
    made.zones = SwitchZones(first_sample, frames, second_sample, cents);

    // The note's key allows for how often the first shape repeats. The second shape plays from the first's root key, at
    // the same rate.
    const int first_offset = WaveKeyOffset(shapes[first]);
    const int second_offset = WaveKeyOffset(shapes[second]);
    if (made.zones.size() == 2 && first_offset != second_offset)
    {
        made.zones[1].generators.insert(made.zones[1].generators.end() - 1,
                                        sf2::Generator::Value(sf2::kOverridingRootKey, 60 + first_offset));
    }

    char text[64];
    std::snprintf(text, sizeof text, frames ? "Wave %d to %d" : "Wave %d", first, second);
    made.name = text;

    return made;
}

} // namespace agbsf2
