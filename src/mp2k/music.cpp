// SPDX-License-Identifier: MIT

// MP2K behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mp2k/convert.h"
#include "mp2k/driver.h"
#include "mp2k/sequencer.h"
#include "mp2k/song.h"
#include "mp2k/soundfont.h"
#include "sf2.h"

namespace agbsf2::mp2k
{
namespace
{

class Mp2kMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    Mp2kMusic(const Rom& rom, DriverInfo info)
        : Music("MP2K, Nintendo's MusicPlayer2000 sound driver", info.log, info.warnings),
          rom_(rom),
          info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    // The files' numbers have as many digits as the largest song's.
    std::string FileNumber(int song) const override
    {
        return SongNumber(song, SongCount());
    }

    // An entry whose header has no tracks is a placeholder.
    bool HasSong(int song) const override
    {
        SongHeader header;
        return ReadSong(rom_, info_, song, header);
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = mp2k::InspectSong(rom_, info_, song, settings);
        r.address = SongAddress(rom_, info_, song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return mp2k::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    bool SupportsVoiceChannels() const override
    {
        return true;
    }

    // Each voice that the songs play gets a preset, under its program number, in the first bank that doesn't have that
    // program yet (see SoundfontBuilder::SharedPreset()).
    void ShareSoundfont() override
    {
        shared_ = std::make_unique<SoundfontBuilder>(rom_, info_);
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File& f = shared_->File();
        f.SetTitle(rom_.Title());

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return mp2k::DumpSong(rom_, info_, song, path, error);
    }

    // Writes each sound channel that plays after each frame, one line for each: the frame, the channel (d and the
    // number of a DirectSound channel, or c and that of a PSG channel), its status, track, keys, velocity, priority,
    // envelope level and volumes, then its frequency setting and sample and its progress through the sample, or the PSG
    // channel's frequency setting, envelope goal and counter, sustain level, outputs and wave.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        Sequencer seq(rom_, info_, song);
        if (!seq.Valid())
        {
            return false;
        }

        for (long long f = 0; f < frames; f++)
        {
            seq.Step();
            for (int i = 0; i < kChannelCount; i++)
            {
                const bool psg = i >= kFirstPsgChannel;
                const Channel& c = seq.GetChannel(i);
                if ((!psg && i >= seq.DirectChannels()) || !c.status)
                {
                    continue;
                }

                std::fprintf(out, "%lld %c%d %02x %d %d %d %d %d %d %d %d", f, psg ? 'c' : 'd',
                             psg ? i - kFirstPsgChannel : i, c.status, c.track, c.midi_key, c.key, c.velocity,
                             c.priority, c.envelope, c.right, c.left);
                if (psg)
                {
                    std::fprintf(out, " %u %d %d %d %02x %08x\n", unsigned(c.frequency), c.goal, c.counter,
                                 c.sustain_goal, c.pan, unsigned(c.wave));
                }
                else
                {
                    std::fprintf(out, " %u %08x %d %d %u\n", unsigned(c.frequency), unsigned(c.wave), int(c.count),
                                 int(c.position - (c.wave + 16)), unsigned(c.fraction));
                }
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

    // Without --driver mp2k, a game whose code doesn't have the routine that starts a song isn't taken to have the
    // driver, whatever song table it's given.
    DriverInfo info;
    const bool found = DetectDriver(rom, ov, info, error);
    if (!info.song_start && overrides.driver != Driver::kMp2k)
    {
        error.clear();
        return nullptr;
    }
    if (!found)
    {
        return nullptr;
    }

    return std::make_unique<Mp2kMusic>(rom, std::move(info));
}

} // namespace agbsf2::mp2k
