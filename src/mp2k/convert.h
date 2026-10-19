// SPDX-License-Identifier: MIT

// Song conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include "mp2k/driver.h"
#include "mp2k/soundfont.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::mp2k
{

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, adds its presets to that SoundFont (see
// SoundfontBuilder::SharedPreset()); otherwise writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

} // namespace agbsf2::mp2k
