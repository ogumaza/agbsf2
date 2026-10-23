// SPDX-License-Identifier: MIT

// Rare's driver behind the Music interface.

#include "music.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rare/convert.h"
#include "rare/driver.h"
#include "rare/sequencer.h"
#include "rare/song.h"
#include "rare/soundfont.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rare
{
namespace
{

class RareMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    RareMusic(const Rom& rom, DriverInfo info)
        : Music("Rare's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return info_.tune_count;
    }

    // Every entry of the tune table up to the count is a tune.
    bool HasSong(int) const override
    {
        return true;
    }

    // The tune that the game plays by the water has a version above water and one under water.
    std::vector<SongVersion> Versions(int song) const override
    {
        return TuneVersions(info_, song);
    }

    // Calculates the tune's duration, loop points and note-playing tracks using the conversion settings.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = rare::InspectSong(rom_, info_, song, settings);
        r.address = TuneAddress(rom_, info_, song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return rare::ConvertSong(rom_, info_, song, settings, shared_.get(), BankOf(song));
    }

    // The tunes that use the first tune's program map and instruments have their presets in bank 0, and each other pair
    // gets a bank of its own: 1, 2 and so on, past the drum channel's bank 128. The presets for notes with envelope
    // settings go in the banks after those.
    void ShareSoundfont(const ConvertSettings&) override
    {
        std::map<std::pair<uint32_t, uint32_t>, int> banks;
        int next = 0;
        for (int s = 0; s < info_.tune_count; s++)
        {
            TuneHeader h;
            ReadTuneHeader(rom_, TuneAddress(rom_, info_, s), h);
            const auto key = std::make_pair(h.program_map, h.instruments);
            if (!banks.count(key))
            {
                next = SkipDrumBank(next);
                banks[key] = next++;
            }

            banks_.push_back(banks[key]);
        }

        shared_ = std::make_unique<SoundfontBuilder>(rom_, info_, next);
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File& f = shared_->File();
        f.SetTitle(rom_.Title());

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return rare::DumpSong(rom_, info_, song, path, error);
    }

    // Writes each note slot that's playing after each frame, one line for each: frame, slot, state, channel, key, pitch
    // key, velocity, envelope phase and level, instrument, sample position and fraction.
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
            for (int i = 0; i < seq.SlotCount(); i++)
            {
                const Slot& v = seq.GetSlot(i);
                if (v.state == kSlotOn || v.state == kSlotReleased)
                {
                    std::fprintf(out, "%lld %d %02x %d %d %d %d %d %d %08x %u %u\n", f, i, v.state, v.channel, v.key,
                                 v.pitch_key, v.velocity - 1, v.phase, int(v.level), unsigned(v.instrument),
                                 unsigned(v.position), unsigned(v.fraction));
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
    // Returns the bank of `song`'s presets in the shared SoundFont, or 0 without one.
    int BankOf(int song) const
    {
        return shared_ && song < int(banks_.size()) ? banks_[size_t(song)] : 0;
    }

    const Rom& rom_;
    DriverInfo info_;
    std::unique_ptr<SoundfontBuilder> shared_;
    std::vector<int> banks_; // each tune's bank in the shared SoundFont
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverOverrides ov;
    ov.tune_table = overrides.song_table;
    ov.tune_count = overrides.song_count;

    // Without --driver rare, a game whose code doesn't have the driver's init isn't taken to have the driver, whatever
    // tune table it's given.
    DriverInfo info;
    const bool found = DetectDriver(rom, ov, info, error);
    if (!info.init && overrides.driver != Driver::kRare)
    {
        error.clear();
        return nullptr;
    }
    if (!found)
    {
        return nullptr;
    }

    return std::make_unique<RareMusic>(rom, std::move(info));
}

} // namespace agbsf2::rare
