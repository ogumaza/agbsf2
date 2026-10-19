// SPDX-License-Identifier: MIT

// The GBA's PSG: the Game Boy's four sound channels, two squares, a wave channel and a noise channel. The tables and
// the noise generator's step that the drivers share, and the SoundFont samples of the channels' sounds.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sf2.h"

namespace agbsf2
{

// The names of the square channels' duty cycles.
inline constexpr const char* kDutyNames[4] = {"12.5%", "25%", "50%", "75%"};

// The square channels' duty waveforms: 8 steps each, most significant bit first.
constexpr uint8_t kDutyPatterns[4] = {0x01, 0x81, 0x87, 0x7E};

// Returns the name of a square channel's sound at duty `duty` (0-3), such as "Square 12.5%".
std::string SquareName(int duty);

// Returns a cycle of a square channel's waveform at duty `duty` (0-3): 64 points, 8 for each step. A high step's
// points are at `amplitude`. A low step's points are at -`amplitude`.
std::vector<int16_t> SquareCycle(int duty, int16_t amplitude);

// Middle C, which the square and wave samples play on their root key.
constexpr double kMiddleC = 261.6255653005986;

// Returns the frequency in Hz of a square channel at frequency setting `setting` (0-2047). A cycle lasts for
// 2048 - `setting` steps of a 131072 Hz clock. At the same setting, the wave channel's pattern of 32 steps plays an
// octave lower.
constexpr double SquareHz(int setting)
{
    return 131072.0 / (2048 - setting);
}

// Returns the semitones from a square or wave channel's frequency setting `from` up to `to`. Both are 0-2047.
inline double PsgSemitones(int from, int to)
{
    return 12 * std::log2(double(2048 - from) / double(2048 - to));
}

// The rate in Hz of a sample of the noise generator's whole sequence. The sample has a point for each of the
// generator's steps. A zone tunes the sample to the generator's clock.
constexpr uint32_t kNoiseSequenceRate = 32768;

// Returns the noise channel's LFSR after a step of its clock. `narrow` selects the 7-bit sequence, NR43's bit 3.
constexpr uint32_t NextLfsr(uint32_t lfsr, bool narrow)
{
    const uint32_t bit = (lfsr ^ (lfsr >> 1)) & 1;
    lfsr = (lfsr >> 1) | (bit << 14);
    if (narrow)
    {
        lfsr = (lfsr & ~0x40u) | (bit << 6);
    }

    return lfsr;
}

// The level of a full-scale FIFO sample.
constexpr int kFifoLevel = 128;

// The level of a PSG channel at volume v (0-15) on one side, as a full-scale waveform, relative to a full-scale FIFO
// sample: 2v of kFifoLevel. That's how the GBA mixes them with the PSG at 100% and a master volume of 7. The PSG puts
// out 0 to v on a channel. The master volume scales that by 8, and the 100% setting divides it by 4. The FIFOs' samples
// are scaled by 4. PsgScaleOf() gives the factor for other settings.
constexpr int kPsgLevelPerVolume = 2;

// The PSG's level on each side: the master volume and the PSG's share of the mix.
struct PsgScale
{
    double left = 1, right = 1;
};

// Returns the PSG's level on each side under SOUNDCNT_L, whose low byte is NR50, and SOUNDCNT_H.
PsgScale PsgScaleOf(uint16_t soundcnt_l, uint16_t soundcnt_h);

// Returns the share of the wave channel's full level that SOUND3CNT_H sets. The register holds NR31 in its low byte and
// NR32 in its high byte. Bits 13 and 14 give 0%, 100%, 50% or 25%. Bit 15 gives 75% instead.
double WaveShare(uint16_t sound3cnt_h);

// The registers of a PSG channel after a frame, as PsgModel reads them.
struct PsgRegisters
{
    uint16_t nr10 = 0;      // square 1's sweep
    uint16_t envelope = 0;  // NRx1 and NRx2: the length, and the envelope or the wave's level
    uint16_t frequency = 0; // NRx3 and NRx4: the frequency setting, and the switch of the length counter
    uint16_t nr30 = 0;      // the wave channel's DAC, in bit 7
    bool restarted = false; // whether the frame restarted the channel
};

// A model of a PSG channel's on state, volume, length counter and frequency sweep, from frame to frame.
class PsgModel
{
public:
    // Models PSG channel `channel`: 0 and 1 for the squares, 2 for the wave and 3 for the noise.
    explicit PsgModel(int channel);

    // Takes in the registers after a frame. Returns the channel's volume at the frame's start (0-15), or -1 if it's
    // off.
    int Frame(const PsgRegisters& r);

    // Returns the frequency setting the channel plays at the start of the last frame.
    int Frequency() const
    {
        return start_frequency_;
    }

    // Returns the changes the sweep made to the frequency in the last frame, as (position in the frame, frequency).
    const std::vector<std::pair<double, int>>& Sweeps() const
    {
        return sweeps_;
    }

private:
    void Restart(uint16_t envelope, uint16_t frequency);

    // Returns the frequency that the sweep's next step works out from its copy of the frequency.
    int SweepNext() const;

    // Runs one step of the 512 Hz frame sequencer.
    void Tick();

    int channel_;
    bool on_ = false;
    int volume_ = 0;
    bool up_ = false;
    int step_ = 0;
    int envelope_ticks_ = 0;
    bool length_on_ = false;
    int length_ = 0;
    int frequency_ = 0; // the frequency setting that plays
    int written_ = 0;   // the frequency setting last written
    int shadow_ = 0;    // the sweep's copy of the frequency setting
    bool sweeping_ = false;
    int sweep_time_ = 0;
    int sweep_shift_ = 0;
    bool sweep_down_ = false;
    int sweep_ticks_ = 0;
    uint64_t cycles_ = 0;
    int sequencer_step_ = 0;
    int start_frequency_ = 0;
    std::vector<std::pair<double, int>> sweeps_;
};

// Takes a PSG channel's model through a song's frames: each frame once and in order.
class PsgFeed
{
public:
    // Feeds a model of PSG channel `channel` (see PsgModel). `registers(f)` returns the channel's registers after frame
    // `f`. Whatever `registers` refers to has to outlive the feed.
    PsgFeed(int channel, std::function<PsgRegisters(uint32_t)> registers);

    // Takes in the frames up to frame `f`. Frame `f` has to come after the frames taken in already. Returns the
    // channel's volume at the start of frame `f` (0-15), or -1 if the channel is off.
    int Volume(uint32_t f);

    // Returns the model. It holds the frequency and the sweep's changes of the last frame taken in.
    const PsgModel& Model() const
    {
        return model_;
    }

private:
    std::function<PsgRegisters(uint32_t)> registers_;
    PsgModel model_;
    uint32_t fed_ = 0; // the frames taken in
};

// A wave channel's sound: the 32 steps of a bank of the wave RAM, or the 64 of both, each 0-15.
struct WaveShape
{
    // Compares the steps one by one. GCC wrongly warns about the memcmp in the vectors' operator< at -O3.
    bool operator<(const WaveShape& o) const
    {
        return std::lexicographical_compare(steps.begin(), steps.end(), o.steps.begin(), o.steps.end(),
                                            [](uint8_t a, uint8_t b) { return a < b; });
    }

    std::vector<uint8_t> steps;
};

// The wave shapes that a conversion's notes play. The shapes are numbered in the order of their first use.
class WaveShapes
{
public:
    // Returns the number of `shape`. A new shape gets the next number.
    int Number(const WaveShape& shape);

    // Returns shape number `number`.
    const WaveShape& operator[](int number) const
    {
        return list_[size_t(number)];
    }

private:
    std::map<WaveShape, int> numbers_;
    std::vector<WaveShape> list_;
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

    // Adds a frame that leaves NR41 and NR42 at `nr41`, and NR43 and NR44 at `nr43`. The frame restarts the channel if
    // `restarted` is set or it's the note's first.
    void AddFrame(uint16_t nr41, uint16_t nr43, bool restarted);

    // Drops the frames at the end that leave the registers as the frame before them did and don't restart the
    // channel. After the last frame that's left, the channel plays on with no more writes.
    void Trim();

    std::vector<std::pair<uint16_t, uint16_t>> frames;
};

// Returns a looped sample at kNoiseSequenceRate of the noise generator's whole sequence: the 15-bit sequence, or with
// `narrow`, the 7-bit one. The sample's pitch is on key 60. The 7-bit sequence repeats so that the loop isn't too
// short. Each point is `level` of full scale either way.
sf2::Sample NoiseSequence(bool narrow, double level);

// Returns how many keys above the pitch of its whole pattern a wave shape sounds: 12 for each time its first half
// repeats.
int WaveKeyOffset(const WaveShape& shape);

// The SoundFont samples of the PSG's sounds in a file. Each sample is made on first use and shared afterwards. A square
// or wave sample plays its waveform at middle C on its root key. A noise sample loops its end if it goes on unchanged.
class PsgSamples
{
public:
    // Renders the noise samples at `noise_rate` Hz, the PSG's output rate under the driver's SOUNDBIAS setting.
    explicit PsgSamples(uint32_t noise_rate);

    // Each returns the index in `file` of the sample for a sound, and adds the sample to `file` on first use.
    int Square(Sf2File& file, int duty);
    int Wave(Sf2File& file, const WaveShape& shape);
    int Noise(Sf2File& file, const NoiseSound& sound);

    // Each returns the name and zones of an instrument whose notes switch from duty or wave shape `first` to `second`
    // after `frames` frames (SwitchZones()). If `frames` is 0, the notes play `first` alone. The zones are tuned by
    // `cents`. Both samples are added to `file` on first use. `shapes` holds the wave shapes by number.
    NamedZones SquareInstrument(Sf2File& file, int first, int frames, int second, int cents = 0);
    NamedZones WaveInstrument(Sf2File& file, const WaveShapes& shapes, int first, int frames, int second,
                              int cents = 0);

private:
    uint32_t noise_rate_;
    std::map<int, int> squares_;
    std::map<WaveShape, int> waves_;
    std::map<NoiseSound, int> noises_;
};

} // namespace agbsf2
