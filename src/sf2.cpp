// SPDX-License-Identifier: MIT

#include "sf2.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "seqlib/sf2/writer.h"

#include "files.h"
#include "program.h"

namespace agbsf2
{

sf2::Preset MakePreset(const std::string& name, uint16_t bank, uint16_t program, int instrument)
{
    sf2::Preset preset;
    preset.name = name;
    preset.bank = bank;
    preset.program = program;
    preset.zones = {{{sf2::Generator::Value(sf2::kInstrument, instrument)}, {}}};

    return preset;
}

int PresetInstrument(const sf2::Preset& preset)
{
    return preset.zones.at(0).generators.at(0).amount;
}

sf2::Instrument MakeInstrument(const std::string& name, std::vector<sf2::Zone> zones)
{
    sf2::Zone global;
    global.modulators.assign(sf2::kNoVelocityFilterFullBend.begin(), sf2::kNoVelocityFilterFullBend.end());
    zones.insert(zones.begin(), std::move(global));

    return {name, std::move(zones)};
}

bool Sf2File::Write(const std::string& path, std::string& error) const
{
    sf2::SoundFont sf = *this;
    sf.info = {{"INAM", name}, {"ISFT", kProgramName}};
    if (!comment.empty())
    {
        sf.info.push_back({"ICMT", comment});
    }

    for (sf2::Sample& s : sf.samples)
    {
        if (!Looped(s))
        {
            s.loop_start = 0;
            s.loop_end = uint32_t(s.data.size());
        }
    }

    std::vector<uint8_t> bytes;
    try
    {
        bytes = sf2::Write(sf, {.sort_presets = true, .pad_empty_sample_data = false});
    }
    catch (const std::runtime_error&)
    {
        error = "soundfont too large (more than 65535 zones, generators or modulators)";
        return false;
    }

    return WriteFile(path, bytes, error);
}

} // namespace agbsf2
