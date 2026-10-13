// SPDX-License-Identifier: MIT

// Tune conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rare/driver.h"
#include "rare/soundfont.h"
#include "rom.h"

namespace agbsf2::rare
{

// Conversion settings and output paths.
struct ConvertOptions
{
    int loops = 2;                // times a looping tune's loop is played
    uint16_t track_mask = 0xFFFF; // tracks to include (bit t = track t)
    bool frame_timing = false;    // each event on the frame the driver plays it in, rather than on the beat
    int bank = 0;                 // the bank of the tune's presets
    std::string out_dir = ".";
    std::string base_name = "song";
};

// The result of converting a tune.
struct SongSummary
{
    bool ok = false;                       // converted, or skipped for having no notes
    bool silent = false;                   // no notes on the chosen tracks: nothing was written
    double seconds = 0;                    // the converted length, at the GBA's speed
    double loop_start = -1, loop_end = -1; // seconds; -1 if the tune doesn't loop
    int tracks = 0;                        // chosen tracks with notes
    double bpm = 0;                        // the first tempo, at the GBA's speed
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Works out a tune's length, loop and tracks the way ConvertSong() does, without writing anything.
SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt);

// Converts one tune. If `shared` is supplied, adds its presets to that SoundFont in bank `opt.bank`; otherwise writes a
// SoundFont next to the MIDI file.
SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        SoundfontBuilder* shared);

// Writes a text listing of every command of every track of a tune. Returns false and sets `error` on failure.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::rare
