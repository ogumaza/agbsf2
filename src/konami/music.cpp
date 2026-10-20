// SPDX-License-Identifier: MIT

// Konami's driver behind the Music interface.

#include "music.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "konami/convert.h"
#include "konami/driver.h"
#include "konami/seqformat.h"
#include "konami/sequencer.h"
#include "konami/soundfont.h"
#include "sf2.h"

namespace agbsf2::konami
{
namespace
{

class KonamiMusic : public Music
{
public:
    // Keeps a reference to `rom`, which has to outlive this.
    KonamiMusic(const Rom& rom, DriverInfo info)
        : Music("Konami's sound driver", info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    bool HasSong(int song) const override
    {
        SongHeader h;
        return ReadSongHeader(rom_, info_.song_table, song, info_.revision, h) && h.base != 0;
    }

    // Calculates the song's duration, loop points and note-playing tracks using the conversion settings.
    SongReport InspectSong(int song, const ConvertSettings& settings) const override
    {
        SongReport r = konami::InspectSong(rom_, info_, song, settings);
        SongHeader h;
        ReadSongHeader(rom_, info_.song_table, song, info_.revision, h);
        r.address = h.base;

        return r;
    }

    SongReport ConvertSong(int song, const ConvertSettings& settings) override
    {
        return konami::ConvertSong(rom_, info_, song, settings, shared_.get());
    }

    // SongBanks gives each song's presets their bank and programs in the shared SoundFont.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<SoundfontBuilder>(rom_, info_, SongCount());
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        Sf2File& f = shared_->File();
        f.SetTitle(rom_.Title(), " (bank = song number)");

        return f.Write(path, error);
    }

    bool DumpSong(int song, const std::string& path, std::string& error) const override
    {
        return konami::DumpSong(rom_, info_, song, path, error);
    }

    // Writes the track output records that the driver builds on its stack every frame, one line for each track: frame,
    // track, pitch, b2, vol, trig, flags, key and two pan fields. These are the record's bytes 8 and 9 in the Ultimate
    // Masters revision, and 10 and 11 in the WCT 2004 and Rave Master revisions. The Eternal Duelist and Dungeon Dice
    // Monsters revisions' records have no pan, and 0 there.
    bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const override
    {
        // Only the song table's entries are songs.
        if (song < 0 || song >= info_.song_count)
        {
            return false;
        }

        const std::unique_ptr<Sequencer> sequencer = Sequencer::Create(rom_, info_, song);
        Sequencer& seq = *sequencer;
        if (!seq.Valid())
        {
            return false;
        }

        const bool older = OlderOutputStage(info_.revision);
        const bool has_pan = RecordHasPan(info_.revision);
        const int tracks = TrackCount(info_.revision);
        for (long long f = 0; f < frames && !seq.Stopped(); f++)
        {
            const auto& records = seq.Step();
            for (int t = 0; t < tracks; t++)
            {
                const TrackOutput& r = records[t];
                const int pan1 = !has_pan ? 0 : older ? r.pan : r.pan_r;
                const int pan2 = !has_pan ? 0 : older ? r.pan_start : r.pan_l;
                std::fprintf(out, "%lld %d %d %d %d %d %d %d %d %d\n", f, t, r.pitch, r.b2, r.vol, r.trig, r.flags,
                             r.key, pan1, pan2);
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
    ov.song_table = overrides.song_table;
    ov.song_count = overrides.song_count;
    ov.sample_table = overrides.sample_table;
    ov.mix_rate = overrides.mix_rate;

    DriverInfo info;
    if (!DetectDriver(rom, ov, info, error))
    {
        return nullptr;
    }

    return std::make_unique<KonamiMusic>(rom, std::move(info));
}

} // namespace agbsf2::konami
