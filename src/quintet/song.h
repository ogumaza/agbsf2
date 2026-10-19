// SPDX-License-Identifier: MIT

// Quintet's song format: a song's header, and the meaning of each command in a channel's data.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "quintet/driver.h"
#include "rom.h"

namespace agbsf2::quintet
{

// The number of channels in a song.
constexpr int kChannels = 6;

// The channels of a song, one for each of the GBA's sound channels.
constexpr int kSquare1 = 0;
constexpr int kSquare2 = 1;
constexpr int kWave = 2;
constexpr int kNoise = 3;
constexpr int kPcmA = 4; // DirectSound FIFO A
constexpr int kPcmB = 5; // DirectSound FIFO B

// A note's length index picks a note value from the length table. The note lasts 384 ticks divided by that value.
constexpr int32_t kWholeNote = 384;

// A song's header: its length in bytes and the start of each of its 6 channels' data.
struct SongHeader
{
    uint32_t address = 0;
    uint32_t length = 0;
    std::array<uint32_t, kChannels> channels = {};
};

// Reads the header of the song at `address`. Returns false if it isn't one the driver can read: a length that covers
// the header, and 6 channels that start inside the song.
bool ReadSongHeader(const Rom& rom, uint32_t address, SongHeader& header);

// A command of a channel's data, as the driver reads it.
struct Command
{
    uint32_t size = 1;  // in bytes
    uint32_t ticks = 0; // a note's or rest's length in ticks, before any dot or tie
    bool note = false;  // a note (0-11), the last note played again (12) or a rest (13)
    bool end = false;   // FF, which ends the channel or goes back to its loop point
    std::string text;
};

// Decodes the command at `address` of channel `channel`.
Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, int channel);

// Writes a text listing of every command of each channel of a song, as the driver plays them. Returns false and sets
// `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::quintet
