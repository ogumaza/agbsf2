// SPDX-License-Identifier: MIT

// Frontline Studios' MOD player behind the Music interface.

#include "music.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "frontline_mod/convert.h"
#include "frontline_mod/driver.h"
#include "frontline_mod/player.h"
#include "frontline_mod/soundfont.h"
#include "module_music.h"
#include "rom.h"

namespace agbsf2::frontline_mod
{
namespace
{

using xsflib::AppendLe16;
using xsflib::AppendLe32;

// Returns a channel's record of 64 bytes in the order of the driver's fields.
std::vector<uint8_t> ChannelBytes(const Channel& c)
{
    std::vector<uint8_t> bytes;
    for (const uint32_t word :
         {c.on, c.step, c.fraction, c.position, c.end, c.loop_start, c.loop_end, c.level, uint32_t(c.tick), c.parameter,
          c.vibrato_phase, uint32_t(c.period), c.vibrato_parameter, c.target, c.memory, c.volume})
    {
        AppendLe32(bytes, word);
    }

    return bytes;
}

// Returns the player's place: the order and the row that it reads next, the speed, the music's volume and the fade
// step.
std::vector<uint8_t> PlaceBytes(const Player& player)
{
    std::vector<uint8_t> bytes;
    AppendLe16(bytes, uint16_t(player.Order()));
    AppendLe16(bytes, uint16_t(player.Row()));
    bytes.push_back(uint8_t(player.Speed()));
    AppendLe16(bytes, uint16_t(player.MusicVolume()));
    AppendLe16(bytes, uint16_t(player.FadeStep()));

    return bytes;
}

class FrontlineModMusic : public SongListMusic<DriverInfo, InstrumentSet>
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
    FrontlineModMusic(const Rom& rom, DriverInfo info)
        : SongListMusic("Frontline Studios' MOD player", rom, std::move(info), frontline_mod::InspectSong,
                        frontline_mod::ConvertSong)
    {
    }

    // A song is a part of a module: its name shows the order that it starts at, and marks a song that no call of the
    // game starts.
    std::vector<SongVersion> Versions(int song) const override
    {
        if (song < 0 || song >= SongCount())
        {
            return Music::Versions(song);
        }

        const Song& entry = Detected().songs[size_t(song)];
        return {{"order " + std::to_string(entry.order) + (entry.unanchored ? ", not started by the game" : ""), ""}};
    }

    // Writes the model's state after each frame: the player's place, then each of the module's channels.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        if (song < 0 || song >= SongCount())
        {
            return false;
        }

        const Song& entry = Detected().songs[size_t(song)];
        Player player(GameRom(), Detected());
        player.Start(Detected().modules[size_t(entry.module)], entry.order, entry.step);
        const auto frame = [&]()
        {
            if (!player.Playing())
            {
                return false;
            }

            player.Frame();
            return true;
        };
        const auto channels = [](const Player& player)
        {
            return player.MusicChannels();
        };
        TraceModel(player, "player", frame, channels, PlaceBytes, ChannelBytes, frames, out, warnings);

        return true;
    }
};

} // namespace

std::unique_ptr<Music> MakeMusic(const Rom& rom, DriverInfo info)
{
    return std::make_unique<FrontlineModMusic>(rom, std::move(info));
}

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides&, std::string& error)
{
    return OpenDetected(rom, DetectDriver, MakeMusic, error);
}

} // namespace agbsf2::frontline_mod
