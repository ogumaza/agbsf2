// SPDX-License-Identifier: MIT

// The driver's data: tune headers, track commands and instruments.

#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include "music.h"
#include "rom.h"

namespace agbsf2::rare
{

// The two ways a track names a command's MIDI channel. The driver revisions of Donkey Kong Country, Sabre Wulf and It's
// Mr. Pants give the channel in a byte after the command, and those of Grunty's Revenge, Donkey Kong Country 2 and
// Banjo-Pilot in the command byte's high nibble.
enum class Format
{
    kChannelByte,
    kChannelNibble
};

// The track commands. The command numbers are those of the low nibble in the kChannelNibble format.
enum Command : uint8_t
{
    kCmdTempo = 0,    // 3 bytes: microseconds per quarter note
    kCmdDelay1 = 1,   // 1 byte: ticks to the next command
    kCmdDelay2 = 2,   // 2 bytes
    kCmdDelay3 = 3,   // 3 bytes
    kCmdNoteOnB = 4,  // the same as kCmdNoteOn
    kCmdNoteOn = 5,   // key, velocity
    kCmdNoteOff = 6,  // key, velocity (ignored)
    kCmdControl = 7,  // controller, value
    kCmdProgram = 8,  // program
    kCmdPressure = 9, // 1 byte, ignored
    kCmdBend = 10,    // 2 bytes: a 16-bit pitch bend, centred on 0x2000
    kCmdEnd = 11,     // the end of the track
    kCmdNop = 12,     // a command that does nothing
    kCommandCount
};

// Returns true if `command` is one of the delays.
constexpr bool IsDelay(uint8_t command)
{
    return command == kCmdDelay1 || command == kCmdDelay2 || command == kCmdDelay3;
}

// The controllers the driver reacts to. Controllers 20-23 set a channel's envelope only in the revision of Donkey Kong
// Country 3.
constexpr int kCtrlModulation = 1;
constexpr int kCtrlVolume = 7;
constexpr int kCtrlAttack = 20;
constexpr int kCtrlDecay = 21;
constexpr int kCtrlSustain = 22;
constexpr int kCtrlRelease = 23;
constexpr int kCtrlLoopStart = 102;
constexpr int kCtrlLoopEnd = 103;
constexpr int kCtrlMonoOn = 126;
constexpr int kCtrlPolyOn = 127;

// A tune's header: 5 words.
struct TuneHeader
{
    uint32_t address = 0;
    uint32_t track_count = 0;
    uint32_t ticks_per_quarter = 0;
    uint32_t track_list = 0;  // the tracks' start addresses, one word each
    uint32_t program_map = 0; // 128 bytes: each MIDI program's instrument, or 0xFF
    uint32_t instruments = 0; // the instrument table: a word for each instrument
    std::vector<uint32_t> tracks;
};

// Reads the header at `address`. Returns false if it isn't a plausible tune header: 1-16 tracks, a tick length from 1
// to 0x7FFF and tables in the ROM.
bool ReadTuneHeader(const Rom& rom, uint32_t address, TuneHeader& header);

// A decoded track command.
struct Event
{
    uint8_t size = 0;    // bytes, including the command's
    uint8_t command = 0; // a Command
    uint8_t channel = 0;
    uint8_t a = 0, b = 0; // the first two argument bytes: key and velocity, controller and value, program
    uint32_t value = 0;   // a delay, tempo or pitch bend
};

// Decodes the command at `address`. Returns false if its command number is unknown or it runs past the ROM's end.
bool DecodeEvent(const Rom& rom, uint32_t address, Format format, Event& event);

// Returns a description of a command for listings, such as "note on ch 2 key 60 vel 100".
std::string DescribeEvent(const Event& event);

// The most commands of a track that ScanTrack() and detection read before they give up on finding its end.
constexpr int kMaxTrackCommands = 1000000;

// Reads the loop and end of the track at `address`. A loop ends at the first loop end controller after a loop start.
// For a track without a loop, the layout's last command is the tick of its last command other than a delay or the end.
TrackLayout ScanTrack(const Rom& rom, uint32_t address, Format format);

// The kinds of instrument.
constexpr uint32_t kInstSample = 0x20;
constexpr uint32_t kInstSampleB = 0x21; // played the same as kInstSample
constexpr uint32_t kInstDrumKit = 0x22;
constexpr uint32_t kInstKeySplit = 0x23;

// The loop modes of a sample instrument. The mixer plays modes 3 and 4 the same as 1 and 2, and the driver frees a
// voice in mode 3 as soon as it's moved past the sample's end.
constexpr uint32_t kLoopOnce = 1;
constexpr uint32_t kLoopForward = 2;
constexpr uint32_t kLoopOnceB = 3;
constexpr uint32_t kLoopForwardB = 4;

// Returns true if the mixer loops a sample in loop mode `mode`.
constexpr bool LoopsForward(uint32_t mode)
{
    return mode == kLoopForward || mode == kLoopForwardB;
}

// An instrument: 17 words. A sample instrument plays a sample of signed 8-bit PCM. A drum kit or key split picks one of
// its instruments for each key from its key map, and a drum kit plays that instrument at its root key.
struct Instrument
{
    bool IsSample() const
    {
        return type == kInstSample || type == kInstSampleB;
    }

    // Returns true if the mixer loops the sample.
    bool Loops() const
    {
        return LoopsForward(loop_mode);
    }

    uint32_t type = 0;
    uint32_t loop_mode = 0;
    uint32_t rate = 0; // Hz
    uint32_t root_key = 0;
    uint32_t start = 0;
    uint32_t loop_length = 0; // the length of the loop at the sample's end, in bytes
    uint32_t end = 0;
    uint32_t key_map = 0;      // a drum kit's or key split's 128 bytes: each key's instrument
    uint32_t key_table = 0;    // the instruments that key_map numbers
    uint32_t attack = 0;       // 0-99, and 99 is instant
    uint32_t decay = 0;        // 0-99, an index into the fade times table
    uint32_t sustain = 0;      // 0-99, a level
    uint32_t release = 0;      // 0-99, an index into the fade times table
    int32_t fine_tune = 0;     // cents
    int32_t bend_range = 0;    // semitones
    uint32_t vibrato_rate = 0; // added to the vibrato phase each frame, a 24-bit fraction of a cycle
    int32_t vibrato_depth = 0; // the vibrato's depth scale
};

// Reads the instrument at `address` as the driver does, without checking that it's in the ROM. A byte outside the ROM
// reads as 0.
Instrument InstrumentAt(const Rom& rom, uint32_t address);

// Reads the instrument at `address`. Returns false if it isn't in the ROM.
bool ReadInstrument(const Rom& rom, uint32_t address, Instrument& instrument);

// The settings that controllers 20-23 give a channel's notes in place of their instruments', in the revision that reads
// them: the attack's steps, the fade table entries of the decay and the release, and the sustain level in 1/128ths of
// the full level. A setting from 0x80 up leaves the instrument's.
struct EnvelopeSettings
{
    auto operator<=>(const EnvelopeSettings&) const = default;

    // Returns true if every setting leaves the instrument's.
    bool None() const
    {
        return attack >= 0x80 && decay >= 0x80 && sustain >= 0x80 && release >= 0x80;
    }

    // Takes the settings of `other` that are below 0x80. The others stay as they are.
    void Merge(const EnvelopeSettings& other)
    {
        attack = other.attack < 0x80 ? other.attack : attack;
        decay = other.decay < 0x80 ? other.decay : decay;
        sustain = other.sustain < 0x80 ? other.sustain : sustain;
        release = other.release < 0x80 ? other.release : release;
    }

    uint8_t attack = 0xFF;
    uint8_t decay = 0xFF;
    uint8_t sustain = 0xFF;
    uint8_t release = 0xFF;
};

// Returns true if a sample instrument's sample is in the ROM and its loop fits in it, with the byte after the end that
// the driver's interpolation reads.
bool SampleValid(const Rom& rom, const Instrument& instrument);

// Returns the address of the instrument that key `key` of a drum kit or key split plays, or 0 if its key map has no
// instrument for the key. The driver plays every type of instrument that isn't a sample or a key split as a drum kit.
uint32_t SplitInstrument(const Rom& rom, const Instrument& split, int key);

} // namespace agbsf2::rare
