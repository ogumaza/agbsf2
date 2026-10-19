// SPDX-License-Identifier: MIT

// The SoundFont of the game's samples that a module's notes play.

#pragma once

#include <cstdint>
#include <map>
#include <utility>

#include "krawall/driver.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::krawall
{

// The note that the player plays a sample at its rate on, C-4, and the MIDI key that the SoundFont gives it.
constexpr uint32_t kRootNote = 48;
constexpr int kRootKey = 60;

// Each instrument is a game sample played from a given offset. Instruments become SoundFont presets, numbered in order
// of first use. A shared SoundFont can use one set for all the game's modules.
class InstrumentSet
{
public:
    // Returns the program of sample `sample`, number `index` in the game's table, played from point `offset`, adding it
    // if it's new. Programs from 128 on go in the banks after the first.
    int Program(uint32_t sample, int index, uint32_t offset);

    // Returns the SoundFont of the instruments.
    Sf2File Build(const Rom& rom, const DriverInfo& info) const;

private:
    struct Instrument
    {
        int index = 0;
        int program = 0;
    };

    std::map<std::pair<uint32_t, uint32_t>, Instrument> programs_;
};

// Returns the rate in Hz at which the player plays a sample's C-4, which is the rate that its SoundFont sample gets, on
// key 60.
uint32_t SampleRate(const Rom& rom, const DriverInfo& info, uint32_t sample);

} // namespace agbsf2::krawall
