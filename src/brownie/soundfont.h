// SPDX-License-Identifier: MIT

// Builds SoundFont samples from the driver's sounds: Game Boy square waves and wave RAM shapes, noise drums rendered
// from the driver's register writes, and the game's samples. InstrumentSet builds the instruments that play them.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "psg.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::brownie
{

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

// Builds and caches SoundFont samples. InstrumentSet::Build() adds the instruments and presets to the same file.
class SoundfontBuilder
{
public:
    // Keeps a reference to `rom`, which has to outlive the builder. The game's samples play at `rate` Hz.
    SoundfontBuilder(const Rom& rom, uint32_t rate);

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

    // Each returns the name and zones of a square or wave instrument whose notes switch from duty or wave shape
    // `first` to `second` after `frames` frames (PsgSamples::SquareInstrument() and WaveInstrument()). The zones are
    // tuned by `cents`. `shapes` holds the wave shapes by number.
    NamedZones SquareInstrument(int first, int frames, int second, int cents);
    NamedZones WaveInstrument(const WaveShapes& shapes, int first, int frames, int second, int cents);

    // Adds an instrument with `zones`. Returns its index.
    int AddInstrument(const std::string& name, std::vector<sf2::Zone> zones);

    // Adds a preset that plays instrument `instrument`.
    void AddPreset(const std::string& name, int bank, int program, int instrument);

private:
    const Rom& rom_;
    uint32_t rate_;
    Sf2File file_;
    PsgSamples psg_;
    std::map<GameSample, int> samples_;
};

// An instrument that notes play.
struct Instrument
{
    enum Kind : uint8_t
    {
        kSquare,
        kWave,
        kNoise,
        kSamples,
    };

    bool operator<(const Instrument& o) const
    {
        return std::tie(kind, first, frames, second, set) < std::tie(o.kind, o.first, o.frames, o.second, o.set);
    }

    Kind kind = kSquare;
    int first = 0;    // the duty or wave shape at a note's start, or a noise note's kit
    int frames = 0;   // the frames before it switches, or 0
    int second = 0;   // the duty or wave shape it switches to
    uint32_t set = 0; // a sample note's sample set, or in the Magical Vacation revision its sample's start
};

// The instruments that songs play, numbered in the order they're first played, which become a SoundFont's presets. One
// set can serve all of a game's songs, for a shared SoundFont.
class InstrumentSet
{
public:
    // Returns an instrument's program number, adding the instrument if it's new. Programs from 128 on go in the banks
    // after the first.
    int Program(const Instrument& inst);

    // Returns a wave shape's number, adding the shape if it's new.
    int Shape(const WaveShape& shape);

    const WaveShape& ShapeAt(int index) const
    {
        return shapes_[index];
    }

    // Returns a noise drum's kit and key, adding the drum if it's new: each kit has 92 drums, from key 36.
    std::pair<int, int> Drum(const NoiseSound& sound);

    // Adds a key of a sample set's instrument: the sample it plays, and the key and the cents that play the sample at
    // the rate the SoundFont gives it.
    void AddSampleKey(uint32_t set, int key, const GameSample& sample, int root, int cents = 0);

    // Adds the instruments and their presets to `sf`, with the PSG's square and wave zones tuned by `cents`. Returns
    // a warning for each key of a sample set whose sample isn't in the ROM.
    std::vector<std::string> Build(SoundfontBuilder& sf, int cents) const;

private:
    // A key of a sample set's instrument.
    struct SampleKey
    {
        GameSample sample;
        int root = 60;
        int cents = 0;
    };

    // The parts of Build() for the noise kits and the sample sets. Each returns the instrument's name and zones.
    // BuildSamples() adds a warning to `warnings` for each key whose sample isn't in the ROM.
    NamedZones BuildDrums(SoundfontBuilder& sf, const Instrument& inst) const;
    NamedZones BuildSamples(SoundfontBuilder& sf, const Instrument& inst, std::vector<std::string>& warnings) const;

    std::map<Instrument, int> programs_;
    WaveShapes shapes_;
    std::map<NoiseSound, int> drums_; // each drum's number, in order of first use
    std::map<uint32_t, std::map<int, SampleKey>> sample_keys_;
};

} // namespace agbsf2::brownie
