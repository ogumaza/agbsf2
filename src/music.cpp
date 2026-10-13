// SPDX-License-Identifier: MIT

#include "music.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "program.h"

namespace agbsf2
{
namespace
{

using Open = std::unique_ptr<Music> (*)(const Rom&, const Overrides&, std::string&);

// The drivers in the order detection looks for them. Rare's, Quintet's, Nintendo R&D2's, Brownie Brown's and Ubisoft
// Milan's drivers, MP2K and Krawall come first, since their detection uses code alone, where Konami's can fall back on
// a data scan, which might mistake another driver's data for a song table. R&D2's comes before MP2K, since a game can
// play most of its music with R&D2's driver and the rest with MP2K.
const struct
{
    Driver driver;
    Open open;
} kDrivers[] = {
    {Driver::kRare, rare::OpenMusic},         {Driver::kQuintet, quintet::OpenMusic},
    {Driver::kRd2, rd2::OpenMusic},           {Driver::kMp2k, mp2k::OpenMusic},
    {Driver::kBrownie, brownie::OpenMusic},   {Driver::kKrawall, krawall::OpenMusic},
    {Driver::kUbiMilan, ubimilan::OpenMusic}, {Driver::kKonami, konami::OpenMusic},
};

// Returns the error for a game in which detection finds none of the drivers that `driver` allows. Rare's driver and
// MP2K can read the songs that --song-table gives without the driver's code, so their errors suggest it. Quintet's,
// Nintendo R&D2's, Brownie Brown's and Ubisoft Milan's drivers and Krawall need their code to find their other tables.
std::string NotFound(Driver driver)
{
    const std::string kUnknown = std::string(": this game's music uses another engine, or a driver version that ") +
                                 kProgramName + " doesn't support";
    switch (driver)
    {
    case Driver::kKonami:
        return "no Konami sound driver found" + kUnknown;
    case Driver::kRare:
        return "no Rare sound driver found (try --song-table)";
    case Driver::kQuintet:
        return "no Quintet sound driver found" + kUnknown;
    case Driver::kRd2:
        return "no Nintendo R&D2 sound driver found" + kUnknown;
    case Driver::kMp2k:
        return "no MP2K sound driver found (try --song-table)";
    case Driver::kBrownie:
        return "no Brownie Brown sound driver found" + kUnknown;
    case Driver::kKrawall:
        return "no Krawall sound driver found" + kUnknown;
    case Driver::kUbiMilan:
        return "no Ubisoft Milan sound driver found" + kUnknown;
    default:
        return "no Konami, Rare, Quintet, Nintendo R&D2, Brownie Brown, Ubisoft Milan, Krawall or MP2K sound driver "
               "found" +
               kUnknown;
    }
}

// Finds the drivers as OpenAllMusic() does, and gives `unread` the errors of those whose tables can't be read. Unless
// `all` is true, it stops at the first driver that it finds or that it can't read.
std::vector<FoundMusic> FindMusic(const Rom& rom, const Overrides& overrides, bool all, std::string& error,
                                  std::vector<std::string>& unread)
{
    error.clear();
    std::vector<FoundMusic> found;
    for (const auto& d : kDrivers)
    {
        if (overrides.driver != Driver::kAny && overrides.driver != d.driver)
        {
            continue;
        }

        std::string driver_error;
        std::unique_ptr<Music> music = d.open(rom, overrides, driver_error);
        if (music)
        {
            found.push_back({d.driver, std::move(music)});
        }
        else if (!driver_error.empty())
        {
            unread.push_back(driver_error);
        }

        if (!all && (!found.empty() || !unread.empty()))
        {
            break;
        }
    }

    if (found.empty())
    {
        error = unread.empty() ? NotFound(overrides.driver) : unread.front();
    }

    return found;
}

} // namespace

std::string Music::FileNumber(int song) const
{
    return SongNumber(song, 0);
}

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error)
{
    std::vector<std::string> unread;
    std::vector<FoundMusic> found = FindMusic(rom, overrides, false, error, unread);
    return found.empty() ? nullptr : std::move(found.front().music);
}

std::vector<FoundMusic> OpenAllMusic(const Rom& rom, const Overrides& overrides, std::string& error,
                                     std::vector<std::string>& unread)
{
    return FindMusic(rom, overrides, true, error, unread);
}

std::string SongNumber(int song, int count)
{
    int digits = 2;
    for (int n = count - 1; n >= 100; n /= 10)
    {
        digits++;
    }

    // Padded by hand: GCC can't tell the field width from `digits` and warns about snprintf.
    const std::string number = std::to_string(song);
    return std::string(size_t(std::max(0, digits - int(number.size()))), '0') + number;
}

uint64_t LoopLength(const std::vector<uint64_t>& lengths)
{
    const uint64_t longest = *std::max_element(lengths.begin(), lengths.end());
    const uint64_t most = longest * kMaxLoopFactor;
    uint64_t length = 1;
    for (uint64_t l : lengths)
    {
        const uint64_t times = length / std::gcd(length, l);
        if (times > most / l)
        {
            return longest;
        }

        length = times * l;
    }

    return length;
}

PassComparison::PassComparison(uint64_t start, uint64_t length) : start_(start), length_(length)
{
}

void PassComparison::Add(uint64_t on, int channel, int key, std::vector<double> settings)
{
    if (on < start_ || length_ == 0)
    {
        return;
    }

    const uint64_t pass = (on - start_) / length_;
    if (pass < 2)
    {
        passes_[pass].push_back({on - start_ - pass * length_, channel, key, std::move(settings)});
    }
}

bool PassComparison::Alike() const
{
    std::array<std::vector<Start>, 2> sorted = passes_;
    for (std::vector<Start>& pass : sorted)
    {
        std::sort(pass.begin(), pass.end());
    }

    return sorted[0] == sorted[1];
}

bool PassComparison::Start::operator<(const Start& other) const
{
    return std::tie(at, channel, key, settings) < std::tie(other.at, other.channel, other.key, other.settings);
}

bool PassComparison::Start::operator==(const Start& other) const
{
    return std::tie(at, channel, key, settings) == std::tie(other.at, other.channel, other.key, other.settings);
}

} // namespace agbsf2
