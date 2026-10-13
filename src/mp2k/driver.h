// SPDX-License-Identifier: MIT

// Locating MP2K, Nintendo's MusicPlayer2000 sound driver, and its tables in a ROM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::mp2k
{

// The frame rate of the GBA's display, which the driver's per-frame routine follows: 280896 CPU cycles a frame.
constexpr double kFrameRate = 16777216.0 / 280896.0;

// The DirectSound channels the driver has room for.
constexpr int kMaxDirectChannels = 12;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t song_table = 0;
    int song_count = 0;
};

// A music player: the driver plays each song on one of these, which the song table names.
struct Player
{
    int track_count = 0; // the most tracks it plays; a song's other tracks stay silent
};

// The driver's tables and settings in a ROM.
struct DriverInfo
{
    uint32_t song_start = 0; // the routine that starts a song (Thumb), or 0 if it wasn't found
    uint32_t song_table = 0; // 8 bytes a song: its header, and the number of its music player
    int song_count = 0;
    uint32_t player_table = 0;
    std::vector<Player> players;
    uint32_t sound_mode = 0;           // the setting the init gives the driver, or 0 if it wasn't found
    int max_channels = 8;              // DirectSound channels
    int master_volume = 15;            // 0-15
    int reverb = 0;                    // 0-127
    int rate_index = 4;                // 1-12, the setting of the mixer's rate
    int samples_per_frame = 224;       // the mixer's output for a frame
    int mix_rate = 13379;              // Hz, as the driver works it out
    uint32_t step_scale = 627;         // the mixer's step for each Hz of a sample's rate, in 2^-23 points
    uint8_t dac_bits = 0x40;           // the output's resolution: bits 6 and 7 of the sound bias register's high byte
    bool special_samples = false;      // the mixer can play samples backwards, and compressed samples
    bool mono = false;                 // the mixer has one output, and plays a sample at the average of its volumes
    bool camelot_sequencer = false;    // Camelot's changes to how a note takes a channel and sets its volume and rate
    bool camelot_mixer = false;        // Camelot's mixer, with straight-line releases and synth voices
    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false and sets `error` if no song could be found, or returns false with `error`
// empty if the game shows no sign of the driver and no song table is given.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// Returns the address of song `song`'s header, or 0 if it's past the table.
uint32_t SongAddress(const Rom& rom, const DriverInfo& info, int song);

// Returns the music player that plays song `song`. A song whose player number is past the table gets a player of 16
// tracks.
Player SongPlayer(const Rom& rom, const DriverInfo& info, int song);

} // namespace agbsf2::mp2k
