// SPDX-License-Identifier: MIT

// Unit tests of the Rare Music class. The tests in ntdlib check the driver. These tests build synthetic cartridges in
// memory. No game ROM is needed.

#include <cstdint>
#include <memory>
#include <string>

#include "ntdlib/rare/song.h"
#include "ntdlib/testing/rare_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::rare
{
namespace
{

using ntdlib::rare::Format;
using ntdlib::testing::rare_cart::Cart;
using ntdlib::testing::rare_cart::kTuneTable;
using ntdlib::testing::rare_cart::Saw;
using ntdlib::testing::rare_cart::Track;

// The tune that the game plays by the water has a version above water and one under water. The Music class returns
// both.
void TestWaterVersions()
{
    Cart cart(Format::kChannelByte);
    const auto bank = cart.Bank({{0, cart.Sample(Saw())}});
    cart.Tune({Track(Format::kChannelByte).Program(0, 0).On(0, 60, 100).Wait(240).End(),
               Track(Format::kChannelByte).Program(1, 0).On(1, 80, 100).Wait(240).End()},
              bank);
    cart.AddLevelTest();
    cart.AddWaterRoutine(0, 0, 0, 1, 1);
    const Rom rom = cart.ToRom();
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_CHECK(music && music->Versions(0).size() == 2 && music->Versions(0)[1].name == "under water");
}

// OpenMusic() finds Rare's driver from its code. A game without the driver's code is read with the driver only when
// --driver rare asks for it, with the tune table given.
void TestMusic()
{
    Cart cart(Format::kChannelByte);
    const auto bank = cart.Bank({{0, cart.Sample(Saw())}});
    const uint32_t header =
        cart.Tune({Track(Format::kChannelByte).Program(1, 0).On(1, 60, 100).Wait(10).Off(1, 60).End()}, bank);
    const Rom rom = cart.ToRom();
    Cart bare(Format::kChannelByte, 6, false);
    const auto bare_bank = bare.Bank({{0, bare.Sample(Saw())}});
    bare.Tune({Track(Format::kChannelByte).Program(1, 0).On(1, 60, 100).Wait(10).Off(1, 60).End()}, bare_bank);
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = kTuneTable;
    Overrides forced = table;
    forced.driver = Driver::kRare;
    Overrides as_konami;
    as_konami.driver = Driver::kKonami;
    std::string error, bare_error, forced_error, konami_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = rare::OpenMusic(bare_rom, table, bare_error);
    const std::unique_ptr<Music> bare_forced = rare::OpenMusic(bare_rom, forced, forced_error);
    const std::unique_ptr<Music> konami = agbsf2::OpenMusic(rom, as_konami, konami_error);

    SEQLIB_CHECK(music && music->Log().front() == "Rare's sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 1 && music->HasSong(0));
    SEQLIB_CHECK(music && music->InspectSong(0, ConvertSettings()).address == header);
    SEQLIB_CHECK(!bare_music && bare_error.empty());
    SEQLIB_CHECK(bare_forced && bare_forced->SongCount() == 1);
    SEQLIB_CHECK(!konami && konami_error.find("no Konami sound driver found") == 0);
}

} // namespace

void RunTests()
{
    TestWaterVersions();
    TestMusic();
}

} // namespace agbsf2::rare
