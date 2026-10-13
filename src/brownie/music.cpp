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
#include "program.h"
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
    BrownieMusic(const Rom& rom, DriverInfo info) : rom_(rom), info_(std::move(info))
    {
        log_.push_back("Brownie Brown's sound driver");
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

    // An entry with no channels is a sound that plays nothing, which the conversion skips.
    bool HasSong(int) const override
    {
        return true;
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = Report(brownie::InspectSong(rom_, info_, song, OptionsFor(settings)));
        r.address = Address(song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        SongReport r = Report(brownie::ConvertSong(rom_, info_, song, OptionsFor(settings), shared_.get()));
        r.address = Address(song);

        return r;
    }

    bool SupportsVoiceChannels() const override
    {
        return false;
    }

    // The shared SoundFont numbers the instruments of every song together.
    void ShareSoundfont() override
    {
        shared_ = std::make_unique<InstrumentSet>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        int base = 0;
        SoundfontBuilder sf(rom_, uint32_t(info_.mix_rate));
        shared_->Build(sf, PsgTuning(rom_, info_, base));
        Sf2File& f = sf.File();
        f.name = rom_.Title();
        f.comment = "Instruments for " + rom_.Title() + ", extracted by " + kProgramName;

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
                std::fprintf(out, "%lld w %08x %d %0*x\n", f, unsigned(w.address), int(w.size), 2 * int(w.size),
                             unsigned(w.value));
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
        return song >= 0 && song < SongCount() ? rom_.U32(info_.song_table + 4 * uint32_t(song)) : 0;
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
