// SPDX-License-Identifier: MIT

// Locating Rare's GBA sound driver and its tables in a ROM.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gba.h"
#include "rare/song.h"
#include "rom.h"

namespace agbsf2::rare
{

// The driver counts each frame as 1/60 s of the song's time, so a song plays slower on the GBA than its tempo says, by
// this factor.
constexpr double kTempoScale = 60.0 / kFrameRate;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t tune_table = 0;
    int tune_count = 0;
};

// The driver's tables and settings in a ROM.
struct DriverInfo
{
    Format format = Format::kChannelByte;
    uint32_t init = 0; // the driver's init routine (Thumb), or 0 if it wasn't found
    uint32_t tune_table = 0;
    int tune_count = 0;
    int mix_rate = 13379;                     // the mixer's output rate in Hz
    int samples_per_frame = 224;              // the mixer's output for a frame
    uint32_t rate_scale = 0x01397FC3;         // the pitch scale for the mix rate: 2^38 / mix_rate
    int voice_limit = 8;                      // maximum voices mixed per frame
    int slots_per_channel = 6;                // the notes a channel can play at once
    bool envelope_controllers = false;        // controllers 20-23 set a channel's envelope, as in BDQE's revision
    bool frees_loop_once = false;             // moving a note past its end frees it in loop mode 1, as in BDQE's
    bool skips_silent_notes = false;          // the mixer leaves out a note at a level of 0, as BDQE's does
    uint32_t pitch_table = 0;                 // 2^(n/12) for n = -64..63 in 9.23 fixed point, at n = 0
    uint32_t sine_table = 0;                  // 257 words: the vibrato's sine
    std::array<uint8_t, 100> fade_table = {}; // the length of each decay and release, from the driver's table
    std::vector<std::string> log;             // detection results for --info
    std::vector<std::string> warnings;        // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false and sets `error` if no tune could be found, or returns false with `error`
// empty if the game shows no sign of the driver and no tune table is given.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// Returns the address of tune `tune`'s header, or 0 if it's past the table.
uint32_t TuneAddress(const Rom& rom, const DriverInfo& info, int tune);

// Returns the number of frames that a decay or release with index `index` (0-99) takes, or 0 for an instant one.
int FadeFrames(const DriverInfo& info, uint32_t index);

// Returns the number of frames that a decay or release takes whose fade table entry is `entry`, or 0 for an instant
// one.
int FadeEntryFrames(uint32_t entry);

// Returns the number of frames that a note's decay or release takes, or 0 for an instant one. A channel's `setting`
// from controller 21 or 23 gives the fade table entry. A setting from 0x80 up leaves the instrument's index `index`.
int FadeFrames(const DriverInfo& info, uint32_t index, uint8_t setting);

} // namespace agbsf2::rare
