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

// Returns the command that byte `b` runs in revision `r`: in the Magical Vacation revision, E2-E9 are F2-F9 again and
// EF is FF.
uint8_t CommandOf(Revision r, uint8_t b);

// Returns true if command `b`, as CommandOf() returns it, does nothing in revision `r`. Such a command has no argument.
// They're E4-E7, EB and EF in the Sword of Mana revision, and E0, EA, EB and EE in the Magical Vacation revision.
bool IsNoOp(Revision r, uint8_t b);

// Returns the NR43 setting that noise note `b` (00-7F) plays: the byte with its nibbles swapped.
constexpr uint8_t NoiseNr43(uint8_t b)
{
    return uint8_t(((b << 4) + (b >> 4)) & 0xFF);
}

// Decodes the command at `address` of channel `channel`.
Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, int channel);

// Returns the byte of the length table that length command `b` (80-DF) reads with length set `set`: the frames that
// each note or rest then lasts, with 0 for 256. The Magical Vacation revision has one table of lengths, in groups of 16
// for each tempo, and no length sets.
uint8_t NoteLength(const Rom& rom, const DriverInfo& info, uint8_t b, int set);

// Writes a text listing of every command of each channel of a sound, as the driver plays them. Returns false and sets
// `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::brownie
