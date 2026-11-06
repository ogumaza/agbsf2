// SPDX-License-Identifier: MIT

// SoundFont instruments for AlphaDream's driver: the samples that tracks 0-7 play and the PSG's channels on tracks
// 8-11.

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "alphadream/driver.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::alphadream
{

// What kind of channel plays an instrument. Each kind has presets of its own, from PresetNumber().
enum class Kind
{
    kSample,
    kSquare,
    kWave,
    kNoise,
};

// Returns the kind of channel that track `track` plays on.
constexpr Kind TrackKind(int track)
{
    return track < kSampleTracks ? Kind::kSample
           : track == 10         ? Kind::kWave
           : track == 11         ? Kind::kNoise
                                 : Kind::kSquare;
}

// Returns the preset number of instrument `instrument` on a kind of channel: 512 numbers a kind.
constexpr int PresetNumber(Kind kind, int instrument)
{
    return int(kind) * 512 + instrument;
}

// The instruments that songs play, which become a SoundFont's presets. One set can serve all of a game's songs, for a
// shared SoundFont.
class InstrumentSet
{
public:
    // Adds instrument `instrument` on a kind of channel, with the region that key `key` plays.
    void Add(Kind kind, int instrument, int key, const Region& region);

    // Adds the instruments and their presets to `file`. Returns the warnings for instruments with nothing to play.
    std::vector<std::string> Build(const Rom& rom, const DriverInfo& info, Sf2File& file) const;

private:
    // (kind, instrument) -> region's address -> the region and the keys that play it
    std::map<std::pair<int, int>, std::map<uint32_t, std::pair<Region, std::set<int>>>> used_;
};

} // namespace agbsf2::alphadream
