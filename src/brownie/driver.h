// SPDX-License-Identifier: MIT

// Locating Brownie Brown's GBA sound driver and its tables in a ROM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::brownie
{

// The CPU cycles in a frame, and a second's.
constexpr uint64_t kFrameCycles = 280896;
constexpr uint64_t kSecondCycles = 16777216;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t song_table = 0;
    int song_count = 0;
};

// The revisions of the driver, which detection tells apart by the number of channels the per-frame routine runs.
enum class Revision
{
    kSwordOfMana,     // Sword of Mana: 13 channels, whose samples a mixer plays on 5 voices
    kMagicalVacation, // Magical Vacation: 12 channels, whose samples play on the FIFOs, a sample on each
};

// Returns the revision's name, as --info prints it.
const char* RevisionName(Revision r);

// Returns the number of channels that a revision's per-frame routine runs: 13, or 12 in the Magical Vacation revision.
int ChannelCount(Revision r);

// The driver's tables in a ROM.
struct DriverInfo
{
    Revision revision = Revision::kSwordOfMana;
    uint32_t frame_routine = 0;        // the per-frame routine (ARM), or 0 if it wasn't found
    uint32_t command_table = 0;        // 32 words: the routines of commands E0 to FF
    uint32_t song_table = 0;           // a word a sound: its list of channels
    int song_count = 0;                // entries in the song table
    uint32_t length_table = 0;         // sets of 16 bytes: the frames that each length command gives a note
    uint32_t frequency_table = 0;      // a halfword a key: the PSG's frequency setting
    uint32_t pan_table = 0;            // 24 bytes: each channel's pan settings when a song starts it
    uint32_t envelope_table = 0;       // a word an envelope: its steps
    uint32_t sample_table = 0;         // a word a sample set: its 12 samples, one for each semitone, or in the Magical
                                       // Vacation revision, a word a sample
    uint32_t rate_table = 0;           // the Magical Vacation revision: a halfword for each 12th of a semitone, the
                                       // reload value of the timer that plays a FIFO's sample
    uint32_t wave_table = 0;           // a word a wave: its 16 bytes for the wave RAM
    uint32_t control_registers = 0;    // a word a PSG channel: the address of its NRx1 and NRx2
    uint32_t frequency_registers = 0;  // a word a PSG channel: the address of its NRx3 and NRx4
    uint32_t point_cycles = 1024;      // the CPU cycles each point of the FIFOs plays for, from Timer 0's setting
    int mix_rate = 16384;              // Hz: the rate the FIFOs play at, and the SoundFont's samples' rate
    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Finds the driver in `rom`. Returns false and sets `error` if no sound could be found, or returns false with `error`
// empty if the game shows no sign of the driver and no song table is given.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// A channel of a sound, as its entry in the song table lists it.
struct SongChannel
{
    int channel = 0;          // 0-3 the music's PSG, 4-7 the sound effects' PSG, 8 and up the samples
    uint32_t data = 0;        // the channel's sequence
    uint32_t subroutines = 0; // a word a subroutine: the places that F6 calls and F8 jumps to
};

// Reads the channels of entry `song` of the song table. Returns false if the entry isn't in the ROM, names a channel
// that the revision doesn't have, or doesn't end within its channels.
bool ReadSong(const Rom& rom, const DriverInfo& info, int song, std::vector<SongChannel>& channels);

} // namespace agbsf2::brownie
