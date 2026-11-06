// SPDX-License-Identifier: MIT
// Reading the Standard MIDI Files that Nintendo R&D1's driver plays.

#include "rd1/song.h"

#include <cstdint>
#include <string>

#include "rd1/driver.h"
#include "rom.h"

namespace agbsf2::rd1
{

namespace
{

uint32_t BigEndian32(const Rom& rom, uint32_t at)
{
    return uint32_t(rom.U8(at)) << 24 | uint32_t(rom.U8(at + 1)) << 16 | uint32_t(rom.U8(at + 2)) << 8 | rom.U8(at + 3);
}

uint16_t BigEndian16(const Rom& rom, uint32_t at)
{
    return uint16_t(rom.U8(at) << 8 | rom.U8(at + 1));
}

} // namespace

uint32_t ReadVariable(const Rom& rom, uint32_t& at)
{
    uint32_t value = 0;
    for (;;)
    {
        const uint8_t b = rom.U8(at++);
        value = value << 7 | (b & 0x7F);
        if (!(b & 0x80) || !rom.Contains(at))
        {
            return value;
        }
    }
}

bool ReadSongFile(const Rom& rom, uint32_t address, SongFile& file, std::string& error)
{
    // The start routine skips "MThd", reads the header's length, then the format, the track count and the division.
    if (!rom.Contains(address, 14) || BigEndian32(rom, address) != 0x4D546864u)
    {
        error = "no MIDI file at " + Hex(address);
        return false;
    }
    const uint32_t header = BigEndian32(rom, address + 4);
    file.format = BigEndian16(rom, address + 8);
    const int tracks = BigEndian16(rom, address + 10) & 0x1F;
    file.division = BigEndian16(rom, address + 12);
    if (file.division == 0 || file.division & 0x8000)
    {
        error = "the MIDI file at " + Hex(address) + " has a division the driver doesn't play";
        return false;
    }
    uint32_t at = address + 8 + header;
    file.tracks.clear();
    for (int t = 0; t < tracks && t < kMaxChannels; t++)
    {
        if (!rom.Contains(at, 8))
        {
            error = "the MIDI file at " + Hex(address) + " ends before its track " + std::to_string(t);
            return false;
        }
        const uint32_t length = BigEndian32(rom, at + 4);
        MidiTrackRange range;
        range.start = at + 8;
        range.end = at + 8 + length;
        if (!rom.Contains(range.start, length))
        {
            error = "track " + std::to_string(t) + " of the MIDI file at " + Hex(address) + " runs past the ROM's end";
            return false;
        }
        file.tracks.push_back(range);
        at = range.end;
    }
    return true;
}

} // namespace agbsf2::rd1
