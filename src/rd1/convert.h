// SPDX-License-Identifier: MIT
// Song conversion for Nintendo R&D1's MIDI-file sound driver.

#pragma once

#include <string>

#include "music.h"
#include "rd1/driver.h"
#include "rd1/soundfont.h"
#include "rom.h"

namespace agbsf2::rd1
{

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts a song to a MIDI file and a SoundFont. With `shared`, its instruments go there instead of in a SoundFont of
// the song.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared);

// Writes a text listing of every event of a song's MIDI file. Returns false and sets `error` if it can't.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::rd1
