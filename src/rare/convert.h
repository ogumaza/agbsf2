// SPDX-License-Identifier: MIT

// Tune conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <string>

#include "music.h"
#include "rare/driver.h"
#include "rare/soundfont.h"
#include "rom.h"

namespace agbsf2::rare
{

// Works out a tune's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one tune. With `shared`, the tune's presets go in bank `bank` of that SoundFont. Without `shared`, the
// conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared, int bank = 0);

// Writes a text listing of every command of every track of a tune. Returns false and sets `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::rare
