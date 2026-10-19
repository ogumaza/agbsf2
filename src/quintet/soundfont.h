// SPDX-License-Identifier: MIT

// Builds SoundFont samples from the driver's sounds: Game Boy square waves and wave RAM shapes, noise drums rendered
// from the driver's noise macros, and the game's PCM samples.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "psg.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"

namespace agbsf2::quintet
{

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

    // Each returns the name and zones of a square or wave instrument whose notes switch from duty or wave shape
    // `first` to `second` after `frames` frames (PsgSamples::SquareInstrument() and WaveInstrument()). `shapes` holds
    // the wave shapes by number.
    NamedZones SquareInstrument(int first, int frames, int second);
    NamedZones WaveInstrument(const WaveShapes& shapes, int first, int frames, int second);

    // Adds an instrument with `zones`. Returns its index.
    int AddInstrument(const std::string& name, std::vector<sf2::Zone> zones);

    // Adds a preset that plays instrument `instrument`.
    void AddPreset(const std::string& name, int bank, int program, int instrument);

private:
    const Rom& rom_;
    Sf2File file_;
    SongBanks banks_;
    PsgSamples psg_;
    std::map<PcmSound, int> pcms_;
    std::map<uint32_t, int> pcm_copies_; // the number of SF2 samples made from each of the game's samples
};

} // namespace agbsf2::quintet
