// SPDX-License-Identifier: MIT

// SoundFont instruments for Ubisoft Milan's driver: channel 9's kit, whose keys play samples or the PSG's noise, and
// the PSG channels' instruments.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "rom.h"
#include "sf2.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{

// The MIDI bank of channel 9's kits. Their programs are the kit's offsets. Like the PSG channels' instruments, the kits
// are in the bank numbered after their channel.
constexpr int kKitBank = kKitChannel;

// The instruments that songs play, which become a SoundFont's presets. One set can serve all of a game's songs, for a
// shared SoundFont.
class InstrumentSet
{
public:
    // Adds key `key` of channel 9's kit at offset `program`, which plays the sample resource at `sample`.
    void AddKitKey(int program, int key, uint32_t sample);

    // Adds key `key` of channel 9's kit at offset `program`, which plays the PSG's noise with NR43 set to `control`.
    void AddNoiseKey(int program, int key, uint8_t control);

    // Adds instrument `program` of PSG channel `channel`: a square with NRx1 set to `duty`, or a wave program.
    void AddPsg(int channel, int program, int setting);

    // Adds the instruments and their presets to `file`, with the samples at the mixer's rate. Returns the warnings for
    // instruments with nothing to play.
    std::vector<std::string> Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const;

private:
    std::map<int, std::map<int, uint32_t>> kit_keys_;  // offset -> key -> sample resource
    std::map<int, std::map<int, uint8_t>> noise_keys_; // offset -> key -> NR43
    std::map<std::pair<int, int>, int> psg_;           // (channel, program) -> NRx1, or the wave's program
};

// The level of a PSG channel at full volume against a full-scale sample. The driver sets SOUNDCNT_L's PSG volume to 3
// of 7, so a channel at volume 15 swings 60 either side of its middle, out of a full scale of -512 to 512, and the
// mixer gives a full-scale sample at velocity 64 the whole scale.
constexpr double kPsgLevel = 60.0 / 512;

} // namespace agbsf2::ubimilan
