// SPDX-License-Identifier: MIT

// Unit tests of the Music class of Frontline Studios' MOD player. The tests in agblib check the player and the
// conversion. Detection requires a game's code. These tests therefore give the class a description of synthetic data
// in memory. No game ROM is needed.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/testing/frontline_mod_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::frontline_mod
{
namespace
{

namespace cart = agblib::testing::frontline_mod_cart;

using agblib::agb::kRomBase;
using agblib::frontline_mod::DriverInfo;
using agblib::frontline_mod::kFullVolume;
using agblib::frontline_mod::Module;
using agblib::frontline_mod::ReadModule;
using agblib::frontline_mod::Song;

constexpr uint32_t kIwramBase = 0x03000000;

// A cartridge with the test data, the second table of periods and a module at cart::kSpare: speed 6, one order of 4
// rows, and the triangle at the root index on channel 0 at row 0.
Rom OneNote()
{
    cart::Data data(kRomBase);
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {int(cart::kRootIndex), cart::kTriangle};
    data.PutModule(cart::kSpare, 6, {rows}, {0});
    Rom rom;
    rom.Assign(cart::WithGameTable(data.Bytes()));
    return rom;
}

// The player as detection would describe it for the data: the tables, the module and one song at order 0.
DriverInfo Describe(const Rom& rom)
{
    DriverInfo info;
    info.periods = kRomBase + cart::kGameTable;
    info.vibrato = kIwramBase;
    info.image_base = kRomBase + cart::kVibrato;
    info.copy_start = kIwramBase;
    info.copy_end = kIwramBase + 64;
    info.log.push_back("song 0: module " + agblib::agb::Hex(kRomBase + cart::kSpare));
    Module module;
    SEQLIB_CHECK(ReadModule(rom, kRomBase + cart::kSpare, module));
    info.modules.push_back(module);
    info.songs.push_back(Song{0, 0, kFullVolume, false});

    return info;
}

// The class gives the player's name and detection's lines, and a song's address is its module's. The trace's first
// frame gives the player's place after row 0: order 0, row 1, speed 6, the music's volume 256 and the step 256. The
// next row clears the step. Then the trace gives channel 0's record. The other channels stay at zero. The trace leaves
// them out. The listing of songs isn't written for this player.
void TestSongs()
{
    const Rom rom = OneNote();
    const std::unique_ptr<Music> music = MakeMusic(rom, Describe(rom));
    std::string error;

    const SongReport report = music->InspectSong(0, ConvertSettings());
    const TraceResult trace = TraceText(*music, 0, 1);
    const bool listed = music->DumpSong(0, "unused.txt", error);

    SEQLIB_REQUIRE_EQ(music->Log().size(), size_t(2));
    SEQLIB_CHECK(music->Log().front() == "Frontline Studios' MOD player");
    SEQLIB_CHECK_EQ(music->SongCount(), 1);
    SEQLIB_CHECK_EQ(report.address, kRomBase + cart::kSpare);
    SEQLIB_CHECK_EQ(report.tracks, 1);
    SEQLIB_CHECK(trace.traced && trace.warnings.empty());
    SEQLIB_CHECK(trace.text.starts_with("0 g 000001000600010001\n0 c0 02000000"));
    SEQLIB_CHECK(trace.text.find(" c1 ") == std::string::npos);
    SEQLIB_CHECK(!listed);
    SEQLIB_CHECK(error == "no listing of this driver's songs");
}

} // namespace

void RunTests()
{
    TestSongs();
}

} // namespace agbsf2::frontline_mod
