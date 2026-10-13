// SPDX-License-Identifier: MIT

// Rare's driver behind the Music interface.

#include "music.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "program.h"
#include "rare/convert.h"
#include "rare/driver.h"
#include "rare/sequencer.h"
#include "rare/song.h"
#include "rare/soundfont.h"
#include "sf2.h"

namespace agbsf2::rare
{
namespace
{

class RareMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    RareMusic(const Rom& rom, DriverInfo info) : rom_(rom), info_(std::move(info))
    {
        log_.push_back("Rare's sound driver");
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
        return info_.tune_count;
    }

    // Every entry of the tune table up to the count is a tune.
    bool HasSong(int) const override
    {
        return true;
    }

    // Calculates the tune's duration, loop points and note-playing tracks using the conversion settings.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = Report(rare::InspectSong(rom_, info_, song, OptionsFor(song, settings)));
        r.address = TuneAddress(rom_, info_, song);

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return Report(rare::ConvertSong(rom_, info_, song, OptionsFor(song, settings), shared_.get()));
    }

    bool SupportsVoiceChannels() const override
    {
        return false;
    }

    // The tunes that use the first tune's program map and instruments have their presets in bank 0, and each other pair
    // gets a bank of its own: 1, 2 and so on, past the drum channel's bank 128. The presets for notes with envelope
    // settings go in the banks after those.
    void ShareSoundfont() override
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
                next += next == kDrumBank ? 1 : 0;
                banks[key] = next++;
            }

            banks_.push_back(banks[key]);
        }

        shared_ = std::make_unique<SoundfontBuilder>(rom_, info_, next);
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
    // Returns the conversion options for `song`: the settings, and the song's bank in the shared SoundFont.
    ConvertOptions OptionsFor(int song, const ConvertSettings& settings) const
    {
        ConvertOptions opt;
        opt.loops = settings.loops;
        opt.track_mask = uint16_t(settings.track_mask);
        opt.frame_timing = settings.frame_timing;
        opt.bank = shared_ && song < int(banks_.size()) ? banks_[size_t(song)] : 0;
        opt.out_dir = settings.out_dir;
        opt.base_name = settings.base_name;

        return opt;
    }

    // Returns a tune's summary as a report.
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
