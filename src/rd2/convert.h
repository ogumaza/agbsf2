// SPDX-License-Identifier: MIT

// Sequence conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rd2/driver.h"
#include "rd2/soundfont.h"
#include "rom.h"

namespace agbsf2::rd2
{

// Conversion settings and output paths.
struct ConvertOptions
{
    int loops = 2;                // times a looping sequence's loop is played
    uint16_t track_mask = 0xFFFF; // tracks to include (bit t = the player's track t)
    bool frame_timing = false;    // each event on the frame the driver plays it in, rather than on the beat
    int bank = 0;                 // the sequence whose bank holds the presets, as SongBanks gives them out
    std::string out_dir = ".";
    std::string base_name = "song";
};

// The result of converting a sequence.
struct SongSummary
{
    bool ok = false;                       // converted, or skipped for having no notes
    bool silent = false;                   // no notes on the chosen tracks: nothing was written
    double seconds = 0;                    // the converted length, at the GBA's speed
    double loop_start = -1, loop_end = -1; // seconds; -1 if the sequence doesn't loop
    int tracks = 0;                        // chosen tracks with notes
    double bpm = 0;                        // the first tempo, at the GBA's speed
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Works out a sequence's length, loop and tracks the way ConvertSong() does, without writing anything.
SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt);

// Converts one sequence. If `shared` is supplied, adds its presets to that SoundFont, in the banks that `opt.bank`
// picks; otherwise writes a SoundFont next to the MIDI file.
SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        SoundfontBuilder* shared);

} // namespace agbsf2::rd2
