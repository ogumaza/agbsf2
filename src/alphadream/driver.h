// SPDX-License-Identifier: MIT

// Detection of AlphaDream's sound driver, which Mario & Luigi: Superstar Saga plays its music with, and the readers of
// its songs, instruments and samples.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::alphadream
{

// The tracks of a player. Tracks 0-7 play samples, and tracks 8-11 the PSG's channels in order.
constexpr int kTracks = 12;
constexpr int kSampleTracks = 8;

// The tempo's divisor that the driver gives a player when it starts a song.
constexpr int kDefaultDivisor = 0x4B;

// The key that a sample plays at its rate, and that a fixed-pitch sample always plays at.
constexpr int kRootKey = 60;

// The versions of the driver, by the games that hold them.
enum class Version
{
    kSuperstarSaga, // A88E
    kHamSports,     // B85A: each note has a third byte, the track's volume, and FA sets the channel's priority
    kHamtaro4,      // A84J, an older version: notes with a volume, and the levels worked out again at every note
};

// What detection found: the driver's tables.
struct DriverInfo
{
    Version version = Version::kSuperstarSaga;
    uint32_t start_routine = 0;    // starts song r1 on player r0
    uint32_t song_table = 0;       // a pointer a song
    int song_count = 0;            // the entries before the table's first 0 or the table of sound effects
    uint32_t effect_table = 0;     // the table of sound effects, which follows the song table, or 0 if not found
    uint32_t instrument_table = 0; // a halfword a instrument: the offset of its regions from the table
    int instrument_count = 0;
    uint32_t sample_table = 0; // a word a sample: the offset of its header from the table
    int sample_count = 0;
    uint32_t step_table = 0;      // 13 words: 2^23 times 2^(n/12) for n from 0 to 12
    uint32_t key_table = 0;       // a byte a key: the octave's shift in the top 4 bits, the note in the bottom 4
    uint32_t psg_pitch_table = 0; // a halfword a key from 36: 131072 over the key's frequency in hertz
    uint32_t noise_table = 0;     // the noise's NR43 setting for keys 21 to 80
    uint32_t wave_table = 0;      // 16 bytes a wave pattern
    uint32_t wave_levels = 0;     // the wave's NR32 setting for each quarter of its volume: 25, 50, 75 and 100%
    std::vector<std::string> log;
    std::vector<std::string> warnings;
};

// Finds the driver in `rom` by its code and reads its tables. Returns false and leaves `error` empty if the game shows
// no sign of the driver. Returns false and sets `error` if a table can't be read.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// An instrument's entry for a range of keys.
struct Region
{
    uint32_t address = 0;
    int lo = 0;
    int hi = 0;
    uint16_t sample = 0; // a sample's number, with bit 15 for a fixed pitch, or the PSG's duty or wave pattern
    std::array<uint8_t, 4> envelope{};
};

// Returns the regions of instrument `instrument`, in the driver's order.
std::vector<Region> Regions(const Rom& rom, const DriverInfo& info, int instrument);

// Finds the first region of instrument `instrument` whose range holds `key`, as the routine at 0x0819A8EC does.
// Returns false if there's none.
bool FindRegion(const Rom& rom, const DriverInfo& info, int instrument, int key, Region& out);

// A sample's header: 4 bytes whose last one holds the loop flag (0x40), the rate in 1024ths of a hertz at key 60, the
// loop's start and the end in points, then the points, unsigned 8-bit.
struct SampleHeader
{
    uint32_t address = 0;
    bool loops = false;
    uint32_t rate = 0;
    uint32_t loop_start = 0;
    uint32_t end = 0;
    uint32_t data = 0;
    bool empty = false; // the table's entry is 0, so the header is the table itself
};

// Reads sample `number`, without its bit 15. Returns false if it's outside the table or its points aren't in the ROM.
// An entry of 0 reads the table as a header, as the driver does, and sets `empty`.
bool ReadSample(const Rom& rom, const DriverInfo& info, int number, SampleHeader& out);

// A song: the mask of the tracks that it plays, and the data of each of them.
struct Song
{
    uint32_t address = 0;
    uint16_t mask = 0;
    std::array<uint32_t, kTracks> tracks{};
};

// Reads song `number`. Returns false if it's outside the table, or plays no track.
bool ReadSong(const Rom& rom, const DriverInfo& info, int number, Song& out);

} // namespace agbsf2::alphadream
