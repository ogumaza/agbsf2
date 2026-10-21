// SPDX-License-Identifier: MIT

#include "rd2/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"

#include "gba.h"
#include "psg.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rd2
{
namespace
{

// The envelope's full level.
constexpr int kFullLevel = 0x7FFF;

// The most points an envelope is read for.
constexpr int kMaxPoints = 64;

// The longest sample read, in points.
constexpr uint32_t kMaxSamplePoints = 1u << 24;

// The Game Boy's square duties: the steps of 8 that are high.
constexpr int kDutyHigh[4] = {1, 2, 4, 6};

// The points a square sample gives each of its 8 steps.
constexpr int kStepPoints = 8;

// C3, which wave samples play on their root keys. Square samples play middle C there.
constexpr double kC3 = kMiddleC / 2;

// The rate a noise sample is rendered at, and its length.
constexpr uint32_t kNoiseRate = 65536;
constexpr uint32_t kNoisePoints = kNoiseRate;

} // namespace

Sf2Envelope EnvelopeFor(const Rom& rom, uint32_t address, int release)
{
    Sf2Envelope env;

    // The envelope starts at 0 and moves in a straight line to each point's level over its frames. It holds the last
    // point's level, and a point of 0 frames holds the level it starts from.
    std::vector<std::pair<int, int>> points;
    for (int i = 0; i < kMaxPoints; i++)
    {
        const int frames = int16_t(rom.U16(address + 4 * uint32_t(i)));
        if (frames <= 0)
        {
            break;
        }

        const int level = std::max(0, int(int16_t(rom.U16(address + 4 * uint32_t(i) + 2))));
        points.emplace_back(frames, level);
    }

    int peak = 0;
    for (const auto& p : points)
    {
        peak = std::max(peak, p.second);
    }
    if (peak == 0)
    {
        env.attenuation = kSilent;
        return env;
    }
    env.attenuation = Centibels(double(peak) / kFullLevel);

    // The attack lasts until the first point at the peak. The driver's first frame is already a step up.
    size_t i = 0;
    int time = 0;
    for (; i < points.size(); i++)
    {
        time += points[i].first;
        if (points[i].second == peak)
        {
            break;
        }
    }
    if (time > 1)
    {
        env.attack = Timecents((time - 1) / kFrameRate);
    }

    // After the peak, the level holds while it's within 1 dB of it, and then decays to the last point's level.
    const int final_level = points.back().second;
    if (final_level < peak)
    {
        const double near = peak * std::pow(10.0, -1.0 / 20.0);
        int hold = 0;
        int decay = 0;
        int level = peak;
        for (size_t k = i + 1; k < points.size(); k++)
        {
            const int frames = points[k].first;
            const int to = points[k].second;
            if (decay == 0 && to >= near)
            {
                hold += frames;
            }
            else if (decay == 0 && level >= near)
            {
                // The segment crosses the 1 dB mark: hold until it does.
                const int part = int(std::lround(frames * (level - near) / std::max(1, level - to)));
                hold += part;
                decay += frames - part;
            }
            else
            {
                decay += frames;
            }
            level = to;
        }

        if (hold > 0)
        {
            env.hold = Timecents(hold / kFrameRate);
        }
        if (final_level == 0)
        {
            env.sustain = kSilent;
            env.decay = decay > 0 ? Timecents(FadeSeconds(decay)) : kInstant;
        }
        else
        {
            // A SoundFont's decay time is for a fall of 100 dB, so it reaches the sustain level in proportion.
            env.sustain = Centibels(double(final_level) / peak);
            env.decay = decay > 0 && env.sustain > 0 ? Timecents(1000.0 / env.sustain * decay / kFrameRate) : kInstant;
        }
    }

    env.release = release;

    return env;
}

Sf2Envelope WaveEnvelope(int release)
{
    Sf2Envelope env;
    env.release = release;

    return env;
}

int SampleRelease(uint8_t release)
{
    const double per_frame = -20.0 * std::log10((release + 0xE6) / 512.0);
    return Timecents(100.0 / per_frame / kFrameRate);
}

int PsgRelease(uint8_t release)
{
    const int steps = release >> 5;
    return steps ? Timecents(FadeSeconds(15.0 * steps / 64.0 * kFrameRate)) : kInstant;
}

void AddEnvelope(sf2::Zone& zone, const Sf2Envelope& envelope)
{
    std::vector<sf2::Generator> gens;
    const sf2::ZoneAttenuation attenuation = sf2::ToZoneAttenuation(envelope.attenuation);
    if (attenuation.generator)
    {
        gens.push_back(sf2::Generator::Value(sf2::kInitialAttenuation, attenuation.generator));
    }
    if (attenuation.modulator)
    {
        zone.modulators.push_back(*attenuation.modulator);
    }
    if (envelope.attack != kInstant)
    {
        gens.push_back(sf2::Generator::Value(sf2::kAttackVolEnv, envelope.attack));
    }
    if (envelope.hold != kInstant)
    {
        gens.push_back(sf2::Generator::Value(sf2::kHoldVolEnv, envelope.hold));
    }
    if (envelope.decay != kInstant)
    {
        gens.push_back(sf2::Generator::Value(sf2::kDecayVolEnv, envelope.decay));
    }
    if (envelope.sustain)
    {
        gens.push_back(sf2::Generator::Value(sf2::kSustainVolEnv, envelope.sustain));
    }
    if (envelope.release != kInstant)
    {
        gens.push_back(sf2::Generator::Value(sf2::kReleaseVolEnv, envelope.release));
    }

    zone.generators.insert(zone.generators.end() - (zone.generators.empty() ? 0 : 1), gens.begin(), gens.end());
}

SoundfontBuilder::SoundfontBuilder(const Rom& rom, double psg_share, int songs)
    : rom_(rom), psg_share_(psg_share), banks_(songs)
{
}

int SoundfontBuilder::GameSample(uint32_t address)
{
    const auto cached = game_samples_.find(address);
    if (cached != game_samples_.end())
    {
        return cached->second;
    }

    // A sample has a header of its length, rate, loop start and loop end, then its 8-bit points. The driver plays up to
    // the loop's end and goes back to its start, or without a loop up to the length.
    const uint32_t length = rom_.U32(address);
    const uint32_t rate = rom_.U32(address + 4);
    const uint32_t loop_start = rom_.U32(address + 8);
    const uint32_t loop_end = rom_.U32(address + 12);
    const uint32_t points = loop_end ? loop_end : length;
    if (points == 0 || points > kMaxSamplePoints || rate == 0 || !rom_.Contains(address + 16, points) ||
        (loop_end && loop_start >= loop_end))
    {
        game_samples_[address] = -1;
        return -1;
    }

    sf2::Sample s;
    s.name = AddressName("Sample", address);
    s.sample_rate = rate;
    s.original_key = 60;
    s.data.reserve(points + kMinLoop + kLoopGuard);
    for (uint32_t i = 0; i < points; i++)
    {
        s.data.push_back(int16_t(rom_.S8(address + 16 + i) * 256));
    }

    // A short loop is repeated until it's long enough, and the points after it repeat its start. The data ends at the
    // loop's end.
    if (loop_end)
    {
        s.loop_start = loop_start;
        s.loop_end = loop_end;
        LengthenLoop(s);
        AddLoopGuard(s);
    }

    file_.samples.push_back(std::move(s));
    game_samples_[address] = int(file_.samples.size()) - 1;

    return game_samples_[address];
}

int SoundfontBuilder::SquareSample(int duty)
{
    duty &= 3;
    const auto cached = squares_.find(duty);
    if (cached != squares_.end())
    {
        return cached->second;
    }

    // The high steps come first. Without the DC offset, which the GBA's output removes, the levels sit either side of
    // 0. They lie as far apart as a wave's levels 0 and 15.
    const int high = kDutyHigh[duty];
    std::vector<int16_t> cycle;
    for (int step = 0; step < 8; step++)
    {
        const double level = step < high ? 2.0 - high / 4.0 : -high / 4.0;
        for (int i = 0; i < kStepPoints; i++)
        {
            cycle.push_back(int16_t(std::lround(level * psg_share_ * 32767)));
        }
    }

    sf2::Sample s;
    s.name = SquareName(duty);
    s.sample_rate = uint32_t(std::lround(8 * kStepPoints * kMiddleC));
    s.original_key = 60;
    SetCycle(s, cycle);
    file_.samples.push_back(std::move(s));
    squares_[duty] = int(file_.samples.size()) - 1;

    return squares_[duty];
}

int SoundfontBuilder::WaveSample(uint32_t address)
{
    const auto cached = waves_.find(address);
    if (cached != waves_.end())
    {
        return cached->second;
    }

    // The wave voice plays 32 steps of 4 bits, the high nibble of each byte first.
    std::vector<int16_t> cycle;
    for (uint32_t i = 0; i < 16; i++)
    {
        const uint8_t b = rom_.U8(address + i);
        for (int v : {b >> 4, b & 15})
        {
            cycle.push_back(int16_t(std::lround((v - 7.5) / 7.5 * psg_share_ * 32767)));
        }
    }

    sf2::Sample s;
    s.name = AddressName("Wave", address);
    s.sample_rate = uint32_t(std::lround(32 * kC3));
    s.original_key = 48;
    SetCycle(s, cycle);
    file_.samples.push_back(std::move(s));
    waves_[address] = int(file_.samples.size()) - 1;

    return waves_[address];
}

int SoundfontBuilder::NoiseSample(uint8_t nr43)
{
    const auto cached = noises_.find(nr43);
    if (cached != noises_.end())
    {
        return cached->second;
    }

    sf2::Sample s;
    char name[32];
    std::snprintf(name, sizeof name, "Noise %02X", unsigned(nr43));
    s.name = name;
    s.sample_rate = kNoiseRate;
    s.original_key = 60;

    // The LFSR's clock is 524288 / r / 2^(s+1) Hz, with r = 0 counting as 0.5, so each point at 65536 Hz moves it on by
    // 8 / ((r ? 2r : 1) << s) steps. The output is high while the LFSR's low bit is 0.
    const int shift = nr43 >> 4;
    const int ratio = nr43 & 7;
    uint32_t lfsr = 0x7FFF;
    uint32_t clock = 0;
    for (uint32_t i = 0; i < kNoisePoints; i++)
    {
        s.data.push_back(shift < 14 ? int16_t(std::lround(((lfsr & 1) ? -32767 : 32767) * psg_share_)) : 0);
        if (shift >= 14)
        {
            continue;
        }

        clock += 8;
        const uint32_t den = uint32_t(ratio ? 2 * ratio : 1) << shift;
        for (; clock >= den; clock -= den)
        {
            lfsr = NextLfsr(lfsr, (nr43 & 8) != 0);
        }
    }

    s.loop_start = 0;
    s.loop_end = uint32_t(s.data.size());
    AddLoopGuard(s);

    file_.samples.push_back(std::move(s));
    noises_[nr43] = int(file_.samples.size()) - 1;

    return noises_[nr43];
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

} // namespace agbsf2::rd2
