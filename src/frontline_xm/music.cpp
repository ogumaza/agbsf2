// SPDX-License-Identifier: MIT

// Frontline Studios' XM-like player behind the Music interface.

#include "music.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "frontline_xm/convert.h"
#include "frontline_xm/driver.h"
#include "frontline_xm/player.h"
#include "frontline_xm/soundfont.h"
#include "module_music.h"
#include "rom.h"

namespace agbsf2::frontline_xm
{
namespace
{

using xsflib::AppendLe16;
using xsflib::AppendLe32;

// Returns a channel's record in the order of the player's fields: the words, then the halfwords and the bytes.
std::vector<uint8_t> ChannelBytes(const Channel& c)
{
    std::vector<uint8_t> bytes;
    for (const uint32_t word : {c.instrument, c.points, c.end, c.position, c.step, c.cell})
    {
        AppendLe32(bytes, word);
    }
    for (const uint32_t half : {uint32_t(c.period), uint32_t(c.on), uint32_t(c.sounding), uint32_t(c.target)})
    {
        AppendLe16(bytes, uint16_t(half));
    }
    for (const uint32_t byte :
         {uint32_t(c.effect), uint32_t(c.column), uint32_t(c.counter), uint32_t(c.retrigger), uint32_t(c.slide_up),
          uint32_t(c.slide_down), uint32_t(c.slide_speed), uint32_t(c.arpeggio), uint32_t(uint8_t(c.volume_slide)),
          uint32_t(c.volume), uint32_t(c.vibrato_place), uint32_t(c.vibrato_depth), uint32_t(c.vibrato_speed),
          uint32_t(c.tremolo_place), uint32_t(c.tremolo_depth), uint32_t(c.tremolo_speed), uint32_t(c.loop_type)})
    {
        bytes.push_back(uint8_t(byte));
    }

    return bytes;
}

// Returns the player's place: the order and the row that it reads next, the tick, the speed, the BPM and the samples
// left to the next tick.
std::vector<uint8_t> PlaceBytes(const Player& player)
{
    std::vector<uint8_t> bytes;
    bytes.push_back(uint8_t(player.Order()));
    bytes.push_back(uint8_t(player.Row()));
    bytes.push_back(uint8_t(player.TickCount()));
    bytes.push_back(uint8_t(player.Speed()));
    bytes.push_back(uint8_t(player.Bpm()));
    AppendLe16(bytes, uint16_t(player.SamplesToTick()));

    return bytes;
}

class FrontlineXmMusic : public SongListMusic<DriverInfo, InstrumentSet>
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
    FrontlineXmMusic(const Rom& rom, DriverInfo info)
        : SongListMusic("Frontline Studios' XM-like player", rom, std::move(info), frontline_xm::InspectSong,
                        frontline_xm::ConvertSong)
    {
    }

    // Writes the model's state after each frame: the player's place, then each channel.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        const auto channels = [](const Player&)
        {
            return agblib::frontline_xm::kChannels;
        };
        return TraceSong<Player>(song, "player", channels, PlaceBytes, ChannelBytes, frames, out, warnings);
    }
};

} // namespace

std::unique_ptr<Music> MakeMusic(const Rom& rom, DriverInfo info)
{
    return std::make_unique<FrontlineXmMusic>(rom, std::move(info));
}

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides&, std::string& error)
{
    return OpenDetected(rom, DetectDriver, MakeMusic, error);
}

} // namespace agbsf2::frontline_xm
