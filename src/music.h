// SPDX-License-Identifier: MIT

// A game's music as its sound driver plays it: the interface between the command line and the code for each driver.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

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
};

// Settings that override detection, from the command line. 0 means detect.
struct Overrides
{
    Driver driver = Driver::kAny;
    uint32_t song_table = 0;
    int song_count = 0;
    uint32_t sample_table = 0; // Konami's driver only
    double mix_rate = 0;       // Konami's driver only
};

// The settings for converting songs.
struct ConvertSettings
{
    int loops = 2;                    // times a looping song's loop is played
    uint32_t track_mask = 0xFFFFFFFF; // tracks to include (bit t = track t; Krawall has 20, the others 16 at most)
    bool voice_channels = false;      // a MIDI channel for each of the driver's sound channels, where it can
    bool frame_timing = false;        // each event on the frame the driver plays it in, rather than on the beat
    std::string out_dir = ".";
    std::string base_name = "song";
};

// A song's length, loop points, track count and conversion result.
struct SongReport
{
    bool ok = false;        // converted, or skipped for playing no notes
    bool silent = false;    // no notes on the chosen tracks, so nothing was written
    uint32_t address = 0;   // the song's data in the ROM: its header, or a Konami song's sequence data
    double seconds = 0;     // duration including the requested number of loop passes
    double loop_start = -1; // seconds; -1 if the song doesn't loop
    double loop_end = -1;
    int tracks = 0; // chosen tracks that play notes
    double bpm = 0;
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Access to a game's songs through its sound driver.
class Music
{
public:
    virtual ~Music() = default;

    // Returns what detection found, as --info prints it, starting with the driver's name.
    virtual const std::vector<std::string>& Log() const = 0;

    // Returns the problems that detection found or the assumptions it made.
    virtual const std::vector<std::string>& Warnings() const = 0;

    // Returns the number of entries in the game's song table.
    virtual int SongCount() const = 0;

    // Returns false if entry `song` of the song table is empty, so there's no song to convert.
    virtual bool HasSong(int song) const = 0;

    // Works out a song's length, loop and tracks the way ConvertSong() does, without writing anything.
    virtual SongReport InspectSong(int song, const ConvertSettings& settings) const = 0;

    // Converts a song to a MIDI file and a SoundFont. After ShareSoundfont(), its instruments go in the shared
    // SoundFont instead of one of its own.
    virtual SongReport ConvertSong(int song, const ConvertSettings& settings) = 0;

    // Returns true if ConvertSong() can give each of the driver's sound channels a MIDI channel, as
    // ConvertSettings::voice_channels asks.
    virtual bool SupportsVoiceChannels() const = 0;

    // Starts a shared SoundFont for all subsequent calls to ConvertSong().
    virtual void ShareSoundfont() = 0;

    // Writes the shared SoundFont. Returns false and sets `error` if it can't be written.
    virtual bool WriteSharedSoundfont(const std::string& path, std::string& error) = 0;

    // Writes a text listing of every command of a song. Returns false and sets `error` if it can't.
    virtual bool DumpSong(int song, const std::string& path, std::string& error) const = 0;

    // Returns a song's number as its files' names give it: two digits, or more for a song from 100 on.
    virtual std::string FileNumber(int song) const;

    // Writes the state of the driver model to `out` after each of the first `frames` frames of a song, in the format of
    // the driver's tools/*/driver_emu.py trace, and adds the song's problems to `warnings`. Returns false if the song
    // can't be played.
    virtual bool Trace(int song, long long frames, std::FILE* out, std::vector<std::string>& warnings) const = 0;
};

// A sound driver that a game has, as OpenAllMusic() finds it.
struct FoundMusic
{
    Driver driver = Driver::kAny;
    std::unique_ptr<Music> music;
};

// Finds the sound driver of the game in `rom`, which has to outlive the result, and reads its tables. Returns null and
// sets `error` if the game has none of the drivers, or the tables of the first one it finds can't be read.
std::unique_ptr<Music> OpenMusic(const Rom& rom, const Overrides& overrides, std::string& error);

// Finds every sound driver that `overrides` allows in the game in `rom`, and reads their tables. A game can have more
// than one driver. `rom` has to outlive the results. The results come in the order OpenMusic() looks for the drivers.
// A driver whose tables can't be read is left out, and `unread` gets the error. Returns none and sets `error` if the
// game has none of the drivers, or if no driver's tables can be read, to the first driver's error.
std::vector<FoundMusic> OpenAllMusic(const Rom& rom, const Overrides& overrides, std::string& error,
                                     std::vector<std::string>& unread);

// Returns a song's number for file names and titles: at least two digits, and as many as the largest of `count` songs'.
std::string SongNumber(int song, int count);

// The most times as long as the longest of its tracks' loops that a song's loop can be.
constexpr uint64_t kMaxLoopFactor = 8;

// Returns the length of a song's loop from its looping tracks' loop lengths, which are above 0: the shortest length
// that each of them divides, so that every track is back where its loop started, or the longest of them if that would
// be more than kMaxLoopFactor times as long.
uint64_t LoopLength(const std::vector<uint64_t>& lengths);

// Collects the notes that start in a song's first two passes through its loop, to tell whether a player that loops on
// the markers can repeat the first. A driver can carry a setting, such as a pan, a level, a program or the tempo, from
// the loop's end into the next pass, so that the first pass, which comes from the song's start, plays unlike the later
// ones. The marked loop then starts at the second pass.
class PassComparison
{
public:
    // The first pass runs from `start` for `length` of the driver's units of time, and the second for another `length`.
    PassComparison(uint64_t start, uint64_t length);

    // Adds a note that starts at `on` on channel or track `channel`, with its key and the settings it starts with, in
    // whatever form the driver has them. A note that starts outside the two passes is left out.
    void Add(uint64_t on, int channel, int key, std::vector<double> settings);

    // Returns true if the two passes start the same notes at the same times from their starts, with the same settings.
    bool Alike() const;

private:
    struct Start
    {
        bool operator<(const Start& other) const;
        bool operator==(const Start& other) const;

        uint64_t at = 0;
        int channel = 0;
        int key = 0;
        std::vector<double> settings;
    };

    uint64_t start_;
    uint64_t length_;
    std::array<std::vector<Start>, 2> passes_;
};

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
} // namespace agbsf2
