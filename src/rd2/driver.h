// SPDX-License-Identifier: MIT

// Locating Nintendo R&D2's GBA sound driver, its tables, and the settings the game gives it in a ROM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::rd2
{

// The size of the game's settings for the driver: 7 table addresses and a word the driver doesn't read.
constexpr uint32_t kSettingsSize = 32;

// The mixer's sample voices.
constexpr int kSampleVoices = 7;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t settings = 0; // the game's settings for the driver
    int sequence_count = 0;
};

// The revisions of the driver, which detection tells apart by the code that works out a sample voice's level.
enum class Revision
{
    kLinkToThePast,      // The Legend of Zelda: A Link to the Past
    kSuperMarioAdvance2, // Super Mario Advance 2, an older revision without the player's second volume, whose bends,
                         // levels and mix differ
    kSuperMarioAdvance3, // Super Mario Advance 3: A Link to the Past's, with sample voices at two thirds of the level
};

// Returns the revision's name, as --info prints it.
const char* RevisionName(Revision r);

// What a revision's code does differently. docs/rd2.md lists the differences. The defaults describe A Link to the
// Past's revision.
struct RevisionTraits
{
    bool byte_tempo = false;   // E4 takes a byte, the tempo, rather than a length
    bool player_volume = true; // EA sets the player's volume, and the player has a second volume
    bool short_notes = false;  // a note's length in units, and its frames, are kept in 16 bits
    bool keeps_note = true;    // a voice keeps its note, before the transpose
    bool scaled_bend = false;  // a bend scales the pitch by a factor in 256ths that goes in a straight line
    bool wrapped_step = false; // the mixer multiplies the frame's step by 176 and divides it by 176 again
    bool older_levels = false; // a sample voice's level takes other shifts, without the player's second volume
    uint32_t volume_scale = 3; // a sample voice's volume is its level times this, shifted right by volume_shift
    int volume_shift = 9;
};

// Returns what revision `r` does differently.
const RevisionTraits& TraitsOf(Revision r);

// The driver's settings that a game's play-music routine makes along with a sequence: the sample voices it gives the
// driver, and the music player's second volume.
struct MusicSettings
{
    int sample_voices = kSampleVoices;
    int volume2 = 0x80;
};

// The driver's tables in a ROM, and the tables the game gives it. Each table of offsets holds a word for each entry,
// counted from the table's start.
struct DriverInfo
{
    Revision revision = Revision::kLinkToThePast;
    uint32_t init = 0;             // the driver's init routine (Thumb), or 0 if it wasn't found
    uint32_t settings = 0;         // the game's settings, which it passes to init
    uint32_t sample_sets = 0;      // the offset of each sample set's table of sample offsets
    uint32_t banks = 0;            // the offset of each bank of instruments
    uint32_t sequences = 0;        // the offset of each sequence
    uint32_t effects = 0;          // the offset of each of the sound effects' sequences
    uint32_t bank_sample_sets = 0; // a halfword for each bank: the sample set that its instruments play
    uint32_t bank_lists = 0;       // the offset of each sequence's list of banks
    uint32_t pitch_table = 0;      // 120 words: a sample's pitch for each pitch index, 0x8000 at the sample's rate
    uint32_t frequency_table = 0;  // 120 halfwords: a square or wave voice's frequency setting for each pitch index
    uint32_t noise_table = 0;      // 120 bytes: a noise voice's NR43 for each pitch index
    uint32_t lfo_table = 0;        // 256 signed bytes: one cycle of the LFO's sine
    uint32_t wave_volumes = 0;     // 5 bytes: the wave voice's NR32 for each of its volumes
    uint32_t voice_classes = 0;    // a byte for each region type: the voices that can play it
    uint32_t key_envelope = 0;     // the envelope of instruments that pick a sample for each key
    std::vector<uint32_t> sequence_addresses;
    std::vector<uint32_t> bank_list_addresses;
    std::vector<MusicSettings> music_settings; // for each sequence, the settings that the game plays it with
    std::vector<std::string> log;              // detection results for --info
    std::vector<std::string> warnings;         // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false and sets `error` if the driver's tables can't be read, or returns false with
// `error` empty if the game shows no sign of the driver.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

} // namespace agbsf2::rd2
