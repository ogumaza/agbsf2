// SPDX-License-Identifier: MIT

// Unit tests of the Ubisoft Milan Music class. The tests in ntdlib check the driver. These tests build synthetic
// cartridges in memory. No game ROM is needed.

#include <memory>
#include <string>

#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/ubimilan_cart.h"
#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::ubimilan
{
namespace
{

using ntdlib::testing::ubimilan_cart::Cart;
using ntdlib::testing::ubimilan_cart::kOnceId;
using ntdlib::testing::ubimilan_cart::OlderBuildCart;

// The Sum of All Fears and Rogue Spear have an older build of the driver. The bank routine of that build copies two
// tables, not three. OpenMusic() returns the error from detection. The error says that the music is recorded samples.
void TestOlderBuild()
{
    const ntdlib::testing::agb_files::CartImage cart = OlderBuildCart();
    std::string error;

    SEQLIB_CHECK(!agbsf2::OpenMusic(cart.ToRom(), Overrides(), error));
    SEQLIB_CHECK(error ==
                 "Found an older build of Ubisoft Milan's sound driver. The music is recorded samples, with no "
                 "sequences to convert.");
}

// The driver comes through OpenMusic with its name, and lists the pieces of music.
void TestMusic()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(music->Log().front() == "Ubisoft Milan's sound driver");
    SEQLIB_CHECK_EQ(music->SongCount(), 3);
    SEQLIB_CHECK_EQ(music->InspectSong(0, ConvertSettings()).address, cart.Resource(kOnceId));
}

} // namespace

void RunTests()
{
    TestOlderBuild();
    TestMusic();
}

} // namespace agbsf2::ubimilan
