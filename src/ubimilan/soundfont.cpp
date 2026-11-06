// SPDX-License-Identifier: MIT

// SoundFont instruments for Ubisoft Milan's driver.

#include "ubimilan/soundfont.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "psg.h"
#include "rom.h"
#include "sf2.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{
namespace
{

// The rate of the noise's drums: the PSG's under SOUNDBIAS. The sound init sets its bit 14, which plays the PSG at
// 65536 Hz.
constexpr uint32_t kNoiseRate = 65536;

// The entries of the frequency table in both games with the driver. The wave table follows it.
constexpr int kFrequencyEntries = 104;

// The keys below the first entry of the frequency table on the squares and on the wave channel.
constexpr int kSquareBaseKey = 36;
constexpr int kWaveBaseKey = 12;

// Returns the pitch of MIDI key `key` in equal temperament, in Hz.
double KeyHz(int key)
{
    return kMiddleC * std::pow(2.0, (key - 60) / 12.0);
}

// Returns the cents from `hz` to the pitch of a square channel at entry `index` of the frequency table, or 0 for an
// entry past the table's ends or one that doesn't play.
int TableCents(const Rom& rom, const DriverInfo& info, int index, double hz)
{
    if (index < 0 || index >= kFrequencyEntries || !info.frequency_table)
    {
        return 0;
    }

    const int setting = rom.U16(info.frequency_table + 2 * uint32_t(index)) & 0x7FF;
    return setting ? SettingCents(setting, hz) : 0;
}

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
            LengthenLoop(s);
            AddLoopGuard(s);
        }

        file_.samples.push_back(s);

        return kit_[address] = int(file_.samples.size()) - 1;
    }

    // Returns the PSG's samples, which the cache makes on first use.
    PsgSamples& Psg()
    {
        return psg_;
    }

private:
    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File& file_;
    std::map<uint32_t, int> kit_;
    PsgSamples psg_{kNoiseRate};
};

// Returns a zone for keys `lo` to `hi` that plays sample `sample` with its root at key `root`, and `cents` sharper. Its
// release is instant.
sf2::Zone InstantReleaseZone(int lo, int hi, int sample, int root, int cents, bool loop)
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, lo, hi));
    // The driver stops a voice, or silences a PSG channel, at once.
    z.generators.push_back(sf2::Generator::Value(sf2::kReleaseVolEnv, kInstant));
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

} // namespace

const char* ChannelName(int channel)
{
    static const char* const kPsgNames[kPsgChannels] = {"Square 1", "Square 2", "Wave"};

    return channel == kKitChannel ? "Kit" : kPsgNames[channel];
}

int SquareCents(const Rom& rom, const DriverInfo& info, int key)
{
    return TableCents(rom, info, key - kSquareBaseKey, KeyHz(key));
}

int WaveCents(const Rom& rom, const DriverInfo& info, int key, int offset)
{
    // The zone plays the wave's 64 steps at the pitch of key `key` - `offset`. The channel plays them at a quarter of a
    // square's pitch at the same setting.
    return TableCents(rom, info, key - kWaveBaseKey, 4 * KeyHz(key - offset));
}

WaveShape ProgramWave(const Rom& rom, const DriverInfo& info, int program)
{
    WaveShape shape;
    const uint32_t address = info.wave_table + 32 * uint32_t(program);
    for (uint32_t i = 0; i < 32; i++)
    {
        const uint8_t byte = rom.U8(address + i);
        shape.steps.push_back(uint8_t(byte >> 4));
        shape.steps.push_back(uint8_t(byte & 15));
    }

    return shape;
}

void InstrumentSet::AddKitKey(int program, int key, uint32_t sample, uint32_t skip)
{
    kit_keys_[program][key] = {sample, skip};
}

int InstrumentSet::AddNoise(const NoiseSound& sound)
{
    return noises_.try_emplace(sound, int(noises_.size())).first->second;
}

void InstrumentSet::AddSquare(int channel, int program, uint8_t duty)
{
    squares_[{channel, program}] = duty;
}

void InstrumentSet::AddWave(int program)
{
    waves_.insert(program);
}

std::vector<std::string> InstrumentSet::Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const
{
    std::vector<std::string> warnings;
    SampleCache samples(rom, info, file);

    // Each of the kit's offsets: a preset whose keys play their samples at the rate the mixer plays them.
    for (const auto& [program, keys] : kit_keys_)
    {
        char name[24];
        std::snprintf(name, sizeof name, "%s %d", ChannelName(kKitChannel), program);
        sf2::Instrument si = MakeInstrument(name);
        for (const auto& [key, source] : keys)
        {
            const int sample = samples.Kit(source.first);
            if (sample < 0)
            {
                continue;
            }

            sf2::Zone zone = InstantReleaseZone(key, key, sample, key, 0, Looped(file.samples[size_t(sample)]));
            if (source.second)
            {
                zone.generators.insert(
                    zone.generators.begin() + 1,
                    {sf2::Generator::Value(sf2::kStartAddrsOffset, int(source.second % 32768)),
                     sf2::Generator::Value(sf2::kStartAddrsCoarseOffset, int(source.second / 32768))});
            }
            si.zones.push_back(zone);
        }

        if (si.zones.size() < 2)
        {
            warnings.push_back(std::string(name) + " has nothing to play");
            continue;
        }

        file.instruments.push_back(si);
        file.presets.push_back(
            MakePreset(name, uint16_t(PresetBank(kKitChannel)), uint16_t(program), int(file.instruments.size()) - 1));
    }

    // Each PSG instrument, first the squares, then the wave: a square of its duty or its wave at full volume, over all
    // the keys, at the pitch of the frequency setting for each key. CC11 sets the volume of each frame.
    const auto add_psg = [&](int channel, int program, int sample, const KeyTuning& tuning)
    {
        char name[32];
        std::snprintf(name, sizeof name, "%s %d", ChannelName(channel), program);
        if (sample < 0)
        {
            warnings.push_back(std::string(name) + " has nothing to play");
            return;
        }

        sf2::Instrument si = MakeInstrument(name);
        for (const auto& [lo, hi] : TuningRuns(tuning))
        {
            const int root = file.samples[size_t(sample)].original_key;
            si.zones.push_back(InstantReleaseZone(lo, hi, sample, root, tuning[size_t(lo)], true));
        }
        file.instruments.push_back(si);
        file.presets.push_back(
            MakePreset(name, uint16_t(PresetBank(channel)), uint16_t(program), int(file.instruments.size()) - 1));
    };
    KeyTuning square_tuning;
    for (int key = 0; key < int(square_tuning.size()); key++)
    {
        square_tuning[size_t(key)] = SquareCents(rom, info, key);
    }
    for (const auto& [which, duty] : squares_)
    {
        add_psg(which.first, which.second, samples.Psg().Square(file, (duty >> 6) & 3), square_tuning);
    }
    for (int program : waves_)
    {
        const uint32_t address = info.wave_table + 32 * uint32_t(program);
        if (!info.wave_table || !rom.Contains(address, 32))
        {
            add_psg(kWaveChannel, program, -1, square_tuning);
            continue;
        }

        const WaveShape shape = ProgramWave(rom, info, program);
        KeyTuning tuning;
        for (int key = 0; key < int(tuning.size()); key++)
        {
            tuning[size_t(key)] = WaveCents(rom, info, key, WaveKeyOffset(shape));
        }
        add_psg(kWaveChannel, program, samples.Psg().Wave(file, shape), tuning);
    }

    // The noise's drums: each sound that a noise note makes, with the channel's volume, its envelope and its length.
    std::vector<const NoiseSound*> drums(noises_.size());
    for (const auto& [sound, number] : noises_)
    {
        drums[size_t(number)] = &sound;
    }
    for (size_t first = 0; first < drums.size(); first += kKitDrums)
    {
        const int program = int(first) / kKitDrums;
        char name[24];
        std::snprintf(name, sizeof name, program ? "Noise drums %d" : "Noise drums", program);
        sf2::Instrument si = MakeInstrument(name);
        for (size_t i = first; i < drums.size() && i < first + kKitDrums; i++)
        {
            const int sample = samples.Psg().Noise(file, *drums[i]);
            const int key = kFirstDrumKey + int(i - first);
            si.zones.push_back(InstantReleaseZone(key, key, sample, key, 0, Looped(file.samples[size_t(sample)])));
        }
        file.instruments.push_back(si);
        file.presets.push_back(
            MakePreset(name, uint16_t(kNoiseBank), uint16_t(program), int(file.instruments.size()) - 1));
    }

    return warnings;
}

} // namespace agbsf2::ubimilan
