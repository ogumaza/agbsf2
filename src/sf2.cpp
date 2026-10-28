// SPDX-License-Identifier: MIT

#include "sf2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"
#include "seqlib/sf2/writer.h"

#include "files.h"
#include "gba.h"
#include "program.h"

namespace agbsf2
{

int Timecents(double seconds)
{
    return sf2::ToTimecents(seconds * 1000.0, kInstant, 8000);
}

int Centibels(double level)
{
    if (level <= 0)
    {
        return kSilent;
    }

    return std::clamp(int(std::lround(-200.0 * std::log10(level))), 0, kSilent);
}

int SidesPan(double left, double right)
{
    if (left <= 0 && right <= 0)
    {
        return 0;
    }

    // Under the sin/cos pan law of MIDI RP-036, pan p from -1 to 1 plays the left side at cos((p + 1) * pi / 4) and the
    // right side at sin((p + 1) * pi / 4). FluidSynth and most other SoundFont players follow that law.
    return int(std::lround(500.0 * (4.0 / std::numbers::pi * std::atan2(right, left) - 1.0)));
}

double FadeStretch(double frames)
{
    return std::clamp(3.0 + 3.0 * (frames - 5) / 11.0, 3.0, 6.0);
}

double FadeSeconds(double frames)
{
    return FadeStretch(frames) * frames / kFrameRate;
}

void AddVolumeEnvelope(sf2::Zone& zone, const VolumeEnvelope& envelope)
{
    const std::pair<uint16_t, int> phases[] = {{sf2::kAttackVolEnv, envelope.attack},
                                               {sf2::kHoldVolEnv, envelope.hold},
                                               {sf2::kDecayVolEnv, envelope.decay},
                                               {sf2::kReleaseVolEnv, envelope.release}};
    for (const auto& [oper, value] : phases)
    {
        if (value != kInstant)
        {
            zone.generators.push_back(sf2::Generator::Value(oper, value));
        }
    }
    if (envelope.sustain)
    {
        zone.generators.push_back(sf2::Generator::Value(sf2::kSustainVolEnv, envelope.sustain));
    }
}

void AddTuning(sf2::Zone& zone, int cents)
{
    if (cents / 100)
    {
        zone.generators.push_back(sf2::Generator::Value(sf2::kCoarseTune, std::clamp(cents / 100, -120, 120)));
    }
    if (cents % 100)
    {
        zone.generators.push_back(sf2::Generator::Value(sf2::kFineTune, cents % 100));
    }
}

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

sf2::Zone RootKeyZone(int sample, bool loop, int lo, int hi, int root)
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, lo, hi));
    z.generators.push_back(sf2::Generator::Value(sf2::kOverridingRootKey, root));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, loop ? 1 : 0));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

    return z;
}

std::vector<sf2::Zone> SwitchZones(int first, int frames, int second, int cents, int lo, int hi)
{
    if (frames == 0)
    {
        return {SampleZone(first, true, lo, hi, cents)};
    }

    // The first sample holds for the frames and then falls silent. The second waits for them.
    const int time = sf2::ToTimecents(FrameSeconds(frames) * 1000.0);
    sf2::Zone a = SampleZone(first, true, lo, hi, cents);
    a.generators.insert(a.generators.begin() + 1, {sf2::Generator::Value(sf2::kHoldVolEnv, time),
                                                   sf2::Generator::Value(sf2::kDecayVolEnv, kInstant),
                                                   sf2::Generator::Value(sf2::kSustainVolEnv, kSilent)});
    sf2::Zone b = SampleZone(second, true, lo, hi, cents);
    b.generators.insert(b.generators.begin() + 1, sf2::Generator::Value(sf2::kDelayVolEnv, time));

    return {a, b};
}

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

void LengthenLoop(sf2::Sample& s)
{
    const uint32_t length = s.loop_end - s.loop_start;
    while (s.loop_end - s.loop_start < kMinLoop)
    {
        for (uint32_t i = 0; i < length; i++)
        {
            s.data.push_back(s.data[s.loop_start + i]);
        }

        s.loop_end += length;
    }
}

void AddLoopGuard(sf2::Sample& s)
{
    for (uint32_t i = 0; i < kLoopGuard; i++)
    {
        s.data.push_back(s.data[s.loop_start + i % (s.loop_end - s.loop_start)]);
    }
}

std::string AddressName(const char* kind, uint32_t address)
{
    char b[32];
    std::snprintf(b, sizeof b, "%s %08X", kind, unsigned(address));

    return b;
}

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
    global.modulators.assign(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    zones.insert(zones.begin(), std::move(global));

    return {name, std::move(zones)};
}

void Sf2File::SetTitle(const std::string& title, const std::string& detail)
{
    name = title;
    comment = "Instruments for " + title + detail + ", extracted by " + kProgramName;
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
