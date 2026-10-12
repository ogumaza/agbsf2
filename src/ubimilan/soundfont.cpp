// SPDX-License-Identifier: MIT

// SoundFont instruments for Ubisoft Milan's driver.

#include "ubimilan/soundfont.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace supergbamidi::ubimilan
{
namespace
{

// The frequency of middle C, which the PSG's samples are tuned to.
constexpr double kMiddleC = 261.6255653005986;

// The points after a loop's end that repeat its start, for players that read past the end when they interpolate.
constexpr uint32_t kLoopGuard = 8;

// The release of every note: the driver stops a voice, or silences a PSG channel, at once.
constexpr int kInstantRelease = -12000;

// The rate of the noise sample, which each zone tunes to the noise's clock.
constexpr uint32_t kNoiseRate = 32768;

// Game Boy square duty waveforms, 8 steps, most significant bit first: 12.5%, 25%, 50% and 75%.
constexpr uint8_t kDutyPatterns[4] = {0x01, 0x81, 0x87, 0x7E};

const char* const kPsgNames[kPsgChannels] = {"Square 1", "Square 2", "Wave"};

// The SoundFont's samples, each added the first time a zone uses it.
class SampleCache
{
public:
    SampleCache(const Rom& rom, const DriverInfo& info, Sf2File& file) : rom_(rom), info_(info), file_(file)
    {
    }

    // Returns the index of the kit's sample resource at `address`, or -1 if it can't be read.
    int Kit(uint32_t address)
    {
        const auto cached = kit_.find(address);
        if (cached != kit_.end())
        {
            return cached->second;
        }

        GameSample game;
        if (!ReadSample(rom_, address, game))
        {
            return kit_[address] = -1;
        }

        sf2::Sample s;
        char name[24];
        std::snprintf(name, sizeof name, "Sample %06X", unsigned(address & 0xFFFFFF));
        s.name = name;
        s.sample_rate = uint32_t(std::lround(PointRate(info_)));
        s.original_key = 60;

        // The mixer reads 8 points at a time and checks for a loop's end after each 8, so a loop lasts a whole number
        // of 8 points from its start, and its last points can come from after the loop's end in the ROM.
        uint32_t points = game.length;
        if (game.loop && game.loop_end > game.loop_start)
        {
            const uint32_t length = (game.loop_end - game.loop_start + kRunPoints - 1) / kRunPoints * kRunPoints;
            points = game.loop_start + length;
            s.loop_start = game.loop_start;
            s.loop_end = points;
        }
        for (uint32_t i = 0; i < points; i++)
        {
            const uint32_t at = game.data + i;
            s.data.push_back(int16_t(rom_.Contains(at) ? rom_.S8(at) * 256 : 0));
        }

        if (Looped(s))
        {
            for (uint32_t i = 0; i < kLoopGuard; i++)
            {
                s.data.push_back(s.data[s.loop_start + i % (s.loop_end - s.loop_start)]);
            }
        }

        file_.samples.push_back(s);

        return kit_[address] = int(file_.samples.size()) - 1;
    }

    // Returns the index of a square wave of duty `duty`, a cycle of 64 points at middle C.
    int Square(int duty)
    {
        const auto cached = squares_.find(duty);
        if (cached != squares_.end())
        {
            return cached->second;
        }

        static const char* const kDutyNames[4] = {"12.5%", "25%", "50%", "75%"};
        sf2::Sample s;
        s.name = std::string("Square ") + kDutyNames[duty];
        const int16_t amplitude = int16_t(std::lround(32767 * kPsgLevel));
        for (int step = 0; step < 8; step++)
        {
            const bool high = (kDutyPatterns[duty] >> (7 - step)) & 1;
            s.data.insert(s.data.end(), 8, high ? amplitude : int16_t(-amplitude));
        }
        Cycle(s);
        file_.samples.push_back(s);

        return squares_[duty] = int(file_.samples.size()) - 1;
    }

    // Returns the index of the wave channel's program `program`: both banks of the wave RAM, 64 points of 4 bits, high
    // half first, at middle C.
    int Wave(int program)
    {
        const auto cached = waves_.find(program);
        if (cached != waves_.end())
        {
            return cached->second;
        }

        const uint32_t address = info_.wave_table + 32 * uint32_t(program);
        if (!info_.wave_table || !rom_.Contains(address, 32))
        {
            return waves_[program] = -1;
        }

        sf2::Sample s;
        char name[24];
        std::snprintf(name, sizeof name, "Wave %d", program);
        s.name = name;
        for (uint32_t i = 0; i < 64; i++)
        {
            const uint8_t byte = rom_.U8(address + i / 2);
            const int point = i % 2 ? byte & 0xF : byte >> 4;
            s.data.push_back(int16_t(std::lround((point - 7.5) * 32767 / 7.5 * kPsgLevel)));
        }
        Cycle(s);
        file_.samples.push_back(s);

        return waves_[program] = int(file_.samples.size()) - 1;
    }

    // Returns the index of the noise generator's whole sequence, a point for each step, looped: the 15-bit one, or the
    // 7-bit one repeated so that its loop isn't too short.
    int Noise(bool narrow)
    {
        const auto cached = noises_.find(narrow);
        if (cached != noises_.end())
        {
            return cached->second;
        }

        sf2::Sample s;
        s.name = narrow ? "Noise 7-bit" : "Noise 15-bit";
        s.sample_rate = kNoiseRate;
        s.original_key = 60;

        const uint32_t points = narrow ? 127 * 8 : 32767;
        const int16_t amplitude = int16_t(std::lround(32767 * kPsgLevel));
        uint32_t lfsr = 0x7FFF;
        for (uint32_t i = 0; i < points + kLoopGuard; i++)
        {
            s.data.push_back((lfsr & 1) ? int16_t(-amplitude) : amplitude);
            const uint32_t bit = (lfsr ^ (lfsr >> 1)) & 1;
            lfsr = (lfsr >> 1) | (bit << 14);
            if (narrow)
            {
                lfsr = (lfsr & ~0x40u) | (bit << 6);
            }
        }

        s.loop_start = 0;
        s.loop_end = points;
        file_.samples.push_back(s);

        return noises_[narrow] = int(file_.samples.size()) - 1;
    }

private:
    // Makes `s` a looped cycle at middle C, with the guard points after it.
    static void Cycle(sf2::Sample& s)
    {
        const uint32_t points = uint32_t(s.data.size());
        for (uint32_t i = 0; i < kLoopGuard; i++)
        {
            s.data.push_back(s.data[i % points]);
        }
        s.sample_rate = uint32_t(std::lround(points * kMiddleC));
        s.original_key = 60;
        s.loop_start = 0;
        s.loop_end = points;
    }

    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File& file_;
    std::map<uint32_t, int> kit_;
    std::map<int, int> squares_;
    std::map<int, int> waves_;
    std::map<bool, int> noises_;
};

// Returns an instrument whose global zone makes velocity scale the level in a straight line, as the driver does. The
// default curve squares it, and this takes its place.
sf2::Instrument LinearInstrument(const std::string& name)
{
    sf2::Instrument si;
    si.name = name;
    sf2::Zone global;
    global.modulators.push_back(
        {sf2::kNoteOnVelocity | sf2::kNegative | sf2::kConcave, sf2::kInitialAttenuation, 480, 0, 0});
    si.zones.push_back(global);

    return si;
}

// Returns a zone for keys `lo` to `hi` that plays sample `sample` with its root at key `root`, and `cents` sharper.
sf2::Zone SampleZone(int lo, int hi, int sample, int root, int cents, bool loop)
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, lo, hi));
    z.generators.push_back(sf2::Generator::Value(sf2::kReleaseVolEnv, kInstantRelease));
    if (cents / 100)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kCoarseTune, cents / 100));
    }
    if (cents % 100)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kFineTune, cents % 100));
    }
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, loop ? 1 : 0));
    z.generators.push_back(sf2::Generator::Value(sf2::kOverridingRootKey, root));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

    return z;
}

// Returns the cents by which a noise sample at kNoiseRate has to be raised to step at the clock that NR43 sets: 524288
// Hz divided by its divisor, of which 0 stands for a half, and by 2 to the power of its shift and 1.
int NoiseCents(uint8_t control)
{
    const int divisor = control & 7;
    const int shift = control >> 4;
    const double clock = 524288.0 / (divisor ? divisor : 0.5) / std::pow(2.0, shift + 1);
    return int(std::lround(1200 * std::log2(clock / kNoiseRate)));
}

} // namespace

void InstrumentSet::AddKitKey(int program, int key, uint32_t sample)
{
    kit_keys_[program][key] = sample;
}

void InstrumentSet::AddNoiseKey(int program, int key, uint8_t control)
{
    noise_keys_[program][key] = control;
}

void InstrumentSet::AddPsg(int channel, int program, int setting)
{
    psg_[{channel, program}] = setting;
}

std::vector<std::string> InstrumentSet::Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const
{
    std::vector<std::string> warnings;
    SampleCache samples(rom, info, file);

    // Each of the kit's offsets: a preset whose keys play their samples at the rate the mixer plays them, or the noise.
    std::map<int, bool> offsets;
    for (const auto& [program, keys] : kit_keys_)
    {
        offsets[program] = true;
    }
    for (const auto& [program, keys] : noise_keys_)
    {
        offsets[program] = true;
    }

    for (const auto& [program, unused] : offsets)
    {
        char name[24];
        std::snprintf(name, sizeof name, "Kit %d", program);
        sf2::Instrument si = LinearInstrument(name);

        const auto noise = noise_keys_.find(program);
        if (noise != noise_keys_.end())
        {
            for (const auto& [key, control] : noise->second)
            {
                si.zones.push_back(SampleZone(key, key, samples.Noise(control & 8), key, NoiseCents(control), true));
            }
        }

        const auto kit = kit_keys_.find(program);
        if (kit != kit_keys_.end())
        {
            for (const auto& [key, address] : kit->second)
            {
                const int sample = samples.Kit(address);
                if (sample < 0)
                {
                    continue;
                }

                si.zones.push_back(SampleZone(key, key, sample, key, 0, Looped(file.samples[size_t(sample)])));
            }
        }

        if (si.zones.size() < 2)
        {
            warnings.push_back(std::string(name) + " has nothing to play");
            continue;
        }

        file.instruments.push_back(si);
        file.presets.push_back(
            MakePreset(name, uint16_t(kKitBank), uint16_t(program), int(file.instruments.size()) - 1));
    }

    // Each PSG instrument: its square's duty or its wave, over all the keys, at the pitch that the key's frequency
    // setting gives.
    for (const auto& [which, setting] : psg_)
    {
        const auto [channel, program] = which;
        char name[32];
        std::snprintf(name, sizeof name, "%s %d", kPsgNames[channel], program);
        const int sample = channel == 2 ? samples.Wave(setting) : samples.Square((setting >> 6) & 3);
        if (sample < 0)
        {
            warnings.push_back(std::string(name) + " has nothing to play");
            continue;
        }

        sf2::Instrument si = LinearInstrument(name);
        si.zones.push_back(SampleZone(0, 127, sample, 60, 0, true));
        file.instruments.push_back(si);
        file.presets.push_back(
            MakePreset(name, uint16_t(channel), uint16_t(program), int(file.instruments.size()) - 1));
    }

    return warnings;
}

} // namespace supergbamidi::ubimilan
