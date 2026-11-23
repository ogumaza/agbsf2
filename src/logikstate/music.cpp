// SPDX-License-Identifier: MIT

// Logik State's sound driver behind the Music interface.

#include "music.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/trace.h"

#include "logikstate/convert.h"
#include "logikstate/driver.h"
#include "logikstate/player.h"
#include "logikstate/song.h"
#include "logikstate/soundfont.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::logikstate
{

namespace
{

using agblib::agb::TraceFrames;
using agblib::agb::TraceRecord;

// The start of detection's warning when it can't read the music volume from the game's code.
constexpr std::string_view kUnreadVolume = "no music volume for this release";

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

        const agblib::logikstate::Song& entry = info_.songs[size_t(song)];
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
        agblib::sequence_export::agb::Sf2File f = shared_->Build(rom_);
        return WriteSoundfont(f, rom_, path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return logikstate::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame: the player's variables and each channel's record. Each record is
    // written only when it has changed since it was last written.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        if (song < 0 || song >= SongCount())
        {
            return false;
        }

        // The players of the earlier formats stop with their module. The driver goes on mixing the channels after a
        // GBAMOD30 module without the loop flag stops. The trace goes on too. The records of a GBAMOD30 module have the
        // layout of the game's build.
        const auto playing = [](const auto& player)
        {
            return player.Playing();
        };
        const auto channel_record = [](const auto& player, int channel)
        {
            return player.ChannelRecord(channel);
        };
        const agblib::logikstate::Song& entry = info_.songs[size_t(song)];
        if (info_.format == Format::kGbaMod1)
        {
            Mod1Player player(rom_, entry, info_.music_volume);
            return TracePlayer(player, kMod1MixChannels, playing, channel_record, frames, out, warnings);
        }
        if (info_.format == Format::kGbaMod30)
        {
            Mod30Settings settings = info_.mod30;
            settings.music = info_.music_volume;
            settings.loop = entry.loop_flag;
            Mod30Player player(rom_, entry.module, settings);
            const auto valid = [](const Mod30Player& p)
            {
                return p.Valid();
            };
            const auto record = [](const Mod30Player& p, int channel)
            {
                return p.Record(channel);
            };
            return TracePlayer(player, settings.build.records, valid, record, frames, out, warnings);
        }

        Player player(rom_, entry, info_.music_volume, info_.build);
        return TracePlayer(player, kMixChannels, playing, channel_record, frames, out, warnings);
    }

private:
    // Writes the trace of `player`: the player's variables (g), then the record that `record` returns for each of the
    // first `channels` channels (c0 on). The trace goes on while `plays` returns true for the player.
    template <typename P, typename Plays, typename Record>
    static bool TracePlayer(P& player, int channels, Plays plays, Record record, long long frames, std::FILE* out,
                            std::vector<std::string>& warnings)
    {
        if (!player.Valid())
        {
            warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());
            return false;
        }

        const auto frame = [&]()
        {
            if (!plays(player))
            {
                return false;
            }

            player.Frame();

            return true;
        };
        const auto records = [&]()
        {
            std::vector<TraceRecord> r = {{"g", player.Globals()}};
            for (int c = 0; c < channels; c++)
            {
                r.push_back({"c" + std::to_string(c), record(player, c)});
            }

            return r;
        };
        TraceFrames(out, frames, frame, records);
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

    // --music-volume replaces the volume that detection read from the game's code, detection's line about it, and the
    // warning that detection couldn't read it.
    if (overrides.music_volume)
    {
        // kTopMusicVolume (music.h) comes from the game's code. The builds for GBAMOD30 modules store any word as the
        // music's volume. In LEGO Knights' Kingdom (BKNE), the routine at 0x080ADDE8 stores it whole. Each frame
        // multiplies a channel's volume by the music's, the master's and the fade, each over 64 (0x080AEA88 to
        // 0x080AEAA6, and 0x080AEB1A to 0x080AEB56 for the mixer of mode 1). The mixer of mode 0 multiplies each point
        // by that volume and adds it, shifted right by 8, to a byte of the mix. Super Puzzle Fighter II Turbo (AZ8E)
        // does so at 0x0834A000 to 0x0834A010. BKNE's copy of the mixer (LZ77 data at 0x080B90EC) does the same. The
        // mixer of mode 1 (LZ77 data at 0x080B9194) reads each channel's volume as a word and sums the products of two
        // points in the halves of a word. At the master volume of 64, a channel at its full volume plays at the
        // music's volume. Up to 256, a point at full scale then still fits its byte or its half of a word. A game with
        // a higher master volume overflows them at a lower music volume. The conversion caps each channel's level at
        // a sample's full scale anyway. The mixer of mode 1 also takes a volume of 255 as the end of its list of
        // channels (the word written at 0x080AEC18). A channel at exactly that volume silences itself and the channels
        // after it in the game. The conversion doesn't follow that.
        const int top = info.format == Format::kGbaMod30 ? kTopMusicVolume : kFullVolume;
        if (overrides.music_volume > top)
        {
            error = "--music-volume needs a volume from 1 to " + std::to_string(top) + " for this game's modules";
            return nullptr;
        }

        const std::string prefix = "music volume ";
        std::erase_if(info.log, [&](const std::string& line) { return line.starts_with(prefix); });
        std::erase_if(info.warnings, [](const std::string& w) { return w.starts_with(kUnreadVolume); });
        info.music_volume = overrides.music_volume;
        const char* const of = info.music_volume > kFullVolume ? ", above the full volume of " : " of ";
        info.log.push_back(prefix + std::to_string(info.music_volume) + of + std::to_string(kFullVolume) +
                           ", from --music-volume");
    }

    return std::make_unique<LogikStateMusic>(rom, std::move(info));
}

} // namespace agbsf2::logikstate
