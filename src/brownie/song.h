// SPDX-License-Identifier: MIT

// The driver's sequence format: the meaning of each command in a channel's data.

#pragma once

#include <cstdint>
#include <string>

#include "brownie/driver.h"
#include "rom.h"

namespace agbsf2::brownie
{

// A command of a channel's data, as the driver reads it.
struct Command
{
    uint32_t size = 1;  // in bytes
    bool waits = false; // a note, rest, hold or release, which ends the channel's reading until its length is over
    bool end = false;   // FF
    std::string text;
};

// Decodes the command at `address` of channel `channel`.
Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, int channel);

// Writes a text listing of every command of each channel of a sound, as the driver plays them. Returns false and sets
// `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::brownie
