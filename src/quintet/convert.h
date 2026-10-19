// SPDX-License-Identifier: MIT

// Song conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include "music.h"
#include "quintet/driver.h"
#include "quintet/soundfont.h"
#include "rom.h"

namespace agbsf2::quintet
{

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. With `shared`, the song's presets go in the banks that `shared` gives the song. Without `shared`,
// the conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

} // namespace agbsf2::quintet
