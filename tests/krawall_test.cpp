// SPDX-License-Identifier: MIT

// Unit tests of the Krawall Music class. The tests in agblib check the player. These tests build synthetic cartridges
// in memory. No game ROM is needed.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/krawall_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::krawall
{
namespace
{

using agblib::agb::Utf8;
using agblib::testing::agb_files::ReadAll;
using agblib::testing::agb_files::TempPath;
using agblib::testing::krawall_cart::Cart;
using agblib::testing::krawall_cart::LoopingCells;

// --dump lists the header, the orders and the rows, --trace gives the records that change each frame, and Music names
// Krawall.
void TestMusic()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    std::string error;
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::string dump = Utf8(TempPath("krawall_dump.txt"));
    SEQLIB_REQUIRE(music != nullptr);

    const bool dumped = music->DumpSong(0, dump, error);
    const TraceResult trace = TraceText(*music, 0, 3);
    const std::vector<uint8_t> dump_bytes = ReadAll(dump);
    const std::string dump_text(dump_bytes.begin(), dump_bytes.end());

    SEQLIB_CHECK(music->Log().front() == "Krawall");
    SEQLIB_CHECK(music->SongCount() == 1);
    SEQLIB_CHECK(dumped && trace.traced);
    SEQLIB_CHECK(dump_text.find("2 channels, 1 orders, restart at order 0, speed 3, tempo 125") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  0 | c0 C-4 i1 v64 | c1 E-4 i2 pan 20\n") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  4 | c0 jump 00\n") != std::string::npos);
    SEQLIB_CHECK(trace.text.starts_with("0 g 4080"));
    SEQLIB_CHECK(trace.text.find("\n1 c0 ") != std::string::npos);
    SEQLIB_CHECK(trace.text.find("\n1 m0 ") != std::string::npos);
}

} // namespace

void RunTests()
{
    TestMusic();
}

} // namespace agbsf2::krawall
