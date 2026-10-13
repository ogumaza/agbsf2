// SPDX-License-Identifier: MIT

// Krawall behind the Music interface.

#include "music.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "krawall/convert.h"
#include "krawall/driver.h"
#include "krawall/player.h"
#include "krawall/song.h"
#include "program.h"
#include "sf2.h"

namespace agbsf2::krawall
{
namespace
{

// Prints a frame's line for a record that has changed since the last one printed, as driver_emu.py's trace does.
template <size_t N>
void PrintChanged(std::FILE* out, long long frame, const char* name, const std::array<uint8_t, N>& record,
                  std::array<uint8_t, N>& last)
{
    if (record == last)
    {
        return;
    }

    last = record;
    std::fprintf(out, "%lld %s ", frame, name);
    for (uint8_t b : record)
    {
        std::fprintf(out, "%02x", b);
    }
    std::fprintf(out, "\n");
}

class KrawallMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    KrawallMusic(const Rom& rom, DriverInfo info) : rom_(rom), info_(std::move(info))
    {
        log_.push_back("Krawall");
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
        SongReport r = Report(krawall::InspectSong(rom_, info_, song, OptionsFor(settings)));
        r.address = Address(song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        SongReport r = Report(krawall::ConvertSong(rom_, info_, song, OptionsFor(settings), shared_.get()));
        r.address = Address(song);

        return r;
    }

    bool SupportsVoiceChannels() const override
    {
        return false;
    }

    // The shared SoundFont numbers the instruments of every module together.
    void ShareSoundfont() override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File f = shared_->Build(rom_, info_);
        f.name = rom_.Title();
        f.comment = "Instruments for " + rom_.Title() + ", extracted by " + kProgramName;

        return f.Write(path, error);
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

        const ModuleSong& entry = info_.songs[size_t(song)];
        Player player(rom_, info_, entry.module, entry.song >= 0 ? kModeLoop | kModeSong : kModeLoop,
                      std::max(entry.song, 0));
        if (!player.Valid())
        {
            warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());
            return false;
        }

        std::array<uint8_t, 16> globals = {};
        std::array<uint8_t, kPlayerRecordSize> record = {};
        std::array<std::array<uint8_t, kChannelRecordSize>, kPlayerChannels> channels = {};
        std::array<std::array<uint8_t, kMixRecordSize>, kMixChannels> mixer = {};
        for (long long f = 0; f < frames; f++)
        {
            player.Frame();
            PrintChanged(out, f, "g", player.Globals(), globals);
            PrintChanged(out, f, "p", player.PlayerRecord(), record);
            for (int c = 0; c < player.Module().channels; c++)
            {
                const std::string name = "c" + std::to_string(c);
                PrintChanged(out, f, name.c_str(), Player::ChannelRecord(player.Channels()[size_t(c)]),
                             channels[size_t(c)]);
            }
            for (int m = 0; m < kMixChannels; m++)
            {
                const std::string name = "m" + std::to_string(m);
                PrintChanged(out, f, name.c_str(), Player::MixRecord(player.Mixer()[size_t(m)]), mixer[size_t(m)]);
            }
            if (!player.Playing())
            {
                break;
            }
        }

        warnings.insert(warnings.end(), player.Warnings().begin(), player.Warnings().end());

        return true;
    }

private:
    // Returns the address of the header of a song's module.
    uint32_t Address(int song) const
    {
        return song >= 0 && song < SongCount() ? info_.songs[size_t(song)].module : 0;
    }

    static ConvertOptions OptionsFor(const ConvertSettings& settings)
    {
        ConvertOptions opt;
        opt.loops = settings.loops;
        opt.track_mask = settings.track_mask;
        opt.out_dir = settings.out_dir;
        opt.base_name = settings.base_name;

        return opt;
    }

    // Returns a module's summary as a report.
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
