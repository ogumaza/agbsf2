// SPDX-License-Identifier: MIT

// Locating Krawall, Sebastian Kienzl's XM and S3M player for the GBA, in a ROM: its code, its tables, the game's
// samples and instruments, and the modules it plays.

#pragma once

#include <array>
#include <compare>
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

// A row's data lists the channels that it has data for, and a 0 ends it. Each channel's data starts with a byte that
// gives the channel in its low 5 bits and the fields that follow in its top 3.
constexpr uint8_t kEntryChannel = 0x1F;
constexpr uint8_t kEntryNote = 0x20;   // the note and the instrument, in 2 bytes
constexpr uint8_t kEntryVolume = 0x40; // the volume column, in a byte
constexpr uint8_t kEntryEffect = 0x80; // the effect and its operand, in 2 bytes

// The note of a pattern that releases the channel's note.
constexpr uint8_t kNoteOff = 127;

// A channel's data in a row. A field that the row leaves out is 0.
struct RowEntry
{
    int channel = 0;
    uint8_t note = 0;        // from 1 for C-0, before the sample's relative note, or kNoteOff
    uint16_t instrument = 0; // from 1
    uint8_t volume = 0;      // the volume column's byte
    uint8_t effect = 0;
    uint8_t effect_op = 0;
};

// The offsets in a module's header. Each field is a byte. The tables of orders, pans and songs' first orders have a
// byte for each entry. Five flags start at kModuleFlags: XM instruments, linear frequencies, fast slides and two that
// the player doesn't read.
constexpr uint32_t kModuleChannels = 0x000;
constexpr uint32_t kModuleOrderCount = 0x001;
constexpr uint32_t kModuleRestart = 0x002;
constexpr uint32_t kModuleOrders = 0x003;      // 256 entries
constexpr uint32_t kModuleChannelPans = 0x103; // 32 entries
constexpr uint32_t kModuleSongStarts = 0x123;  // 64 entries
constexpr uint32_t kModuleGlobalVolume = 0x163;
constexpr uint32_t kModuleSpeed = 0x164;
constexpr uint32_t kModuleTempo = 0x165;
constexpr uint32_t kModuleFlags = 0x166;

// A module's header size up to the patterns' table.
constexpr uint32_t kModulePatterns = 0x16C;

// The offsets in a sample's header. Its points follow it, at kSamplePoints: 8-bit, unsigned, with 0x80 as silence.
constexpr uint32_t kSampleLoopLength = 0x00; // a word: the loop's length, in points
constexpr uint32_t kSampleEnd = 0x04;        // a word: the address just past the last point
constexpr uint32_t kSampleFineTune = 0x0C;   // signed, in 128ths of a semitone
constexpr uint32_t kSampleRelativeNote = 0x0D;
constexpr uint32_t kSampleVolume = 0x0E;
constexpr uint32_t kSamplePan = 0x0F;
constexpr uint32_t kSampleLoop = 0x10;        // 0 none, 1 forwards, 2 back and forth
constexpr uint32_t kSampleInterpolate = 0x11; // 1 if the mixer interpolates the sample in its quality mode 1
constexpr uint32_t kSamplePoints = 0x12;

// The offsets in an instrument. A halfword for each of the 96 notes comes first: the number of the sample it plays in
// the game's table of samples.
constexpr uint32_t kInstrumentVolumeEnvelope = 0x0C0;
constexpr uint32_t kInstrumentPanEnvelope = 0x0F4;
constexpr uint32_t kInstrumentFade = 0x128; // a halfword: the fade's step after the release
constexpr uint32_t kInstrumentVibratoDepth = 0x12C;
constexpr uint32_t kInstrumentVibratoRate = 0x12D;

// The offsets in an envelope. Its 12 points of 4 bytes come first.
constexpr uint32_t kEnvelopeLast = 0x30; // the point where the envelope loops or ends
constexpr uint32_t kEnvelopeSustain = 0x31;
constexpr uint32_t kEnvelopeLoopStart = 0x32;
constexpr uint32_t kEnvelopeFlags = 0x33;

// The bits of an envelope's flags.
constexpr uint8_t kEnvelopeOn = 1;
constexpr uint8_t kEnvelopeSustains = 2;
constexpr uint8_t kEnvelopeLoops = 4;

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

// Reads the fields that `flags` names from `at`, and moves `at` past them. `flags` is the first byte of a channel's
// data in a row.
RowEntry ReadEntry(const Rom& rom, uint8_t flags, uint32_t& at);

// Returns the number of songs that a module's orders separate with +++, or 0 if they have no marker. A song is a run of
// orders between markers.
int SongsInModule(const ModuleInfo& module);

} // namespace agbsf2::krawall
