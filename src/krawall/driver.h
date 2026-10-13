// SPDX-License-Identifier: MIT

// Locating Krawall, Sebastian Kienzl's XM and S3M player for the GBA, in a ROM: its code, its tables, the game's
// samples and instruments, and the modules it plays.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::krawall
{

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t module_table = 0; // a table of the game's modules, a word each, which takes the place of the scan
    int module_count = 0;
};

// A module's header.
struct ModuleInfo
{
    uint32_t address = 0;
    uint8_t channels = 0;
    uint8_t order_count = 0;
    uint8_t restart = 0; // the order that the module goes back to after its last
    std::array<uint8_t, 256> orders = {};
    std::array<int8_t, 32> channel_pan = {};
    std::array<uint8_t, 64> song_starts = {}; // each song's first order, for modules with songs separated by +++
    uint8_t global_volume = 0;
    uint8_t speed = 0;        // ticks a row
    uint8_t tempo = 0;        // BPM
    bool instruments = false; // XM instruments, rather than S3M samples
    bool linear = false;      // XM's linear frequencies, rather than Amiga periods
    bool fast_slides = false; // S3M's fast volume slides, which slide on a row's first tick too
    uint8_t volume_opt = 0;   // a flag that the player doesn't read
    uint8_t amiga_limits = 0; // a flag that the player doesn't read
    std::vector<uint32_t> patterns;
};

// The order entries that skip to the next (+++) and end the module (---).
constexpr uint8_t kOrderSkip = 254;
constexpr uint8_t kOrderEnd = 255;

// A pattern's header: an index of the row data's offsets for every 4th row, then the row count and the data.
constexpr uint32_t kPatternIndexEntries = 16;
constexpr uint32_t kPatternRows = 32;
constexpr uint32_t kPatternData = 33;

// A module's header size up to the patterns' table.
constexpr uint32_t kModulePatterns = 0x16C;

// A piece of music that the conversion converts: a module, or one of the songs that its orders separate with +++.
struct ModuleSong
{
    auto operator<=>(const ModuleSong&) const = default;

    uint32_t module = 0;
    int song = -1; // the song's number in its module, or -1 for a whole module without markers
};

// Krawall's tables, which the player reads, and the settings that the game gives the mixer.
struct DriverInfo
{
    uint32_t version = 0;     // the library's version string, or 0 if the game doesn't have it
    uint32_t play = 0;        // krapPlay(), which starts a module (Thumb)
    uint32_t samples = 0;     // the game's table of samples: a word for each sample's header
    uint32_t instruments = 0; // the game's table of instruments: a word for each instrument
    uint32_t sine = 0;        // 64 signed halfwords each: the vibrato's sine, ramp, square and random waves
    uint32_t ramp = 0;
    uint32_t square = 0;
    uint32_t random = 0;
    uint32_t periods = 0;          // 120 halfwords: the Amiga period of each note from C-0
    uint32_t fine_tunes = 0;       // 64 halfwords: the factor of each fine tune, 32768 for none
    uint32_t linear = 0;           // 768 words: the linear frequencies of the top octave's 768 steps
    int mix_rate = 16384;          // Hz
    int frame_samples = 276;       // the samples that the mixer mixes at each call, once a frame
    bool stereo = true;            // the game's choice for kragInit()
    uint8_t quality = 0;           // the game's choice for kramQualityMode(): interpolation (0-2) and ramping (16)
    uint32_t master_volume = 0x80; // the game's kramSetMasterVol(): the output's level, and above, the mix's shift
    uint32_t module_table = 0;     // the game's table of modules, a word each, which the scan found, or 0
    int table_count = 0;           // its entries, which come first in `modules`
    std::vector<uint32_t> modules;
    std::vector<ModuleSong> songs;     // the pieces that the conversion numbers, in the modules' order
    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Finds Krawall in `rom`. Returns false and sets `error` if the player's tables or the game's modules can't be found,
// or returns false with `error` empty if the game shows no sign of Krawall.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// Reads a module's header. Returns false if it isn't one that the player can play.
bool ReadModule(const Rom& rom, uint32_t address, ModuleInfo& module);

// Returns the number of songs that a module's orders separate with +++, or 0 if they have no marker. A song is a run of
// orders between markers.
int SongCount(const ModuleInfo& module);

} // namespace agbsf2::krawall
