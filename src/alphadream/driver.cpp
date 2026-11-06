// SPDX-License-Identifier: MIT

// Detection of AlphaDream's sound driver. Each routine is found by its code, and its literals give the tables.

#include "alphadream/driver.h"

#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"
#include "thumb.h"

namespace agbsf2::alphadream
{
namespace
{

// The part of the routine that starts a song on a player that sets and clears the player's flags, then loads the song
// table (0x0819B12A in A88E). The Hamtaro games (A84J and B85A) change the code before it. Its literal at +0x1C is the
// song table. The routine starts with a push of r4 to r7 and lr up to 0x40 bytes before.
constexpr const char* kStartRoutine = "4Bxx 18C9 19x9 780A 2001 4310 7008 6831 18C9 19x9 780A 20FD 4010 7008 48xx";
constexpr uint32_t kSongTableLiteral = 0x1C;
constexpr uint32_t kStartSearch = 0x40;

// The start of a note in the routine that runs a track (0x0819B5A0 in A88E): it reads the key and keeps it.
constexpr const char* kNoteRoutine = "8928 1C41 8129 0400 0C00 1810 7804 207F 4020 72E8";

// What follows in B85A and A84J: the note's third byte, kept as the track's volume.
constexpr const char* kNoteVolume = "1C48 8128 0409 0C09 1851 7808 7368";
constexpr uint32_t kNoteVolumeAt = 0x14;

// What follows that in A84J: a call of the track's volume handler.
constexpr const char* kVolumeCall = "6879 1C28";
constexpr uint32_t kVolumeCallAt = 0x22;

// The routine that runs a command from the game (0x0819B33E in A88E): a jump through a table by the command's top 4
// bits. Its literal at +8 is the table. Command 5 starts a sound effect through a call at +6. The first literal of
// the routine that it calls is the table of sound effects, which follows the song table.
constexpr const char* kCommandRoutine = "0B18 28xx D8xx 0080 49xx 1840 6800 4687";
constexpr uint32_t kCommandTableLiteral = 8;
constexpr uint32_t kEffectCommand = 5;
constexpr uint32_t kEffectCall = 6;

// The routine that finds an instrument's region for a key (0x0819A8EC). Its literal at +4 is the instrument table.
constexpr const char* kRegionLookup =
    "B500 1C0B 4Axx 0041 1889 8809 1889 3001 0040 1880 8800 1882 4291 D0xx 7808 4298 "
    "DCxx 7848 4298 DBxx 1C08 Exxx";
constexpr uint32_t kInstrumentLiteral = 4;

// The routine that mixes a sample channel (0x0819A398). Its literal at +0xC is the sample table.
constexpr const char* kVoiceRoutine = "B5F0 4657 464D 4643 B4A8 8872 49xx 0450 0BC0 580F 187F 0452 D3xx 6878 213C 2200";
constexpr uint32_t kSampleLiteral = 0xC;

// The routine that turns a sample's rate and a pitch into the mixer's step (0x0819A204). Its two adr instructions
// give the table of keys and the table of steps.
constexpr const char* kStepRoutine = "4684 A31A 5CC9 A30C 0708 0E80 18C0 6803 6840 1AC0 4350 0A00 181B 0909 40CB 4Axx";

// The routine that turns a PSG pitch into a frequency setting (0x0819A96C). Its literal at +0x14 is the pitch table.
constexpr const char* kPsgPitchRoutine =
    "B510 0400 0E03 21FF 0409 4001 0C0C 3B24 2B00 DBxx 4Axx 0058 1880 8801 1C58 "
    "0040 1880 8800 1A40 4360 0A00 1809 2280 0112";
constexpr uint32_t kPsgPitchLiteral = 0x14;

// The routine that gives a key's noise setting (0x0819A9B4). Its literal at +0x10 is the noise table.
constexpr const char* kNoiseRoutine = "B500 1C01 2914 DCxx 20D7 Exxx 2950 DCxx 48xx 3915";
constexpr uint32_t kNoiseLiteral = 0x10;

// The routine that loads a wave pattern (0x0819A9DC). Its literal at +2 is the wave table.
constexpr const char* kWaveRoutine = "0100 49xx 1840 4Bxx 21C0 7019";
constexpr uint32_t kWaveLiteral = 2;
// The wave's start in the routine that plays the wave (0x0819AF4C in A88E): the NR32 setting for the channel's volume
// over 4. Its literal at +0 is the table of settings.
constexpr const char* kWaveLevelRoutine = "49xx 7918 0880 1840 7800 0200 8010";

// The most songs, instruments and samples that detection reads.
constexpr int kMaxEntries = 4096;

// Returns the address that the Thumb `adr rX, #imm` at `at` gives.
uint32_t AdrTarget(const Rom& rom, uint32_t at)
{
    return ((at + 4) & ~3u) + (rom.U16(at) & 0xFF) * 4;
}

// Returns the only match of a pattern, or 0 if there's none or more than one.
uint32_t FindOnly(const Rom& rom, const char* pattern)
{
    const std::vector<uint32_t> hits = FindThumb(rom, ParseThumbPattern(pattern));
    return hits.size() == 1 ? hits.front() : 0;
}

// Returns true if the code at `at` matches a pattern.
bool MatchesAt(const Rom& rom, uint32_t at, const char* pattern)
{
    const std::vector<ThumbPattern> halfwords = ParseThumbPattern(pattern);
    for (size_t i = 0; i < halfwords.size(); i++)
    {
        const uint32_t a = at + 2 * uint32_t(i);
        if (!rom.Contains(a, 2) || (rom.U16(a) & halfwords[i].mask) != (halfwords[i].value & halfwords[i].mask))
        {
            return false;
        }
    }
    return true;
}

// Returns the table of sound effects, from the routine that runs the game's commands, or 0 if it isn't found.
uint32_t FindEffectTable(const Rom& rom)
{
    const uint32_t commands = FindOnly(rom, kCommandRoutine);
    if (!commands)
    {
        return 0;
    }
    const uint32_t table = ThumbLiteral(rom, commands + kCommandTableLiteral);
    if (!rom.Contains(table + 4 * kEffectCommand, 4))
    {
        return 0;
    }
    const uint32_t handler = rom.U32(table + 4 * kEffectCommand) & ~1u;
    const uint32_t routine = BlTarget(rom, handler + kEffectCall);
    // The first literal in the ROM among the routine's first 16 instructions.
    for (uint32_t at = routine; routine && rom.Contains(at, 2) && at < routine + 32; at += 2)
    {
        if ((rom.U16(at) & 0xF800) == 0x4800)
        {
            const uint32_t literal = ThumbLiteral(rom, at);
            if (rom.Contains(literal, 4))
            {
                return literal;
            }
        }
    }
    return 0;
}

} // namespace

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    info = DriverInfo{};
    const uint32_t flags = FindOnly(rom, kStartRoutine);
    for (uint32_t at = flags; flags && at + kStartSearch >= flags && at >= kRomBase + 2 && !info.start_routine; at -= 2)
    {
        if (rom.U16(at) == 0xB5F0)
        {
            info.start_routine = at;
        }
    }
    if (!info.start_routine)
    {
        return false;
    }

    const uint32_t note = FindOnly(rom, kNoteRoutine);
    const uint32_t lookup = FindOnly(rom, kRegionLookup);
    const uint32_t voice = FindOnly(rom, kVoiceRoutine);
    const uint32_t step = FindOnly(rom, kStepRoutine);
    const uint32_t psg = FindOnly(rom, kPsgPitchRoutine);
    const uint32_t noise = FindOnly(rom, kNoiseRoutine);
    const uint32_t wave = FindOnly(rom, kWaveRoutine);
    const uint32_t wave_level = FindOnly(rom, kWaveLevelRoutine);
    if (!note || !lookup || !voice || !step || !psg || !noise || !wave || !wave_level)
    {
        error = "found AlphaDream's sound driver at " + Hex(info.start_routine) +
                ", but not its routines for notes, instruments, samples or the PSG";
        return false;
    }
    if (MatchesAt(rom, note + kNoteVolumeAt, kNoteVolume))
    {
        info.version = MatchesAt(rom, note + kVolumeCallAt, kVolumeCall) ? Version::kHamtaro4 : Version::kHamSports;
    }

    info.song_table = ThumbLiteral(rom, flags + kSongTableLiteral);
    info.instrument_table = ThumbLiteral(rom, lookup + kInstrumentLiteral);
    info.sample_table = ThumbLiteral(rom, voice + kSampleLiteral);
    info.key_table = AdrTarget(rom, step + 2);
    info.step_table = AdrTarget(rom, step + 6);
    info.psg_pitch_table = ThumbLiteral(rom, psg + kPsgPitchLiteral);
    info.noise_table = ThumbLiteral(rom, noise + kNoiseLiteral);
    info.wave_table = ThumbLiteral(rom, wave + kWaveLiteral);
    info.wave_levels = ThumbLiteral(rom, wave_level);
    if (!rom.Contains(info.key_table, 128) || !rom.Contains(info.step_table, 13 * 4) ||
        !rom.Contains(info.psg_pitch_table, 2) || !rom.Contains(info.noise_table, 60) ||
        !rom.Contains(info.wave_table) || !rom.Contains(info.wave_levels, 4))
    {
        error = "AlphaDream's sound driver's pitch tables aren't in the ROM";
        return false;
    }

    // The song table ends at its first 0 or at the table of sound effects.
    info.effect_table = FindEffectTable(rom);
    const uint32_t table_end = info.effect_table > info.song_table ? info.effect_table : 0xFFFFFFFF;
    while (info.song_count < kMaxEntries && rom.Contains(info.song_table + 4 * info.song_count, 4) &&
           info.song_table + 4 * info.song_count < table_end && rom.U32(info.song_table + 4 * info.song_count) != 0)
    {
        info.song_count++;
    }

    // The instrument table's first offset is the size of its list of offsets. The last offset ends the last instrument.
    const uint32_t first_instrument = rom.U16(info.instrument_table);
    info.instrument_count = int(first_instrument / 2) - 1;
    const uint32_t first_sample = rom.U32(info.sample_table);
    info.sample_count = int(first_sample / 4);
    if (info.song_count == 0 || info.instrument_count <= 0 || info.instrument_count > kMaxEntries ||
        info.sample_count <= 0 || info.sample_count > kMaxEntries ||
        !rom.Contains(info.instrument_table, first_instrument) || !rom.Contains(info.sample_table, first_sample))
    {
        error = "AlphaDream's sound driver's song, instrument or sample table can't be read";
        return false;
    }

    if (info.version == Version::kHamSports)
    {
        info.log.push_back("the version of Hamu-Hamu Sports: a note sets the track's volume");
    }
    else if (info.version == Version::kHamtaro4)
    {
        info.log.push_back("the older version of Tottoko Hamtaro 4: a note sets the track's volume");
    }
    info.log.push_back("song table at " + Hex(info.song_table) + ", " + std::to_string(info.song_count) + " songs");
    info.log.push_back("instruments at " + Hex(info.instrument_table) + ", " + std::to_string(info.instrument_count));
    info.log.push_back("samples at " + Hex(info.sample_table) + ", " + std::to_string(info.sample_count));
    info.log.push_back("wave patterns at " + Hex(info.wave_table) + ", noise settings at " + Hex(info.noise_table));
    return true;
}

std::vector<Region> Regions(const Rom& rom, const DriverInfo& info, int instrument)
{
    std::vector<Region> out;
    if (instrument < 0 || instrument >= info.instrument_count)
    {
        return out;
    }

    const uint32_t from = info.instrument_table + rom.U16(info.instrument_table + 2 * uint32_t(instrument));
    const uint32_t to = info.instrument_table + rom.U16(info.instrument_table + 2 * uint32_t(instrument) + 2);
    for (uint32_t at = from; at + 8 <= to && rom.Contains(at, 8); at += 8)
    {
        Region r;
        r.address = at;
        r.lo = rom.U8(at);
        r.hi = rom.U8(at + 1);
        r.sample = rom.U16(at + 2);
        for (uint32_t i = 0; i < 4; i++)
        {
            r.envelope[i] = rom.U8(at + 4 + i);
        }
        out.push_back(r);
    }
    return out;
}

bool FindRegion(const Rom& rom, const DriverInfo& info, int instrument, int key, Region& out)
{
    for (const Region& r : Regions(rom, info, instrument))
    {
        if (r.lo <= key && key <= r.hi)
        {
            out = r;
            return true;
        }
    }
    return false;
}

bool ReadSample(const Rom& rom, const DriverInfo& info, int number, SampleHeader& out)
{
    if (number < 0 || number >= info.sample_count)
    {
        return false;
    }

    out = SampleHeader{};
    const uint32_t offset = rom.U32(info.sample_table + 4 * uint32_t(number));
    out.empty = offset == 0;
    out.address = info.sample_table + offset;
    out.loops = rom.U8(out.address + 3) & 0x40;
    out.rate = rom.U32(out.address + 4);
    out.loop_start = rom.U32(out.address + 8);
    out.end = rom.U32(out.address + 12);
    out.data = out.address + 16;
    return rom.Contains(out.address, 16) && out.end > 0 && rom.Contains(out.data, out.end) &&
           (!out.loops || out.loop_start < out.end);
}

bool ReadSong(const Rom& rom, const DriverInfo& info, int number, Song& out)
{
    if (number < 0 || number >= info.song_count)
    {
        return false;
    }

    out = Song{};
    out.address = rom.U32(info.song_table + 4 * uint32_t(number));
    if (!rom.Contains(out.address, 2))
    {
        return false;
    }
    out.mask = rom.U16(out.address) & ((1 << kTracks) - 1);
    uint32_t at = out.address + 2;
    for (int t = 0; t < kTracks; t++)
    {
        if (out.mask & (1 << t))
        {
            out.tracks[size_t(t)] = out.address + rom.U16(at);
            at += 2;
        }
    }
    return out.mask != 0;
}

} // namespace agbsf2::alphadream
