// SPDX-License-Identifier: MIT

// Locating Quintet's GBA sound driver and its tables in a ROM.

#pragma once

#include <array>
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
    kJ, // Super Robot Taisen J's, with banks of samples and noise macros that F0 switches between, five wave volumes,
        // and timer settings that wait for the end of the frame
};

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
    uint32_t songs = 0;                   // the music file: the songs, one after another
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
