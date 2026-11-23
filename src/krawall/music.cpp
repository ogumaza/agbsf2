// SPDX-License-Identifier: MIT

// Krawall behind the Music interface.

#include "music.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/trace.h"

#include "krawall/convert.h"
#include "krawall/driver.h"
#include "krawall/player.h"
#include "krawall/song.h"
#include "krawall/soundfont.h"
#include "program.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::krawall
{
namespace
{

using agblib::agb::TraceFrames;
using agblib::agb::TraceRecord;

class KrawallMusic : public Music
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
    KrawallMusic(const Rom& rom, DriverInfo info)
        : Music("Krawall", info.log, info.warnings), rom_(info.unpacked ? *info.unpacked : rom), info_(std::move(info))
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

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = krawall::InspectSong(rom_, info_, song, settings);
        r.address = Address(song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return krawall::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // The shared SoundFont numbers the instruments of every module together.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        agblib::sequence_export::agb::Sf2File f = shared_->Build(rom_, info_);
        return WriteSoundfont(f, rom_, path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return krawall::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame: mixer variables, the player record, module channels and mixer
    // channels. Each record is written only when it has changed since it was last written.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        if (song < 0 || song >= SongCount())
        {
            return false;
        }

        Player player(rom_, info_, info_.songs[size_t(song)]);
        if (!player.Valid())
        {
            warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());
            return false;
        }

        // The frame in which the player stops is the last.
        bool playing = true;
        const auto frame = [&]()
        {
            if (!playing)
            {
                return false;
            }

            player.Frame();
            playing = player.Playing();

            return true;
        };
        const auto records = [&]()
        {
            std::vector<TraceRecord> r = {{"g", player.Globals()}, {"p", player.PlayerRecord()}};
            for (int c = 0; c < player.Module().channels; c++)
            {
                r.push_back({"c" + std::to_string(c), player.ChannelRecord(player.Channels()[size_t(c)])});
            }

            for (int m = 0; m < player.MixChannels(); m++)
            {
                r.push_back({"m" + std::to_string(m), player.MixRecord(player.Mixer()[size_t(m)])});
            }

            return r;
        };
        TraceFrames(out, frames, frame, records);
        warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());

        return true;
    }

private:
    // Returns the address of the header of a song's module.
    uint32_t Address(int song) const
    {
        return song >= 0 && song < SongCount() ? info_.songs[size_t(song)].module : 0;
    }

    const Rom& rom_; // the ROM, or for the modified build, `info_.unpacked`
    DriverInfo info_;
    std::unique_ptr<InstrumentSet> shared_;
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverOverrides ov;
    ov.program = kProgramName;
    ov.module_table = overrides.song_table;
    ov.module_count = overrides.song_count;

    DriverInfo info;
    if (!DetectDriver(rom, ov, info, error))
    {
        return nullptr;
    }

    return std::make_unique<KrawallMusic>(rom, std::move(info));
}

} // namespace agbsf2::krawall
