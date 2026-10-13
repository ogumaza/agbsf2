// SPDX-License-Identifier: MIT

// MP2K's data: song headers, track commands, voices and samples.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace agbsf2::mp2k
{

// The track commands. Bytes 0x80 to 0xB0 are waits, and 0xD0 to 0xFF notes with a length.
enum Command : uint8_t
{
    kCmdWait = 0x80,   // 0x80-0xB0: a wait of ClockLength(command - 0x80) ticks
    kCmdFine = 0xB1,   // the end of the track
    kCmdGoto = 0xB2,   // 4 bytes: an address to go on from
    kCmdPatt = 0xB3,   // 4 bytes: the address of a pattern to play, which ends at a kCmdPend
    kCmdPend = 0xB4,   // the end of a pattern
    kCmdRept = 0xB5,   // a count and 4 bytes: an address to go back to, so that the part plays count times in all
    kCmdMemAcc = 0xB9, // an operation, a byte of the memory area, a value, and an address for the conditional jumps
    kCmdPrio = 0xBA,   // the track's priority
    kCmdTempo = 0xBB,  // half the tempo, in quarter notes a minute
    kCmdKeySh = 0xBC,  // a key shift in semitones
    kCmdVoice = 0xBD,  // a voice of the voice group
    kCmdVol = 0xBE,
    kCmdPan = 0xBF,   // the track's pan, with 0x40 the centre
    kCmdBend = 0xC0,  // the pitch bend, with 0x40 the centre
    kCmdBendR = 0xC1, // the bend range in semitones
    kCmdLfoS = 0xC2,  // the LFO's speed
    kCmdLfoDl = 0xC3, // the LFO's delay in ticks
    kCmdMod = 0xC4,   // the LFO's depth
    kCmdModT = 0xC5,  // the LFO's target: 0 pitch, 1 volume, 2 pan
    kCmdTune = 0xC8,  // a fine tune in 64ths of a semitone, with 0x40 the centre
    kCmdPort = 0xCC,  // a sound register and a value to write to it
    kCmdXcmd = 0xCD,  // an extended command: a number and its arguments
    kCmdEot = 0xCE,   // the end of a tied note, with its key if it's given
    kCmdTie = 0xCF,   // a note that lasts until kCmdEot
};

// The extended commands that the driver knows.
enum Xcommand : uint8_t
{
    kXcmdWave = 0x01,   // 4 bytes: the address of the track's sample
    kXcmdType = 0x02,   // the track's voice type
    kXcmdAttack = 0x04, // the track's envelope
    kXcmdDecay = 0x05,
    kXcmdSustain = 0x06,
    kXcmdRelease = 0x07,
    kXcmdEchoVol = 0x08, // the level of the echo after a note's release
    kXcmdEchoLen = 0x09, // the echo's length in frames
    kXcmdLength = 0x0A,  // a PSG voice's length
    kXcmdSweep = 0x0B,   // a PSG voice's sweep
    kXcmdWait = 0x0C,    // 2 bytes: a wait in ticks
    kXcmdOffset = 0x0D,  // 4 bytes: the point in the sample that the track's notes start from
    kXcmdCount
};

// Returns true if a command ends the track: kCmdFine, and the command numbers that the driver's command table gives the
// same handler.
bool EndsTrack(uint8_t command);

// Returns the length in ticks of wait or note length `index` (0-48).
int ClockLength(int index);

// A song's header: a track count, a priority, a reverb setting and a voice group, followed by the track addresses.
struct SongHeader
{
    uint32_t address = 0;
    int track_count = 0;
    uint8_t priority = 0;
    uint8_t reverb = 0; // the song's reverb: with bit 7 set, the driver's reverb is set to the low 7 bits
    uint32_t voices = 0;
    std::vector<uint32_t> tracks;
};

// Reads the header at `address`. Returns false if it isn't a plausible header: up to 16 tracks, with its voice group
// and tracks in the ROM unless it has no tracks.
bool ReadSongHeader(const Rom& rom, uint32_t address, SongHeader& header);

// The kinds of voice, in a voice's type byte.
constexpr uint8_t kVoicePsg = 0x07;     // the PSG channel: 1-2 square, 3 wave, 4 noise; 0 for a DirectSound sample
constexpr uint8_t kVoiceFixed = 0x08;   // a sample that plays at the mixer's rate, whatever the key
constexpr uint8_t kVoiceReverse = 0x10; // a sample that plays backwards
constexpr uint8_t kVoiceCompressed = 0x20;
constexpr uint8_t kVoiceKeySplit = 0x40; // a key split: each key plays a voice of a voice table, chosen by a key table
constexpr uint8_t kVoiceDrumKit = 0x80;  // a drum kit: each key plays a voice of a voice table at that voice's key

// A voice: 12 bytes.
struct Voice
{
    int PsgChannel() const
    {
        return type & kVoicePsg;
    }

    bool IsSplit() const
    {
        return (type & (kVoiceKeySplit | kVoiceDrumKit)) != 0;
    }

    uint32_t address = 0;
    uint8_t type = 0;
    uint8_t key = 60;      // the key that a drum kit's voice plays at
    uint8_t length = 0;    // a PSG voice's length
    uint8_t pan_sweep = 0; // a drum kit voice's pan, with bit 7 set, or the sweep of PSG channel 1
    uint32_t wave = 0;     // a sample, a square wave's duty, a wave pattern, a noise type, or a voice table
    uint8_t attack = 0;
    uint8_t decay = 0;
    uint8_t sustain = 0;
    uint8_t release = 0;
    uint32_t key_table = 0; // a key split's 128 bytes: each key's voice in the voice table
};

// Reads the voice at `address`. Returns false if it isn't in the ROM.
bool ReadVoice(const Rom& rom, uint32_t address, Voice& voice);

// Returns the address of the voice that `key` plays in a key split or drum kit, or 0 if it has none.
uint32_t SplitVoice(const Rom& rom, const Voice& split, int key);

// A sample: a 16-byte header, then signed 8-bit PCM or compressed data.
struct Wave
{
    uint32_t Data() const
    {
        return address + 16;
    }

    uint32_t address = 0;
    uint16_t type = 0; // 0 for PCM, or any other value for compressed data
    bool loops = false;
    uint32_t frequency = 0; // 1024 times the rate at which key 60 plays it
    uint32_t loop_start = 0;
    uint32_t size = 0;
};

// Reads the sample at `address`. Returns false if its header or its data isn't in the ROM.
bool ReadWave(const Rom& rom, uint32_t address, Wave& wave);

// A decoded track command.
struct Event
{
    uint8_t size = 0;    // bytes, including the command byte if it's there
    uint8_t command = 0; // a Command, or a wait or note
    int args = 0;        // argument bytes
    uint8_t arg[4] = {};

    // The address that kCmdGoto, kCmdPatt and kCmdRept go to, kCmdMemAcc's conditional jump, or the 4-byte argument of
    // kXcmdWave or kXcmdOffset.
    uint32_t target = 0;
};

// Decodes the command at `address`, after commands that left `running` as the running status (0 for none). Returns
// false if the command is unknown or runs past the end of the ROM, or needs the running status when there's none.
bool DecodeEvent(const Rom& rom, uint32_t address, uint8_t running, Event& event);

// Returns the running status after `event`: its command if it's a command from kCmdVoice up, or else `running`.
uint8_t RunningStatus(const Event& event, uint8_t running);

// Returns a description of a command for listings, such as "N24 key 60 vel 100".
std::string DescribeEvent(const Event& event);

// A track's loop and end, in ticks, from its commands without timing them.
struct TrackLayout
{
    bool loops = false;
    uint64_t loop_start = 0;   // the tick that the loop starts at, the first time through
    uint64_t loop_end = 0;     // the tick at which the track goes back to the loop's start
    uint64_t end = 0;          // the end of a track without a loop
    uint64_t last_command = 0; // the tick by which a track without a loop has played its last command other than a
                               // wait, and its notes have been released
};

// Reads the loop and end of the track at `address`. A loop is a kCmdGoto, or a kCmdRept with a count of 0, that goes
// back to a command played before. Conditional jumps read the memory area as the driver leaves it at the start, with
// every byte 0.
TrackLayout ScanTrack(const Rom& rom, uint32_t address);

} // namespace agbsf2::mp2k
