// SPDX-License-Identifier: MIT

// SoundFont instruments for AlphaDream's driver. Each region that a song plays becomes a zone over the region's keys.

#include "alphadream/soundfont.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "alphadream/driver.h"
#include "gba.h"
#include "midi.h"
#include "psg.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::alphadream
{
namespace
{

// The level of a PSG channel at volume 15 against a full-scale sample at the full volume. The driver sets SOUNDCNT_L's
// master volumes to 7 and the PSG's share to 100%. A square at volume 15 then swings 120 either side of its middle, out
// of the mix's -512 to 512. The mixer writes a full-scale sample at volume 127 and the envelope's full level as 63 of
// the FIFO's 128, which the FIFO's share of 100% makes 251.
constexpr double kPsgLevel = 120.0 / 251;

// The envelope's full level.
constexpr double kFullLevel = 255;

class SampleCache
{
public:
    SampleCache(const Rom& rom, const DriverInfo& info, Sf2File& file) : rom_(rom), info_(info), file_(file)
    {
    }

    // Returns the index of sample `number`, or -1 if it can't be read or its entry is empty. Its points are unsigned
    // 8-bit. The driver plays an empty entry's table as points, at a level that hardly moves.
    int Sample(int number)
    {
        const auto cached = samples_.find(number);
        if (cached != samples_.end())
        {
            return cached->second;
        }

        SampleHeader h;
        if (!ReadSample(rom_, info_, number, h) || h.empty)
        {
            return samples_[number] = -1;
        }

        sf2::Sample s;
        s.name = AddressName("Sample", h.address);
        s.sample_rate = uint32_t(std::lround(h.rate / 1024.0));
        s.original_key = uint8_t(kRootKey);
        for (uint32_t i = 0; i < h.end; i++)
        {
            s.data.push_back(int16_t((rom_.U8(h.data + i) - 128) * 256));
        }
        if (h.loops)
        {
            s.loop_start = h.loop_start;
            s.loop_end = h.end;
            LengthenLoop(s);
            AddLoopGuard(s);
        }
        file_.samples.push_back(s);
        return samples_[number] = int(file_.samples.size()) - 1;
    }

    // Returns the index of a square of duty `duty`, a cycle of 64 points at middle C.
    int Square(int duty)
    {
        const auto cached = squares_.find(duty);
        if (cached != squares_.end())
        {
            return cached->second;
        }
        sf2::Sample s;
        s.name = SquareName(duty);
        Cycle(s, SquareCycle(duty, int16_t(std::lround(32767 * kPsgLevel))));
        file_.samples.push_back(s);
        return squares_[duty] = int(file_.samples.size()) - 1;
    }

    // Returns the index of wave pattern `number`: 32 points of 4 bits, high half first. The driver plays the wave
    // through the squares' pitch table, and it sets the 32-step mode (0x0819A9DC). The hardware then plays the pattern
    // an octave below a square at the same setting, so key 72 plays it at middle C.
    int Wave(int number)
    {
        const auto cached = waves_.find(number);
        if (cached != waves_.end())
        {
            return cached->second;
        }
        const uint32_t address = info_.wave_table + 16 * uint32_t(number);
        if (!rom_.Contains(address, 16))
        {
            return waves_[number] = -1;
        }
        sf2::Sample s;
        char name[24];
        std::snprintf(name, sizeof name, "Wave %d", number);
        s.name = name;
        std::vector<int16_t> cycle;
        for (uint32_t i = 0; i < 32; i++)
        {
            const uint8_t byte = rom_.U8(address + i / 2);
            const int point = i % 2 ? byte & 0xF : byte >> 4;
            cycle.push_back(int16_t(std::lround((point - 7.5) * 32767 / 7.5 * kPsgLevel)));
        }
        Cycle(s, cycle);
        s.original_key = 72;
        file_.samples.push_back(s);
        return waves_[number] = int(file_.samples.size()) - 1;
    }

    // Returns the index of the noise's sequence, 15-bit or 7-bit.
    int Noise(bool narrow)
    {
        const auto cached = noises_.find(narrow);
        if (cached != noises_.end())
        {
            return cached->second;
        }
        file_.samples.push_back(NoiseSequence(narrow, kPsgLevel));
        return noises_[narrow] = int(file_.samples.size()) - 1;
    }

private:
    static void Cycle(sf2::Sample& s, const std::vector<int16_t>& cycle)
    {
        s.data = cycle;
        s.sample_rate = uint32_t(std::lround(double(cycle.size()) * kMiddleC));
        s.original_key = 60;
        s.loop_start = 0;
        s.loop_end = uint32_t(cycle.size());
        LengthenLoop(s);
        AddLoopGuard(s);
    }

    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File& file_;
    std::map<int, int> samples_;
    std::map<int, int> squares_;
    std::map<int, int> waves_;
    std::map<bool, int> noises_;
};

// Returns the cents by which the noise's sequence at kNoiseSequenceRate has to be raised to step at the clock that
// NR43 sets: 524288 Hz over its divisor (a half for 0) and over 2 to the power of one more than its shift.
int NoiseCents(uint8_t control)
{
    const int divisor = control & 7;
    const int shift = control >> 4;
    const double clock = 524288.0 / (divisor ? divisor : 0.5) / std::pow(2.0, shift + 1);
    return int(std::lround(1200 * std::log2(clock / kNoiseSequenceRate)));
}

// The volume envelope of a sample's region (the mixer at 0x0819A2F0). The level starts at the attack's step and rises
// by it each frame to 255. It stays there for a frame and then falls to the sustain level. The release takes the
// release's step off each frame.
VolumeEnvelope SampleEnvelope(const Region& r)
{
    VolumeEnvelope e;
    const int attack = r.envelope[0];
    const double attack_frames = attack ? std::ceil((kFullLevel - attack) / attack) : 0;
    e.attack = attack_frames > 0 ? Timecents(FrameSeconds(attack_frames)) : kInstant;
    e.hold = Timecents(FrameSeconds(1));
    e.sustain = Centibels(r.envelope[2] / kFullLevel);
    const int release = r.envelope[3];
    e.release = release ? Timecents(FadeSeconds(std::ceil(r.envelope[2] / double(release)))) : Timecents(100);
    return e;
}

} // namespace

void InstrumentSet::Add(Kind kind, int instrument, int key, const Region& region)
{
    auto& entry = used_[{int(kind), instrument}][region.address];
    entry.first = region;
    entry.second.insert(key);
}

std::vector<std::string> InstrumentSet::Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const
{
    static const char* const kKindNames[] = {"Instrument", "Square", "Wave", "Noise"};
    std::vector<std::string> warnings;
    SampleCache samples(rom, info, file);
    for (const auto& [which, regions] : used_)
    {
        const Kind kind = Kind(which.first);
        char name[40];
        std::snprintf(name, sizeof name, "%s %d", kKindNames[which.first], which.second);
        sf2::Instrument si = MakeInstrument(name);
        for (const auto& [address, entry] : regions)
        {
            const Region& r = entry.first;
            if (kind == Kind::kSample)
            {
                const int sample = samples.Sample(r.sample & 0x7FFF);
                if (sample < 0)
                {
                    continue;
                }
                sf2::Zone z = SampleZone(sample, Looped(file.samples[size_t(sample)]), r.lo, r.hi);
                if (r.sample & 0x8000)
                {
                    z.generators.insert(z.generators.end() - 1, sf2::Generator::Value(sf2::kScaleTuning, 0));
                }
                AddVolumeEnvelope(z, SampleEnvelope(r));
                si.zones.push_back(z);
            }
            else if (kind == Kind::kNoise)
            {
                // A zone for each key, at the clock that the key's setting gives.
                for (int key : entry.second)
                {
                    const uint8_t control = key <= 0x14   ? 0xD7
                                            : key <= 0x50 ? rom.U8(info.noise_table + key - 0x15)
                                                          : 0;
                    sf2::Zone z = SampleZone(samples.Noise(control & 8), true, key, key);
                    z.generators.insert(z.generators.begin() + 1, sf2::Generator::Value(sf2::kOverridingRootKey, key));
                    AddTuning(z, NoiseCents(control));
                    si.zones.push_back(z);
                }
            }
            else
            {
                const int sample = kind == Kind::kWave ? samples.Wave(r.sample & 0xFF) : samples.Square(r.sample & 3);
                if (sample < 0)
                {
                    continue;
                }
                // The driver mutes the wave at its note's end. The squares' and the noise's notes end where the
                // hardware's envelope falls silent. Each release is instant.
                si.zones.push_back(SampleZone(sample, true, r.lo, r.hi));
            }
        }
        if (si.zones.size() < 2)
        {
            warnings.push_back(std::string(name) + " has nothing to play");
            continue;
        }
        file.instruments.push_back(si);
        const BankProgram preset = NumberedPreset(PresetNumber(kind, which.second));
        file.presets.push_back(
            MakePreset(name, uint16_t(preset.bank), uint16_t(preset.program), int(file.instruments.size()) - 1));
    }
    return warnings;
}

} // namespace agbsf2::alphadream
