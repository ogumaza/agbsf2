// SPDX-License-Identifier: MIT

// Unit tests of the Music class of Frontline Studios' XM-like player. The tests in agblib check the player and the
// conversion. Detection requires a game's code. These tests therefore give the class a description of synthetic data
// in memory. No game ROM is needed.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/testing/frontline_xm_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::frontline_xm
{
namespace
{

namespace cart = agblib::testing::frontline_xm_cart;

using agblib::agb::kRomBase;
using agblib::frontline_xm::DriverInfo;
using agblib::frontline_xm::Library;
using agblib::frontline_xm::Module;
using agblib::frontline_xm::ReadModule;

// A cartridge with the test data and a module at cart::kSpare: one order of 4 rows, and the triangle's note 48 on
// channel 0 at row 0. Sets `module` to the module's address.
Rom OneNote(uint32_t& module)
{
    cart::Data data(kRomBase);
    const cart::Cell e;
    std::vector<cart::Row> rows(4, cart::Row{e, e, e, e});
    rows[0][0] = {48, cart::kTriangle};
    module = kRomBase + data.PutModule(cart::kSpare, cart::kSpeed, {rows}, {0}) - cart::kHeaderSize;
    Rom rom;
    rom.Assign(data.Bytes());
    return rom;
}

// The player as detection would describe it for the data: the tables, the module and one song of its list.
DriverInfo Describe(const Rom& rom, uint32_t address)
{
    DriverInfo info;
    Library& lib = info.library;
    lib.steps = kRomBase + cart::kSteps;
    lib.periods = kRomBase + cart::kPeriods;
    lib.step_rates = kRomBase + cart::kStepRates;
    lib.frame_samples = kRomBase + cart::kFrameSamples;
    lib.rates = kRomBase + cart::kRates;
    lib.silence = kRomBase + cart::kSilence;
    lib.waves = {kRomBase + cart::kWaves, kRomBase + cart::kWaves + 256, kRomBase + cart::kWaves + 512};
    info.rate_index = cart::kRateIndex;
    info.log.push_back("song 0: module " + agblib::agb::Hex(address));
    Module module;
    SEQLIB_CHECK(ReadModule(rom, address, module));
    info.modules.push_back(module);
    info.songs.push_back({0, module.orders, module.last_order, cart::kFirstVolume});

    return info;
}

// Returns `value` as the trace prints a word: in hex, low byte first.
std::string WordHex(uint32_t value)
{
    char text[9];
    std::snprintf(text, sizeof text, "%02x%02x%02x%02x", value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF,
                  value >> 24);
    return text;
}

// The class gives the player's name and detection's lines, and a song's address is its module's. A frame mixes 336
// samples, and a tick at BPM 125 lasts 401. The trace's first frame gives the player's place after its first tick:
// order 0, row 0, tick 1, speed 3, BPM 125 and 65 samples to the next tick. The first row comes after 3 ticks, at
// sample 1,203, in frame 3, where channel 0's record starts with the triangle's record. The listing of songs isn't
// written for this player.
void TestSongs()
{
    uint32_t module = 0;
    const Rom rom = OneNote(module);
    const std::unique_ptr<Music> music = MakeMusic(rom, Describe(rom, module));
    std::string error;

    const SongReport report = music->InspectSong(0, ConvertSettings());
    const TraceResult trace = TraceText(*music, 0, 4);
    const bool listed = music->DumpSong(0, "unused.txt", error);

    const uint32_t triangle = rom.U32(rom.U32(module + agblib::frontline_xm::kHeaderInstruments) + 4 * cart::kTriangle);
    SEQLIB_REQUIRE_EQ(music->Log().size(), size_t(2));
    SEQLIB_CHECK(music->Log().front() == "Frontline Studios' XM-like player");
    SEQLIB_CHECK_EQ(music->SongCount(), 1);
    SEQLIB_CHECK_EQ(report.address, module);
    SEQLIB_CHECK_EQ(report.tracks, 1);
    SEQLIB_CHECK(trace.traced && trace.warnings.empty());
    SEQLIB_CHECK(trace.text.starts_with("0 g 000001037d4100\n"));
    SEQLIB_CHECK(trace.text.find("\n3 c0 " + WordHex(triangle)) != std::string::npos);
    SEQLIB_CHECK(!listed);
    SEQLIB_CHECK(error == "no listing of this driver's songs");
}

} // namespace

void RunTests()
{
    TestSongs();
}

} // namespace agbsf2::frontline_xm
