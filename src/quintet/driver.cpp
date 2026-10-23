// SPDX-License-Identifier: MIT

#include "quintet/driver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "quintet/song.h"
#include "rom.h"
#include "thumb.h"

namespace agbsf2::quintet
{
namespace
{

// The most songs and samples detection reads, so that unrelated ROM data can't keep it busy.
constexpr int kMaxSongs = 1024;
constexpr int kMaxSamples = 128;

// The code that tells the revisions apart, in Revision's order. Detection takes the first revision whose play routine
// it finds.
struct RevisionCode
{
    // The play routine. It resets the channels from its table of them, and in the J revision, the sound effects'
    // channels from theirs, with their banks.
    const char* play;

    // The routine that plays a sound effect. It skips each channel whose data starts with FF.
    const char* play_effect;

    // The code that loads the variable of the noise macros, and the halfword of its ldr. The routine that gives the
    // driver bank 0's macros stores them in the variable, and the one for bank 1 in the word after it.
    const char* macro_variable;
    int macro_ldr;

    // The routine that loads a sample file.
    const char* loader;

    // What the log adds after the play routine.
    const char* label;
};

constexpr RevisionCode kRevisionCode[2] = {
    {"B5F0 1C07 2400 4Exx 4Dxx CE01 Fxxx Fxxx CD01", "B570 49xx 6008 2500 4Exx 48xx 6801 0068 1840 8840 1844 7820 28FF",
     "0600 0E03 1C19 48xx 7800 2800 D0xx 48xx 6802", 7, "B570 1C02 48xx 2500 024C 237F", ""},
    {"B5F0 1C07 2500 4Exx 00AC CE01 2100 Fxxx Fxxx 48xx 1824 6820",
     "B570 49xx 6008 2500 48xx 6801 0068 1840 8840 1846 7830 28FF",
     "B510 0600 0E03 48xx 6802 1C14 3490 2000 6020 2B00 D0xx 49xx 7B52 0090 1840", 11,
     "B570 4Bxx 008A 18D2 1C03 6013 48xx 2200 024D 247F", ", J revision"},
};

// The stores in the routines that give the driver a table. The first puts the table in a variable: str r0, [r1]. The
// second puts it in the word after the variable: str r0, [r1, #4].
constexpr uint16_t kStoreInVariable = 0x6008;
constexpr uint16_t kStoreAfterVariable = 0x6048;

// Returns the literal that the ldr at halfword `index` of the first match of `pattern` loads, or 0 if there's no match.
uint32_t FindLiteral(const Rom& rom, const char* pattern, int index)
{
    const uint32_t at = FindFirstThumb(rom, pattern);
    return at ? ThumbLiteral(rom, at + 2 * uint32_t(index)) : 0;
}

// Returns the address that the game's lookup of a file gives, where the instruction before the bl at `call` is
// `movs r1, #file` and the one before that `movs r0, #archive`, or nothing if it isn't such a lookup.
std::optional<uint32_t> GameLookup(const Rom& rom, uint32_t call)
{
    const uint32_t lookup = BlTarget(rom, call);
    const uint16_t archive = rom.U16(call - 4);
    const uint16_t file = rom.U16(call - 2);
    if (!lookup || (archive & 0xFF00) != 0x2000 || (file & 0xFF00) != 0x2100)
    {
        return std::nullopt;
    }

    const std::optional<uint32_t> address = RunThumb(rom, lookup, {archive & 0xFFu, file & 0xFFu, 0, 0});
    if (!address || !rom.Contains(*address))
    {
        return std::nullopt;
    }

    return address;
}

// Returns the table that the game gives the driver through `setter`, from a call to the setter right after a call to
// the game's lookup, or 0 if no such call is found.
uint32_t TableFromSetter(const Rom& rom, uint32_t setter)
{
    if (!setter)
    {
        return 0;
    }

    for (uint32_t call : FindCalls(rom, setter))
    {
        if (const std::optional<uint32_t> table = GameLookup(rom, call - 4))
        {
            return *table;
        }
    }

    return 0;
}

// Finds the routine that stores r0 with `store` in the driver's variable `variable`: ldr r1, =variable; store; bx lr.
uint32_t FindSetter(const Rom& rom, uint32_t variable, uint16_t store)
{
    if (!variable)
    {
        return 0;
    }

    const std::vector<ThumbPattern> setter = {{0x4901, 0xFFFF}, {store, 0xFFFF}, {0x4770, 0xFFFF}};
    for (uint32_t at : FindThumb(rom, setter))
    {
        if (ThumbLiteral(rom, at) == variable)
        {
            return at;
        }
    }

    return 0;
}

// Finds the music file from the game's calls to the driver's play routine: the game looks the file up, skips to the
// song, and passes its address. Returns 0 if no call does so. The sound effects' file is found the same way.
uint32_t FindSongs(const Rom& rom, uint32_t play)
{
    if (!play)
    {
        return 0;
    }

    for (uint32_t call : FindCalls(rom, play))
    {
        // The lookup's bl comes a few instructions before, after the moves of its arguments.
        for (uint32_t back = 4; back <= 40; back += 2)
        {
            const std::optional<uint32_t> songs = GameLookup(rom, call - back);
            SongHeader header;
            if (songs && ReadSongHeader(rom, *songs, header))
            {
                return *songs;
            }
        }
    }

    return 0;
}

// Returns the samples in the sample file at `samples`, as the driver's routine that loads the file lists them: each
// sample starts with its size, and the file ends where a size of 0 follows a sample, or after 128 samples.
std::vector<uint32_t> ReadSampleList(const Rom& rom, uint32_t samples)
{
    std::vector<uint32_t> list;
    uint32_t at = samples;
    while (int(list.size()) < kMaxSamples && rom.Contains(at, 6))
    {
        list.push_back(at);
        at += rom.U32(at);
        if (rom.U32(at) == 0)
        {
            break;
        }
    }

    return list;
}

// Returns the game's bank 0 and bank 1 tables that it gives the driver by storing them in `variable` and the word after
// it: the noise macros' tables. The J revision's routine for bank 1 stores 4 bytes past the variable. The A revision's
// loads the address of the word after it. Either table is 0 if it isn't found.
std::array<uint32_t, 2> BankTables(const Rom& rom, uint32_t variable)
{
    uint32_t bank1 = TableFromSetter(rom, FindSetter(rom, variable, kStoreAfterVariable));
    if (!bank1 && variable)
    {
        bank1 = TableFromSetter(rom, FindSetter(rom, variable + 4, kStoreInVariable));
    }

    return {TableFromSetter(rom, FindSetter(rom, variable, kStoreInVariable)), bank1};
}

// Returns the game's sample files for banks 0 and 1, from the routines that pass each to the driver's sample loader
// with the bank in r1. Either is 0 if it isn't found.
std::array<uint32_t, 2> SampleFiles(const Rom& rom, uint32_t loader)
{
    std::array<uint32_t, 2> files = {};
    if (!loader)
    {
        return files;
    }

    for (int bank = 0; bank < 2; bank++)
    {
        for (uint32_t setter : FindThumb(
                 rom, ParseThumbPattern(bank ? "B500 2101 Fxxx Fxxx BC01 4700" : "B500 2100 Fxxx Fxxx BC01 4700")))
        {
            if (BlTarget(rom, setter + 4) == loader)
            {
                files[size_t(bank)] = TableFromSetter(rom, setter);
                break;
            }
        }
    }

    return files;
}

// Returns the songs in the music file at `songs`, where each song's length gives the start of the next. The list stops
// before the first song whose header the driver can't read, or at `limit` songs.
std::vector<uint32_t> ReadSongList(const Rom& rom, uint32_t songs, int limit)
{
    std::vector<uint32_t> list;
    uint32_t at = songs;
    SongHeader header;
    while (int(list.size()) < limit && ReadSongHeader(rom, at, header))
    {
        list.push_back(at);
        at += header.length;
    }

    return list;
}

// Finds the driver's play routine and the revision that it comes from. info.play stays 0, and the revision A, if no
// revision's routine is found.
void FindPlayRoutine(const Rom& rom, DriverInfo& info)
{
    for (size_t r = 0; r < std::size(kRevisionCode); r++)
    {
        const uint32_t play = FindFirstThumb(rom, kRevisionCode[r].play);
        if (play)
        {
            info.revision = Revision(r);
            info.play = play;
            info.play_effect = FindFirstThumb(rom, kRevisionCode[r].play_effect);
            return;
        }
    }
}

// Finds the tables in the driver's code. Returns false and sets `error` if one of them isn't in the ROM.
bool FindCodeTables(const Rom& rom, DriverInfo& info, std::string& error)
{
    info.length_table = FindLiteral(rom, "2x0D DD00 E1xx 2x0C DD00 E1xx 48xx 18xx 7801 20C0 0040", 6);
    info.frequency_table = FindLiteral(rom, "2E53 DD00 2653 4Dxx 1C30 210C", 3);
    info.pcm_pitch_table = FindLiteral(rom, "2A34 DD00 2234 48xx 0051", 3);
    info.lfo_table = FindLiteral(rom, "48xx 0411 1409 1809 2000 5608", 0);

    const RomTable tables[] = {{info.length_table, 13, "length"},
                               {info.frequency_table, kFrequencyEntries * 2, "frequency"},
                               {info.pcm_pitch_table, kPcmPitchEntries * 2, "PCM pitch"},
                               {info.lfo_table, 256, "LFO"}};
    if (const char* missing = MissingTable(rom, tables))
    {
        error = std::string(info.play ? "found Quintet's sound driver, but not its "
                                      : "no Quintet sound driver found, so it has no ") +
                missing + " table";
        return false;
    }

    return true;
}

// Finds the tables that the game gives the driver. Also lists the samples in each bank's sample file.
void FindGameFiles(const Rom& rom, DriverInfo& info)
{
    // The game gives the driver its wave table, noise macros and samples through routines that store them in the
    // driver's variables, which the routines that use them load. The A revision's music only ever reads bank 0. Its
    // sound effects read bank 1.
    const uint32_t wave_variable = FindLiteral(rom, "B530 0600 0E02 48xx 6801 1C05 2900", 3);
    info.waves = TableFromSetter(rom, FindSetter(rom, wave_variable, kStoreInVariable));
    const RevisionCode& code = kRevisionCode[size_t(info.revision)];
    info.macros = BankTables(rom, FindLiteral(rom, code.macro_variable, code.macro_ldr));
    info.samples = SampleFiles(rom, FindFirstThumb(rom, code.loader));
    for (size_t bank = 0; bank < 2; bank++)
    {
        if (info.samples[bank])
        {
            info.sample_addresses[bank] = ReadSampleList(rom, info.samples[bank]);
        }
    }
}

// Finds the music file and lists its songs. `overrides` may give the music file instead. Returns false and sets `error`
// if there's no music file or it has no songs.
bool FindMusicFile(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    if (overrides.song_table)
    {
        info.music_file = overrides.song_table;
        info.log.push_back("music file at " + Hex(info.music_file) + " (user supplied)");
    }
    else
    {
        info.music_file = FindSongs(rom, info.play);
        if (!info.music_file)
        {
            error = "found Quintet's sound driver, but not the game's music file";
            return false;
        }

        info.log.push_back("music file at " + Hex(info.music_file) + " (from the game's play-song code)");
    }

    info.song_addresses = ReadSongList(rom, info.music_file, overrides.song_count ? overrides.song_count : kMaxSongs);
    if (info.song_addresses.empty())
    {
        error = "the music file at " + Hex(info.music_file) + " has no songs";
        return false;
    }
    if (overrides.song_count && int(info.song_addresses.size()) < overrides.song_count)
    {
        info.warnings.push_back("the music file has only " + std::to_string(info.song_addresses.size()) + " songs");
    }

    return true;
}

// Finds the sound effects' file from the game's calls to the routine that plays a sound effect, and lists the sound
// effects. A game without them has none.
void FindEffectsFile(const Rom& rom, DriverInfo& info)
{
    info.effects_file = FindSongs(rom, info.play_effect);
    if (info.effects_file)
    {
        info.effect_addresses = ReadSongList(rom, info.effects_file, kMaxSongs);
    }
}

// Logs what detection found: the songs, the play routine and the tables. Adds a warning for each of the game's tables
// that detection didn't find.
void ReportDetection(DriverInfo& info)
{
    auto found = [](uint32_t address)
    {
        return address ? Hex(address) : std::string("not found");
    };

    auto samples = [&](size_t bank)
    {
        return info.samples[bank]
                   ? Hex(info.samples[bank]) + " (" + std::to_string(info.sample_addresses[bank].size()) + ")"
                   : std::string("not found");
    };

    info.log.push_back(std::to_string(info.song_addresses.size()) + " songs, play routine at " + Hex(info.play) +
                       kRevisionCode[size_t(info.revision)].label);
    if (info.effects_file)
    {
        const int songs = int(info.song_addresses.size());
        info.log.push_back("sound effects' file at " + Hex(info.effects_file) + " (" +
                           std::to_string(info.effect_addresses.size()) + " sound effects: songs " +
                           std::to_string(songs) + "-" + std::to_string(SongCount(info) - 1) + "), play routine at " +
                           Hex(info.play_effect));
    }
    info.log.push_back("wave table " + found(info.waves) + ", noise macros " + found(info.macros[0]) +
                       ", PCM samples " + samples(0));
    info.log.push_back("the sound effects' noise macros " + found(info.macros[1]) + ", PCM samples " + samples(1));
    info.log.push_back("length table " + Hex(info.length_table) + ", frequency table " + Hex(info.frequency_table) +
                       ", PCM pitch table " + Hex(info.pcm_pitch_table) + ", LFO table " + Hex(info.lfo_table));

    if (!info.waves)
    {
        info.warnings.push_back("the game's wave table wasn't found, so the wave channel plays silence");
    }
    if (!info.macros[0])
    {
        info.warnings.push_back("the game's noise macros weren't found, so the noise channel ignores them");
    }
    if (!info.samples[0])
    {
        info.warnings.push_back("the game's PCM samples weren't found, so the PCM channels play silence");
    }
}

} // namespace

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    error.clear();

    FindPlayRoutine(rom, info);
    if (!info.play && !overrides.song_table)
    {
        return false;
    }
    if (!FindCodeTables(rom, info, error))
    {
        return false;
    }

    FindGameFiles(rom, info);
    if (!FindMusicFile(rom, overrides, info, error))
    {
        return false;
    }

    FindEffectsFile(rom, info);
    ReportDetection(info);

    return true;
}

} // namespace agbsf2::quintet
