// SPDX-License-Identifier: MIT

#include "brownie/soundfont.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "brownie/driver.h"

namespace agbsf2::brownie
{
namespace
{

// Loop-start copies after a loop's end, so that players can interpolate across it.
constexpr int kGuardPoints = 8;

// Game Boy square duty waveforms, 8 steps, most significant bit first.
constexpr uint8_t kDutyPatterns[4] = {0x01, 0x81, 0x87, 0x7E};

// Middle C, which a square or wave sample plays on its root key.
constexpr double kMiddleC = 261.6255653005986;

// The rate at which noise is rendered: the PSG's output rate under the BIOS's SOUNDBIAS setting, which the driver
// keeps.
constexpr uint32_t kNoiseRate = 32768;

// The longest noise sound rendered after its last change, in frames: about 4 seconds.
constexpr size_t kMaxNoiseFrames = 240;

// The CPU cycles in a point of the noise sample.
constexpr uint64_t kNoisePointCycles = kSecondCycles / kNoiseRate;

// The points of noise in a frame, rounded up.
constexpr size_t kNoiseFramePoints = (kFrameCycles + kNoisePointCycles - 1) / kNoisePointCycles;

// The steps of the noise channel's LFSR clock in each point of the noise sample, at a divisor of 1 and no shift: its
// 524288 Hz over the noise rate.
constexpr uint32_t kNoiseClockStep = 524288 / kNoiseRate;

// The CPU cycles between the frame sequencer's ticks of the envelope (64 Hz) and of the length counter (256 Hz).
constexpr uint64_t kEnvelopeTickCycles = kSecondCycles / 64;
constexpr uint64_t kLengthTickCycles = kSecondCycles / 256;

// Stores one waveform cycle as three copies looping over the middle one, so players get lead-in and lead-out points
// around the loop.
void SetCycle(sf2::Sample& out, const std::vector<int16_t>& cycle)
{
    out.data.clear();
    for (int i = 0; i < 3; i++)
    {
        out.data.insert(out.data.end(), cycle.begin(), cycle.end());
    }

    out.loop_start = uint32_t(cycle.size());
    out.loop_end = uint32_t(2 * cycle.size());
}

// A model of the Game Boy's noise channel, enough to render what the driver plays on it: the LFSR, the envelope and the
// length counter.
class NoiseChannel
{
public:
    // Applies the registers: a restart if `nr43` has bit 15 set, and otherwise the new frequency settings alone.
    void Write(uint16_t nr41, uint16_t nr43)
    {
        settings_ = uint8_t(nr43);
        if (!(nr43 & 0x8000))
        {
            return;
        }

        // The envelope and the length counter count the 64 Hz and 256 Hz ticks of the frame sequencer, which runs on
        // its own, so a restart can come anywhere between two ticks. The model starts halfway between them.
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
            envelope_cycles_ += kNoisePointCycles;
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
            length_cycles_ += kNoisePointCycles;
            if (length_cycles_ >= kLengthTickCycles)
            {
                length_cycles_ = 0;
                if (--length_ <= 0)
                {
                    on_ = false;
                }
            }
        }

        // The LFSR's clock is 524288 / r / 2^(s+1) Hz, with r = 0 counting as 0.5, so each point moves it on by
        // kNoiseClockStep / ((r ? 2r : 1) << s) steps.
        const int shift = settings_ >> 4;
        const int ratio = settings_ & 7;
        if (shift < 14)
        {
            clock_ += kNoiseClockStep;
            const uint32_t den = uint32_t(ratio ? 2 * ratio : 1) << shift;
            for (; clock_ >= den; clock_ -= den)
            {
                const uint32_t bit = (lfsr_ ^ (lfsr_ >> 1)) & 1;
                lfsr_ = (lfsr_ >> 1) | (bit << 14);
                if (settings_ & 8)
                {
                    lfsr_ = (lfsr_ & ~0x40u) | (bit << 6);
                }
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

SoundfontBuilder::SoundfontBuilder(const Rom& rom, uint32_t rate) : rom_(rom), rate_(rate)
{
}

int SoundfontBuilder::WaveKeyOffset(const WaveShape& shape)
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

int SoundfontBuilder::SquareSample(int duty)
{
    duty &= 3;
    if (auto it = squares_.find(duty); it != squares_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    out.name = std::string("Square ") + kDutyNames[duty];

    // 8 points a step, 64 a cycle.
    std::vector<int16_t> cycle;
    for (int step = 0; step < 8; step++)
    {
        const bool high = (kDutyPatterns[duty] >> (7 - step)) & 1;
        cycle.insert(cycle.end(), 8, int16_t(high ? 32767 : -32767));
    }

    SetCycle(out, cycle);
    out.sample_rate = uint32_t(64 * kMiddleC + 0.5);
    out.original_key = 60;

    file_.samples.push_back(std::move(out));
    return squares_[duty] = int(file_.samples.size()) - 1;
}

int SoundfontBuilder::WaveSample(const WaveShape& shape)
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

    file_.samples.push_back(std::move(out));
    return waves_[shape] = int(file_.samples.size()) - 1;
}

int SoundfontBuilder::NoiseSample(const NoiseSound& sound)
{
    if (auto it = noises_.find(sound); it != noises_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    char name[32];
    std::snprintf(name, sizeof name, "Noise %d", int(noises_.size()));
    out.name = name;
    out.sample_rate = kNoiseRate;
    out.original_key = 60;

    // Each of the note's frames applies the driver's settings; after the last, the channel plays on until it falls
    // silent for good or its sound stops changing.
    NoiseChannel noise;
    uint64_t cycles = 0;
    uint64_t points = 0;
    for (size_t frame = 0; frame < sound.frames.size() + kMaxNoiseFrames; frame++)
    {
        if (frame < sound.frames.size())
        {
            noise.Write(sound.frames[frame].first, sound.frames[frame].second);
        }

        cycles += kFrameCycles;
        for (; points * kNoisePointCycles < cycles; points++)
        {
            out.data.push_back(int16_t(noise.Next() * 32767));
        }

        if (frame + 1 >= sound.frames.size() && (!noise.Sounding() || noise.Steady()))
        {
            break;
        }
    }

    // A sound that goes on unchanged loops its last 2 frames, and the rest end in a few points of silence.
    if (noise.Sounding())
    {
        const size_t loop = out.data.size() >= 2 * kNoiseFramePoints ? out.data.size() - 2 * kNoiseFramePoints : 0;
        out.loop_start = uint32_t(loop);
        out.loop_end = uint32_t(out.data.size());
        for (int i = 0; i < kGuardPoints; i++)
        {
            out.data.push_back(out.data[loop + size_t(i)]);
        }
    }
    else
    {
        out.data.insert(out.data.end(), kGuardPoints, int16_t(0));
    }

    file_.samples.push_back(std::move(out));
    return noises_[sound] = int(file_.samples.size()) - 1;
}

int SoundfontBuilder::GameSampleIndex(const GameSample& sample)
{
    if (auto it = samples_.find(sample); it != samples_.end())
    {
        return it->second;
    }

    if (sample.end <= sample.start || !rom_.Contains(sample.start, sample.end - sample.start))
    {
        return samples_[sample] = -1;
    }

    sf2::Sample out;
    char name[32];
    std::snprintf(name, sizeof name, "Sample %08X", unsigned(sample.start));
    out.name = name;
    out.sample_rate = rate_;
    out.original_key = 60;

    // The samples are 8-bit and signed. One that loops back to a point within it loops there; the mixer plays one that
    // loops back outside it from there, which the SoundFont leaves out.
    const uint32_t length = sample.end - sample.start;
    for (uint32_t i = 0; i < length; i++)
    {
        out.data.push_back(int16_t(rom_.S8(sample.start + i) * 256));
    }

    if (sample.loop >= sample.start && sample.loop < sample.end)
    {
        out.loop_start = sample.loop - sample.start;
        out.loop_end = length;
        for (uint32_t i = 0; i < kGuardPoints; i++)
        {
            out.data.push_back(out.data[out.loop_start + i % (length - out.loop_start)]);
        }
    }
    else
    {
        out.data.insert(out.data.end(), kGuardPoints, int16_t(0));
    }

    file_.samples.push_back(std::move(out));
    return samples_[sample] = int(file_.samples.size()) - 1;
}

int SoundfontBuilder::AddInstrument(const std::string& name, std::vector<sf2::Zone> zones)
{
    sf2::Instrument inst;
    inst.name = name;
    inst.zones = std::move(zones);
    file_.instruments.push_back(std::move(inst));

    return int(file_.instruments.size()) - 1;
}

void SoundfontBuilder::AddPreset(const std::string& name, int bank, int program, int instrument)
{
    file_.presets.push_back(MakePreset(name, uint16_t(bank), uint16_t(program), instrument));
}

} // namespace agbsf2::brownie
