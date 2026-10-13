// SPDX-License-Identifier: MIT

// Module conversion: the model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "krawall/driver.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::krawall
{

// Conversion settings and output paths.
struct ConvertOptions
{
    int loops = 2;                    // times a looping module's loop is played
    uint32_t track_mask = 0xFFFFFFFF; // channels to include (bit c = channel c)
    std::string out_dir = ".";
    std::string base_name = "song";
};

// The result of converting a module.
struct SongSummary
{
    bool ok = false;                       // converted, or skipped for having no notes
    bool silent = false;                   // no notes on the chosen channels: nothing was written
    double seconds = 0;                    // the converted length, at the GBA's speed
    double loop_start = -1, loop_end = -1; // seconds; -1 if the module doesn't loop
    int tracks = 0;                        // chosen channels with notes
    double bpm = 0;                        // the module's first tempo, in quarter notes a minute
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Each instrument is a game sample played from a given offset. Instruments become SoundFont presets, numbered in order
// of first use. A shared SoundFont can use one set for all the game's modules.
class InstrumentSet
{
public:
    // Returns the program of sample `sample`, number `index` in the game's table, played from point `offset`, adding it
    // if it's new. Programs from 128 on go in the banks after the first.
    int Program(uint32_t sample, int index, uint32_t offset);

    // Returns the SoundFont of the instruments.
    Sf2File Build(const Rom& rom, const DriverInfo& info) const;

private:
    struct Instrument
    {
        int index = 0;
        int program = 0;
    };

    std::map<std::pair<uint32_t, uint32_t>, Instrument> programs_;
};

// Returns the rate in Hz at which the player plays a sample's C-4, which is the rate that its SoundFont sample gets, on
// key 60.
uint32_t SampleRate(const Rom& rom, const DriverInfo& info, uint32_t sample);

// Works out a module's length, loop and channels the way ConvertSong() does, without writing anything.
SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt);

// Converts one module. If `shared` is supplied, the module's instruments are added to it for a shared SoundFont;
// otherwise a SoundFont is written next to the MIDI file.
SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        InstrumentSet* shared);

} // namespace agbsf2::krawall
