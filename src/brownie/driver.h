// SPDX-License-Identifier: MIT

// Locating Brownie Brown's GBA sound driver and its tables in a ROM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::brownie
{

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

// The driver's sound channels: 0-3 play the music on the PSG's square 1, square 2, wave and noise channels, 4-7 play
// sound effects on the same PSG channels, and the rest play samples. In the Sword of Mana revision, 8-12 play the
// music's samples on the mixer's voices 0-4. In the Magical Vacation revision, 8 and 9 play the music's samples on FIFO
// A and FIFO B, and 10 and 11 play sound effects' samples, taking the FIFO from 8 or 9. kChannelCount is the most that
// a revision has.
constexpr int kChannelCount = 13;
constexpr int kFirstEffectChannel = 4;
constexpr int kFirstSampleChannel = 8;
constexpr int kFirstEffectSampleChannel = 10;
constexpr int kWaveChannel = 2;
constexpr int kNoiseChannel = 3;

// The Sword of Mana revision keeps a 14th record after the 13 channels. The per-frame routine doesn't run it. The
// driver's routine that starts a sound starts it for a sound that lists channel 13, and E2, E3 and the end of a fade
// out change its flags. ReadSong() rejects such a sound.
constexpr int kRecordCount = 14;

// The mixer's voices in the Sword of Mana revision: one for each sample channel. The mixer's mode 0 mixes voices 0 and
// 1 only.
constexpr int kVoiceCount = 5;
constexpr int kMode0Voices = 2;

// The FIFOs. In the Magical Vacation revision, each plays a sample: FIFO A at the rate of Timer 0, and FIFO B at the
// rate of Timer 1.
constexpr int kFifoCount = 2;

// The numbers and names that differ between the revisions, apart from the size of the driver's variables and the
// offsets of SOUNDCNT_L's copy and of the master level among them. sequencer.h keeps those beside the variables. Code
// whose behaviour differs between the revisions tests the revision.
struct RevisionLayout
{
    const char* name;   // the revision's name for --info
    int channels;       // the channels that the per-frame routine runs
    int records;        // the channel records that a sound can start
    int voices;         // the mixer's voices, or the FIFOs
    uint8_t queue_mask; // the mask that wraps the request queue's indexes: its size in bytes, less 1

    // The sample channels' names, with null after the last sample channel.
    const char* sample_names[kChannelCount - kFirstSampleChannel];
};

// Returns the layout of revision `r`.
const RevisionLayout& LayoutOf(Revision r);

// What a channel plays: one of the PSG's channels, or samples, through the mixer in the Sword of Mana revision or
// straight through a FIFO in the Magical Vacation revision.
enum class ChannelKind
{
    kSquare,
    kWave,
    kNoise,
    kMixerSample,
    kFifoSample,
};

// Returns what channel `c` of revision `r` plays.
ChannelKind KindOf(Revision r, int c);

// Returns the name of channel `c` of revision `r`, such as "effect square 1".
const char* ChannelName(Revision r, int c);

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

// Finds the driver in `rom`. Returns false with `error` empty if the game shows no sign of the driver and no song table
// is given. Returns false and sets `error` if one of the driver's tables wasn't found or the song table has no sounds.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// A channel of a sound, as its entry in the song table lists it.
struct SongChannel
{
    int channel = 0;          // 0-3 the music's PSG, 4-7 the sound effects' PSG, 8 and up the samples
    uint32_t data = 0;        // the channel's sequence
    uint32_t subroutines = 0; // a word a subroutine: the places that F6 calls and F8 jumps to
};

// Returns the address of sound `song`'s list of channels: the word at entry `song` of the song table. It doesn't test
// that the entry is in the table.
uint32_t SongAddress(const Rom& rom, const DriverInfo& info, int song);

// Reads the channels of entry `song` of the song table. Returns false if the entry isn't in the ROM, names a channel
// that the revision doesn't have, or doesn't end within its channels.
bool ReadSong(const Rom& rom, const DriverInfo& info, int song, std::vector<SongChannel>& channels);

// The tuning of the PSG's frequency table.
struct Tuning
{
    int base = 0;  // the MIDI key that the driver's key 0 stands for
    int cents = 0; // how far the table is out from equal temperament, from -50 to 49
};

// Returns the tuning of the PSG's frequency table: the median over the keys of the PSG's lowest four octaves.
Tuning PsgTuning(const Rom& rom, const DriverInfo& info);

} // namespace agbsf2::brownie
