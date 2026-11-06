// SPDX-License-Identifier: MIT

// Song conversion for AlphaDream's driver.

#pragma once

#include <string>

#include "alphadream/driver.h"
#include "alphadream/soundfont.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::alphadream
{

// Works out a song's length, loop and tracks as ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts a song to a MIDI file and a SoundFont. With `shared`, its instruments go there instead.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// Writes a listing of each track's commands. Returns false and sets `error` if it can't.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::alphadream
