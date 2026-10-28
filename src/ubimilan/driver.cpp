// SPDX-License-Identifier: MIT

// Detection of Ubisoft Milan's driver: its sequencer's code, the sound bank that the game hands it, the PSG's tables
// and the mixer's rate.

#include "ubimilan/driver.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gba.h"
#include "rom.h"
#include "thumb.h"

namespace agbsf2::ubimilan
{
namespace
{

// The sequencer's per-frame step: if the track plays and its countdown is 0, it takes the next sequence that an end
// command left it, or reads the next command.
constexpr const char* kStepCode =
    "B530 1C04 68E0 2800 D0xx 8820 1C25 3514 2800 D1xx 6920 2800 D0xx 2000 6120 68A2 "
    "2A00 D0xx 6062 7811 7850 0200 4301 1C90 6060 8021";

// The routine that takes the sound bank: it copies the bank's counts and the addresses of its three tables.
constexpr const char* kBankCode = "4Axx 6110 8801 8011 8841 8051 6841 1841 6051 6881 1841 6091 68C1 1840 60D0 4770";

// The routine that returns a PSG instrument: 16 bytes in one of three tables, by the channel in r1. The routine that
// reads an instrument's flags starts the same way, but doesn't go on to the first table.
constexpr const char* kInstrumentCode =
    "B500 0600 0E00 2901 D0xx 2901 DCxx 2900 D0xx Exxx 2902 D0xx Exxx 0100 49xx "
    "1840";

// A table lookup in the instrument routine: lsls r0, r0, #4; ldr r1, [pc, #x]; adds r0, r0, r1.
constexpr const char* kInstrumentLookup = "0100 49xx 1840";

// The noise note: 4 bytes of the noise table for the key, the second ORed with the velocity, to NR41-NR44.
constexpr const char* kNoiseCode = "00B1 48xx 1809 780A 3101 7808 3101 4338 0200 4310 4Axx 8010";

// Square 1's note: the frequency settings of the new key and the last one.
constexpr const char* kFrequencyCode = "49xx 4642 0050 1840 8807 6BE0 0040 1840 8800";

// The wave channel's program change: 32 bytes of the wave table for the program.
constexpr const char* kWaveCode = "0168 49xx 1840";

// The sound init: SOUNDCNT_H, SOUNDCNT_X, SOUNDBIAS, the mixer's copy to IWRAM, the DMA interrupts, then Timer 0's
// reload, worked out in floating point from the single-precision literal that the last ldr loads.
constexpr const char* kSoundInitCode =
    "49xx 2001 6008 49xx 200E 7008 3101 20A9 7008 3101 2080 7008 4Axx 8810 2380 "
    "01DB 1C19 4308 8010 4Cxx 48xx 49xx 6809 Fxxx Fxxx 6020 49xx 2009 Fxxx Fxxx "
    "49xx 200A Fxxx Fxxx 48xx";

// Timer 0's reload in the one game known to have the driver, for a game whose sound init detection doesn't find.
constexpr uint32_t kDefaultReload = 0xFBFF;

// Returns the only match of `code`, or 0 if it has none or more than one.
uint32_t FindOnly(const Rom& rom, const char* code)
{
    const std::vector<uint32_t> hits = FindThumb(rom, ParseThumbPattern(code));
    return hits.size() == 1 ? hits[0] : 0;
}

// Returns the address of the sound bank: the file that the game's start hands to the routine that takes the bank. The
// game calls a routine that does nothing else first, with the bank from `movs r0, #N` and a call to the routine that
// finds the archive's file N, which this runs.
std::optional<uint32_t> FindBank(const Rom& rom)
{
    const uint32_t take_bank = FindOnly(rom, kBankCode);
    if (!take_bank)
    {
        return std::nullopt;
    }

    for (uint32_t call : FindCalls(rom, take_bank))
    {
        if (!rom.Contains(call - 2, 2) || rom.U16(call - 2) != 0xB500)
        {
            continue;
        }

        for (uint32_t site : FindCalls(rom, call - 2))
        {
            const uint32_t file = BlTarget(rom, site - 4);
            if (!file || !rom.Contains(site - 6, 2) || (rom.U16(site - 6) & 0xFF00) != 0x2000)
            {
                continue;
            }

            const std::optional<uint32_t> bank = RunThumb(rom, file, {uint32_t(rom.U16(site - 6) & 0xFF), 0, 0, 0});
            if (bank && rom.Contains(*bank, 16))
            {
                return bank;
            }
        }
    }

    return std::nullopt;
}

// Returns Timer 0's reload as the sound init works it out: 0xFFFF less the whole part of the literal plus 0.5. The
// init adds the double 0.5 to the literal with the game's floating-point library.
std::optional<uint32_t> FindReload(const Rom& rom)
{
    const uint32_t init = FindOnly(rom, kSoundInitCode);
    if (!init)
    {
        return std::nullopt;
    }

    const uint32_t bits = ThumbLiteral(rom, init + 0x44);
    float value = 0;
    std::memcpy(&value, &bits, sizeof value);
    const double sum = double(value) + 0.5;
    if (!(sum >= 1 && sum < 0xFFFF))
    {
        return std::nullopt;
    }

    return uint32_t(0xFFFF - uint32_t(sum));
}

// Finds the PSG's tables from the code that reads them, and logs them.
void FindPsgTables(const Rom& rom, DriverInfo& info)
{
    const uint32_t instruments = FindOnly(rom, kInstrumentCode);
    if (instruments)
    {
        int channel = 0;
        const std::vector<ThumbPattern> lookup = ParseThumbPattern(kInstrumentLookup);
        for (uint32_t at = instruments; at < instruments + 0x40 && channel < kPsgChannels; at += 2)
        {
            bool match = rom.Contains(at, 6);
            for (size_t i = 0; match && i < lookup.size(); i++)
            {
                match = (rom.U16(at + 2 * uint32_t(i)) & lookup[i].mask) == lookup[i].value;
            }
            if (match)
            {
                info.instrument_tables[size_t(channel++)] = ThumbLiteral(rom, at + 2);
            }
        }
    }

    const uint32_t noise = FindOnly(rom, kNoiseCode);
    info.noise_table = noise ? ThumbLiteral(rom, noise + 2) : 0;
    const uint32_t frequency = FindOnly(rom, kFrequencyCode);
    info.frequency_table = frequency ? ThumbLiteral(rom, frequency) : 0;
    const uint32_t wave = FindOnly(rom, kWaveCode);
    info.wave_table = wave ? ThumbLiteral(rom, wave + 2) : 0;

    char line[160];
    std::snprintf(line, sizeof line, "PSG instruments: 0x%08X, 0x%08X, 0x%08X", unsigned(info.instrument_tables[0]),
                  unsigned(info.instrument_tables[1]), unsigned(info.instrument_tables[2]));
    info.log.push_back(line);
    std::snprintf(line, sizeof line, "PSG frequency table: 0x%08X, noise table: 0x%08X, wave table: 0x%08X",
                  unsigned(info.frequency_table), unsigned(info.noise_table), unsigned(info.wave_table));
    info.log.push_back(line);
    if (!info.instrument_tables[2] || !info.frequency_table || !info.noise_table || !info.wave_table)
    {
        info.warnings.push_back("some of the PSG's tables weren't found, so the PSG's notes can't be converted");
    }
}

// Reads the sound bank at `song_table`, or the one that FindBank() finds if that's 0: two counts, then the offsets of
// its tables of the game's sound events, of resources and of kits. Logs it. Returns false and sets `error` if the bank
// or its tables aren't in the ROM.
bool ReadBank(const Rom& rom, uint32_t song_table, DriverInfo& info, std::string& error)
{
    if (song_table)
    {
        info.bank = song_table;
    }
    else if (const std::optional<uint32_t> bank = FindBank(rom))
    {
        info.bank = *bank;
    }
    if (!info.bank || !rom.Contains(info.bank, 16))
    {
        error = info.bank ? "the sound bank isn't in the ROM" : "the sound bank wasn't found";
        return false;
    }

    info.resource_count = rom.U16(info.bank + 2);
    info.resource_table = info.bank + rom.U32(info.bank + 8);
    info.kit_table = info.bank + rom.U32(info.bank + 12);
    if (!info.resource_count || !rom.Contains(info.resource_table, 4 * uint32_t(info.resource_count)) ||
        !rom.Contains(info.kit_table, 2))
    {
        error = "the sound bank's tables aren't in the ROM";
        return false;
    }

    char line[160];
    std::snprintf(line, sizeof line, "sound bank: 0x%08X, %d resources at 0x%08X, kits at 0x%08X", unsigned(info.bank),
                  info.resource_count, unsigned(info.resource_table), unsigned(info.kit_table));
    info.log.push_back(line);

    return true;
}

// Finds the songs. They're the pieces of music: the bank's resources of kind 0, in order. If `song_count` is above 0,
// no more than `song_count` are kept. Logs their count. Returns false and sets `error` if there are none.
bool FindSongs(const Rom& rom, int song_count, DriverInfo& info, std::string& error)
{
    for (int id = 0; id < info.resource_count; id++)
    {
        const uint32_t address = ResourceAddress(rom, info, id);
        if (address && rom.U8(address) == kMusic)
        {
            info.songs.push_back(id);
        }
    }
    if (song_count > 0 && size_t(song_count) < info.songs.size())
    {
        info.songs.resize(size_t(song_count));
    }
    if (info.songs.empty())
    {
        error = "the sound bank has no music";
        return false;
    }

    char line[160];
    std::snprintf(line, sizeof line, "music: %d pieces", int(info.songs.size()));
    info.log.push_back(line);

    return true;
}

// Finds the kit that the driver's init gives channel 9: the one that the kit table's first entry names. Logs it, or
// adds a warning if the bank has none.
void FindKit(const Rom& rom, DriverInfo& info)
{
    const uint16_t kit_id = rom.U16(info.kit_table);
    const uint32_t kit = kit_id != kNoResource ? ResourceAddress(rom, info, kit_id) : 0;
    if (kit && rom.U8(kit) == kKit && rom.Contains(kit + 4, 256))
    {
        info.kit = kit + 4;
        char line[160];
        std::snprintf(line, sizeof line, "channel 9's kit: resource %d at 0x%08X", int(kit_id), unsigned(kit));
        info.log.push_back(line);
    }
    else
    {
        info.warnings.push_back("the sound bank has no kit, so channel 9's notes play nothing");
    }
}

// Finds the mixer's rate from Timer 0's reload, or takes Tomb Raider's with a warning if the sound init isn't found.
// Logs it.
void FindMixerRate(const Rom& rom, DriverInfo& info)
{
    if (const std::optional<uint32_t> reload = FindReload(rom))
    {
        info.timer_reload = *reload;
    }
    else
    {
        info.timer_reload = kDefaultReload;
        info.warnings.push_back("the sound init wasn't found, so the mixer is taken to run as in Tomb Raider");
    }

    char line[160];
    std::snprintf(line, sizeof line, "mixer: %.1f Hz output, each voice at %.1f Hz (Timer 0 reload 0x%04X)",
                  OutputRate(info), PointRate(info), unsigned(info.timer_reload));
    info.log.push_back(line);
}

} // namespace

uint32_t OutputCycles(const DriverInfo& info)
{
    return 0x10000 - info.timer_reload;
}

double OutputRate(const DriverInfo& info)
{
    return double(kSecondCycles) / double(OutputCycles(info));
}

double PointRate(const DriverInfo& info)
{
    return OutputRate(info) / 2;
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    error.clear();

    info.step_routine = FindOnly(rom, kStepCode);
    if (!info.step_routine && !overrides.song_table)
    {
        return false;
    }

    if (info.step_routine)
    {
        char line[160];
        std::snprintf(line, sizeof line, "sequencer step: 0x%08X", unsigned(info.step_routine));
        info.log.push_back(line);
    }

    // The sound bank and its songs, then what plays them: the kit, the PSG and the mixer. Each step logs what it finds.
    if (!ReadBank(rom, overrides.song_table, info, error) || !FindSongs(rom, overrides.song_count, info, error))
    {
        return false;
    }

    FindKit(rom, info);
    FindPsgTables(rom, info);
    FindMixerRate(rom, info);

    return true;
}

uint32_t ResourceAddress(const Rom& rom, const DriverInfo& info, int id)
{
    if (id < 0 || id >= info.resource_count)
    {
        return 0;
    }

    const uint32_t address = info.bank + rom.U32(info.resource_table + 4 * uint32_t(id));
    return rom.Contains(address, 4) ? address : 0;
}

bool ReadMusic(const Rom& rom, const DriverInfo& info, int song, MusicPiece& piece)
{
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        return false;
    }

    // A count, the flags, then the sequences' resource numbers.
    piece = MusicPiece();
    piece.address = ResourceAddress(rom, info, info.songs[size_t(song)]);
    if (!piece.address || !rom.Contains(piece.address, 8))
    {
        return false;
    }

    const int count = rom.U16(piece.address + 4);
    piece.flags = rom.U16(piece.address + 6);
    if (!count || !rom.Contains(piece.address + 8, 2 * uint32_t(count)))
    {
        return false;
    }

    for (int i = 0; i < count; i++)
    {
        piece.sequences.push_back(rom.U16(piece.address + 8 + 2 * uint32_t(i)));
    }

    return true;
}

bool ReadSample(const Rom& rom, uint32_t address, GameSample& sample)
{
    // The kind, a loop flag of 1, then the length, the loop's start and its end, then the points.
    if (!rom.Contains(address, 16) || rom.U8(address) != kSample)
    {
        return false;
    }

    sample = GameSample();
    sample.data = address + 16;
    sample.length = rom.U32(address + 4);
    sample.loop = rom.U8(address + 1) == 1;
    if (sample.loop)
    {
        sample.loop_start = rom.U32(address + 8);
        sample.loop_end = rom.U32(address + 12);
    }

    return rom.Contains(sample.data, sample.length);
}

} // namespace agbsf2::ubimilan
