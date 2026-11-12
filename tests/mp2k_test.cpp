// SPDX-License-Identifier: MIT

// Unit tests of the MP2K Music class. The tests in ntdlib check the driver. These tests build synthetic cartridges in
// memory. No game ROM is needed.

#include <cstdint>
#include <memory>
#include <string>

#include "ntdlib/mp2k/song.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::mp2k
{
namespace
{

using enum ntdlib::mp2k::Command;
using ntdlib::testing::mp2k_cart::Cart;
using ntdlib::testing::mp2k_cart::kMode;
using ntdlib::testing::mp2k_cart::kSongTable;
using ntdlib::testing::mp2k_cart::Saw;
using ntdlib::testing::mp2k_cart::Track;

// OpenMusic() finds MP2K from its code. A game without the code is read with the driver only when --driver mp2k asks
// for it, with the song table given. An entry whose header has no tracks isn't a song.
void TestMusic()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({}, voices);
    const uint32_t header = cart.Song({Track().Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    Cart bare(kMode, {10}, false);
    bare.Song({Track().Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine()}, voices);
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = kSongTable;
    Overrides forced = table;
    forced.driver = Driver::kMp2k;
    std::string error, bare_error, forced_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = mp2k::OpenMusic(bare_rom, table, bare_error);
    const std::unique_ptr<Music> bare_forced = agbsf2::OpenMusic(bare_rom, forced, forced_error);

    SEQLIB_CHECK(music && music->Log().front() == "MP2K, Nintendo's MusicPlayer2000 sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 2 && !music->HasSong(0) && music->HasSong(1));
    SEQLIB_CHECK(music && music->InspectSong(1, ConvertSettings()).address == header);
    SEQLIB_CHECK(!bare_music && bare_error.empty());
    SEQLIB_CHECK(bare_forced && bare_forced->SongCount() == 1);
}

} // namespace

void RunTests()
{
    TestMusic();
}

} // namespace agbsf2::mp2k
