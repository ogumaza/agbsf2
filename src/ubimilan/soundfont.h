// SPDX-License-Identifier: MIT

// SoundFont instruments for Ubisoft Milan's driver: channel 9's kit, whose keys play samples or the PSG's noise, and
// the PSG channels' instruments.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rom.h"
#include "sf2.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{

// Returns the MIDI bank of sequence channel `channel`'s presets: the bank numbered after the channel. A PSG channel's
// programs are its instruments, and channel 9's are the offsets into its kit.
constexpr int PresetBank(int channel)
{
    return channel;
}

// Returns the name of sequence channel `channel`: "Square 1", "Square 2" or "Wave" for a PSG channel, and "Kit" for
// channel 9. The channel's MIDI track has the name, and its instruments' names start with it.
const char* ChannelName(int channel);

// The instruments that songs play, which become a SoundFont's presets. One set can serve all of a game's songs, for a
// shared SoundFont.
class InstrumentSet
{
public:
    // Adds key `key` of channel 9's kit at offset `program`, which plays the sample resource at `sample`.
    void AddKitKey(int program, int key, uint32_t sample);

    // Adds key `key` of channel 9's kit at offset `program`, which plays the PSG's noise with NR43 set to `control`.
    void AddNoiseKey(int program, int key, uint8_t control);

    // Adds instrument `program` of square channel `channel`, 0 or 1, whose notes set NRx1 to `duty`. NRx1's top 2 bits
    // are the duty.
    void AddSquare(int channel, int program, uint8_t duty);

    // Adds the wave channel's program `program`.
    void AddWave(int program);

    // Adds the instruments and their presets to `file`, with the samples at the mixer's rate. Returns the warnings for
    // instruments with nothing to play.
    std::vector<std::string> Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const;

private:
    std::map<int, std::map<int, uint32_t>> kit_keys_;  // offset -> key -> sample resource
    std::map<int, std::map<int, uint8_t>> noise_keys_; // offset -> key -> NR43
    std::map<std::pair<int, int>, uint8_t> squares_;   // (channel, program) -> NRx1
    std::set<int> waves_;                              // the wave channel's programs
};

} // namespace agbsf2::ubimilan
