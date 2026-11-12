// SPDX-License-Identifier: MIT

// Unit tests for shared code. Each driver's tests are in a separate file.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::test
{
namespace
{

// A game with none of the drivers gets an error that names the drivers looked for, from OpenMusic() and OpenAllMusic()
// alike.
void TestNoDriver()
{
    Rom rom;
    rom.Assign(std::vector<uint8_t>(0x1000, 0));
    Overrides konami_only, rare_only, quintet_only, rd2_only, mp2k_only, brownie_only, krawall_only, ubimilan_only,
        rd1_only, alphadream_only;
    konami_only.driver = Driver::kKonami;
    rare_only.driver = Driver::kRare;
    quintet_only.driver = Driver::kQuintet;
    rd2_only.driver = Driver::kRd2;
    mp2k_only.driver = Driver::kMp2k;
    brownie_only.driver = Driver::kBrownie;
    krawall_only.driver = Driver::kKrawall;
    ubimilan_only.driver = Driver::kUbiMilan;
    rd1_only.driver = Driver::kRd1;
    alphadream_only.driver = Driver::kAlphaDream;
    std::string any, konami, rare, quintet, rd2, mp2k, brownie, krawall, ubimilan, rd1, alphadream, all, all_mp2k;

    const bool found_any = OpenMusic(rom, Overrides(), any) != nullptr;
    const bool found_konami = OpenMusic(rom, konami_only, konami) != nullptr;
    const bool found_rare = OpenMusic(rom, rare_only, rare) != nullptr;
    const bool found_quintet = OpenMusic(rom, quintet_only, quintet) != nullptr;
    const bool found_rd2 = OpenMusic(rom, rd2_only, rd2) != nullptr;
    const bool found_mp2k = OpenMusic(rom, mp2k_only, mp2k) != nullptr;
    const bool found_brownie = OpenMusic(rom, brownie_only, brownie) != nullptr;
    const bool found_krawall = OpenMusic(rom, krawall_only, krawall) != nullptr;
    const bool found_ubimilan = OpenMusic(rom, ubimilan_only, ubimilan) != nullptr;
    const bool found_rd1 = OpenMusic(rom, rd1_only, rd1) != nullptr;
    const bool found_alphadream = OpenMusic(rom, alphadream_only, alphadream) != nullptr;
    std::vector<std::string> unread, unread_mp2k;
    const bool found_all = !OpenAllMusic(rom, Overrides(), all, unread).empty();
    const bool found_all_mp2k = !OpenAllMusic(rom, mp2k_only, all_mp2k, unread_mp2k).empty();

    SEQLIB_CHECK(!found_any && !found_konami && !found_rare && !found_quintet && !found_rd2 && !found_mp2k &&
                 !found_brownie && !found_krawall && !found_ubimilan && !found_rd1 && !found_alphadream);
    SEQLIB_CHECK(!found_all && !found_all_mp2k && unread.empty() && unread_mp2k.empty());
    SEQLIB_CHECK(all == any && all_mp2k == mp2k);
    SEQLIB_CHECK(
        any ==
        "no Konami, Rare, Quintet, Nintendo R&D1, Nintendo R&D2, AlphaDream, Brownie Brown, Ubisoft Milan, Krawall "
        "or MP2K sound driver found: "
        "this game's music uses another engine, or a driver version that agbsf2 doesn't support");
    SEQLIB_CHECK(konami ==
                 "no Konami sound driver found: this game's music uses another engine, or a driver version "
                 "that agbsf2 doesn't support");
    SEQLIB_CHECK(rare == "no Rare sound driver found (try --song-table)");
    SEQLIB_CHECK(quintet ==
                 "no Quintet sound driver found: this game's music uses another engine, or a driver version "
                 "that agbsf2 doesn't support");
    SEQLIB_CHECK(rd2 ==
                 "no Nintendo R&D2 sound driver found: this game's music uses another engine, or a driver "
                 "version that agbsf2 doesn't support");
    SEQLIB_CHECK(mp2k == "no MP2K sound driver found (try --song-table)");
    SEQLIB_CHECK(brownie ==
                 "no Brownie Brown sound driver found: this game's music uses another engine, or a driver "
                 "version that agbsf2 doesn't support");
    SEQLIB_CHECK(krawall ==
                 "no Krawall sound driver found: this game's music uses another engine, or a driver version "
                 "that agbsf2 doesn't support");
    SEQLIB_CHECK(ubimilan ==
                 "no Ubisoft Milan sound driver found: this game's music uses another engine, or a driver "
                 "version that agbsf2 doesn't support");
    SEQLIB_CHECK(rd1 ==
                 "no Nintendo R&D1 sound driver found: this game's music uses another engine, or a driver "
                 "version that agbsf2 doesn't support");
    SEQLIB_CHECK(alphadream ==
                 "no AlphaDream sound driver found: this game's music uses another engine, or a driver "
                 "version that agbsf2 doesn't support");
}

// Runs every test. Returns the exit code.
int Run()
{
    TestNoDriver();
    konami::RunTests();
    rare::RunTests();
    quintet::RunTests();
    mp2k::RunTests();
    rd2::RunTests();
    brownie::RunTests();
    krawall::RunTests();
    ubimilan::RunTests();

    std::printf("%d checks, %d failures\n", seqlib::testing::checks, seqlib::testing::failures);

    return seqlib::testing::failures ? 1 : 0;
}

} // namespace
} // namespace agbsf2::test

int main()
{
    return agbsf2::test::Run();
}
