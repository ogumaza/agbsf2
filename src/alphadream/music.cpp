// SPDX-License-Identifier: MIT

// AlphaDream's sound driver behind the Music interface.

#include "music.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/trace.h"

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

using agblib::agb::PrintChanged;
using agblib::agb::PrintHex;

// The bytes of a track record.
using TrackBytes = std::array<uint8_t, 32>;

// Returns a channel's record as the trace prints it.
template <typename C>
std::vector<uint8_t> RecordBytes(const C& channel)
{
    const std::array<uint8_t, 12> bytes = channel.TraceBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Bytes 4 to 7 of a track record contain the address of the track data. The trace leaves them out.
constexpr size_t kDataAddressStart = 4;
constexpr size_t kDataAddressEnd = 8;

class AlphaDreamMusic : public Music
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this.
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
        agblib::sequence_export::agb::Sf2File f;
        shared_->Build(rom_, info_, f);
        return WriteSoundfont(f, rom_, path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return alphadream::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame: the player while a track plays, each track that plays, and each
    // sample channel's record and each square's and the noise's record when it changes.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        Song s;
        if (!ReadSong(rom_, info_, song, s))
        {
            return false;
        }

        Sequencer seq(rom_, info_, s);
        std::array<std::vector<uint8_t>, kSampleTracks> last;
        std::array<std::vector<uint8_t>, kTracks - kSampleTracks> last_psg;
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

                const TrackBytes b = track.Bytes();
                std::fprintf(out, "%lld t 1.%d ", f, t);
                PrintHex(out, std::span<const uint8_t>(b).first(kDataAddressStart));
                std::fprintf(out, " ");
                PrintHex(out, std::span<const uint8_t>(b).subspan(kDataAddressEnd));
                std::fprintf(out, "\n");
            }

            for (int c = 0; c < kSampleTracks; c++)
            {
                const std::string name = "h " + std::to_string(c);
                PrintChanged(out, f, name.c_str(), RecordBytes(seq.Channel(c)), last[size_t(c)]);
            }

            for (int c = 0; c < kTracks - kSampleTracks; c++)
            {
                const std::string name = "q " + std::to_string(c);
                PrintChanged(out, f, name.c_str(), RecordBytes(seq.Psg(c)), last_psg[size_t(c)]);
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
