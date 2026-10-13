// SPDX-License-Identifier: MIT

// The driver's sequence format: the meaning of each command in a track.

#pragma once

#include <cstdint>
#include <string>

#include "rd2/driver.h"
#include "rom.h"

namespace agbsf2::rd2
{

// A command of a track, as the driver reads it.
struct Command
{
    uint32_t size = 1;   // in bytes
    uint32_t ticks = 0;  // a wait's ticks, or a note's length
    bool note = false;   // 00-BF
    bool wait = false;   // C0 and C1
    bool end = false;    // FF
    uint32_t target = 0; // F0's, F4's and F8's address, as an offset from the sequence's start
    uint16_t length = 0; // a note's new length, or C1's new wait, if it gives one
    bool has_length = false;
    std::string text;
};

// Decodes the command at `address` of a track in the driver's revision. `length`, `velocity` and `wait` are the track's
// settings for notes and waits that don't give their own.
Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, uint16_t length, uint8_t velocity,
                      uint16_t wait);

// Writes a text listing of every command of each track of a sequence, as the driver plays them. Returns false and sets
// `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::rd2
