// SPDX-License-Identifier: MIT

// Song conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include "brownie/driver.h"
#include "brownie/soundfont.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::brownie
{

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

} // namespace agbsf2::brownie
