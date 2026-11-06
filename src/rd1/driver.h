// SPDX-License-Identifier: MIT
// Locating Nintendo R&D1's MIDI-file sound driver and its tables in a ROM.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::rd1
{

// The most players, channels and voices that the driver has. A player plays one song at a time on its channels.
constexpr int kMaxPlayers = 16;
constexpr int kMaxChannels = 31;
constexpr int kMaxVoices = 16;
constexpr int kPsgVoices = 4;

// What the init sets a player up with: its channels and whether it plays sound effects.
struct PlayerSetup
{
    int channels = 0;     // the channels, and the most tracks that a song on the player can have
    bool effects = false; // set for the players that the game's sound effects use
};

// The driver's tables and settings in a ROM.
struct DriverInfo
{
    uint32_t play_routine = 0;  // starts entry r0 of the song table
    uint32_t start_routine = 0; // starts a song's record on a player
    uint32_t frame_routine = 0; // the per-frame routine
    uint32_t init_routine = 0;  // the driver's init
    uint32_t echo_routine = 0;  // sets the echo's four settings
    uint32_t lfo_routine = 0;   // runs a channel's LFO and works out its level
    bool group_levels = false;  // the revision with 8-bit levels and a level for a group of tracks (Rhythm Tengoku)
    uint32_t sine_table = 0;    // the LFO's sine: 256 s16 values
    uint32_t pitch_table = 0;   // each key's pitch in Hz: 128 u16 values
    uint32_t noise_table = 0;   // the noise's NR43 setting for each key from 21 to 80
    uint32_t wave_levels = 0;   // the wave channel's NR32 settings for its four levels
    uint32_t song_table = 0;    // 8 bytes an entry: the song's record and the player that plays it
    int song_count = 0;         // entries in the song table, empty ones included
    uint32_t player_table = 0;  // 12 bytes a player: its record in RAM
    uint32_t player_setup = 0;  // 20 bytes a player: what the init sets it up from
    uint32_t bank_table = 0;    // a word a bank: its 128 instruments
    int bank_count = 0;         // banks in the bank table
    int mix_rate = 13379;       // Hz: the rate that the mixer runs at
    int voices = 8;             // the mixer's sample voices
    std::vector<PlayerSetup> players;
    std::array<int, 4> echo = {};      // the echo's settings that the game's init passes to the echo routine
    uint32_t echo_call = 0;            // the game's call that passes them
    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false with `error` empty if the game shows no sign of the driver. Returns false
// and sets `error` if one of the driver's tables wasn't found or the song table has no songs.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// A song's record: the MIDI file and the settings that the start routine takes from it.
struct SongRecord
{
    uint32_t address = 0; // the record
    uint32_t midi = 0;    // the Standard MIDI File
    int player = 0;       // the player that plays it
    int bank = 0;         // the instrument bank, an index into the bank table
    int volume = 0;       // 0-127: the song's level
    int priority = 0;     // a song on a player replaces one of the same or a lower priority
    std::string name;     // the name that the game gives the song, such as "m_BGM_01"
};

// Reads entry `song` of the song table. Returns false if the entry is empty or isn't a record of a MIDI file.
bool ReadSongRecord(const Rom& rom, const DriverInfo& info, int song, SongRecord& record);

} // namespace agbsf2::rd1
