// SPDX-License-Identifier: MIT
// Reading the Standard MIDI Files that Nintendo R&D1's driver plays.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::rd1
{

// A track of a MIDI file: where its events start and where its chunk ends.
struct MidiTrackRange
{
    uint32_t start = 0; // the first event's delta time
    uint32_t end = 0;   // the byte after the chunk
};

// A Standard MIDI File as the driver's start routine reads it.
struct SongFile
{
    int format = 0;
    int division = 24; // ticks a quarter note
    std::vector<MidiTrackRange> tracks;
};

// Reads the header and track chunks of the MIDI file at `address`. The driver plays the first 31 tracks at most.
// Returns false and sets `error` if the file isn't a MIDI file that the driver can play or runs past the end of the
// ROM.
bool ReadSongFile(const Rom& rom, uint32_t address, SongFile& file, std::string& error);

// Reads a variable-length number at `at` and moves `at` past it. Like the driver, it reads until a byte below 0x80,
// however many bytes that takes.
uint32_t ReadVariable(const Rom& rom, uint32_t& at);

} // namespace agbsf2::rd1
