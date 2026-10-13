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
#include "program.h"
#include "sf2.h"

namespace agbsf2::mp2k
{
namespace
{

class Mp2kMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    Mp2kMusic(const Rom& rom, DriverInfo info) : rom_(rom), info_(std::move(info))
    {
        log_.push_back("MP2K, Nintendo's MusicPlayer2000 sound driver");
        log_.insert(log_.end(), info_.log.begin(), info_.log.end());
    }

    const std::vector<std::string>& Log() const override
    {
        return log_;
    }

    const std::vector<std::string>& Warnings() const override
    {
        return info_.warnings;
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
        return ReadSongHeader(rom_, SongAddress(rom_, info_, song), header) && header.track_count > 0;
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = Report(mp2k::InspectSong(rom_, info_, song, OptionsFor(settings)));
        r.address = SongAddress(rom_, info_, song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return Report(mp2k::ConvertSong(rom_, info_, song, OptionsFor(settings), shared_.get()));
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
        f.name = rom_.Title();
        f.comment = "Instruments for " + rom_.Title() + ", extracted by " + kProgramName;

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
    // Returns the conversion options for the settings.
    static ConvertOptions OptionsFor(const ConvertSettings& settings)
    {
        ConvertOptions opt;
        opt.loops = settings.loops;
        opt.track_mask = uint16_t(settings.track_mask);
        opt.frame_timing = settings.frame_timing;
        opt.voice_channels = settings.voice_channels;
        opt.out_dir = settings.out_dir;
        opt.base_name = settings.base_name;

        return opt;
    }

    // Returns a song's summary as a report.
    static SongReport Report(const SongSummary& sum)
    {
        SongReport r;
        r.ok = sum.ok;
        r.silent = sum.silent;
        r.seconds = sum.seconds;
        r.loop_start = sum.loop_start;
        r.loop_end = sum.loop_end;
        r.tracks = sum.tracks;
        r.bpm = sum.bpm;
        r.midi_path = sum.midi_path;
        r.sf2_path = sum.sf2_path;
        r.warnings = sum.warnings;

        return r;
    }

    const Rom& rom_;
    DriverInfo info_;
    std::vector<std::string> log_;
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
