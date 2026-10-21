// SPDX-License-Identifier: MIT

// Locating Quintet's GBA sound driver and its tables in a ROM, and how the driver's revisions differ.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::quintet
{

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t song_table = 0; // the first song of the music file
    int song_count = 0;
};

// The revisions of the driver that agbsf2 reads.
enum class Revision : uint8_t
{
    kA, // Super Robot Taisen A's, which D and R have too
    kJ, // Super Robot Taisen J's: kRevisionTraits gives how it differs
};

// How each revision of the driver behaves where the two differ. The fields follow the rows of the documentation's table
// of the J revision's differences. The wave channel's volume codes go in bits 13 to 15 of SOUND3CNT_H: 0 for silence, 3
// for 25%, 2 for 50%, 1 for 100%, and 4 for 75%.
struct RevisionTraits
{
    bool banks = false;                     // F0 switches between bank 0 and bank 1 of the samples and noise macros
    uint16_t start_tempo = 0;               // each channel's tempo before the song sets one
    uint8_t start_envelope_up = 0;          // 1 if each channel's hardware envelope starts out going up
    bool reseeds = false;                   // the play routine and each loop set the random seed back to 1
    int wave_levels = 0;                    // how many volume levels the wave channel has, silence included
    std::array<uint8_t, 5> wave_codes = {}; // the volume code of each level
    uint16_t envelope_wave_mask = 0;        // the bits of SOUND3CNT_H that the volume envelope writes
    uint8_t envelope_wave_above = 0;        // the code that the volume envelope gives a level above the others
    bool envelope_holds = false;            // after the decay, the volume envelope goes on applying the end level
    bool idle_after_rest = false;           // after a rest, the volume and pitch envelopes do nothing
    int pcm_offset_factor = 0;              // what each unit of a PCM pitch effect's offset counts for
    int dma_clear_size = 0;                 // the bytes that a FIFO start writes to clear the DMA's count
    bool timers_at_frame_end = false;       // timer settings wait for the end of the frame
    bool clears_missing_macro = false;      // a noise macro without a table clears the channel's macro
};

inline constexpr RevisionTraits kRevisionTraits[2] = {
    // The A revision.
    {
        .banks = false,
        .start_tempo = 90,
        .start_envelope_up = 1,
        .reseeds = false,
        .wave_levels = 4,
        .wave_codes = {0, 3, 2, 1},
        .envelope_wave_mask = 0x6000,
        .envelope_wave_above = 1,
        .envelope_holds = false,
        .idle_after_rest = false,
        .pcm_offset_factor = 2,
        .dma_clear_size = 2,
        .timers_at_frame_end = false,
        .clears_missing_macro = false,
    },
    // The J revision.
    {
        .banks = true,
        .start_tempo = 120,
        .start_envelope_up = 0,
        .reseeds = true,
        .wave_levels = 5,
        .wave_codes = {0, 3, 2, 4, 1},
        .envelope_wave_mask = 0xE000,
        .envelope_wave_above = 0,
        .envelope_holds = true,
        .idle_after_rest = true,
        .pcm_offset_factor = 1,
        .dma_clear_size = 4,
        .timers_at_frame_end = true,
        .clears_missing_macro = true,
    },
};

// Returns the traits of `revision`.
inline const RevisionTraits& TraitsOf(Revision revision)
{
    return kRevisionTraits[size_t(revision)];
}

// The entries of the driver's frequency table and of its PCM pitch table. The frequency table has one for each note
// from C2.
constexpr int kFrequencyEntries = 84;
constexpr int kPcmPitchEntries = 53;

// The driver's tables in a ROM, and the tables the game gives it. The game gives the driver two banks of noise macros
// and PCM samples: bank 0 for music and bank 1 for sound effects, which the J revision's music can switch to.
struct DriverInfo
{
    Revision revision = Revision::kA;
    uint32_t play = 0;                    // the driver's play-song routine (Thumb), or 0 if it wasn't found
    uint32_t length_table = 0;            // 13 bytes: the note values that a note's length index picks
    uint32_t frequency_table = 0;         // 84 halfwords: the square and wave channels' frequency setting for each note
    uint32_t pcm_pitch_table = 0;         // 53 halfwords: each note's PCM rate change in 1/1000s of the sample's rate
    uint32_t lfo_table = 0;               // 256 signed bytes: one cycle of the LFO's sine
    uint32_t music_file = 0;              // the music file: the songs back to back
    uint32_t waves = 0;                   // the game's wave table, or 0
    std::array<uint32_t, 2> macros = {};  // each bank's noise macro table, or 0
    std::array<uint32_t, 2> samples = {}; // each bank's PCM sample file, or 0
    std::vector<uint32_t> song_addresses;

    // Each sample's header, as the driver's sample tables hold them.
    std::array<std::vector<uint32_t>, 2> sample_addresses;

    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false and sets `error` if the driver's tables can't be read, or returns false with
// `error` empty if the game shows no sign of the driver.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

} // namespace agbsf2::quintet
