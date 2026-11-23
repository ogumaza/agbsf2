// SPDX-License-Identifier: MIT

// Webfoot Technologies' sound driver behind the Music interface.

#include "music.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/trace.h"
#include "xsflib/bytes.h"

#include "module_music.h"
#include "rom.h"
#include "webfoot/convert.h"
#include "webfoot/driver.h"
#include "webfoot/player.h"
#include "webfoot/soundfont.h"

namespace agbsf2::webfoot
{
namespace
{

using xsflib::AppendLe16;
using xsflib::AppendLe32;

using agblib::agb::Hex;
using agblib::agb::kFrameCycles;
using agblib::agb::TraceFrames;
using agblib::agb::TraceRecord;

// The part of detection's line for each song table. OpenMusic() writes each table's line again, with its songs.
constexpr std::string_view kTableLine = " build: song table at ";

// The start of detection's warning for a song table whose music volume the game sets at run time.
constexpr std::string_view kUnreadVolume = "no music volume for the song table at ";

// Returns a channel's record in the order of the driver's fields.
std::vector<uint8_t> ChannelBytes(const Channel& c)
{
    std::vector<uint8_t> bytes;
    AppendLe32(bytes, c.step);
    AppendLe32(bytes, c.target);
    for (const uint8_t b :
         {uint8_t(c.volume), uint8_t(c.pan), c.flags, c.slide, c.volume_memory, c.pitch_memory, c.retrigger_memory,
          c.vibrato_speed, c.vibrato_depth, c.vibrato_place, c.counter, uint8_t(c.routine), c.mask, c.last_mask, c.note,
          c.instrument, c.volume_byte, c.effect, c.parameter})
    {
        bytes.push_back(b);
    }

    return bytes;
}

// Returns a voice's record in the order of the driver's fields.
std::vector<uint8_t> VoiceBytes(const Voice& v)
{
    std::vector<uint8_t> bytes;
    AppendLe32(bytes, uint32_t(v.position));
    AppendLe32(bytes, uint32_t(v.step));
    AppendLe32(bytes, v.points);
    bytes.push_back(uint8_t(v.flags));
    bytes.push_back(uint8_t(v.sample_flags));
    AppendLe16(bytes, uint16_t(v.left));
    AppendLe16(bytes, uint16_t(v.right));
    bytes.push_back(uint8_t(v.volume));
    bytes.push_back(uint8_t(v.pan));
    AppendLe16(bytes, uint16_t(v.rate));
    AppendLe32(bytes, v.speed);
    AppendLe32(bytes, uint32_t(v.loop_start));
    AppendLe32(bytes, uint32_t(v.end));
    AppendLe32(bytes, v.sample);
    AppendLe32(bytes, v.pitch);

    return bytes;
}

// Returns the player's place in the song: the order and the row that it last read, the speed and the tempo.
std::vector<uint8_t> PlaceBytes(const Player& player)
{
    std::vector<uint8_t> bytes;
    AppendLe16(bytes, uint16_t(player.RowOrder()));
    bytes.push_back(uint8_t(player.RowNumber()));
    bytes.push_back(uint8_t(player.Speed()));
    bytes.push_back(uint8_t(player.Tempo()));

    return bytes;
}

// Returns the line of --info for `table`. The songs of the table are numbered from `first`. `volume` says where the
// music volume comes from, or is empty for the game.
std::string TableLine(const SongTable& table, int first, const std::string& volume)
{
    const int last = first + int(table.songs.size()) - 1;
    const std::string songs =
        first == last ? "song " + std::to_string(first) : "songs " + std::to_string(first) + "-" + std::to_string(last);
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.2f", MixerOf(table.build).Rate());

    return songs + ": song table at " + Hex(table.address) + ", " + (table.build == Build::k2002 ? "2002" : "2003") +
           " build, program at " + Hex(table.program) + ", mixer " + rate + " Hz, " +
           std::to_string(table.mix_samples) + " samples a mix, music volume " + std::to_string(table.music_volume) +
           " of " + std::to_string(kFullMusicVolume) + volume;
}

class WebfootMusic : public ModuleMusic<DriverInfo, InstrumentSet>
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
    WebfootMusic(const Rom& rom, DriverInfo info) : ModuleMusic("Webfoot Technologies' driver", rom, std::move(info))
    {
    }

    int SongCount() const override
    {
        return Detected().SongCount();
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = webfoot::InspectSong(GameRom(), Detected(), song, settings);
        int index = 0;
        const SongTable* table = Detected().TableOf(song, index);
        r.address = table ? table->songs[size_t(index)].address : 0;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return webfoot::ConvertSong(GameRom(), Detected(), song, settings, Shared());
    }

    // Writes the model's state after each frame: the player's place in the song, then each channel's record and its
    // voice's record.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        int index = 0;
        const SongTable* table = Detected().TableOf(song, index);
        if (!table)
        {
            return false;
        }

        Player player(GameRom(), Detected(), song, table->music_volume);
        if (!player.Valid())
        {
            warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());
            return false;
        }

        // A frame's samples are those that the FIFOs play by its end.
        const uint64_t cycles = player.MixerRules().timer_cycles;
        uint64_t mixed = 0;
        uint64_t played = 0;
        const auto frame = [&]()
        {
            if (!player.Playing())
            {
                return false;
            }

            const uint64_t end = ++played * kFrameCycles / cycles;
            player.Mix(uint32_t(end - mixed), nullptr);
            mixed = end;
            return true;
        };
        const auto records = [&]()
        {
            std::vector<TraceRecord> r = {{"g", PlaceBytes(player)}};
            for (int c = 0; c < kChannels; c++)
            {
                const std::string number = std::to_string(c);
                r.push_back({"c" + number, ChannelBytes(player.Channels()[size_t(c)])});
                r.push_back({"v" + number, VoiceBytes(player.Voices()[size_t(c)])});
            }
            return r;
        };
        TraceFrames(out, frames, frame, records);
        warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());

        return true;
    }
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverInfo info;
    if (!DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    // --music-volume replaces the volume of every song table, and the warnings that the game sets a table's volume at
    // run time. The driver's volume routine takes 0 to 8.
    std::string volume;
    if (overrides.music_volume)
    {
        if (overrides.music_volume > kFullMusicVolume)
        {
            error = "--music-volume needs a volume from 1 to " + std::to_string(kFullMusicVolume) + " for this driver";
            return nullptr;
        }

        for (SongTable& table : info.song_tables)
        {
            table.music_volume = overrides.music_volume;
        }
        std::erase_if(info.warnings, [](const std::string& w) { return w.starts_with(kUnreadVolume); });
        volume = ", from --music-volume";
    }

    // Each song table's line gives the range of its songs. The songs count through the tables.
    std::erase_if(info.log, [](const std::string& line) { return line.find(kTableLine) != std::string::npos; });
    int first = 0;
    for (const SongTable& table : info.song_tables)
    {
        info.log.push_back(TableLine(table, first, volume));
        first += int(table.songs.size());
    }

    return std::make_unique<WebfootMusic>(rom, std::move(info));
}

} // namespace agbsf2::webfoot
