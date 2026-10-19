// SPDX-License-Identifier: MIT

// Krawall behind the Music interface.

#include "music.h"

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
    KrawallMusic(const Rom& rom, DriverInfo info)
        : Music("Krawall", info.log, info.warnings), rom_(rom), info_(std::move(info))
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

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = krawall::InspectSong(rom_, info_, song, settings);
        r.address = Address(song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return krawall::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // The shared SoundFont numbers the instruments of every module together.
    void ShareSoundfont() override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File f = shared_->Build(rom_, info_);
        f.SetTitle(rom_.Title());

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

        Player player(rom_, info_, info_.songs[size_t(song)]);
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

    const Rom& rom_;
    DriverInfo info_;
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
