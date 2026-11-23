// SPDX-License-Identifier: MIT

// Unit tests for shared code. Each driver's tests are in a separate file.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "seqlib/testing/check.h"

#include "driver_tests.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::test
{
namespace
{

// The end of the error for a driver that detection doesn't find, apart from Rare's driver and MP2K.
constexpr std::string_view kUnknown =
    ": this game's music uses another engine, or a driver version that agbsf2 doesn't support";

// A driver, the name that --driver takes for it, and the error that OpenMusic() gives for a game without it.
struct DriverCase
{
    Driver driver;
    const char* name;
    std::string error;
};

// Returns the case of each driver.
std::vector<DriverCase> DriverCases()
{
    const auto unknown = [](const char* start)
    {
        return std::string(start) + std::string(kUnknown);
    };

    return {
        {Driver::kKonami, "konami", unknown("no Konami sound driver found")},
        {Driver::kRare, "rare", "no Rare sound driver found (try --song-table)"},
        {Driver::kQuintet, "quintet", unknown("no Quintet sound driver found")},
        {Driver::kRd2, "rd2", unknown("no Nintendo R&D2 sound driver found")},
        {Driver::kMp2k, "mp2k", "no MP2K sound driver found (try --song-table)"},
        {Driver::kBrownie, "brownie", unknown("no Brownie Brown sound driver found")},
        {Driver::kKrawall, "krawall", unknown("no Krawall sound driver found")},
        {Driver::kUbiMilan, "ubimilan", unknown("no Ubisoft Milan sound driver found")},
        {Driver::kRd1, "rd1", unknown("no Nintendo R&D1 sound driver found")},
        {Driver::kAlphaDream, "alphadream", unknown("no AlphaDream sound driver found")},
        {Driver::kLogikState, "logikstate", unknown("no Logik State sound driver found")},
        {Driver::kWebfoot, "webfoot", unknown("no Webfoot Technologies sound driver found")},
        {Driver::kFrontlineMod, "frontline_mod", unknown("no Frontline Studios MOD player found")},
        {Driver::kFrontlineXm, "frontline_xm", unknown("no Frontline Studios XM-like player found")},
    };
}

// A game with none of the drivers gets an error that names the drivers looked for, from OpenMusic() and OpenAllMusic()
// alike. With --driver, the error names that driver.
void TestNoDriver()
{
    Rom rom;
    rom.Assign(std::vector<uint8_t>(0x1000, 0));
    Overrides mp2k_only;
    mp2k_only.driver = Driver::kMp2k;
    std::string any, all, all_mp2k;
    std::vector<std::string> unread, unread_mp2k;

    const bool found_any = OpenMusic(rom, Overrides(), any) != nullptr;
    const bool found_all = !OpenAllMusic(rom, Overrides(), all, unread).empty();
    const bool found_all_mp2k = !OpenAllMusic(rom, mp2k_only, all_mp2k, unread_mp2k).empty();

    SEQLIB_CHECK(!found_any && !found_all && !found_all_mp2k && unread.empty() && unread_mp2k.empty());
    SEQLIB_CHECK(all == any && all_mp2k == "no MP2K sound driver found (try --song-table)");
    SEQLIB_CHECK(any ==
                 "no Konami, Rare, Quintet, Nintendo R&D1, Nintendo R&D2, AlphaDream, Brownie Brown, Ubisoft Milan, "
                 "Logik State, Webfoot Technologies, Frontline Studios MOD or XM-like, Krawall or MP2K sound driver "
                 "found" +
                     std::string(kUnknown));
    for (const DriverCase& c : DriverCases())
    {
        Overrides only;
        only.driver = c.driver;
        std::string error;

        const bool found = OpenMusic(rom, only, error) != nullptr;

        SEQLIB_CHECK(!found);
        SEQLIB_CHECK(error == c.error);
    }
}

// --driver takes each driver's name, and DriverName() gives it back. Only Logik State's and Webfoot Technologies'
// drivers take --music-volume.
void TestDriverNames()
{
    for (const DriverCase& c : DriverCases())
    {
        SEQLIB_CHECK(std::string(DriverName(c.driver)) == c.name);
        SEQLIB_CHECK(DriverNamed(c.name) == c.driver);
        SEQLIB_CHECK(TakesMusicVolume(c.driver) == (c.driver == Driver::kLogikState || c.driver == Driver::kWebfoot));
    }
    SEQLIB_CHECK(std::string(DriverName(Driver::kAny)).empty());
    SEQLIB_CHECK(DriverNamed("") == Driver::kAny && DriverNamed("frontline") == Driver::kAny);
    SEQLIB_CHECK(!TakesMusicVolume(Driver::kAny));
}

// Runs every test. Returns the exit code.
int Run()
{
    TestNoDriver();
    TestDriverNames();
    konami::RunTests();
    rare::RunTests();
    quintet::RunTests();
    mp2k::RunTests();
    rd2::RunTests();
    brownie::RunTests();
    krawall::RunTests();
    ubimilan::RunTests();
    logikstate::RunTests();
    webfoot::RunTests();
    frontline_mod::RunTests();
    frontline_xm::RunTests();

    std::printf("%d checks, %d failures\n", seqlib::testing::checks, seqlib::testing::failures);

    return seqlib::testing::failures ? 1 : 0;
}

} // namespace
} // namespace agbsf2::test

namespace agbsf2
{

TraceResult TraceText(const Music& music, int song, long long frames)
{
    TraceResult result;
    std::FILE* out = std::tmpfile();
    SEQLIB_CHECK(out != nullptr);
    if (!out)
    {
        return result;
    }

    result.traced = music.Trace(song, frames, out, result.warnings);

    std::rewind(out);
    for (int c = std::fgetc(out); c != EOF; c = std::fgetc(out))
    {
        result.text += char(c);
    }
    std::fclose(out);

    return result;
}

} // namespace agbsf2

int main()
{
    return agbsf2::test::Run();
}
