// SPDX-License-Identifier: MIT

// Unit tests of the Brownie Brown Music class. The tests in ntdlib check the driver. These tests build synthetic
// cartridges in memory. No game ROM is needed.

#include <memory>
#include <string>

#include "ntdlib/testing/brownie_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::brownie
{
namespace
{

using ntdlib::testing::brownie_cart::Cart;
using ntdlib::testing::brownie_cart::kPsgHold;
using ntdlib::testing::brownie_cart::kSongTable;

// OpenMusic() finds the driver from its code. A game without the driver's code isn't read, even with the song table
// given, and only --driver brownie makes that an error.
void TestMusic()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    Cart bare(false);
    bare.Sound(1, {{0, {0xFF}, {}}});
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = kSongTable;
    Overrides forced = table;
    forced.driver = Driver::kBrownie;
    std::string error, table_error, forced_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = brownie::OpenMusic(bare_rom, table, table_error);
    const std::unique_ptr<Music> bare_forced = brownie::OpenMusic(bare_rom, forced, forced_error);

    SEQLIB_CHECK(music && music->Log().front() == "Brownie Brown's sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 2);
    SEQLIB_CHECK(!bare_music && table_error.empty());
    SEQLIB_CHECK(!bare_forced && forced_error == "the driver's command table wasn't found");
}

} // namespace

void RunTests()
{
    TestMusic();
}

} // namespace agbsf2::brownie
