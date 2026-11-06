// SPDX-License-Identifier: MIT

#include "music.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "program.h"
#include "rom.h"

namespace agbsf2
{
namespace
{

using Open = std::unique_ptr<Music> (*)(const Rom&, const Overrides&, std::string&);

// The drivers in the order detection looks for them, with the names that --driver takes. Rare's, Quintet's, Nintendo
// R&D2's, Nintendo R&D1's, AlphaDream's, Brownie Brown's and Ubisoft Milan's drivers, MP2K and Krawall come first.
// Their detection uses code alone. Konami's detection can fall back on a scan of the data. A scan might mistake another
// driver's data for a song table.
// R&D2's comes before MP2K, since a game can play most of its music with R&D2's driver and the rest with MP2K.
// AlphaDream's comes before MP2K too. Mario & Luigi: Superstar Saga plays its music with it, and MP2K plays only the
// Mario Bros. Classic that the game holds.
const struct
{
    Driver driver;
    const char* name;
    Open open;
} kDrivers[] = {
    {Driver::kRare, "rare", rare::OpenMusic},
    {Driver::kQuintet, "quintet", quintet::OpenMusic},
    {Driver::kRd2, "rd2", rd2::OpenMusic},
    {Driver::kRd1, "rd1", rd1::OpenMusic},
    {Driver::kAlphaDream, "alphadream", alphadream::OpenMusic},
    {Driver::kMp2k, "mp2k", mp2k::OpenMusic},
    {Driver::kBrownie, "brownie", brownie::OpenMusic},
    {Driver::kKrawall, "krawall", krawall::OpenMusic},
    {Driver::kUbiMilan, "ubimilan", ubimilan::OpenMusic},
    {Driver::kKonami, "konami", konami::OpenMusic},
};

// Returns the error for a game in which detection finds none of the drivers that `driver` allows. Rare's driver and
// MP2K can read the songs that --song-table gives without the driver's code, so their errors suggest it. Detection of
// Quintet's, Nintendo R&D1's, Nintendo R&D2's, AlphaDream's, Brownie Brown's and Ubisoft Milan's drivers and Krawall
// finds their other tables through the driver's code.
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
    case Driver::kRd1:
        return "no Nintendo R&D1 sound driver found" + kUnknown;
    case Driver::kAlphaDream:
        return "no AlphaDream sound driver found" + kUnknown;
    default:
        return "no Konami, Rare, Quintet, Nintendo R&D1, Nintendo R&D2, AlphaDream, Brownie Brown, Ubisoft Milan, "
               "Krawall or MP2K sound driver found" +
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

        // MP2K's detection takes the play routine of Nintendo R&D1's driver for a song start. Those games have no MP2K.
        const auto is_rd1 = [](const FoundMusic& m)
        {
            return m.driver == Driver::kRd1;
        };
        if (d.driver == Driver::kMp2k && std::any_of(found.begin(), found.end(), is_rd1))
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

const char* DriverName(Driver driver)
{
    for (const auto& d : kDrivers)
    {
        if (d.driver == driver)
        {
            return d.name;
        }
    }

    return "";
}

Driver DriverNamed(const std::string& name)
{
    for (const auto& d : kDrivers)
    {
        if (name == d.name)
        {
            return d.driver;
        }
    }

    return Driver::kAny;
}

Music::Music(const std::string& name, const std::vector<std::string>& log, std::vector<std::string> warnings)
    : warnings_(std::move(warnings))
{
    log_.push_back(name);
    log_.insert(log_.end(), log.begin(), log.end());
}

const std::vector<std::string>& Music::Log() const
{
    return log_;
}

const std::vector<std::string>& Music::Warnings() const
{
    return warnings_;
}

std::vector<SongVersion> Music::Versions(int) const
{
    return {SongVersion()};
}

bool Music::SupportsVoiceChannels() const
{
    return false;
}

bool Music::SupportsPsgExclusive() const
{
    return false;
}

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

} // namespace agbsf2
