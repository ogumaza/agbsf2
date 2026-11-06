// SPDX-License-Identifier: MIT

// Locating Ubisoft Milan's GBA sound driver, its sound bank and its tables in a ROM.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::ubimilan
{

// The sequence channel whose notes play the kit's samples, or the PSG's noise at keys up to kLastNoiseKey.
constexpr int kKitChannel = 9;
constexpr int kLastNoiseKey = 11;

// Returns true if a note of sequence channel `channel` at key `key` plays the PSG's noise.
constexpr bool PlaysNoise(int channel, int key)
{
    return channel == kKitChannel && key <= kLastNoiseKey;
}

// Returns true if a note of sequence channel `channel` at key `key` plays one of the kit's samples.
constexpr bool PlaysKit(int channel, int key)
{
    return channel == kKitChannel && key > kLastNoiseKey;
}

// Sequence channels 0-2 play on the PSG's square 1, square 2 and wave channels.
constexpr int kPsgChannels = 3;
constexpr int kWaveChannel = 2;

// The mixer's voices, which play the kit's samples.
constexpr int kVoiceCount = 4;

// The points each voice moves on at each of the mixer's runs, and the points of output each run makes, twice as many.
constexpr uint32_t kRunPoints = 8;
constexpr uint32_t kRunOutput = 16;

// The kinds of the sound bank's resources, from their first byte.
enum ResourceType : uint8_t
{
    kMusic = 0,    // a piece of music: the sequences it plays one after another, and whether it loops
    kSample = 3,   // 8-bit samples
    kSequence = 6, // a sequence of events
    kKit = 7,      // a halfword for each of channel 9's keys: the sample it plays, or 0x8000 for none
};

// The volume of a voice that plays a sample at the whole scale. The sound events give this volume to the samples that
// play as music.
constexpr int kFullVolume = 64;

// The MIDI key of the first sample that plays as music. The kit's keys end below it. Each sample has a key, and a
// shared SoundFont can then hold them all. A sample whose loop doesn't start with its first point has a second key this
// far above the first. The second key plays from the loop's start.
constexpr int kFirstSampleKey = 81;
constexpr int kLoopKeyOffset = 20;

// The resource number that an entry of a kit or of the kit table holds for none.
constexpr uint16_t kNoResource = 0x8000;

// The bytes before a sequence's first countdown: its kind and three that the driver doesn't read.
constexpr uint32_t kSequenceHeader = 4;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t song_table = 0; // the sound bank
    int song_count = 0;
};

// The driver's tables in a ROM.
struct DriverInfo
{
    uint32_t step_routine = 0;   // the sequencer's per-frame step (Thumb), or 0 if it wasn't found
    uint32_t bank = 0;           // the sound bank
    int resource_count = 0;      // entries in the resource table
    uint32_t resource_table = 0; // a word a resource: its offset from the bank
    uint32_t kit_table = 0;      // a halfword for each of channel 9's kits: its resource, or 0x8000 for none
    uint32_t kit = 0;            // the halfwords of the kit that channel 9 plays, which the driver's init chooses
    std::vector<int> songs;      // the pieces of music and the samples that play as music, by their resource numbers
    int piece_count = 0;         // the songs that are pieces of music. The samples follow them
    std::array<uint32_t, kPsgChannels> instrument_tables = {}; // 16 bytes for each of a PSG channel's instruments
    uint32_t noise_table = 0;          // 4 bytes for each of channel 9's noise keys: what they write to NR41-NR44
    uint32_t frequency_table = 0;      // a halfword a key: the PSG's frequency setting
    uint32_t fine_table = 0;           // a halfword a cent, 100 to a key: the PSG's frequency setting
    uint32_t wave_table = 0;           // 32 bytes for each of the wave channel's programs: both banks of the wave RAM
    uint32_t timer_reload = 0;         // Timer 0's reload, which sets the mixer's rate
    uint32_t older_bank = 0;           // the sound bank of a game with the older build (not converted)
    std::vector<int> kit_off_songs;    // the songs that the game starts with channel 9's kit switched off
    std::vector<std::string> log;      // detection results for --info
    std::vector<std::string> warnings; // assumptions detection had to make
};

// Returns the CPU cycles that each point of the mixer's output lasts: Timer 0's period, from its reload.
uint32_t OutputCycles(const DriverInfo& info);

// Returns the rate of the mixer's output, which is twice the rate at which each voice moves through its sample.
double OutputRate(const DriverInfo& info);

// Returns the rate at which each voice moves through its sample: half the output's.
double PointRate(const DriverInfo& info);

// Finds the driver in `rom`. Returns false and sets `error` if its sound bank can't be read, or if the game has an
// older build of the driver. Returns false with `error` empty if the game shows no sign of the driver and no sound bank
// is given.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// Returns true if channel 9's kit plays in version `version` of song `song`. The game starts the songs in
// DriverInfo::kit_off_songs with the kit switched off. The first version of such a song leaves the kit out. The second
// plays it, as the game does once it switches the kit on.
bool KitPlays(const DriverInfo& info, int song, int version);

// Returns the address of resource `id`, or 0 if the bank has no such resource or it isn't in the ROM.
uint32_t ResourceAddress(const Rom& rom, const DriverInfo& info, int id);

// A piece of music: the sequences it plays one after another.
struct MusicPiece
{
    // Returns true if the list starts again after its last sequence: flag 1.
    bool Loops() const
    {
        return flags & 1;
    }

    // Returns true if channel 9 keeps a voice for the whole piece: flag 8.
    bool KeepsKitVoice() const
    {
        return flags & 8;
    }

    // Returns how many parts a pass of the piece has: its sequences, or for a piece that is a sample, that sample.
    size_t Parts() const
    {
        return sample >= 0 ? 1 : sequences.size();
    }

    uint32_t address = 0;
    int flags = 0;              // the bits that Loops() and KeepsKitVoice() read
    std::vector<int> sequences; // the sequences' resource numbers
    int sample = -1;            // the resource of a piece that is a sample, or -1. Its flags hold 1 if the sample loops
};

// Reads song `song`, a piece of music or a sample that the game plays as music. Returns false if it isn't in the ROM or
// lists no sequences.
bool ReadMusic(const Rom& rom, const DriverInfo& info, int song, MusicPiece& piece);

// A sample, as a resource of the sound bank describes it.
struct GameSample
{
    uint32_t data = 0;   // the first point
    uint32_t length = 0; // points
    bool loop = false;
    uint32_t loop_start = 0; // points from the first, if it loops
    uint32_t loop_end = 0;
};

// Reads the sample resource at `address`. Returns false if it isn't a sample in the ROM.
bool ReadSample(const Rom& rom, uint32_t address, GameSample& sample);

} // namespace agbsf2::ubimilan
