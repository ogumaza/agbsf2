// SPDX-License-Identifier: MIT

#include "quintet/soundfont.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "psg.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::quintet
{
namespace
{

// The rate at which noise is rendered: the PSG's output rate under the driver's SOUNDBIAS setting.
constexpr uint32_t kNoiseRate = 65536;

} // namespace

SoundfontBuilder::SoundfontBuilder(const Rom& rom, int songs) : rom_(rom), banks_(songs), psg_(kNoiseRate)
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

int SoundfontBuilder::PcmSample(const PcmSound& sound)
{
    if (auto it = pcms_.find(sound); it != pcms_.end())
    {
        return it->second;
    }

    const uint32_t loop_length = sound.looped ? sound.loop_length : 0;
    if (sound.first + loop_length == 0 || !rom_.Contains(sound.start + sound.from, sound.first) ||
        !rom_.Contains(sound.start + sound.loop, loop_length))
    {
        return pcms_[sound] = -1;
    }

    // A sample that plays at several rates, which make its passes different lengths, gets a numbered copy for each.
    sf2::Sample out;
    const int copy = ++pcm_copies_[sound.start];
    out.name = AddressName("PCM", sound.start);
    if (copy > 1)
    {
        out.name += " " + std::to_string(copy);
    }

    out.sample_rate = sound.rate;
    out.original_key = 60;

    // The bytes from the start, then a pass through the loop, which is the SF2 sample's loop. The samples are 8-bit and
    // signed.
    for (uint32_t i = 0; i < sound.first; i++)
    {
        out.data.push_back(int16_t(rom_.S8(sound.start + sound.from + i) * 256));
    }
    for (uint32_t i = 0; i < loop_length; i++)
    {
        out.data.push_back(int16_t(rom_.S8(sound.start + sound.loop + i) * 256));
    }

    if (loop_length)
    {
        out.loop_start = sound.first;
        out.loop_end = sound.first + loop_length;
        LengthenLoop(out);
        AddLoopGuard(out);
    }
    else
    {
        out.data.insert(out.data.end(), kLoopGuard, int16_t(0));
    }

    file_.samples.push_back(std::move(out));
    return pcms_[sound] = int(file_.samples.size()) - 1;
}

NamedZones SoundfontBuilder::SquareInstrument(int first, int frames, int second)
{
    return psg_.SquareInstrument(file_, first, frames, second);
}

NamedZones SoundfontBuilder::WaveInstrument(const WaveShapes& shapes, int first, int frames, int second)
{
    return psg_.WaveInstrument(file_, shapes, first, frames, second);
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

} // namespace agbsf2::quintet
