// SPDX-License-Identifier: MIT

#include "konami/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "konami/driver.h"
#include "konami/pitch.h"
#include "konami/seqformat.h"
#include "psg.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::konami
{
namespace
{

// The notes of the driver's PSG frequency table, C2 to B8, and its entries for each note: one every 1/32 semitone.
constexpr int kTableNotes = 84;
constexpr int kEntriesPerNote = 32;

// Returns the frequency of GB square note 0, from the first entry of the driver's frequency table, or C2 if there's no
// usable entry.
double SquareNoteZeroHz(const Rom& rom, const DriverInfo& info)
{
    if (info.psg_freq_table)
    {
        const uint32_t x = rom.U16(info.psg_freq_table) & 0x7FF;
        if (x < 2040)
        {
            return SquareHz(int(x));
        }
    }

    return kC2;
}

} // namespace

std::string WaveName(int wave, int volume)
{
    char name[32];
    std::snprintf(name, sizeof name, "Wave %02d vol %d", wave, volume);

    return name;
}

SoundfontBuilder::SoundfontBuilder(const Rom& rom, const DriverInfo& info, int songs)
    : rom_(rom),
      info_(info),
      square_note0_hz_(SquareNoteZeroHz(rom, info)),
      square_base_(NearestKey(square_note0_hz_)),
      banks_(songs)
{
}

const SoundfontBuilder::Pitch& SoundfontBuilder::PitchOf(int index)
{
    auto it = pitch_.find(index);
    if (it != pitch_.end())
    {
        return it->second;
    }

    Pitch p;
    const SampleInfo s = info_.Sample(rom_, index);
    const double hz = EstimateSamplePitch(rom_, s, s.Rate(info_.mix_rate));
    if (hz > 0)
    {
        p.key = NearestKey(hz);
        p.pitched = true;
    }

    return pitch_[index] = p;
}

void SoundfontBuilder::Restore(const Checkpoint& c)
{
    file_.samples.resize(c.samples);
    file_.instruments.resize(c.instruments);
    file_.presets.resize(c.presets);
    banks_ = c.banks;

    // Remove stale cache entries so discarded samples can be rebuilt on demand.
    for (auto it = cache_.begin(); it != cache_.end();)
    {
        if (it->second >= int(c.samples))
        {
            it = cache_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

int SoundfontBuilder::DsRootKey(int index)
{
    return PitchOf(index).key;
}

bool SoundfontBuilder::DsPitched(int index)
{
    return PitchOf(index).pitched;
}

int SoundfontBuilder::DsSample(int index)
{
    const auto key = std::make_pair(Kind::kSample, index);
    if (auto it = cache_.find(key); it != cache_.end())
    {
        return it->second;
    }

    const SampleInfo s = info_.Sample(rom_, index);
    if (!s.valid)
    {
        return cache_[key] = -1;
    }

    sf2::Sample out;
    char name[32];
    std::snprintf(name, sizeof name, "Sample %03d", index);
    out.name = name;
    out.sample_rate = uint32_t(std::max(1L, std::lround(s.Rate(info_.mix_rate))));
    out.original_key = uint8_t(DsRootKey(index));

    out.data.reserve(size_t(s.length) + kLoopGuard);
    for (int32_t i = 0; i < s.length; i++)
    {
        out.data.push_back(int16_t(rom_.S8(s.data + uint32_t(i)) * 256));
    }

    if (s.Looped())
    {
        out.loop_start = uint32_t(s.loop_start);
        out.loop_end = uint32_t(s.length);
        LengthenLoop(out);
        AddLoopGuard(out);
    }

    file_.samples.push_back(std::move(out));
    return cache_[key] = int(file_.samples.size()) - 1;
}

int SoundfontBuilder::SquareSample(int duty)
{
    duty &= 3;
    const auto key = std::make_pair(Kind::kSquare, duty);
    if (auto it = cache_.find(key); it != cache_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    out.name = SquareName(duty);
    SetCycle(out, SquareCycle(duty, 32767));

    // Root key C4; one cycle lasts 64 points.
    const double c4 = square_note0_hz_ * std::pow(2.0, (60 - square_base_) / 12.0);
    out.sample_rate = uint32_t(std::lround(64 * c4));
    out.original_key = 60;

    file_.samples.push_back(std::move(out));
    return cache_[key] = int(file_.samples.size()) - 1;
}

uint32_t SoundfontBuilder::WaveRowAddress(int wave, int volume) const
{
    // The Dungeon Dice Monsters revision keeps each wave at full scale, and sets the volume with NR32.
    if (info_.revision == Revision::kDungeonDiceMonsters)
    {
        return info_.wave_table + 16 * uint32_t(wave);
    }

    const uint32_t row = ((uint32_t((wave & 0x7F) << 4) + uint32_t(volume)) << 4);
    if (wave & 0x80)
    {
        return info_.sfx_wave_table ? info_.sfx_wave_table + row : 0;
    }

    return info_.wave_table + row;
}

void SoundfontBuilder::WavePattern(int wave, int (&steps)[32]) const
{
    if (info_.wave_table)
    {
        // The driver keeps each wave pre-scaled to 16 volumes; use the loudest row present.
        for (int vol = 15; vol >= 1; vol--)
        {
            const uint32_t row = WaveRowAddress(wave, vol);
            if (!rom_.Contains(row, 16))
            {
                continue;
            }

            bool any = false;
            for (uint32_t i = 0; i < 16; i++)
            {
                any |= rom_.U8(row + i) != 0;
            }
            if (!any)
            {
                continue;
            }

            for (uint32_t i = 0; i < 16; i++)
            {
                const uint8_t b = rom_.U8(row + i);
                steps[2 * i] = std::min(15, int(std::lround((b >> 4) * 15.0 / vol)));
                steps[2 * i + 1] = std::min(15, int(std::lround((b & 15) * 15.0 / vol)));
            }

            return;
        }
    }

    for (int i = 0; i < 32; i++)
    {
        steps[i] = i < 16 ? i : 31 - i; // triangle fallback
    }
}

int SoundfontBuilder::WaveCycles(int wave) const
{
    int steps[32];
    WavePattern(wave, steps);

    for (int period = 1; period < 32; period *= 2)
    {
        bool repeats = true;
        for (int i = 0; i < 32 && repeats; i++)
        {
            repeats = steps[i] == steps[(i + period) % 32];
        }
        if (repeats)
        {
            return period == 1 ? 1 : 32 / period;
        }
    }

    return 1;
}

int SoundfontBuilder::WaveKey(int wave, int note) const
{
    const int cycles = WaveCycles(wave);
    int octaves = 0;
    while ((1 << (octaves + 1)) <= cycles)
    {
        octaves++;
    }

    // The wave channel plays its 32 steps at half the square channel's rate.
    return square_base_ - 12 + 12 * octaves + note;
}

KeyTuning SoundfontBuilder::SquareTuning() const
{
    return NoteTuning(square_base_);
}

KeyTuning SoundfontBuilder::WaveTuning(int wave) const
{
    return NoteTuning(WaveKey(wave, 0));
}

KeyTuning SoundfontBuilder::NoteTuning(int note0_key) const
{
    KeyTuning tuning{};
    const uint32_t table = info_.psg_freq_table;
    if (!table || info_.revision == Revision::kDungeonDiceMonsters ||
        !rom_.Contains(table, 2 * kTableNotes * kEntriesPerNote))
    {
        return tuning;
    }

    // A note plays its table entry. The keys outside the table take the tuning of its nearest note.
    for (int key = 0; key < int(tuning.size()); key++)
    {
        const int note = std::clamp(key - note0_key, 0, kTableNotes - 1);
        const int setting = rom_.U16(table + 2 * uint32_t(note * kEntriesPerNote)) & 0x7FF;
        tuning[size_t(key)] = SettingCents(setting, square_note0_hz_ * std::pow(2.0, note / 12.0));
    }

    return tuning;
}

void SoundfontBuilder::WaveRow(int wave, int volume, int (&steps)[32]) const
{
    if (info_.wave_table)
    {
        const uint32_t row = WaveRowAddress(wave, volume);
        if (rom_.Contains(row, 16))
        {
            for (uint32_t i = 0; i < 16; i++)
            {
                const uint8_t b = rom_.U8(row + i);
                steps[2 * i] = b >> 4; // high nibble plays first
                steps[2 * i + 1] = b & 15;
            }

            return;
        }
    }

    WavePattern(wave, steps);
    for (int& v : steps)
    {
        v = v * volume / 15;
    }
}

int SoundfontBuilder::WaveSample(int wave, int volume)
{
    const auto key = std::make_pair(Kind::kWave, (wave << 4) | (volume & 15));
    if (auto it = cache_.find(key); it != cache_.end())
    {
        return it->second;
    }

    // The driver keeps every wave pre-scaled to 16 volumes and loads the row for the note's volume, so quiet notes are
    // coarser. Each row used gets its own sample: DC removed, all rows on one scale (15 steps = half of full scale).
    int steps[32];
    WaveRow(wave, volume, steps);
    double mean = 0;
    for (int v : steps)
    {
        mean += v;
    }
    mean /= 32;

    sf2::Sample out;
    out.name = WaveName(wave, volume);

    constexpr int kPerStep = 4; // 128 points per 32-step period
    std::vector<int16_t> cycle;
    for (int v : steps)
    {
        for (int i = 0; i < kPerStep; i++)
        {
            cycle.push_back(int16_t(std::lround((v - mean) / 15.0 * 32767)));
        }
    }
    SetCycle(out, cycle);

    // At the root key the period plays at the frequency of wave note 36.
    const double period_hz = square_note0_hz_ / 2 * 8;
    out.sample_rate = uint32_t(std::lround(128 * period_hz));
    out.original_key = uint8_t(std::clamp(WaveKey(wave, 36), 0, 127));

    file_.samples.push_back(std::move(out));
    return cache_[key] = int(file_.samples.size()) - 1;
}

int SoundfontBuilder::NoiseSample(uint16_t setting)
{
    const int nr43 = setting & 0xFF;
    const auto key = std::make_pair(Kind::kNoise, nr43);
    if (auto it = cache_.find(key); it != cache_.end())
    {
        return it->second;
    }

    sf2::Sample out;
    char name[32];
    std::snprintf(name, sizeof name, "Noise %02X", unsigned(nr43));
    out.name = name;
    out.sample_rate = 32768; // the GBA's default output rate: the hardware point-samples the LFSR
    out.original_key = 60;

    const int shift = nr43 >> 4, ratio = nr43 & 7;
    const bool narrow = nr43 & 8;
    if (shift >= 14)
    {
        out.data.assign(64, 0); // no clock: silence
    }
    else
    {
        // The LFSR clock is 524288 / r / 2^(s+1) Hz (r = 0 counts as 0.5), so each 32768 Hz output point advances it by
        // 16 / den steps, exactly.
        const uint64_t den = uint64_t(ratio ? 2 * ratio : 1) << shift;
        const uint64_t period = narrow ? 127 : 32767;

        // Long enough for the point-sampled sequence to repeat seamlessly, up to 65536 points. The slowest noise
        // settings would need more, and loop with a seam.
        uint64_t length = period * den / std::gcd<uint64_t>(den, 16);
        if (length > 65536)
        {
            length = 65536;
        }

        uint32_t lfsr = 0x7FFF;
        uint64_t done = 0;
        for (uint64_t k = 0; k < length; k++)
        {
            const uint64_t target = k * 16 / den;
            for (; done < target; done++)
            {
                lfsr = NextLfsr(lfsr, narrow);
            }

            out.data.push_back((lfsr & 1) ? -32767 : 32767);
        }
    }

    // Loop the whole sequence, wrapped in copies of its ends as lead-in and lead-out.
    const size_t n = out.data.size();
    std::vector<int16_t> wrapped(out.data.end() - kLoopGuard, out.data.end());
    wrapped.insert(wrapped.end(), out.data.begin(), out.data.end());
    out.data = std::move(wrapped);
    out.loop_start = kLoopGuard;
    out.loop_end = uint32_t(kLoopGuard + n);
    AddLoopGuard(out);

    file_.samples.push_back(std::move(out));
    return cache_[key] = int(file_.samples.size()) - 1;
}

} // namespace agbsf2::konami
