// SPDX-License-Identifier: MIT

// Logik State's sound driver behind the Music interface.

#include "music.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "logikstate/convert.h"
#include "logikstate/driver.h"
#include "logikstate/player.h"
#include "logikstate/song.h"
#include "logikstate/soundfont.h"
#include "rom.h"
#include "sf2.h"
#include "trace.h"

namespace agbsf2::logikstate
{

namespace
{

class LogikStateMusic : public Music
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
    LogikStateMusic(const Rom& rom, DriverInfo info)
        : Music("Logik State's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return int(info_.songs.size());
    }

    // The files' numbers have as many digits as the largest song's.
    std::string FileNumber(int song) const override
    {
        return SongNumber(song, SongCount());
    }

    bool HasSong(int) const override
    {
        return true;
    }

    // A song of a GBAMOD1 module is a piece of it: its name shows the order that it starts at, and marks a jingle.
    // A jingle plays once.
    std::vector<SongVersion> Versions(int song) const override
    {
        if (info_.format != Format::kGbaMod1 || song < 0 || song >= SongCount())
        {
            return Music::Versions(song);
        }

        const ntdlib::logikstate::Song& entry = info_.songs[size_t(song)];
        return {{"order " + std::to_string(entry.order) + (entry.plays_once ? ", jingle" : ""), ""}};
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = logikstate::InspectSong(rom_, info_, song, settings);
        r.address = song >= 0 && song < SongCount() ? info_.songs[size_t(song)].module : 0;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return logikstate::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // The shared SoundFont numbers the instruments of every module together.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        ntdlib::sequence_export::agb::Sf2File f = shared_->Build(rom_);
        return WriteSoundfont(f, rom_, path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return logikstate::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame, like the trace of driver_emu.py: the player's variables and each
    // channel's record. Each record is written only when it has changed since it was last written.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        if (song < 0 || song >= SongCount())
        {
            return false;
        }

        const ntdlib::logikstate::Song& entry = info_.songs[size_t(song)];
        if (info_.format == Format::kGbaMod1)
        {
            Mod1Player player(rom_, entry, info_.music_volume);
            return TracePlayer(player, kMod1MixChannels, frames, out, warnings);
        }
        if (info_.format == Format::kGbaMod30)
        {
            Mod30Settings settings = info_.mod30;
            settings.music = info_.music_volume;
            settings.loop = entry.loop_flag;
            return TraceMod30(Mod30Player(rom_, entry.module, settings), settings.build.records, frames, out, warnings);
        }

        Player player(rom_, entry, info_.music_volume, info_.build);
        return TracePlayer(player, kMixChannels, frames, out, warnings);
    }

private:
    // Writes the trace of `player`, whose mixer has `channels` channels.
    template <typename P>
    static bool TracePlayer(P& player, int channels, long long frames, std::FILE* out,
                            std::vector<std::string>& warnings)
    {
        if (!player.Valid())
        {
            warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());
            return false;
        }

        std::vector<uint8_t> globals;
        std::vector<std::vector<uint8_t>> records(static_cast<size_t>(channels));
        for (long long f = 0; f < frames && player.Playing(); f++)
        {
            player.Frame();
            PrintChanged(out, f, "g", player.Globals(), globals);
            for (int c = 0; c < channels; c++)
            {
                const std::string name = "c" + std::to_string(c);
                PrintChanged(out, f, name.c_str(), player.ChannelRecord(c), records[size_t(c)]);
            }
        }

        warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());

        return true;
    }

    // Writes the trace of a GBAMOD30 module in the form of driver_emu_mod30.py: each record in the layout of the game's
    // build. The driver goes on mixing the channels after a module without the loop flag stops, and so does the trace.
    static bool TraceMod30(Mod30Player player, int records, long long frames, std::FILE* out,
                           std::vector<std::string>& warnings)
    {
        if (!player.Valid())
        {
            warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());
            return false;
        }

        std::vector<uint8_t> globals;
        std::vector<std::vector<uint8_t>> last(static_cast<size_t>(records));
        for (long long f = 0; f < frames && player.Valid(); f++)
        {
            player.Frame();
            PrintChanged(out, f, "g", player.Globals(), globals);
            for (int c = 0; c < records; c++)
            {
                const std::string name = "c" + std::to_string(c);
                PrintChanged(out, f, name.c_str(), player.Record(c), last[size_t(c)]);
            }
        }
        warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());

        return true;
    }

    const Rom& rom_;
    DriverInfo info_;
    std::unique_ptr<InstrumentSet> shared_;
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverInfo info;
    if (!DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    // --music-volume replaces the volume that detection read from the game's code, and detection's line about it.
    if (overrides.music_volume)
    {
        const std::string prefix = "music volume ";
        std::erase_if(info.log, [&](const std::string& line) { return line.starts_with(prefix); });
        info.music_volume = overrides.music_volume;
        info.log.push_back(prefix + std::to_string(info.music_volume) + " of 64, from --music-volume");
    }

    return std::make_unique<LogikStateMusic>(rom, std::move(info));
}

} // namespace agbsf2::logikstate
