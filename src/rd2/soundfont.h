// SPDX-License-Identifier: MIT

// Builds SoundFont samples from the driver's sounds: the game's 8-bit samples and the Game Boy's square waves, wave
// patterns and noise.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rd2/driver.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"

namespace agbsf2::rd2
{

// Returns the level of a PSG voice at its full volume, as a share of a full-scale sample at the driver's default
// volumes. The driver mixes the PSG at 50%. Volume 15 is therefore 15/128 of the GBA's full scale. A sample voice plays
// a full-scale sample at about 47/128 on each side, or 31/128 in the Super Mario Advance 2 and 3 revisions.
constexpr double PsgShare(Revision revision)
{
    return revision == Revision::kLinkToThePast ? 15.0 / 47.0 : 15.0 / 31.0;
}

// Returns the SoundFont release of a sample voice for a region's release value. The release takes a share of the
// voice's level off each frame.
int SampleRelease(uint8_t release);

// Returns true if SoundfontBuilder::GameSample() can make a sample from the header at `address`. The sample requires
// points, a rate and a loop that starts before it ends. Its points have to be inside the ROM.
bool SampleReadable(const Rom& rom, uint32_t address);

// A SoundFont being built, with a cache of the samples made for it. The converter adds the instruments and presets.
class SoundfontBuilder
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive the builder. The square, wave and noise samples play at
    // `psg_share` of full scale, the PSG's level against a full-scale sample that PsgShare() gives. The level is in the
    // samples rather than the zones' attenuation. Synthesizers differ in the share of a zone's attenuation that they
    // apply. They all play a sample's level. In a SoundFont that `songs` sequences share, SongBanks gives each
    // sequence's presets their bank and programs.
    SoundfontBuilder(const Rom& rom, double psg_share, int songs = 1);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the banks and programs of the presets.
    SongBanks& Banks()
    {
        return banks_;
    }

    // Each returns the index of the SF2 sample for a sound, which is made on first use and shared afterwards.
    // GameSample() takes the address of a sample's header and returns -1 if it can't be read; the sample plays at its
    // own rate on key 60. A square sample plays middle C on key 60, a wave sample C3 on key 48, and a noise sample its
    // NR43 setting's noise on key 60.
    int GameSample(uint32_t address);
    int SquareSample(int duty);
    int WaveSample(uint32_t address);
    int NoiseSample(uint8_t nr43);

    // Adds an instrument with `zones`. Returns its index.
    int AddInstrument(const std::string& name, std::vector<sf2::Zone> zones);

    // Adds a preset that plays instrument `instrument`.
    void AddPreset(const std::string& name, int bank, int program, int instrument);

private:
    const Rom& rom_;
    double psg_share_;
    Sf2File file_;
    SongBanks banks_;
    std::map<uint32_t, int> game_samples_;
    std::map<int, int> squares_;
    std::map<uint32_t, int> waves_;
    std::map<uint8_t, int> noises_;
};

} // namespace agbsf2::rd2
