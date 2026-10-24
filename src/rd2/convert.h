// SPDX-License-Identifier: MIT

// Sequence conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <vector>

#include "music.h"
#include "rd2/driver.h"
#include "rd2/soundfont.h"
#include "rom.h"

namespace agbsf2::rd2
{

// Returns the versions of song `song` that ConvertSong() writes: one for each entry of the sequence's
// DriverInfo::music_settings. A version after the first is named after its music number where that differs from the
// music number of the first. A version that plays the tracks that Super Mario Advance 2 mutes unless the player rides
// Yoshi is named "riding Yoshi".
std::vector<SongVersion> SongVersions(const DriverInfo& info, int song);

// Works out a sequence's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one sequence. With `shared`, the sequence's presets go in the banks that `shared` gives the sequence.
// Without `shared`, the conversion writes a SoundFont next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

} // namespace agbsf2::rd2
