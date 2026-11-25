// SPDX-License-Identifier: MIT

// Engine Software's driver behind the Music interface.

#include "music.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "engine_software/convert.h"
#include "engine_software/driver.h"
#include "engine_software/player.h"
#include "engine_software/soundfont.h"
#include "module_music.h"
#include "rom.h"

namespace agbsf2::engine_software
{
namespace
{

using xsflib::AppendLe16;
using xsflib::AppendLe32;

// Returns a channel's record in the order of the driver's fields, without the envelopes' states.
std::vector<uint8_t> ChannelBytes(const Channel& c)
{
    std::vector<uint8_t> bytes;
    AppendLe32(bytes, c.handler);
    AppendLe16(bytes, c.flags);
    bytes.push_back(c.delay);
    bytes.push_back(c.retrigger);
    AppendLe32(bytes, c.instrument);
    for (const uint32_t byte : {uint32_t(c.note), uint32_t(c.base), uint32_t(uint8_t(c.relative)),
                                uint32_t(uint8_t(c.volume)), uint32_t(uint8_t(c.pan)), uint32_t(c.target)})
    {
        bytes.push_back(uint8_t(byte));
    }
    AppendLe16(bytes, uint16_t(c.fine_tune));
    AppendLe16(bytes, uint16_t(c.slide));
    AppendLe16(bytes, c.slide_speed);
    for (const uint32_t byte : {uint32_t(uint8_t(c.volume_slide)), uint32_t(c.vibrato_speed), uint32_t(c.vibrato_depth),
                                uint32_t(c.vibrato_place)})
    {
        bytes.push_back(uint8_t(byte));
    }
    for (const uint32_t word : {c.fade, c.loop_bytes, c.right, c.level, c.end, c.fraction, c.step, c.place})
    {
        AppendLe32(bytes, word);
    }

    return bytes;
}

// Returns the driver's place: the order, the row, the tick, the speed, the tempo, the pattern delay, the music volume
// and the samples a tick.
std::vector<uint8_t> PlaceBytes(const Player& player)
{
    std::vector<uint8_t> bytes;
    bytes.push_back(player.Order());
    AppendLe16(bytes, player.Row());
    bytes.push_back(player.TickCount());
    bytes.push_back(player.Speed());
    bytes.push_back(player.Tempo());
    bytes.push_back(player.PatternDelay());
    AppendLe16(bytes, player.MusicVolume());
    AppendLe16(bytes, player.TickSamples());

    return bytes;
}

class EngineSoftwareMusic : public SongListMusic<DriverInfo, InstrumentSet>
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
    EngineSoftwareMusic(const Rom& rom, DriverInfo info)
        : SongListMusic("Engine Software's driver", rom, std::move(info), engine_software::InspectSong,
                        engine_software::ConvertSong)
    {
    }

    // Writes the model's state after each frame: the driver's place, then each music channel.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        const auto channels = [](const Player& player)
        {
            return player.MusicChannels();
        };
        return TraceSong<Player>(song, "driver", channels, PlaceBytes, ChannelBytes, frames, out, warnings);
    }
};

} // namespace

std::unique_ptr<Music> MakeMusic(const Rom& rom, DriverInfo info)
{
    return std::make_unique<EngineSoftwareMusic>(rom, std::move(info));
}

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides&, std::string& error)
{
    return OpenDetected(rom, DetectDriver, MakeMusic, error);
}

} // namespace agbsf2::engine_software
