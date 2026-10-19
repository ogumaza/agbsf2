// SPDX-License-Identifier: MIT

// Song conversion for Ubisoft Milan's driver: the model's notes -> MIDI file + SoundFont instruments.

#pragma once

#include "music.h"
#include "rom.h"
#include "ubimilan/driver.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

} // namespace agbsf2::ubimilan
