// SPDX-License-Identifier: MIT

// Module conversion: the model's output -> MIDI file + SoundFont instruments.

#pragma once

#include "krawall/driver.h"
#include "krawall/soundfont.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::krawall
{

// Works out a module's length, loop and channels the way ConvertSong() does, without writing anything.
// ConvertSettings::frame_timing has no effect on a module's conversion. The player's ticks fall on samples rather than
// frames.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one module. If `shared` is supplied, the module's instruments are added to it for a shared SoundFont;
// otherwise a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

} // namespace agbsf2::krawall
