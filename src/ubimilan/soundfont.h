// SPDX-License-Identifier: MIT

// SoundFont instruments for Ubisoft Milan's driver: channel 9's kit, whose keys play samples or the PSG's noise, and
// the PSG channels' instruments.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "psg.h"
#include "rom.h"
#include "sf2.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{

// Returns the MIDI bank of sequence channel `channel`'s presets: the bank numbered after the channel. A PSG channel's
// programs are its instruments, and channel 9's are the offsets into its kit.
constexpr int PresetBank(int channel)
{
    return channel;
}

// The bank of the noise's drums. Channel 9's noise keys play on the PSG's noise channel, which gets a MIDI track of its
// own. No sequence channel 3 plays notes, so its bank is free.
constexpr int kNoiseBank = 3;

// The MIDI keys of the noise's drums: kKitDrums drums to a program, from kFirstDrumKey.
constexpr int kFirstDrumKey = 36;
constexpr int kKitDrums = 92;

// Returns the name of sequence channel `channel`: "Square 1", "Square 2" or "Wave" for a PSG channel, and "Kit" for
// channel 9. The channel's MIDI track has the name, and its instruments' names start with it.
const char* ChannelName(int channel);

// Returns the cents by which a square zone of key `key` plays the key's pitch in the game's frequency table: entry
// `key` - 36. A key past the table's ends takes no tuning.
int SquareCents(const Rom& rom, const DriverInfo& info, int key);

// Returns the cents by which a wave zone of key `key` plays the key's pitch in the game's frequency table: entry
// `key` - 12, which plays the 64 steps of the wave RAM's two banks. `offset` is WaveKeyOffset() of the program's wave.
int WaveCents(const Rom& rom, const DriverInfo& info, int key, int offset);

// Returns the wave that the wave channel plays for program `program`: both banks of the wave RAM, the program's 32
// bytes of the wave table, as 64 steps.
WaveShape ProgramWave(const Rom& rom, const DriverInfo& info, int program);

// The instruments that songs play, which become a SoundFont's presets. One set can serve all of a game's songs, for a
// shared SoundFont.
class InstrumentSet
{
public:
    // Adds key `key` of channel 9's kit at offset `program`. The key plays the sample resource at `sample`, from its
    // point `skip`.
    void AddKitKey(int program, int key, uint32_t sample, uint32_t skip = 0);

    // Returns the number of the noise's drum `sound`, which plays at key kFirstDrumKey + number % kKitDrums of program
    // number / kKitDrums. A new sound gets the next number.
    int AddNoise(const NoiseSound& sound);

    // Adds instrument `program` of square channel `channel`, 0 or 1, whose notes set NRx1 to `duty`. NRx1's top 2 bits
    // are the duty.
    void AddSquare(int channel, int program, uint8_t duty);

    // Adds the wave channel's program `program`.
    void AddWave(int program);

    // Adds the instruments and their presets to `file`, with the samples at the mixer's rate. The square and wave zones
    // follow the game's frequency table. Returns the warnings for instruments with nothing to play.
    std::vector<std::string> Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const;

private:
    std::map<int, std::map<int, std::pair<uint32_t, uint32_t>>> kit_keys_; // offset -> key -> sample resource, skip
    std::map<std::pair<int, int>, uint8_t> squares_;                       // (channel, program) -> NRx1
    std::set<int> waves_;                                                  // the wave channel's programs
    std::map<NoiseSound, int> noises_;                                     // the noise's drums, by number
};

} // namespace agbsf2::ubimilan
