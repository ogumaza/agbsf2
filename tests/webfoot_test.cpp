// SPDX-License-Identifier: MIT

// Unit tests of the Webfoot Technologies Music class. The tests in agblib check the driver. These tests build synthetic
// cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/testing/webfoot_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::webfoot
{
namespace
{

using agblib::agb::Hex;
using agblib::testing::webfoot_cart::Build;
using agblib::testing::webfoot_cart::kSongs;
using agblib::testing::webfoot_cart::NoteEntry;
using agblib::testing::webfoot_cart::Pattern;
using agblib::testing::webfoot_cart::TestCart;

// Returns whether `music` has a line among what detection found that starts with `start`.
bool Logs(const Music& music, const std::string& start)
{
    const auto starts = [&](const std::string& line)
    {
        return line.starts_with(start);
    };
    return std::any_of(music.Log().begin(), music.Log().end(), starts);
}

// Returns a cartridge whose two songs each play a note: song 0 at volume 3 and song 1 at volume 64.
Rom TwoSongs()
{
    TestCart cart(Build::k2003);
    cart.AddSample({});
    cart.AddSong({Pattern({NoteEntry(0, 60, 0, 3)}, 4)}, 125, 6);
    cart.AddSong({Pattern({NoteEntry(0, 60, 0, 64)}, 4)}, 125, 6);

    return cart.ToRom();
}

// The driver comes through OpenMusic with its name, at the music volume that the driver's init sets: 4 of 8. A
// channel's level on each side is the music volume × the channel's volume × 64 >> 10, and a note at volume 3 is
// therefore silent. --music-volume replaces the volume of each song table, and the note sounds at 8. The line of each
// song table names the range of its songs.
void TestMusicVolume()
{
    const Rom rom = TwoSongs();
    Overrides full;
    full.music_volume = 8;
    std::string error;
    const std::string table = "songs 0-1: song table at " + Hex(kSongs) + ", 2003 build, program at 0x08000000, ";

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> loud = agbsf2::OpenMusic(rom, full, error);

    SEQLIB_REQUIRE(music != nullptr && loud != nullptr);
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(music->Log().front() == "Webfoot Technologies' driver");
    SEQLIB_CHECK_EQ(music->SongCount(), 2);
    SEQLIB_CHECK(Logs(*music, table + "mixer 15978.30 Hz, "));
    SEQLIB_CHECK(music->Log().back().ends_with(", music volume 4 of 8"));
    SEQLIB_CHECK(loud->Log().back().ends_with(", music volume 8 of 8, from --music-volume"));
    SEQLIB_CHECK(music->InspectSong(0, ConvertSettings()).silent);
    SEQLIB_CHECK_EQ(music->InspectSong(1, ConvertSettings()).tracks, 1);
    SEQLIB_CHECK_EQ(loud->InspectSong(0, ConvertSettings()).tracks, 1);
}

// The driver takes a music volume up to 8. --music-volume above that is an error. In a release whose volume the game
// sets at run time, --music-volume also replaces detection's warning about it.
void TestMusicVolumeTop()
{
    const Rom rom = TwoSongs();
    Overrides set;
    set.music_volume = 2;
    Overrides over = set;
    over.music_volume = 9;
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> quiet = agbsf2::OpenMusic(rom, set, error);

    SEQLIB_REQUIRE(music != nullptr && quiet != nullptr);
    SEQLIB_CHECK(error.empty());
    SEQLIB_REQUIRE_EQ(music->Warnings().size(), size_t(1));
    SEQLIB_CHECK(music->Warnings()[0].starts_with("no music volume for the song table at "));
    SEQLIB_CHECK(quiet->Warnings().empty());
    SEQLIB_CHECK(quiet->Log().back().ends_with(", music volume 2 of 8, from --music-volume"));
    SEQLIB_CHECK(agbsf2::OpenMusic(rom, over, error) == nullptr);
    SEQLIB_CHECK(error == "--music-volume needs a volume from 1 to 8 for this driver");
}

// With --music-volume and no --driver, detection looks only for Logik State's and Webfoot Technologies' drivers. A
// game with neither gets an error that names the two.
void TestMusicVolumeDrivers()
{
    Rom empty;
    empty.Assign(std::vector<uint8_t>(0x1000, 0));
    Overrides set;
    set.music_volume = 4;
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(TwoSongs(), set, error);
    const std::unique_ptr<Music> none = agbsf2::OpenMusic(empty, set, error);

    SEQLIB_CHECK(music != nullptr && none == nullptr);
    SEQLIB_CHECK(error ==
                 "no Logik State or Webfoot Technologies sound driver found: this game's music uses another engine, "
                 "or a driver version that agbsf2 doesn't support");
}

// A song's address is its header's. The trace's first frame gives the player's place: order 0, row 0, speed 6 and
// tempo 125. Then it gives the record of the channel and of the voice that play the note. The listing of songs isn't
// written for this driver.
void TestSongs()
{
    const Rom rom = TwoSongs();
    std::string error;
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    SEQLIB_REQUIRE(music != nullptr);

    const SongReport report = music->InspectSong(1, ConvertSettings());
    const TraceResult trace = TraceText(*music, 1, 1);
    const bool listed = music->DumpSong(1, "unused.txt", error);

    SEQLIB_CHECK_EQ(report.address, kSongs + 20);
    SEQLIB_CHECK(trace.traced && trace.warnings.empty());
    SEQLIB_CHECK(trace.text.starts_with("0 g 000000067d\n0 c0 "));
    SEQLIB_CHECK(trace.text.find("\n0 v0 ") != std::string::npos && trace.text.find(" c1 ") == std::string::npos);
    SEQLIB_CHECK(!listed);
    SEQLIB_CHECK(error == "no listing of this driver's songs");
}

} // namespace

void RunTests()
{
    TestMusicVolume();
    TestMusicVolumeTop();
    TestMusicVolumeDrivers();
    TestSongs();
}

} // namespace agbsf2::webfoot
