// SPDX-License-Identifier: MIT

// Unit tests of the Music class of Engine Software's driver. The tests in agblib check the driver's model and the
// conversion. Detection requires a game's code. These tests therefore give the class a description of agblib's
// synthetic bank in memory. No game ROM is needed.

#include <memory>
#include <string>

#include "agblib/agb/rom.h"
#include "agblib/engine_software/driver.h"
#include "agblib/testing/engine_software_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::engine_software
{
namespace
{

namespace cart = agblib::testing::engine_software_cart;

using agblib::engine_software::Build;
using agblib::engine_software::Builds;

// The class gives the driver's name, the bank's three songs and each song's address. The trace's first frame gives the
// driver's place after the first frame of the first song: order 0, row 1, tick 1, speed 3, tempo 140, no pattern
// delay, the music volume at the first step of its ramp, 4, and 243 samples a tick. Channel 0's record follows. The
// jingle ends, so its report has no loop. The listing of songs isn't written for this driver.
void TestSongs()
{
    const Build& build = Builds().front();
    Rom rom;
    rom.Assign(cart::Image(build));
    const std::unique_ptr<Music> music = MakeMusic(rom, cart::TestInfo(rom, build));
    std::string error;

    const SongReport report = music->InspectSong(0, ConvertSettings());
    const SongReport jingle = music->InspectSong(1, ConvertSettings());
    const TraceResult trace = TraceText(*music, 0, 2);
    const bool listed = music->DumpSong(0, "unused.txt", error);

    SEQLIB_CHECK(music->Log().front() == "Engine Software's driver");
    SEQLIB_CHECK_EQ(music->SongCount(), 3);
    SEQLIB_CHECK(report.ok && !report.silent);
    SEQLIB_CHECK_EQ(report.address,
                    rom.U32(cart::kBankAddress + agblib::engine_software::kBankSongTable) + cart::kBankAddress);
    SEQLIB_CHECK(report.loop_start >= 0);
    SEQLIB_CHECK(jingle.ok && jingle.loop_start < 0);
    SEQLIB_CHECK(trace.traced && trace.warnings.empty());
    SEQLIB_CHECK(trace.text.starts_with("0 g 00010001038c000400f300\n"));
    SEQLIB_CHECK(trace.text.find("\n0 c0 ") != std::string::npos);
    SEQLIB_CHECK(!listed);
    SEQLIB_CHECK(error == "no listing of this driver's songs");
}

} // namespace

void RunTests()
{
    TestSongs();
}

} // namespace agbsf2::engine_software
