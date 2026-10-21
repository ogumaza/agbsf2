// SPDX-License-Identifier: MIT

// Ubisoft Milan's driver behind the Music interface.

#include "music.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gba.h"
#include "rom.h"
#include "sf2.h"
#include "ubimilan/convert.h"
#include "ubimilan/driver.h"
#include "ubimilan/sequencer.h"
#include "ubimilan/song.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{
namespace
{

class UbiMilanMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    UbiMilanMusic(const Rom& rom, DriverInfo info)
        : Music("Ubisoft Milan's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
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

    // The report gives the address of the song's piece of music. Only InspectSong() gives a song's address.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = ubimilan::InspectSong(rom_, info_, song, settings);
        MusicPiece piece;
        r.address = ReadMusic(rom_, info_, song, piece) ? piece.address : 0;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return ubimilan::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // The shared SoundFont collects the instruments of every song.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File f;
        shared_->Build(rom_, info_, f);
        f.SetTitle(rom_.Title());

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return ubimilan::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame: the frame's writes to the PSG's registers, the track, and each voice
    // that the mixer plays.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        Sequencer seq(rom_, info_, song);
        if (!seq.Valid())
        {
            return false;
        }

        for (long long f = 0; f < frames; f++)
        {
            for (const RegisterWrite& w : seq.Step())
            {
                PrintWrite(out, f, w, "w");
            }
            const std::array<uint32_t, 5> t = seq.TrackState();
            std::fprintf(out, "%lld t %u %08x %08x %u %u\n", f, unsigned(t[0]), unsigned(t[1]), unsigned(t[2]),
                         unsigned(t[3]), unsigned(t[4]));
            for (int v = 0; v < kVoiceCount; v++)
            {
                if (seq.VoicePlays(v))
                {
                    const std::array<uint32_t, 6> s = seq.VoiceState(v);
                    std::fprintf(out, "%lld v%d %08x %08x %08x %08x %u %u\n", f, v, unsigned(s[0]), unsigned(s[1]),
                                 unsigned(s[2]), unsigned(s[3]), unsigned(s[4]), unsigned(s[5]));
                }
            }
            if (f > 0 && !seq.Playing())
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
    std::unique_ptr<InstrumentSet> shared_;
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverOverrides ov;
    ov.song_table = overrides.song_table;
    ov.song_count = overrides.song_count;

    // Without --driver ubimilan, a game whose code doesn't have the driver isn't taken to have it, whatever sound bank
    // it's given.
    DriverInfo info;
    const bool found = DetectDriver(rom, ov, info, error);
    if (!info.step_routine && overrides.driver != Driver::kUbiMilan)
    {
        error.clear();
        return nullptr;
    }
    if (!found)
    {
        return nullptr;
    }

    return std::make_unique<UbiMilanMusic>(rom, std::move(info));
}

} // namespace agbsf2::ubimilan
