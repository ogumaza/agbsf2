// SPDX-License-Identifier: MIT

// Nintendo R&D2's driver behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gba.h"
#include "rd2/convert.h"
#include "rd2/driver.h"
#include "rd2/sequencer.h"
#include "rd2/song.h"
#include "rd2/soundfont.h"
#include "sf2.h"

namespace agbsf2::rd2
{
namespace
{

// Prints a voice's state as driver_emu.py's trace does.
void PrintVoice(std::FILE* out, long long frame, int index, const Voice& v)
{
    char region[16];
    if (v.region)
    {
        std::snprintf(region, sizeof region, "%08x", unsigned(v.region));
    }
    else
    {
        std::snprintf(region, sizeof region, "fixed");
    }

    std::fprintf(out,
                 "%lld v%d %d %d t%d p%d n%d v%d %x %x vol%u f%u e%d %d:%d lfo%u,%u sl%u,%u,%d,%d,%d "
                 "env%d,%d,%u,%d,%u %s r%u s%08x pos%x x%x\n",
                 frame, index, v.state, v.type, v.track, v.priority, v.note, v.velocity, unsigned(v.base_pitch),
                 unsigned(v.pitch), unsigned(v.volume), unsigned(v.frames), v.echo, v.own_pan, v.pan,
                 unsigned(v.lfo_phase), unsigned(v.lfo_delay), unsigned(v.slide_delay), unsigned(v.slide_frames),
                 int(v.slide), int(v.slide_end), int(v.slide_step), int(v.level), int(v.target), unsigned(v.segment),
                 int(v.level_step), unsigned(v.point), region, unsigned(v.release), unsigned(v.sample),
                 unsigned(v.position), unsigned(v.psg));
}

class Rd2Music : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    Rd2Music(const Rom& rom, DriverInfo info)
        : Music("Nintendo R&D2's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return int(info_.sequence_addresses.size());
    }

    bool HasSong(int) const override
    {
        return true;
    }

    // Works out the sequence's length, loop points and tracks with notes, using the conversion settings.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = rd2::InspectSong(rom_, info_, song, settings);
        r.address = song >= 0 && song < SongCount() ? info_.sequence_addresses[size_t(song)] : 0;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return rd2::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // Starts the shared SoundFont. SongBanks gives each sequence's presets their bank and programs.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<SoundfontBuilder>(rom_, PsgShare(info_.revision), SongCount());
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File& f = shared_->File();
        f.SetTitle(rom_.Title());

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return rd2::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the PSG register writes, the state of each voice that plays and the order the mixer takes the sample
    // voices in, after each frame.
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
            for (const RegisterWrite& w : seq.Writes())
            {
                PrintWrite(out, f, w);
            }
            for (int v = 0; v < kVoices; v++)
            {
                if (seq.Voices()[size_t(v)].state != kVoiceFree)
                {
                    PrintVoice(out, f, v, seq.Voices()[size_t(v)]);
                }
            }

            std::fprintf(out, "%lld active", f);
            for (int v : seq.ActiveOrder())
            {
                std::fprintf(out, " v%d", v);
            }
            std::fprintf(out, "\n");
            if (f > 0 && seq.Ended())
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
    ov.settings = overrides.song_table;
    ov.sequence_count = overrides.song_count;

    // Without --driver rd2, a game whose code doesn't have the driver isn't taken to have it.
    DriverInfo info;
    const bool found = DetectDriver(rom, ov, info, error);
    if (!info.init && overrides.driver != Driver::kRd2)
    {
        error.clear();
        return nullptr;
    }
    if (!found)
    {
        return nullptr;
    }

    return std::make_unique<Rd2Music>(rom, std::move(info));
}

} // namespace agbsf2::rd2
