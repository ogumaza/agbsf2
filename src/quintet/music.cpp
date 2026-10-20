// SPDX-License-Identifier: MIT

// Quintet's driver behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gba.h"
#include "quintet/convert.h"
#include "quintet/driver.h"
#include "quintet/sequencer.h"
#include "quintet/song.h"
#include "quintet/soundfont.h"
#include "sf2.h"

namespace agbsf2::quintet
{
namespace
{

class QuintetMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    QuintetMusic(const Rom& rom, DriverInfo info)
        : Music("Quintet's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return int(info_.song_addresses.size());
    }

    bool HasSong(int) const override
    {
        return true;
    }

    // Works out the song's length, loop points and channels with notes, using the conversion settings.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = quintet::InspectSong(rom_, info_, song, settings);
        r.address = song >= 0 && song < SongCount() ? info_.song_addresses[size_t(song)] : 0;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return quintet::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // SongBanks gives each song's presets their bank and programs in the shared SoundFont.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<SoundfontBuilder>(rom_, SongCount());
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File& f = shared_->File();
        f.SetTitle(rom_.Title());

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return quintet::DumpSong(rom_, info_, song, path, error);
    }

    // Writes every register write the model makes, frame by frame, starting with the play routine's at frame -1.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        Sequencer seq(rom_, info_, song);
        if (!seq.Valid())
        {
            return false;
        }

        for (const RegisterWrite& w : seq.StartWrites())
        {
            PrintWrite(out, -1, w);
        }
        for (long long f = 0; f < frames; f++)
        {
            for (const RegisterWrite& w : seq.Step())
            {
                PrintWrite(out, f, w);
            }
            if (seq.Ended())
            {
                break;
            }
        }

        warnings.insert(warnings.end(), seq.Warnings().begin(), seq.Warnings().end());

        return true;
    }

private:
    const Rom& rom_;
    DriverInfo info_;
    std::unique_ptr<SoundfontBuilder> shared_;
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverOverrides ov;
    ov.song_table = overrides.song_table;
    ov.song_count = overrides.song_count;

    // Without --driver quintet, a game whose code doesn't have the driver isn't taken to have it, whatever song table
    // it's given.
    DriverInfo info;
    const bool found = DetectDriver(rom, ov, info, error);
    if (!info.play && overrides.driver != Driver::kQuintet)
    {
        error.clear();
        return nullptr;
    }
    if (!found)
    {
        return nullptr;
    }

    return std::make_unique<QuintetMusic>(rom, std::move(info));
}

} // namespace agbsf2::quintet
