// SPDX-License-Identifier: MIT

// Unit tests of the Krawall Music class. The tests in ntdlib check the player. These tests build synthetic cartridges
// in memory. No game ROM is needed.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/agb/files.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/krawall_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::krawall
{
namespace
{

using ntdlib::agb::Utf8;
using ntdlib::testing::agb_files::OpenTempFile;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::krawall_cart::Cart;
using ntdlib::testing::krawall_cart::LoopingCells;

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
    const std::string trace = Utf8(TempPath("krawall_trace.txt"));

    const bool dumped = music && music->DumpSong(0, dump, error);
    std::FILE* out = OpenTempFile("krawall_trace.txt");
    std::vector<std::string> warnings;
    const bool traced = music && out && music->Trace(0, 3, out, warnings);
    if (out)
    {
        std::fclose(out);
    }
    const std::vector<uint8_t> dump_bytes = ReadAll(dump);
    const std::vector<uint8_t> trace_bytes = ReadAll(trace);
    const std::string dump_text(dump_bytes.begin(), dump_bytes.end());
    const std::string trace_text(trace_bytes.begin(), trace_bytes.end());

    SEQLIB_CHECK(music && music->Log().front() == "Krawall");
    SEQLIB_CHECK(music && music->SongCount() == 1);
    SEQLIB_CHECK(dumped && traced);
    SEQLIB_CHECK(dump_text.find("2 channels, 1 orders, restart at order 0, speed 3, tempo 125") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  0 | c0 C-4 i1 v64 | c1 E-4 i2 pan 20\n") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  4 | c0 jump 00\n") != std::string::npos);
    SEQLIB_CHECK(trace_text.rfind("0 g 4080", 0) == 0);
    SEQLIB_CHECK(trace_text.find("\n1 c0 ") != std::string::npos);
    SEQLIB_CHECK(trace_text.find("\n1 m0 ") != std::string::npos);
}

} // namespace

void RunTests()
{
    TestMusic();
}

} // namespace agbsf2::krawall
