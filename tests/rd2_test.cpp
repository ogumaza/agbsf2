// SPDX-License-Identifier: MIT

// Unit tests of the Nintendo R&D2 Music class. The tests in ntdlib check the driver. These tests build synthetic
// cartridges in memory. No game ROM is needed.

#include <memory>
#include <string>

#include "ntdlib/testing/rd2_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::rd2
{
namespace
{

using ntdlib::testing::rd2_cart::Cart;
using ntdlib::testing::rd2_cart::kEffectData;
using ntdlib::testing::rd2_cart::kPlain;
using ntdlib::testing::rd2_cart::kSequenceData;
using ntdlib::testing::rd2_cart::TrackData;

void TestMutedTracks()
{
    // The game's play-music routine mutes tracks of sequence 1: track 2 for music 2, and track 1 for the other music
    // numbers that play it. Music 1 plays the sequence first and gives the first version. Music 2 makes a second
    // version. Music 3 mutes the same track as music 1 and makes no version.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kPlain}).Note(50, 20, 127).Wait(20).End()},
                    {TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(62, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(6).End()}});
    cart.MutingPlayMusic({0x0000, 0x0001, 0x0001, 0x0001, 0x0009}, 1, 2, {1, 1}, {1, 2});
    const Rom rom = cart.ToRom();
    std::string error;

    // The Music interface returns the versions.
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK_EQ(music->Versions(1).size(), 2);
    SEQLIB_CHECK(music->Versions(1)[1].name == "as music 2");
}

// The Music interface lists the sound effects as songs, after the sequences. The empty entry isn't a song. A table of
// three tracks is 6 bytes long. The first track starts at offset 6.
void TestEffects()
{
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    cart.Effects({{TrackData().Bytes({0xC2, kPlain}).Note(60, 3, 127).End(),
                   {},
                   TrackData().Bytes({0xC2, kPlain}).Note(67, 2, 127).End()}});
    const Rom rom = cart.ToRom();
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK_EQ(music->SongCount(), 4);
    SEQLIB_CHECK(music->HasSong(1) && !music->HasSong(2) && music->HasSong(3));
    SEQLIB_CHECK_EQ(music->InspectSong(1, ConvertSettings()).address, kEffectData + 6);
}

void TestMusic()
{
    // The Music interface finds the driver, names it, and reports a sequence's tracks and address.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End()}});
    const Rom rom = cart.ToRom();
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK(music->Log()[0] == "Nintendo R&D2's sound driver");
    SEQLIB_CHECK_EQ(music->SongCount(), 1);

    const SongReport report = music->InspectSong(0, ConvertSettings());

    SEQLIB_CHECK(report.ok && report.tracks == 1);
    SEQLIB_CHECK_EQ(report.address, kSequenceData);
}

} // namespace

void RunTests()
{
    TestMutedTracks();
    TestEffects();
    TestMusic();
}

} // namespace agbsf2::rd2
