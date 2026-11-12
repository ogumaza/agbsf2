// SPDX-License-Identifier: MIT

// Unit tests of the Konami Music class. The tests in ntdlib check the driver. These tests build synthetic cartridges in
// memory. No game ROM is needed.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/testing/konami_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "gba.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::konami
{
namespace
{

using ntdlib::testing::konami_cart::kSampleTable;
using ntdlib::testing::konami_cart::kScanSongTable;
using ntdlib::testing::konami_cart::kSongEntrySize;
using ntdlib::testing::konami_cart::kSongTable;
using ntdlib::testing::konami_cart::ScanRom;
using ntdlib::testing::konami_cart::SyntheticRom;

// OpenMusic() reads a game with Konami's driver through the Music interface, which leaves out the empty entries of the
// song table and gives --info the address of each song's data and the length that converting it would give.
void TestMusic()
{
    // The scan image's tables are found from its data, and SyntheticRom()'s are given, with an empty third entry. Its
    // first song plays a loop of 15 frames from the start.
    const Rom scan = ScanRom();
    const Rom synthetic = SyntheticRom();
    Overrides given;
    given.song_table = kSongTable;
    given.song_count = 3;
    given.sample_table = kSampleTable;
    given.mix_rate = 16000;
    ConvertSettings three_loops;
    three_loops.loops = 3;
    Overrides as_rare;
    as_rare.driver = Driver::kRare;
    std::string found_error, given_error, rare_error;
    std::vector<std::string> trace_warnings;

    const std::unique_ptr<Music> found = agbsf2::OpenMusic(scan, Overrides(), found_error);
    const std::unique_ptr<Music> by_hand = agbsf2::OpenMusic(synthetic, given, given_error);
    const std::unique_ptr<Music> rare = agbsf2::OpenMusic(scan, as_rare, rare_error);

    SEQLIB_CHECK(found && found->Log().front() == "Konami's sound driver");
    SEQLIB_CHECK(found && found->SongCount() == 2);
    SEQLIB_CHECK(found && found->InspectSong(0, ConvertSettings()).address == kScanSongTable + 2 * kSongEntrySize);
    SEQLIB_CHECK(by_hand && by_hand->SongCount() == 3);
    SEQLIB_CHECK(by_hand && by_hand->HasSong(0) && by_hand->HasSong(1) && !by_hand->HasSong(2));
    SEQLIB_CHECK(by_hand && by_hand->InspectSong(0, ConvertSettings()).tracks == 3);
    SEQLIB_CHECK(by_hand && by_hand->InspectSong(0, three_loops).seconds == FrameSeconds(45));
    SEQLIB_CHECK(by_hand && by_hand->InspectSong(0, three_loops).loop_end == FrameSeconds(15));
    SEQLIB_CHECK(!rare && rare_error == "no Rare sound driver found (try --song-table)");

    // Only the song table's entries can be traced: song 2 is past the scan image's two, and so is song 1 << 30, whose
    // entry's address would wrap around to song 0's. Neither writes anything.
    SEQLIB_CHECK(found && !found->Trace(2, 1, stdout, trace_warnings));
    SEQLIB_CHECK(found && !found->Trace(1 << 30, 1, stdout, trace_warnings));
}

} // namespace

void RunTests()
{
    TestMusic();
}

} // namespace agbsf2::konami
