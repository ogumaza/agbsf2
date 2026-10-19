// SPDX-License-Identifier: MIT

#include "krawall/soundfont.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

#include "krawall/player.h"
#include "sf2.h"

namespace agbsf2::krawall
{

int InstrumentSet::Program(uint32_t sample, int index, uint32_t offset)
{
    const auto key = std::make_pair(sample, offset);
    return programs_.try_emplace(key, Instrument{index, int(programs_.size())}).first->second.program;
}

Sf2File InstrumentSet::Build(const Rom& rom, const DriverInfo& info) const
{
    Sf2File f;
    std::map<uint32_t, int> samples;
    for (const auto& [key, inst] : programs_)
    {
        const auto [header, offset] = key;
        char name[32];
        std::snprintf(name, sizeof name, "Sample %03d", inst.index);

        // The game's sample: 8-bit points, unsigned, from its header's end. A loop back and forth plays its points
        // backwards after them, so the SoundFont's sample loops over both.
        auto it = samples.find(header);
        if (it == samples.end())
        {
            sf2::Sample s;
            s.name = name;
            s.sample_rate = SampleRate(rom, info, header);
            s.original_key = uint8_t(kRootKey);

            const uint32_t data = header + kSamplePoints;
            const uint32_t end = rom.U32(header + kSampleEnd);
            const uint32_t length = end > data && rom.Contains(data, end - data) ? end - data : 0;
            for (uint32_t i = 0; i < length; i++)
            {
                s.data.push_back(int16_t((rom.U8(data + i) - 128) * 256));
            }

            const uint32_t loop_length = rom.U32(header + kSampleLoopLength);
            const uint8_t loop = rom.U8(header + kSampleLoop);
            if (loop && loop_length > 0 && loop_length <= length)
            {
                s.loop_start = length - loop_length;
                if (loop == 2)
                {
                    for (uint32_t i = 0; i < loop_length; i++)
                    {
                        s.data.push_back(s.data[length - 1 - i]);
                    }
                }
                s.loop_end = uint32_t(s.data.size());
                AddLoopGuard(s);
            }

            if (s.data.empty())
            {
                s.data.push_back(0);
            }

            f.samples.push_back(std::move(s));
            it = samples.emplace(header, int(f.samples.size()) - 1).first;
        }

        // A note that starts within the sample starts its zone there.
        const sf2::Sample& sample = f.samples[size_t(it->second)];
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
        if (offset)
        {
            z.generators.push_back(sf2::Generator::Value(sf2::kStartAddrsOffset, int(offset % 32768)));
            if (offset >= 32768)
            {
                z.generators.push_back(sf2::Generator::Value(sf2::kStartAddrsCoarseOffset, int(offset / 32768)));
            }
            std::snprintf(name, sizeof name, "Sample %03d +%u", inst.index, unsigned(offset));
        }
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, Looped(sample) ? 1 : 0));
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, it->second));

        f.instruments.push_back(MakeInstrument(name, {z}));
        f.presets.push_back(MakePreset(name, uint16_t(inst.program / 128), uint16_t(inst.program % 128),
                                       int(f.instruments.size()) - 1));
    }

    return f;
}

uint32_t SampleRate(const Rom& rom, const DriverInfo& info, uint32_t sample)
{
    // C-4's period, scaled by the sample's fine tune, divides the clock of the Amiga's periods.
    const uint32_t period = AmigaPeriod(rom, info, kRootNote, rom.S8(sample + kSampleFineTune));
    return period ? uint32_t(kAmigaClock) / period : 8363;
}

} // namespace agbsf2::krawall
