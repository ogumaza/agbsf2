// SPDX-License-Identifier: MIT

// Song conversion for Ubisoft Milan's driver: the model's notes -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"
#include "ubimilan/driver.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{

// Conversion settings and output paths.
struct ConvertOptions
{
    int loops = 2;                // times a looping song's loop is played
    uint16_t track_mask = 0xFFFF; // sequence channels to include (bit c = channel c)
    bool frame_timing = false;    // each event on the frame the driver plays it in, rather than on the beat
    std::string out_dir = ".";
    std::string base_name = "song";
};

// The result of converting a song.
struct SongSummary
{
    bool ok = false;                       // converted, or skipped for having no notes
    bool silent = false;                   // no notes on the chosen channels: nothing was written
    double seconds = 0;                    // the converted length, at the GBA's speed
    double loop_start = -1, loop_end = -1; // seconds; -1 if the song doesn't loop
    int tracks = 0;                        // chosen channels with notes
    double bpm = 0;                        // the song's first tempo, from its beat, at the GBA's speed
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        InstrumentSet* shared);

// Writes a text listing of each sequence of a song. Returns false and sets `error` if it can't.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::ubimilan
