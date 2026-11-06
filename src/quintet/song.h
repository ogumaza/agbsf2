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

// In the S revision, a byte below 80 starts a note: the byte gives its pitch, and the byte after it the note value.
// Pitches 0 to 11 are notes, 14 plays again and 15 rests. The other pitches only wait for the note's length.
constexpr uint8_t kFirstCommandS = 0x80;
constexpr uint8_t kPlayAgainS = 14;
constexpr uint8_t kRestS = 15;

// The S revision's command that skips the byte after it. The A revision has no such command.
constexpr uint8_t kSkipByteS = 0xB0;

// The S revision's numbers for D1 and D4. A note looks for them after it.
constexpr uint8_t kDotS = 0x9B;
constexpr uint8_t kTieS = 0x9E;

// The S revision numbers its commands from 80, in the order of the A revision's: 0E to CF, D0 to E5, its command that
// skips a byte, and E6 to EF. Returns the A revision's command for the S revision's command `command`. A command that
// the A revision lacks gives F2. F2 does nothing there.
uint8_t CommandOfS(uint8_t command);

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
    uint32_t size = 1;   // in bytes
    uint32_t ticks = 0;  // a note's or rest's length in ticks, before any dot or tie
    bool note = false;   // a note (0-11), the last note played again (12) or a rest (13)
    bool end = false;    // FF: the channel ends or goes back to its loop point
    uint8_t command = 0; // a command in the A revision's numbering
    std::string text;
};

// Decodes the command at `address` of channel `channel`.
Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, int channel);

// Writes a text listing of every command of each channel of a song, as the driver plays them. Returns false and sets
// `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::quintet
