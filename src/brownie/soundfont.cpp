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

// The noise kits' first key, and the drums in each kit.
constexpr int kFirstDrumKey = 36;
constexpr int kKitDrums = 92;

// The rate at which noise is rendered: the PSG's output rate under the BIOS's SOUNDBIAS setting, which the driver
// keeps.
constexpr uint32_t kNoiseRate = 32768;

} // namespace

SoundfontBuilder::SoundfontBuilder(const Rom& rom, uint32_t rate) : rom_(rom), rate_(rate), psg_(kNoiseRate)
{
}

int SoundfontBuilder::SquareSample(int duty)
{
    return psg_.Square(file_, duty);
}

int SoundfontBuilder::WaveSample(const WaveShape& shape)
{
    return psg_.Wave(file_, shape);
}

int SoundfontBuilder::NoiseSample(const NoiseSound& sound)
{
    return psg_.Noise(file_, sound);
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
    out.name = AddressName("Sample", sample.start);
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
        AddLoopGuard(out);
    }
    else
    {
        out.data.insert(out.data.end(), kLoopGuard, int16_t(0));
    }

    file_.samples.push_back(std::move(out));
    return samples_[sample] = int(file_.samples.size()) - 1;
}

NamedZones SoundfontBuilder::SquareInstrument(int first, int frames, int second, int cents)
{
    return psg_.SquareInstrument(file_, first, frames, second, cents);
}

NamedZones SoundfontBuilder::WaveInstrument(const WaveShapes& shapes, int first, int frames, int second, int cents)
{
    return psg_.WaveInstrument(file_, shapes, first, frames, second, cents);
}

int SoundfontBuilder::AddInstrument(const std::string& name, std::vector<sf2::Zone> zones)
{
    file_.instruments.push_back(MakeInstrument(name, std::move(zones)));

    return int(file_.instruments.size()) - 1;
}

void SoundfontBuilder::AddPreset(const std::string& name, int bank, int program, int instrument)
{
    file_.presets.push_back(MakePreset(name, uint16_t(bank), uint16_t(program), instrument));
}

int InstrumentSet::Program(const Instrument& inst)
{
    return programs_.try_emplace(inst, int(programs_.size())).first->second;
}

int InstrumentSet::Shape(const WaveShape& shape)
{
    return shapes_.Number(shape);
}

std::pair<int, int> InstrumentSet::Drum(const NoiseSound& sound)
{
    const int number = drums_.try_emplace(sound, int(drums_.size())).first->second;
    return {number / kKitDrums, kFirstDrumKey + number % kKitDrums};
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
        NamedZones made;
        switch (inst.kind)
        {
        case Instrument::kSquare:
            made = sf.SquareInstrument(inst.first, inst.frames, inst.second, cents);
            break;
        case Instrument::kWave:
            made = sf.WaveInstrument(shapes_, inst.first, inst.frames, inst.second, cents);
            break;
        case Instrument::kNoise:
            made = BuildDrums(sf, inst);
            break;
        case Instrument::kSamples:
            made = BuildSamples(sf, inst, warnings);
            break;
        }

        sf.AddPreset(made.name, int(program) / 128, int(program) % 128,
                     sf.AddInstrument(made.name, std::move(made.zones)));
    }

    return warnings;
}

NamedZones InstrumentSet::BuildDrums(SoundfontBuilder& sf, const Instrument& inst) const
{
    NamedZones made;
    for (const auto& [sound, index] : drums_)
    {
        if (index / kKitDrums != inst.first)
        {
            continue;
        }

        const int key = kFirstDrumKey + index % kKitDrums;
        const int sample = sf.NoiseSample(sound);
        made.zones.push_back(RootKeyZone(sample, Looped(sf.File().samples[size_t(sample)]), key, key, key));
    }

    char text[64];
    std::snprintf(text, sizeof text, inst.first ? "Noise drums %d" : "Noise drums", inst.first + 1);
    made.name = text;

    return made;
}

NamedZones InstrumentSet::BuildSamples(SoundfontBuilder& sf, const Instrument& inst,
                                       std::vector<std::string>& warnings) const
{
    // A zone for each key that a song plays. It plays the sample of the key's semitone at the key's octave. In the
    // Magical Vacation revision, it plays the sample at the key's rate.
    NamedZones made;
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

            sf2::Zone z = SampleZone(index, Looped(sf.File().samples[size_t(index)]), key, key, entry.cents);
            z.generators.insert(z.generators.end() - 1, sf2::Generator::Value(sf2::kOverridingRootKey, entry.root));
            made.zones.push_back(z);
        }
    }

    made.name = AddressName("Samples", inst.set);

    return made;
}

} // namespace agbsf2::brownie
