// SPDX-License-Identifier: MIT

#include "rd2/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"

#include "rd2/driver.h"

namespace agbsf2::rd2
{
namespace
{

// The shortest time a SoundFont envelope can give a phase, in timecents: 1 ms.
constexpr int kInstant = -12000;

// The greatest attenuation, in centibels.
constexpr int kSilent = 1440;

// The envelope's full level.
constexpr int kFullLevel = 0x7FFF;

// The most points an envelope is read for.
constexpr int kMaxPoints = 64;

// The shortest loop a sample gets, in points. A shorter one is repeated until it's at least this long, since some
// players can't play a very short loop.
constexpr uint32_t kMinLoop = 32;

// The points after a loop's end that repeat its start, for players that read past the end when they interpolate.
constexpr uint32_t kLoopGuard = 8;

// The longest sample read, in points.
constexpr uint32_t kMaxSamplePoints = 1u << 24;

// The Game Boy's square duties: the steps of 8 that are high.
constexpr int kDutyHigh[4] = {1, 2, 4, 6};

// The points a square sample gives each of its 8 steps.
constexpr int kStepPoints = 8;

// Middle C and C3, which square and wave samples play on their root keys.
constexpr double kMiddleC = 261.6255653005986;
constexpr double kC3 = kMiddleC / 2;

// The rate a noise sample is rendered at, and its length.
constexpr uint32_t kNoiseRate = 65536;
constexpr uint32_t kNoisePoints = kNoiseRate;

// Returns a time in timecents, the unit of SoundFont envelope times.
int Timecents(double seconds)
{
    return sf2::ToTimecents(seconds * 1000.0, kInstant, 8000);
}

// Returns the SoundFont duration for a fade to silence over `frames` frames. The driver's level falls linearly, so its
// fade stays loud for most of its length, where a SoundFont's level falls linearly in decibels and becomes quiet
// sooner. The SoundFont fade is lengthened to keep their loudness close while the fade is audible. Short fades fall in
// larger steps and need less stretching.
double FadeSeconds(double frames)
{
    const double stretch = std::clamp(3.0 + 3.0 * (frames - 5) / 11.0, 3.0, 6.0);
    return stretch * frames / kFrameRate;
}

// Returns the centibels of a level below the full level.
int Centibels(double level, double full)
{
    return level <= 0 ? kSilent : std::clamp(int(std::lround(-200.0 * std::log10(level / full))), 0, kSilent);
}

// Returns the SoundFont release of a sample or wave voice. Its release takes a share of its level off each frame.
int SampleRelease(uint8_t release)
{
    const double per_frame = -20.0 * std::log10((release + 0xE6) / 512.0);
    return Timecents(100.0 / per_frame / kFrameRate);
}

std::string Name(const char* kind, uint32_t address)
{
    char b[32];
    std::snprintf(b, sizeof b, "%s %08X", kind, unsigned(address));

    return b;
}

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

} // namespace

Sf2Envelope EnvelopeFor(const Rom& rom, uint32_t address, uint8_t release, bool psg)
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
    env.attenuation = Centibels(peak, kFullLevel);

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
            env.sustain = Centibels(final_level, peak);
            env.decay = decay > 0 && env.sustain > 0 ? Timecents(1000.0 / env.sustain * decay / kFrameRate) : kInstant;
        }
    }

    // A sample or wave voice's release takes a share of its level off each frame. A square or noise voice's release is
    // the PSG's envelope, which steps its volume down from 15 at most every 1/64 s for each step of the release's top 3
    // bits, or silences it at once.
    if (!psg)
    {
        env.release = SampleRelease(release);
    }
    else if (release >> 5)
    {
        env.release = Timecents(FadeSeconds(15.0 * (release >> 5) / 64.0 * kFrameRate));
    }

    return env;
}

Sf2Envelope WaveEnvelope(uint8_t release)
{
    Sf2Envelope env;
    env.release = SampleRelease(release);

    return env;
}

void AddEnvelope(sf2::Zone& zone, const Sf2Envelope& envelope)
{
    std::vector<sf2::Generator> gens;
    if (envelope.attenuation)
    {
        gens.push_back(sf2::Generator::Value(sf2::kInitialAttenuation, envelope.attenuation));
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
    s.name = Name("Sample", address);
    s.sample_rate = rate;
    s.original_key = 60;
    s.data.reserve(points + kMinLoop + kLoopGuard);
    for (uint32_t i = 0; i < points; i++)
    {
        s.data.push_back(int16_t(rom_.S8(address + 16 + i) * 256));
    }

    if (loop_end)
    {
        // A short loop is repeated until it's long enough, and the points after it repeat its start.
        const uint32_t loop = loop_end - loop_start;
        uint32_t end = loop_end;
        while (end - loop_start < kMinLoop)
        {
            for (uint32_t i = 0; i < loop; i++)
            {
                s.data.push_back(s.data[loop_start + i]);
            }
            end += loop;
        }
        for (uint32_t i = 0; i < kLoopGuard; i++)
        {
            s.data.push_back(s.data[loop_start + i % (end - loop_start)]);
        }

        s.loop_start = loop_start;
        s.loop_end = end;
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
    // 0.
    const int high = kDutyHigh[duty];
    std::vector<int16_t> cycle;
    for (int step = 0; step < 8; step++)
    {
        const double level = step < high ? 1.0 - high / 8.0 : -high / 8.0;
        for (int i = 0; i < kStepPoints; i++)
        {
            cycle.push_back(int16_t(std::lround(level * psg_share_ * 32767)));
        }
    }

    static constexpr const char* kNames[4] = {"Square 12.5%", "Square 25%", "Square 50%", "Square 75%"};
    sf2::Sample s;
    s.name = kNames[duty];
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
    s.name = Name("Wave", address);
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
        s.data.push_back(shift < 14 ? int16_t(std::lround(((lfsr & 1) ? -16384 : 16384) * psg_share_)) : 0);
        if (shift >= 14)
        {
            continue;
        }

        clock += 8;
        const uint32_t den = uint32_t(ratio ? 2 * ratio : 1) << shift;
        for (; clock >= den; clock -= den)
        {
            const uint32_t bit = (lfsr ^ (lfsr >> 1)) & 1;
            lfsr = (lfsr >> 1) | (bit << 14);
            if (nr43 & 8)
            {
                lfsr = (lfsr & ~0x40u) | (bit << 6);
            }
        }
    }

    s.loop_start = 0;
    s.loop_end = uint32_t(s.data.size());
    for (uint32_t i = 0; i < kLoopGuard; i++)
    {
        s.data.push_back(s.data[i]);
    }

    file_.samples.push_back(std::move(s));
    noises_[nr43] = int(file_.samples.size()) - 1;

    return noises_[nr43];
}

int SoundfontBuilder::AddInstrument(const std::string& name, std::vector<sf2::Zone> zones)
{
    file_.instruments.push_back({name, std::move(zones)});

    return int(file_.instruments.size()) - 1;
}

void SoundfontBuilder::AddPreset(const std::string& name, int bank, int program, int instrument)
{
    file_.presets.push_back(MakePreset(name, uint16_t(bank), uint16_t(program), instrument));
}

} // namespace agbsf2::rd2
