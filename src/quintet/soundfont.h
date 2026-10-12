// SPDX-License-Identifier: MIT

// Builds SoundFont samples from the driver's sounds: Game Boy square waves and wave RAM shapes, noise drums rendered
// from the driver's noise macros, and the game's PCM samples.

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
#include "song_banks.h"

namespace supergbamidi::quintet
{

// The CPU cycles in a frame, and a second's.
constexpr uint64_t kFrameCycles = 280896;
constexpr uint64_t kSecondCycles = 16777216;

// The level of a PSG channel at volume v (0-15), as a full-scale waveform, relative to a full-scale FIFO sample: 2v of
// 128. That's how the GBA mixes them under the driver's settings: PSG at 100% and a master volume of 7.
constexpr int kPsgLevelPerVolume = 2;

// The level of a full-scale FIFO sample.
constexpr int kFifoLevel = 128;

// The names of the square channels' duty cycles.
inline constexpr const char* kDutyNames[4] = {"12.5%", "25%", "50%", "75%"};

// A wave channel's sound: the 32 steps of a bank, or the 64 of both, each 0-15.
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
// frame that changes them. Each is NR41 and NR42, then NR43 and NR44, with bit 15 set if the frame restarted the
// channel.
struct NoiseSound
{
    bool operator<(const NoiseSound& o) const
    {
        return frames < o.frames;
    }

    std::vector<std::pair<uint16_t, uint16_t>> frames;
};

// A PCM sound: the bytes of a sample that a FIFO plays from a note's start until the driver stops or loops it, and then
// those it plays on each pass through the loop. The driver stops or loops a FIFO at the end of a frame, so each pass
// lasts a whole number of frames.
struct PcmSound
{
    bool operator<(const PcmSound& o) const
    {
        return std::tie(start, from, first, loop, loop_length, looped, rate) <
               std::tie(o.start, o.from, o.first, o.loop, o.loop_length, o.looped, o.rate);
    }

    uint32_t start = 0; // the sample's data
    uint32_t from = 0;  // the first byte it plays, counted from the start
    uint32_t first = 0; // the bytes it plays from there
    uint32_t loop = 0;  // the byte each pass through the loop starts at
    uint32_t loop_length = 0;
    bool looped = false;
    uint32_t rate = 0; // the sample's rate in Hz
};

// Builds and caches SoundFont samples. The converter adds the instruments and presets to the same file.
class SoundfontBuilder
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive the builder. In a SoundFont that `songs` songs share,
    // SongBanks gives each song's presets their bank and programs.
    explicit SoundfontBuilder(const Rom& rom, int songs = 1);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the banks and programs of the presets.
    SongBanks& Banks()
    {
        return banks_;
    }

    // Each returns the index of the SF2 sample for a sound, which is made on first use and shared afterwards. A square
    // or wave sample plays its waveform at middle C on its root key; a noise sample is rendered at 65536 Hz, and loops
    // its end if it goes on unchanged; a PCM sample plays at its own rate on key 60. PcmSample() returns -1 for a sound
    // with nothing to play.
    int SquareSample(int duty);
    int WaveSample(const WaveShape& shape);
    int NoiseSample(const NoiseSound& sound);
    int PcmSample(const PcmSound& sound);

    // Returns how many keys above the pitch of its whole pattern a wave shape sounds: 12 for each time its first half
    // repeats.
    static int WaveKeyOffset(const WaveShape& shape);

    // Adds an instrument with `zones`. Returns its index.
    int AddInstrument(const std::string& name, std::vector<sf2::Zone> zones);

    // Adds a preset that plays instrument `instrument`.
    void AddPreset(const std::string& name, int bank, int program, int instrument);

private:
    const Rom& rom_;
    Sf2File file_;
    SongBanks banks_;
    std::map<int, int> squares_;
    std::map<WaveShape, int> waves_;
    std::map<NoiseSound, int> noises_;
    std::map<PcmSound, int> pcms_;
    std::map<uint32_t, int> pcm_copies_; // the number of SF2 samples made from each of the game's samples
};

} // namespace supergbamidi::quintet
