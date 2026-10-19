// SPDX-License-Identifier: MIT

// Sequence conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include "music.h"
#include "rd2/driver.h"
#include "rd2/soundfont.h"
#include "rom.h"

namespace agbsf2::rd2
{

// Works out a sequence's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one sequence. With `shared`, the sequence's presets go in the banks that `shared` gives the sequence.
// Without `shared`, the conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

} // namespace agbsf2::rd2
