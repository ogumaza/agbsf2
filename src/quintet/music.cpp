// SPDX-License-Identifier: MIT

// Quintet's driver behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "program.h"
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

// Prints a register write as driver_emu.py's trace does: frame, address, size in bytes and value.
void PrintWrite(std::FILE* out, long long frame, const RegisterWrite& w)
{
    std::fprintf(out, "%lld %08x %d %0*x\n", frame, unsigned(w.address), int(w.size), 2 * int(w.size),
                 unsigned(w.value));
}

class QuintetMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    QuintetMusic(const Rom& rom, DriverInfo info) : rom_(rom), info_(std::move(info))
    {
        log_.push_back("Quintet's sound driver");
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
        return int(info_.song_addresses.size());
    }

    bool HasSong(int) const override
    {
        return true;
    }

    // Works out the song's length, loop points and channels with notes, using the conversion settings.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = Report(quintet::InspectSong(rom_, info_, song, OptionsFor(song, settings)));
        r.address = song >= 0 && song < SongCount() ? info_.song_addresses[size_t(song)] : 0;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        SongReport r = Report(quintet::ConvertSong(rom_, info_, song, OptionsFor(song, settings), shared_.get()));
        r.address = song >= 0 && song < SongCount() ? info_.song_addresses[size_t(song)] : 0;

        return r;
    }

    bool SupportsVoiceChannels() const override
    {
        return false;
    }

    // SongBanks gives each song's presets their bank and programs in the shared SoundFont.
    void ShareSoundfont() override
    {
        shared_ = std::make_unique<SoundfontBuilder>(rom_, SongCount());
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
    // Returns the conversion options for `song`: the settings, and with a shared SoundFont the song's bank.
    ConvertOptions OptionsFor(int song, const ConvertSettings& settings) const
    {
        ConvertOptions opt;
        opt.loops = settings.loops;
        opt.track_mask = uint16_t(settings.track_mask);
        opt.frame_timing = settings.frame_timing;
        opt.bank = shared_ ? song : 0;
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
