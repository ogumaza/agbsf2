// SPDX-License-Identifier: MIT

// Song conversion: sequencer output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "konami/driver.h"
#include "konami/soundfont.h"
#include "rom.h"

namespace agbsf2::konami
{

// Conversion settings and output paths.
struct ConvertOptions
{
    int loops = 2;                // times the looped section is played
    uint16_t track_mask = 0xFFFF; // game tracks to include (bit t = track t)
    bool frame_timing = false;    // each event on the frame the driver plays it in, rather than on the beat
    std::string out_dir = ".";
    std::string base_name = "song";
};

// The result of converting a song.
struct SongSummary
{
    bool ok = false;           // converted, or skipped for having no notes
    bool silent = false;       // no notes on the chosen tracks: nothing was written
    uint32_t frames = 0;       // converted length
    int loop_start_frame = -1; // -1: song doesn't loop
    int loop_end_frame = -1;
    int tracks = 0; // chosen tracks with notes
    double bpm = 0;
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt);

// Converts one song. If `shared` is supplied, adds instruments to it with bank = song; otherwise writes a separate SF2
// next to the MIDI file.
SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        SoundfontBuilder* shared);

// Writes a text listing of every command of every track of a song. Returns false and sets `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::konami
