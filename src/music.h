// SPDX-License-Identifier: MIT

// A game's music as its sound driver plays it: the interface between the command line and the code for each driver.

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/agb/song_plan.h"
#include "ntdlib/sequence_export/agb/settings.h"

#include "program.h"
#include "rom.h"

namespace agbsf2
{

// The sound drivers that agbsf2 reads.
enum class Driver
{
    kAny, // whichever the game has
    kKonami,
    kRare,
    kQuintet,
    kRd2,
    kMp2k,
    kBrownie,
    kKrawall,
    kUbiMilan,
    kRd1,
    kAlphaDream,
    kLogikState,
};

// Settings that override detection, from the command line. A value of 0, or kAny for the driver, leaves it to
// detection.
struct Overrides
{
    Driver driver = Driver::kAny;
    uint32_t song_table = 0;
    int song_count = 0;
    uint32_t sample_table = 0; // Konami's driver only
    double mix_rate = 0;       // Konami's driver only
    int music_volume = 0;      // Logik State's driver only: 1 to 64, or 0 for the volume that the game sets
};

// The settings for converting songs. The files' comments name agbsf2.
struct ConvertSettings : ntdlib::sequence_export::agb::ConvertSettings
{
    ConvertSettings()
    {
        program = kProgramName;
    }
};

using ntdlib::sequence_export::agb::SongReport;
using ntdlib::sequence_export::agb::SongVersion;

// Access to a game's songs through its sound driver. Each driver's class passes the driver's name and what detection
// found to the constructor.
class Music
{
public:
    virtual ~Music() = default;

    // Returns what detection found, as --info prints it, starting with the driver's name.
    const std::vector<std::string>& Log() const;

    // Returns the problems that detection found or the assumptions it made.
    const std::vector<std::string>& Warnings() const;

    // Returns the number of entries in the game's song table.
    virtual int SongCount() const = 0;

    // Returns false if entry `song` of the song table is empty, so there's no song to convert.
    virtual bool HasSong(int song) const = 0;

    // Returns the versions of a song that ConvertSong() writes, each to separate files. The first is the song as the
    // game starts it. It has no name or suffix. Only the drivers of Rare, Nintendo R&D2, Konami and Ubisoft Milan
    // return more. Donkey Kong Country 3 plays Enchanted Riverbank above water and under water. Super Mario Advance 2
    // mutes one track of a sequence for one music number and another track for another. That game also plays a track of
    // four sequences only while the player rides Yoshi. Two Mermaid Melody games play some songs both as a request
    // leaves them and in a live stage. The live stage changes some of their tracks. The Mummy starts most of its pieces
    // with the kit on channel 9 switched off. Each of them has a second version with the kit.
    virtual std::vector<SongVersion> Versions(int song) const;

    // Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
    virtual SongReport InspectSong(int song, const ConvertSettings& settings) const = 0;

    // Converts a song to a MIDI file and a SoundFont. After ShareSoundfont(), its instruments go in the shared
    // SoundFont instead of one of its own.
    virtual SongReport ConvertSong(int song, const ConvertSettings& settings) = 0;

    // Returns true if ConvertSong() can give each of the driver's sound channels a MIDI channel for
    // ConvertSettings::voice_channels. Only MP2K's conversion can.
    virtual bool SupportsVoiceChannels() const;

    // Returns true if the SoundFont can give the PSG voices an exclusive class for ConvertSettings::psg_exclusive. Only
    // MP2K's conversion can.
    virtual bool SupportsPsgExclusive() const;

    // Starts a shared SoundFont for all subsequent calls to ConvertSong() with `settings`.
    virtual void ShareSoundfont(const ConvertSettings& settings) = 0;

    // Writes the shared SoundFont. Returns false and sets `error` if it can't be written.
    virtual bool WriteSharedSoundfont(const std::string& path, std::string& error) = 0;

    // Writes a text listing of every command of a song. Returns false and sets `error` if it can't.
    virtual bool DumpSong(int song, const std::string& path, std::string& error) const = 0;

    // Returns a song's number as its files' names give it: two digits, or more for a song from 100 on. A driver can
    // give every number the width of its largest song number.
    virtual std::string FileNumber(int song) const;

    // Writes the state of the driver model to `out` after each of the first `frames` frames of a song, in the format of
    // the driver's tools/*/driver_emu.py trace, and adds the song's problems to `warnings`. Returns false if the song
    // can't be played.
    virtual bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const = 0;

protected:
    // Starts the log with `name`, the driver's name. `log`, what detection found, comes after it. `warnings` are
    // detection's warnings.
    Music(const std::string& name, const std::vector<std::string>& log, std::vector<std::string> warnings);

private:
    std::vector<std::string> log_;
    std::vector<std::string> warnings_;
};

// A sound driver that a game has, as OpenAllMusic() finds it.
struct FoundMusic
{
    Driver driver = Driver::kAny;
    std::unique_ptr<Music> music;
};

// Returns the name that --driver takes for a driver, such as "mp2k", or an empty one for kAny.
const char* DriverName(Driver driver);

// Returns the driver that --driver calls `name`, or kAny if no driver has that name.
Driver DriverNamed(const std::string& name);

// Finds the sound driver of the game in `rom`, which has to outlive the result, and reads its tables. Returns null and
// sets `error` if the game has none of the drivers, or the tables of the first one it finds can't be read.
std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

// Finds every sound driver that `overrides` allows in the game in `rom`, and reads their tables. A game can have more
// than one driver. `rom` has to outlive the results. The results come in the order OpenMusic() looks for the drivers. A
// driver whose tables can't be read is left out, and `unread` gets the error. Returns none and sets `error` if the game
// has none of the drivers, or if no driver's tables can be read, to the first driver's error.
std::vector<FoundMusic> OpenAllMusic(const Rom& rom, const Overrides& overrides, std::string& error,
                                     std::vector<std::string>& unread);

using ntdlib::sequence_export::agb::SongNumber;
using ntdlib::sequence_export::agb::SongStem;

// Each driver's part of OpenMusic(), which looks for that driver only. It returns null and sets `error` if the driver's
// tables can't be read, or returns null and leaves `error` empty if the game shows no sign of the driver.
namespace konami
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace konami

namespace rare
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace rare

namespace quintet
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace quintet

namespace rd2
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace rd2

namespace mp2k
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace mp2k

namespace brownie
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace brownie

namespace krawall
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace krawall

namespace ubimilan
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace ubimilan

namespace rd1
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace rd1

namespace alphadream
{

std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

} // namespace alphadream

namespace logikstate
{
std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);
} // namespace logikstate
} // namespace agbsf2
