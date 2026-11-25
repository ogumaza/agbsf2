// SPDX-License-Identifier: MIT

// What the Music classes of the module players share: the ROM, what detection found, the shared SoundFont, the
// conversion of a list of songs, the trace of a model and the opening of the music after detection.

#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/trace.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/agb/sf2.h"

#include "music.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2
{

// The Music of a module player. Every entry of the song table is a song, the shared SoundFont collects the instruments
// in an `Instruments`, and no listing of the songs exists. `Info` is the player's DriverInfo, with its `log` and its
// `warnings`.
template <typename Info, typename Instruments>
class ModuleMusic : public Music
{
public:
    bool HasSong(int) const override
    {
        return true;
    }

    // The shared SoundFont numbers the instruments of every song together.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<Instruments>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        agblib::sequence_export::agb::Sf2File f = shared_->Build(rom_);
        return WriteSoundfont(f, rom_, path, error);
    }

    bool DumpSong(int, const std::string&, std::string& error) const override
    {
        error = "no listing of this driver's songs";
        return false;
    }

protected:
    // Keeps a reference to `rom`. The ROM has to outlive this. `name` is the driver's name.
    ModuleMusic(const std::string& name, const Rom& rom, Info info)
        : Music(name, info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    const Rom& GameRom() const
    {
        return rom_;
    }

    const Info& Detected() const
    {
        return info_;
    }

    // Returns the shared SoundFont's instruments, or null before ShareSoundfont().
    Instruments* Shared() const
    {
        return shared_.get();
    }

private:
    const Rom& rom_;
    Info info_;
    std::unique_ptr<Instruments> shared_;
};

// The Music of a module player whose songs are the entries of `Info::songs`, converted by agblib's functions of the
// player.
template <typename Info, typename Instruments>
class SongListMusic : public ModuleMusic<Info, Instruments>
{
public:
    // agblib's InspectSong() and ConvertSong() of the player.
    using Inspect = SongReport (*)(const Rom&, const Info&, int, const agblib::sequence_export::agb::ConvertSettings&);
    using Convert = SongReport (*)(const Rom&, const Info&, int, const agblib::sequence_export::agb::ConvertSettings&,
                                   Instruments*);

    int SongCount() const override
    {
        return int(this->Detected().songs.size());
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        return inspect_(this->GameRom(), this->Detected(), song, settings);
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return convert_(this->GameRom(), this->Detected(), song, settings, this->Shared());
    }

protected:
    // Keeps a reference to `rom`. The ROM has to outlive this. `name` is the player's name.
    SongListMusic(const std::string& name, const Rom& rom, Info info, Inspect inspect, Convert convert)
        : ModuleMusic<Info, Instruments>(name, rom, std::move(info)), inspect_(inspect), convert_(convert)
    {
    }

    // Writes the state of `player`, the player's model, after each of the first `frames` frames, through agblib's
    // TraceFrames(): the record "g" from `place(player)`, then a record "cN" from `channel(player.ChannelAt(N))` for
    // each of the first `channels(player)` channels at that frame. `frame` runs a frame, or returns false once the song
    // has ended. Adds a warning where the model reached paths that it doesn't follow. `what` names the player in the
    // warning.
    template <typename Player, typename Frame, typename Channels, typename Place, typename Channel>
    static void TraceModel(Player& player, const std::string& what, Frame frame, Channels channels, Place place,
                           Channel channel, long long frames, std::FILE* out, std::vector<std::string>& warnings)
    {
        const auto records = [&]()
        {
            std::vector<agblib::agb::TraceRecord> r = {{"g", place(player)}};
            const int count = int(channels(player));
            for (int c = 0; c < count; c++)
            {
                r.push_back({"c" + std::to_string(c), channel(player.ChannelAt(c))});
            }
            return r;
        };
        agblib::agb::TraceFrames(out, frames, frame, records);
        if (player.Unmodelled() != 0)
        {
            warnings.push_back("the " + what + " reached " + std::to_string(player.Unmodelled()) +
                               " paths that the model doesn't follow");
        }
    }

    // TraceModel() of `song` on a new model, `Player(rom, info)` started with Start(song), that runs every frame.
    template <typename Player, typename Channels, typename Place, typename Channel>
    bool TraceSong(int song, const std::string& what, Channels channels, Place place, Channel channel, long long frames,
                   std::FILE* out, std::vector<std::string>& warnings) const
    {
        if (song < 0 || song >= SongCount())
        {
            return false;
        }

        Player player(this->GameRom(), this->Detected());
        player.Start(song);
        const auto frame = [&]()
        {
            player.Frame();
            return true;
        };
        TraceModel(player, what, frame, channels, place, channel, frames, out, warnings);

        return true;
    }

private:
    Inspect inspect_;
    Convert convert_;
};

// Detects the player in `rom` with `detect`, and returns its Music from `make`. Returns null and sets `error` where
// detection fails.
template <typename Info>
std::unique_ptr<Music> OpenDetected(const Rom& rom, bool (*detect)(const Rom&, Info&, std::string&),
                                    std::unique_ptr<Music> (*make)(const Rom&, Info), std::string& error)
{
    Info info;
    if (!detect(rom, info, error))
    {
        return nullptr;
    }

    return make(rom, std::move(info));
}

} // namespace agbsf2
