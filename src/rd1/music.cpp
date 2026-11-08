// SPDX-License-Identifier: MIT
// Nintendo R&D1's MIDI-file driver behind the Music interface.

#include "music.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gba.h"
#include "program.h"
#include "rd1/convert.h"
#include "rd1/driver.h"
#include "rd1/sequencer.h"
#include "rd1/soundfont.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rd1
{

namespace
{

class Rd1Music : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    Rd1Music(const Rom& rom, DriverInfo info)
        : Music("Nintendo R&D1's MIDI-file sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
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

    bool HasSong(int song) const override
    {
        SongRecord record;
        return ReadSongRecord(rom_, info_, song, record);
    }

    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        return rd1::InspectSong(rom_, info_, song, settings);
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return rd1::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // The shared SoundFont holds the instruments of every song together.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<InstrumentSet>(rom_, info_);
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        ntdlib::sequence_export::agb::Sf2File f = shared_->Build();
        f.program = kProgramName;
        f.SetTitle(rom_.Title());
        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return rd1::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the model's state after each frame, as driver_emu.py's trace does: the player while it plays the song,
    // each track that hasn't ended, and each channel's record when it changes.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        SongRecord record;
        if (!ReadSongRecord(rom_, info_, song, record))
        {
            return false;
        }
        Sequencer seq(rom_, info_, record);
        if (!seq.Valid())
        {
            warnings.push_back(seq.Error());
            return false;
        }
        std::map<int, ChannelRecord> last;
        for (long long f = 0; f < frames; f++)
        {
            seq.Step();
            if (!seq.Playing())
            {
                break;
            }
            std::fprintf(out, "%lld p %d song %08x tracks %d bpm %d step %u volume %d fade %04x/%04x mode %d\n", f,
                         record.player, record.address, seq.TrackCount(), seq.Bpm(), seq.TickStep(), 256,
                         seq.FadeLevel(), seq.FadeStep(), seq.FadeMode());
            for (int t = 0; t < seq.TrackCount(); t++)
            {
                const TrackState& s = seq.Track(t);
                if (s.playing)
                {
                    std::fprintf(out, "%lld t %d.%d at +%x wait %u status %02x\n", f, record.player, t,
                                 s.at - record.midi, s.wait, s.status);
                }
            }
            for (int c = 0; c < seq.ChannelCount(); c++)
            {
                const ChannelRecord& r = seq.Channel(c);
                const auto found = last.find(c);
                if (found != last.end() && found->second.bytes == r.bytes)
                {
                    continue;
                }
                last[c] = r;
                std::fprintf(out, "%lld c %d.%d ", f, record.player, c);
                for (uint8_t b : r.bytes)
                {
                    std::fprintf(out, "%02x", b);
                }
                std::fprintf(out, "\n");
            }
        }
        warnings.insert(warnings.end(), seq.Warnings().begin(), seq.Warnings().end());
        return true;
    }

private:
    const Rom& rom_;
    DriverInfo info_;
    std::unique_ptr<InstrumentSet> shared_;
};

} // namespace

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides&, std::string& error)
{
    DriverOverrides overrides;
    overrides.program = kProgramName;
    DriverInfo info;
    if (!DetectDriver(rom, overrides, info, error))
    {
        return nullptr;
    }
    return std::make_unique<Rd1Music>(rom, std::move(info));
}

} // namespace agbsf2::rd1
