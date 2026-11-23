// SPDX-License-Identifier: MIT

// Unit tests of the Logik State Music class. The tests in agblib check the driver. These tests build synthetic
// cartridges in memory. No game ROM is needed.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agblib/testing/logikstate_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::logikstate
{
namespace
{

using agblib::testing::logikstate_cart::Cart;
using agblib::testing::logikstate_cart::kBank;
using agblib::testing::logikstate_cart::TestModule;
using agblib::testing::logikstate_cart::TestRow;

// Returns whether `music` has `line` among what detection found.
bool Logs(const Music& music, const std::string& line)
{
    return std::find(music.Log().begin(), music.Log().end(), line) != music.Log().end();
}

// The driver comes through OpenMusic with its name, at the music volume that the game sets. The driver subtracts the
// difference between that volume and 64 from each channel's volume. A channel at volume 16 is therefore silent at the
// game's volume of 48. --music-volume replaces the game's volume, and the channel plays at 64.
void TestMusicVolume()
{
    Cart cart({std::vector<int8_t>(400, 40), std::vector<int8_t>(400, 40)});
    TestModule module;
    module.channels = 4;
    module.patterns = {std::vector<TestRow>(64)};
    module.samples[0] = {400, 0, 64, 0, 0};
    module.samples[1] = {400, 0, 16, 0, 0};
    module.patterns[0][0] = {{200, 1, 0, 0}, {200, 2, 0, 0}};
    cart.AddCall(cart.AddModule(module), kBank, 0);
    cart.AddMusicVolume(48);
    const Rom rom = cart.ToRom();
    Overrides full;
    full.music_volume = 64;
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> loud = agbsf2::OpenMusic(rom, full, error);

    SEQLIB_REQUIRE(music != nullptr && loud != nullptr);
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(music->Log().front() == "Logik State's sound driver");
    SEQLIB_CHECK(!Logs(*music, "music volume 64 of 64, from --music-volume"));
    SEQLIB_CHECK(Logs(*loud, "music volume 64 of 64, from --music-volume"));
    SEQLIB_CHECK_EQ(music->InspectSong(0, ConvertSettings()).tracks, 1);
    SEQLIB_CHECK_EQ(loud->InspectSong(0, ConvertSettings()).tracks, 2);
}

// The builds for GBAMOD2. modules take a music volume up to 64. --music-volume above that is an error. In a release
// whose volume detection can't read, --music-volume also replaces detection's warning about it.
void TestMusicVolumeTop()
{
    Cart cart({std::vector<int8_t>(400, 40)});
    TestModule module;
    module.channels = 4;
    module.patterns = {std::vector<TestRow>(64)};
    module.samples[0] = {400, 0, 64, 0, 0};
    module.patterns[0][0] = {{200, 1, 0, 0}};
    cart.AddCall(cart.AddModule(module), kBank, 0);
    cart.UseLongRecords();
    const Rom rom = cart.ToRom();
    Overrides set;
    set.music_volume = 40;
    Overrides over = set;
    over.music_volume = 65;
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> quiet = agbsf2::OpenMusic(rom, set, error);
    SEQLIB_REQUIRE(music != nullptr && quiet != nullptr);
    SEQLIB_CHECK(error.empty());
    SEQLIB_REQUIRE(music->Warnings().size() == 1);
    SEQLIB_CHECK(music->Warnings()[0].starts_with("no music volume for this release"));
    SEQLIB_CHECK(quiet->Warnings().empty());
    SEQLIB_CHECK(Logs(*quiet, "music volume 40 of 64, from --music-volume"));

    SEQLIB_CHECK(agbsf2::OpenMusic(rom, over, error) == nullptr);
    SEQLIB_CHECK(error == "--music-volume needs a volume from 1 to 64 for this game's modules");
}

} // namespace

void RunTests()
{
    TestMusicVolume();
    TestMusicVolumeTop();
}

} // namespace agbsf2::logikstate
