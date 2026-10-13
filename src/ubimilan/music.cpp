// SPDX-License-Identifier: MIT

// Ubisoft Milan's driver behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "program.h"
#include "sf2.h"
#include "ubimilan/convert.h"
#include "ubimilan/driver.h"
#include "ubimilan/sequencer.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{
namespace
{

class UbiMilanMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    UbiMilanMusic(const Rom& rom, DriverInfo info) : rom_(rom), info_(std::move(info))
    {
        log_.push_back("Ubisoft Milan's sound driver");
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
        SongReport r = Report(ubimilan::InspectSong(rom_, info_, song, OptionsFor(settings)));
        r.address = Address(song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        SongReport r = Report(ubimilan::ConvertSong(rom_, info_, song, OptionsFor(settings), shared_.get()));
        r.address = Address(song);

        return r;
    }

    bool SupportsVoiceChannels() const override
    {
        return false;
    }

    // The shared SoundFont collects the instruments of every song.
    void ShareSoundfont() override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File f;
        shared_->Build(rom_, info_, f);
        f.name = rom_.Title();
        f.comment = "Instruments for " + rom_.Title() + ", extracted by " + kProgramName;

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
                std::fprintf(out, "%lld w %08x %d %0*x\n", f, unsigned(w.address), int(w.size), 2 * int(w.size),
                             unsigned(w.value));
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
    // Returns the address of a song's piece of music.
    uint32_t Address(int song) const
    {
        MusicPiece piece;
        return ReadMusic(rom_, info_, song, piece) ? piece.address : 0;
    }

    static ConvertOptions OptionsFor(const ConvertSettings& settings)
    {
        ConvertOptions opt;
        opt.loops = settings.loops;
        opt.track_mask = uint16_t(settings.track_mask);
        opt.frame_timing = settings.frame_timing;
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
