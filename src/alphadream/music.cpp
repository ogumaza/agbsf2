// SPDX-License-Identifier: MIT

// AlphaDream's sound driver behind the Music interface.

#include "music.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alphadream/convert.h"
#include "alphadream/driver.h"
#include "alphadream/sequencer.h"
#include "alphadream/soundfont.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::alphadream
{
namespace
{

class AlphaDreamMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    AlphaDreamMusic(const Rom& rom, DriverInfo info)
        : Music("AlphaDream's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    bool HasSong(int song) const override
    {
        Song s;
        return ReadSong(rom_, info_, song, s);
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        return alphadream::InspectSong(rom_, info_, song, settings);
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return alphadream::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

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
        return alphadream::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame, as driver_emu.py's trace does: the player while a track plays, each
    // track that plays, and each sample channel's record and each square's and the noise's record when it changes.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        Song s;
        if (!ReadSong(rom_, info_, song, s))
        {
            return false;
        }
        Sequencer seq(rom_, info_, s);
        std::array<std::array<uint8_t, 12>, kSampleTracks> last{};
        std::array<std::array<uint8_t, 12>, kTracks - kSampleTracks> last_psg{};
        for (long long f = 0; f < frames; f++)
        {
            seq.Step();
            bool any = false;
            for (int t = 0; t < kTracks; t++)
            {
                any = any || (seq.Track(t).flags & flag::kUsed);
            }
            if (any)
            {
                std::fprintf(out, "%lld p 1 flags 01 volume %04x divisor %d\n", f, seq.PlayerVolume(), kDefaultDivisor);
            }
            for (int t = 0; t < kTracks; t++)
            {
                const TrackState& track = seq.Track(t);
                if (!(track.flags & flag::kUsed))
                {
                    continue;
                }
                const std::array<uint8_t, 32> b = track.Bytes();
                std::fprintf(out, "%lld t 1.%d ", f, t);
                for (size_t i = 0; i < 4; i++)
                {
                    std::fprintf(out, "%02x", b[i]);
                }
                std::fprintf(out, " ");
                for (size_t i = 8; i < 32; i++)
                {
                    std::fprintf(out, "%02x", b[i]);
                }
                std::fprintf(out, "\n");
            }
            for (int c = 0; c < kSampleTracks; c++)
            {
                const std::array<uint8_t, 12> b = seq.Channel(c).TraceBytes();
                if (b == last[size_t(c)])
                {
                    continue;
                }
                last[size_t(c)] = b;
                std::fprintf(out, "%lld h %d ", f, c);
                for (uint8_t x : b)
                {
                    std::fprintf(out, "%02x", x);
                }
                std::fprintf(out, "\n");
            }
            for (int c = 0; c < kTracks - kSampleTracks; c++)
            {
                const std::array<uint8_t, 12> b = seq.Psg(c).TraceBytes();
                if (b == last_psg[size_t(c)])
                {
                    continue;
                }
                last_psg[size_t(c)] = b;
                std::fprintf(out, "%lld q %d ", f, c);
                for (uint8_t x : b)
                {
                    std::fprintf(out, "%02x", x);
                }
                std::fprintf(out, "\n");
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

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides&, std::string& error)
{
    DriverInfo info;
    if (!DetectDriver(rom, info, error))
    {
        return nullptr;
    }
    return std::make_unique<AlphaDreamMusic>(rom, std::move(info));
}

} // namespace agbsf2::alphadream
