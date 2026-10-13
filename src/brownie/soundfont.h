// SPDX-License-Identifier: MIT

// Builds SoundFont samples from the driver's sounds: Game Boy square waves and wave RAM shapes, noise drums rendered
// from the driver's register writes, and the game's samples.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "rom.h"
#include "sf2.h"

namespace agbsf2::brownie
{

// The level of a PSG channel at volume v (0-15), as a full-scale waveform, relative to a full-scale FIFO sample: 2v of
// 128. That's how the GBA mixes them under the driver's settings: PSG at 100% and a master volume of 7.
constexpr int kPsgLevelPerVolume = 2;

// The level of a full-scale FIFO sample.
constexpr int kFifoLevel = 128;

// The names of the square channels' duty cycles.
inline constexpr const char* kDutyNames[4] = {"12.5%", "25%", "50%", "75%"};

// A wave channel's sound: the 32 steps of a bank, each 0-15.
struct WaveShape
{
    // Compares the steps one by one, since GCC wrongly warns about the memcmp in the vectors' operator< at -O3.
    bool operator<(const WaveShape& o) const
    {
        return std::lexicographical_compare(steps.begin(), steps.end(), o.steps.begin(), o.steps.end(),
                                            [](uint8_t a, uint8_t b) { return a < b; });
    }

    std::vector<uint8_t> steps;
};

// A noise channel's sound: the registers that the driver leaves after each frame of a note, from its start to the last
// frame that changes them. Each is NR41 and NR42, then NR43 and NR44, with bit 15 set if the frame started the channel
// again.
struct NoiseSound
{
    bool operator<(const NoiseSound& o) const
    {
        return frames < o.frames;
    }

    std::vector<std::pair<uint16_t, uint16_t>> frames;
};

// A sample of the game's: its 8-bit points from `start` up to `end`, going back to `loop` at the end, unless `loop` is
// `end`.
struct GameSample
{
    bool operator<(const GameSample& o) const
    {
        return std::tie(start, end, loop) < std::tie(o.start, o.end, o.loop);
    }

    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t loop = 0;
};

// Builds and caches SoundFont samples. The converter adds the instruments and presets to the same file.
class SoundfontBuilder
{
public:
    // Keeps a reference to `rom`, which has to outlive the builder. The game's samples play at `rate` Hz.
    SoundfontBuilder(const Rom& rom, uint32_t rate);

    // Returns how many keys above the pitch of its whole pattern a wave shape sounds: 12 for each time its first half
    // repeats.
    static int WaveKeyOffset(const WaveShape& shape);

    Sf2File& File()
    {
        return file_;
    }

    // Each returns the index of the SF2 sample for a sound, which is made on first use and shared afterwards. A square
    // or wave sample plays its waveform at middle C on its root key; a noise sample is rendered at 32768 Hz, and loops
    // its end if it goes on unchanged; a game's sample plays at the mixer's rate on key 60. GameSampleIndex() returns
    // -1 for a sample that isn't in the ROM or has nothing to play.
    int SquareSample(int duty);
    int WaveSample(const WaveShape& shape);
    int NoiseSample(const NoiseSound& sound);
    int GameSampleIndex(const GameSample& sample);

    // Adds an instrument with `zones`. Returns its index.
    int AddInstrument(const std::string& name, std::vector<sf2::Zone> zones);

    // Adds a preset that plays instrument `instrument`.
    void AddPreset(const std::string& name, int bank, int program, int instrument);

private:
    const Rom& rom_;
    uint32_t rate_;
    Sf2File file_;
    std::map<int, int> squares_;
    std::map<WaveShape, int> waves_;
    std::map<NoiseSound, int> noises_;
    std::map<GameSample, int> samples_;
};

} // namespace agbsf2::brownie
