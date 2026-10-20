// SPDX-License-Identifier: MIT

// Brownie Brown's driver behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "brownie/convert.h"
#include "brownie/driver.h"
#include "brownie/sequencer.h"
#include "brownie/song.h"
#include "brownie/soundfont.h"
#include "gba.h"
#include "sf2.h"

namespace agbsf2::brownie
{
namespace
{

// Prints bytes in hex, as driver_emu.py's trace does.
void PrintHex(std::FILE* out, const uint8_t* bytes, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        std::fprintf(out, "%02x", bytes[i]);
    }
    std::fprintf(out, "\n");
}

class BrownieMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    BrownieMusic(const Rom& rom, DriverInfo info)
        : Music("Brownie Brown's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
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

    // An entry with no channels is a sound that plays nothing, which the conversion skips.
    bool HasSong(int) const override
    {
        return true;
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = brownie::InspectSong(rom_, info_, song, settings);
        r.address = Address(song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return brownie::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // The shared SoundFont numbers the instruments of every song together.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        SoundfontBuilder sf(rom_, uint32_t(info_.mix_rate));
        shared_->Build(sf, PsgTuning(rom_, info_).cents);
        Sf2File& f = sf.File();
        f.SetTitle(rom_.Title());

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return brownie::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame: the frame's writes to the sound registers, each channel's record that
    // isn't clear, each voice or FIFO's sample that isn't, and the driver's other variables when they change.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        Sequencer seq(rom_, info_, song);
        if (!seq.Valid())
        {
            return false;
        }

        Globals last = {};
        for (long long f = 0; f < frames; f++)
        {
            for (const RegisterWrite& w : seq.Step())
            {
                PrintWrite(out, f, w, "w");
            }

            for (int c = 0; c < seq.ChannelCount(); c++)
            {
                const std::array<uint8_t, 0x38> record = Sequencer::RecordBytes(seq.GetChannel(c));
                if (record[0])
                {
                    std::fprintf(out, "%lld c%d ", f, c);
                    PrintHex(out, record.data(), record.size());
                }
            }

            for (int v = 0; v < seq.VoiceCount(); v++)
            {
                const std::array<uint8_t, 0x10> voice = seq.VoiceRecord(v);
                bool any = false;
                for (uint8_t b : voice)
                {
                    any = any || b;
                }
                if (any)
                {
                    std::fprintf(out, "%lld v%d ", f, v);
                    PrintHex(out, voice.data(), voice.size());
                }
            }

            if (f == 0 || seq.GetGlobals() != last)
            {
                last = seq.GetGlobals();
                std::fprintf(out, "%lld g ", f);
                PrintHex(out, last.data(), seq.GlobalsSize());
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
    // Returns the address of a sound's list of channels.
    uint32_t Address(int song) const
    {
        return song >= 0 && song < SongCount() ? SongAddress(rom_, info_, song) : 0;
    }

    const Rom& rom_;
    DriverInfo info_;
    std::unique_ptr<InstrumentSet> shared_;
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    DriverOverrides ov;
    ov.song_table = overrides.song_table;
    ov.song_count = overrides.song_count;

    // Without --driver brownie, a game whose code doesn't have the driver isn't taken to have it, whatever song table
    // it's given.
    DriverInfo info;
    const bool found = DetectDriver(rom, ov, info, error);
    if (!info.frame_routine && overrides.driver != Driver::kBrownie)
    {
        error.clear();
        return nullptr;
    }
    if (!found)
    {
        return nullptr;
    }

    return std::make_unique<BrownieMusic>(rom, std::move(info));
}

} // namespace agbsf2::brownie
