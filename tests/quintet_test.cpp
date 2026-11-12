// SPDX-License-Identifier: MIT

// Unit tests of the Quintet Music class. The tests in ntdlib check the driver. These tests build synthetic cartridges
// in memory. No game ROM is needed.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/agb/files.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/quintet_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::quintet
{
namespace
{

using ntdlib::agb::Utf8;
using ntdlib::quintet::Revision;
using ntdlib::testing::quintet_cart::Cart;
using ntdlib::testing::quintet_cart::Channel;
using ntdlib::testing::quintet_cart::FileAddress;
using ntdlib::testing::quintet_cart::kMusicFile;
using ntdlib::testing::quintet_cart::Song;

// OpenMusic() finds Quintet's driver from its code. A game without the driver's code isn't read, even with the music
// file given, and only --driver quintet makes that an error.
void TestMusic()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    Cart bare(Revision::kA, false);
    bare.Songs({Song({Channel().Note(0, 3).End()})});
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = FileAddress(kMusicFile);
    Overrides forced = table;
    forced.driver = Driver::kQuintet;
    std::string error, table_error, forced_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = quintet::OpenMusic(bare_rom, table, table_error);
    const std::unique_ptr<Music> bare_forced = quintet::OpenMusic(bare_rom, forced, forced_error);

    SEQLIB_CHECK(music && music->Log().front() == "Quintet's sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 1);
    SEQLIB_CHECK(!bare_music && table_error.empty());
    SEQLIB_CHECK(!bare_forced && forced_error == "no Quintet sound driver found, so it has no length table");
}

// A game whose driver of the J revision loads a music file gives the driver its tables in the file. The game's code
// passes the loader one file, and a scan of the ROM finds another. The conversion numbers the second file's songs after
// the first file's songs and sound effects, and names their files by those numbers.
void TestLoadedFiles()
{
    Cart cart(Revision::kJ);
    const std::vector<uint8_t> song = Song({Channel().Bytes({0x5E, 15}).Note(0, 3).End()});
    const std::array<uint32_t, 7> first = cart.MusicFile(8, {song, song}, {song});
    const std::array<uint32_t, 7> second = cart.MusicFile(10, {song}, {});
    cart.Loader(8);
    const Rom rom = cart.ToRom();
    std::string error;
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    ConvertSettings opt;
    opt.out_dir = Utf8(ntdlib::testing::agb_files::TempFolderPath());
    opt.base_name = "loaded_files";
    SEQLIB_REQUIRE(music && music->SongCount() == 4);

    const SongReport last = music->ConvertSong(3, opt);

    SEQLIB_CHECK_EQ(music->InspectSong(2, opt).address, first[1]);
    SEQLIB_CHECK_EQ(music->InspectSong(3, opt).address, second[0]);
    SEQLIB_CHECK(last.ok && !last.silent);
    SEQLIB_CHECK(last.midi_path == SongStem(opt, SongNumber(3, 0)) + ".mid");
}

} // namespace

void RunTests()
{
    TestMusic();
    TestLoadedFiles();
}

} // namespace agbsf2::quintet
