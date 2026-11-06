// SPDX-License-Identifier: MIT
// The SoundFont of Nintendo R&D1's driver: its instrument records as presets.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "rd1/driver.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rd1
{

// The PSG volume that a PSG zone plays at its full level. The driver's volume is (v >> 3) + (v >> 4), at most 15, for a
// voice's level v of at most 125 (0x080F28F8). A zone plays the straight line 3v/16 that the volume follows under its
// limit, up to v = 125. Each note's velocity scales the line down to the driver's limit and whole steps.
constexpr double kPsgFullVolume = 125 * 3 / 16.0;

// The kinds of instrument record, by their first byte.
enum class InstrumentKind
{
    kSample,   // 'A' and the other letters: a sample
    kFixed,    // 'F': a sample that plays at the mixer's rate on every key
    kPsg,      // 'P' and 'Q': a PSG channel
    kDrums,    // 'R': an instrument for each key
    kKeySplit, // 'S': a table of keys that picks an instrument
    kNone,     // no record
};

// An instrument record. The records of the kinds that play hold an envelope; the sizes of the records differ.
struct InstrumentRecord
{
    uint32_t address = 0;
    InstrumentKind kind = InstrumentKind::kNone;
    int key = 60;         // +1 bits 0-6: the key that a drum kit's instrument plays
    int pan = 0x7F;       // s16 +2: an offset added to the channel's pan; 0x7F leaves the pan alone
    uint32_t sample = 0;  // +4: the sample's header
    uint32_t start = 0;   // +8: the envelope's level at the note's start, of 0x7F0000
    uint32_t sustain = 0; // +0xC: the sustain level
    uint32_t attack = 0;  // +0x10: what each frame adds to the level until it reaches 0x7F0000
    uint32_t decay = 0;   // +0x14: what each frame takes from it down to the sustain level
    uint32_t fall = 0;    // +0x18: what each frame takes from it from the sustain level on
    uint32_t release = 0; // +0x1C: what each frame takes from it after the note's end
    int psg_channel = 0;  // 'P': +0x20 bits 0-1
    int psg_length = 0;   // +0x20 bits 2-9: the PSG's length setting, or 0
    int psg_sweep = 0;    // +0x20 bits 10-16: square 1's sweep, or 0
    int psg_setting = 0;  // u16 +0x22: the duty (bits 1-2) or the noise's width (bit 3)
    uint32_t table = 0;   // 'R': the instruments by key; 'S': the key table
    uint32_t parts = 0;   // 'S': the instruments
};

// Returns the driver's pitch of key `key` in Hz: the pitch table's whole number.
int KeyPitch(const Rom& rom, const DriverInfo& info, int key);

// Returns the square or wave channel's frequency setting for a pitch of `hz`, as the driver works it out
// (0x080F28C4).
int PsgSetting(int hz);

// Returns whether record `r` plays square 1 with its sweep: NR10's sweep time above 0. The driver then leaves the
// channel alone after the note's restart (0x080F2AB0).
bool Sweeps(const InstrumentRecord& r);

// Returns the volume envelope of record `r`'s zones. A note with a sweep keeps its volume to its end (Sweeps()). The
// release falls from `release_from`, 0 to 1, or from the sustain level when it's negative.
VolumeEnvelope ZoneEnvelope(const InstrumentRecord& r, double release_from = -1);

// Reads the instrument record at `address`.
InstrumentRecord ReadInstrument(const Rom& rom, uint32_t address);

// Returns the instrument that program `program` of bank `bank` plays.
uint32_t ProgramInstrument(const Rom& rom, const DriverInfo& info, int bank, int program);

// Returns the record that plays key `key` of instrument `program`: a drum kit's or a key split's part, or the
// instrument itself.
InstrumentRecord PlayedInstrument(const Rom& rom, const InstrumentRecord& program, int key);

// The instruments that songs play, and the keys that they play on drum kits and key splits.
class InstrumentSet
{
public:
    // Keeps references to `rom` and `info`, which have to outlive the set.
    InstrumentSet(const Rom& rom, const DriverInfo& info);

    // Adds key `key` of program `program` of bank `bank`. `release_level` is the driver's level, 0 to 0x7F, at which a
    // note on the key starts its release, or -1 if the note ends before its release.
    void Add(int bank, int program, int key, int release_level);

    // Returns the level, 0 to 1, from which the sample zone of key `key` of program `program` of bank `bank` releases:
    // the mean level at which the notes added on its zone start their release. A drum kit and a key split have a zone
    // for each key, and other programs one for all keys. Returns -1 if none of the notes releases.
    double ReleaseFrom(int bank, int program, int key) const;

    // Returns a SoundFont with a preset for each program added: bank `bank`, program `program`.
    Sf2File Build() const;

private:
    const Rom& rom_;
    const DriverInfo& info_;
    std::map<std::pair<int, int>, std::set<int>> keys_;
    std::map<std::tuple<int, int, int>, std::pair<int64_t, int>> releases_; // the levels' sum and count on each key
};

} // namespace agbsf2::rd1
