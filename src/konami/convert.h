// SPDX-License-Identifier: MIT

// Song conversion: sequencer output -> MIDI file + SoundFont instruments.

#pragma once

#include <string>

#include "konami/driver.h"
#include "konami/soundfont.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::konami
{

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings);

// Converts one song. If `shared` is supplied, adds instruments to it with bank = song; otherwise writes a separate SF2
// next to the MIDI file.
SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared);

// Writes a text listing of every command of every track of a song. Returns false and sets `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::konami
