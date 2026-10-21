// SPDX-License-Identifier: MIT

#include "krawall/driver.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "program.h"
#include "rom.h"
#include "thumb.h"

namespace agbsf2::krawall
{
namespace
{

// The most modules detection lists, so that unrelated ROM data can't make the list endless.
constexpr size_t kMaxModules = 1024;

// The mixer's rate that the model's rates and ticks are for: the library's default, which sets the step of a sample's
// points as 4 times its frequency and the sound timer's ticks from 15 seconds of the mix.
constexpr int kModelRate = 16384;

// The start of the version string that the library holds.
constexpr const char* kVersionText = "$Id: Krawall";

// The start of the player's tables, which are the same in every build.
constexpr int16_t kSine[] = {0, 25, 50, 74, 98, 120, 142, 162, 180, 197, 212, 225, 236, 244, 250, 254};
constexpr int16_t kRamp[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120};
constexpr int16_t kSquare[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
constexpr int16_t kRandom[] = {-28, 185, 162, -32, -161, 170, -222, -186, -177, -193, -2, 232, -252, -133, 121, -127};
constexpr uint16_t kPeriods[] = {27392, 25855, 24403, 23034, 21741, 20521, 19369, 18282, 17256, 16287, 15373, 14510};
constexpr uint16_t kFineTunes[] = {34716, 34654, 34591, 34529, 34467, 34405, 34343, 34281};
constexpr uint32_t kLinear[] = {1096155136, 1097144902, 1098135561, 1099127114, 1100119563, 1101112908};

// The start of krapPlay(), which starts a module: push {r4-r7, lr}, then its arguments and the test of the jingle mode.
constexpr const char* kPlayPattern = "B5F0 4657 464E 4645 B4E0 1C0F 4680 4692 1C38 2204 4010 2800";

// The note on of the row's routine for a module with instruments: ldr r1, =instruments ... ldr r7, =samples.
constexpr const char* kTablesPattern = "0407 49xx 0BBA 588F 0071 616F 3902 5A78 4Fxx 0082 58B8 6128";
constexpr int kInstrumentsIndex = 1;
constexpr int kSamplesIndex = 8;

// The timers' setup in kragInit(): the ldr of timer 0's reload value. The value gives the mixer's rate.
constexpr const char* kTimerPattern = "48xx 49xx 4Bxx 8008 3102 48xx 800B 3906 8008";
constexpr int kTimerIndex = 5;

// The end of getDmaAddress(), which moves on by the samples of a frame: movs r3, #n/2; lsls r0, r3, #1.
constexpr const char* kFramePattern = "49xx 23xx 0058 680B 181A 600A";
constexpr int kFrameIndex = 1;

// The start of kragInit(), which the game calls with its choice of stereo, and of kramQualityMode() and
// kramSetMasterVol(). kragInit() calls kramSetMasterVol() itself, at the offset given, before the game can.
constexpr const char* kInitPattern = "B510 1C04 2080 F000 F9xx 1C20 F000 F8xx F000 F8xx";
constexpr const char* kQualityPattern = "B530 2210 4002 2A00 D0xx 49xx 2301 700B";
constexpr const char* kMasterPattern = "B5F0 1C04 0C20 2800 D0xx 49xx";
constexpr uint32_t kInitMasterCall = 6;

// Returns the address of the first copy of `bytes` in the ROM at a multiple of `align`, or 0.
uint32_t FindBytes(const Rom& rom, const std::vector<uint8_t>& bytes, uint32_t align)
{
    const uint8_t* data = rom.Ptr(kRomBase);
    if (!data || bytes.empty())
    {
        return 0;
    }

    const uint8_t* end = data + rom.Size();
    for (const uint8_t* at = data; (at = std::search(at, end, bytes.begin(), bytes.end())) != end; at++)
    {
        if ((at - data) % align == 0)
        {
            return kRomBase + uint32_t(at - data);
        }
    }

    return 0;
}

// Returns the address of the first copy of a table's values in the ROM, at a multiple of their size, or 0.
template <typename T, size_t N>
uint32_t FindTable(const Rom& rom, const T (&values)[N])
{
    std::vector<uint8_t> bytes;
    for (T v : values)
    {
        for (size_t i = 0; i < sizeof(T); i++)
        {
            bytes.push_back(uint8_t(uint32_t(v) >> (8 * i)));
        }
    }

    return FindBytes(rom, bytes, uint32_t(sizeof(T)));
}

// Returns the immediate of a `movs r0, #imm` just before each of the game's calls to `routine`, the first one found, or
// -1 if no call has one.
int CallArgument(const Rom& rom, uint32_t routine)
{
    for (uint32_t call : FindCalls(rom, routine))
    {
        const uint16_t before = rom.U16(call - 2);
        if ((before & 0xFF00) == 0x2000)
        {
            return before & 0xFF;
        }
    }

    return -1;
}

// Returns the argument that the game loads into r0, with `movs` or `ldr` from a literal, just before its first call to
// `routine` other than the one at `skip`, or -1 if no call has one.
int64_t CallLiteral(const Rom& rom, uint32_t routine, uint32_t skip)
{
    for (uint32_t call : FindCalls(rom, routine))
    {
        if (call == skip)
        {
            continue;
        }

        const uint16_t before = rom.U16(call - 2);
        if ((before & 0xFF00) == 0x2000)
        {
            return before & 0xFF;
        }
        if ((before & 0xFF00) == 0x4800)
        {
            return ThumbLiteral(rom, call - 2);
        }
    }

    return -1;
}

// Returns the library's version string at `at`: its printable characters.
std::string VersionText(const Rom& rom, uint32_t at)
{
    std::string text;
    for (; rom.Contains(at) && rom.U8(at) >= 0x20 && rom.U8(at) < 0x7F && text.size() < 80; at++)
    {
        text += char(rom.U8(at));
    }

    return text;
}

// Returns true if a pattern's header is one the player can read: a row count, and an index of every 4th row's data that
// starts at the data and doesn't go back.
bool PlausiblePattern(const Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, kPatternData))
    {
        return false;
    }

    const uint32_t rows = rom.U8(at + kPatternRows);
    if (rows == 0 || rom.U16(at) != 0)
    {
        return false;
    }

    uint16_t last = 0;
    for (uint32_t i = 1; i < kPatternIndexEntries && 4 * i < rows; i++)
    {
        const uint16_t offset = rom.U16(at + 2 * i);
        if (offset < last || !rom.Contains(at + kPatternData + offset))
        {
            return false;
        }

        last = offset;
    }

    return true;
}

// Finds the modules by their headers, in the order they're in the ROM.
std::vector<uint32_t> ScanModules(const Rom& rom)
{
    std::vector<uint32_t> found;
    for (uint32_t at = kRomBase; rom.Contains(at, kModulePatterns + 4) && found.size() < kMaxModules; at += 4)
    {
        // The cheap checks first: the channels, the orders and the speed.
        const uint8_t channels = rom.U8(at + kModuleChannels);
        const uint8_t orders = rom.U8(at + kModuleOrderCount);
        if (channels == 0 || channels > 32 || orders == 0 || rom.U8(at + kModuleRestart) >= orders)
        {
            continue;
        }
        if (rom.U8(at + kModuleSpeed) == 0 || rom.U8(at + kModuleTempo) < 0x20)
        {
            continue;
        }

        ModuleInfo module;
        if (ReadModule(rom, at, module))
        {
            found.push_back(at);
        }
    }

    return found;
}

// Finds the game's table of modules: the longest run of at least two pointers to modules found by the scan. Sets the
// table's address and length, or leaves both at 0 if no table is found.
void FindModuleTable(const Rom& rom, const std::vector<uint32_t>& modules, uint32_t& table, int& count)
{
    std::vector<uint32_t> sorted = modules;
    std::sort(sorted.begin(), sorted.end());
    uint32_t run_start = 0;
    int run = 0;
    for (uint32_t at = kRomBase; rom.Contains(at, 4); at += 4)
    {
        if (!std::binary_search(sorted.begin(), sorted.end(), rom.U32(at)))
        {
            run = 0;
            continue;
        }

        if (run++ == 0)
        {
            run_start = at;
        }
        if (run > count && run >= 2)
        {
            table = run_start;
            count = run;
        }
    }
}

// Reads the tables that the player's code at `table_code` names, and finds the player's other tables. Returns false and
// sets `error` if one isn't in the ROM.
bool ReadTables(const Rom& rom, uint32_t table_code, DriverInfo& info, std::string& error)
{
    info.instruments = ThumbLiteral(rom, table_code + 2 * kInstrumentsIndex);
    info.samples = ThumbLiteral(rom, table_code + 2 * kSamplesIndex);

    // The player's tables.
    info.sine = FindTable(rom, kSine);
    info.ramp = FindTable(rom, kRamp);
    info.square = FindTable(rom, kSquare);
    info.random = FindTable(rom, kRandom);
    info.periods = FindTable(rom, kPeriods);
    info.fine_tunes = FindTable(rom, kFineTunes);
    info.linear = FindTable(rom, kLinear);
    const RomTable tables[] = {
        {info.sine, 128, "vibrato's sine"},
        {info.ramp, 128, "vibrato's ramp"},
        {info.square, 128, "vibrato's square"},
        {info.random, 128, "vibrato's random wave"},
        {info.periods, 240, "table of periods"},
        {info.fine_tunes, 128, "table of fine tunes"},
        {info.linear, 3072, "table of linear frequencies"},
        {info.samples, 4, "table of samples"},
    };
    if (const char* missing = MissingTable(rom, tables))
    {
        error = std::string("found Krawall, but not its ") + missing;
        return false;
    }

    return true;
}

// Reads the mixer's rate and the samples it mixes a frame, which the build of the library sets. Returns false and sets
// `error` if the code that sets them isn't found.
bool ReadMixRate(const Rom& rom, DriverInfo& info, std::string& error)
{
    const uint32_t timer = FindFirstThumb(rom, kTimerPattern);
    const uint32_t reload = timer ? ThumbLiteral(rom, timer + 2 * kTimerIndex) & 0xFFFF : 0;
    const uint32_t frame = FindFirstThumb(rom, kFramePattern);
    if (!reload || !frame)
    {
        error = "found Krawall, but not its mixer's rate";
        return false;
    }
    info.mix_rate = int((1u << 24) / (0x10000 - reload));
    info.frame_samples = (rom.U16(frame + 2 * kFrameIndex) & 0xFF) << 1;
    if (info.mix_rate != kModelRate)
    {
        info.warnings.push_back("the mixer runs at " + std::to_string(info.mix_rate) +
                                " Hz, but the model plays notes "
                                "and ticks as the library does at " +
                                std::to_string(kModelRate) + " Hz");
    }

    return true;
}

// Reads the game's settings for stereo and the mixer's quality, and its master volume.
void ReadGameSettings(const Rom& rom, DriverInfo& info)
{
    const uint32_t init = FindFirstThumb(rom, kInitPattern);
    const uint32_t quality = FindFirstThumb(rom, kQualityPattern);
    const int stereo = init ? CallArgument(rom, init) : -1;
    const int mode = quality ? CallArgument(rom, quality) : -1;
    info.stereo = stereo != 0;
    info.quality = uint8_t(std::max(mode, 0));
    if (stereo < 0)
    {
        info.warnings.push_back("found no call to kragInit(), so the mixer is taken to be stereo");
    }

    // The game's master volume, from its call to kramSetMasterVol() rather than kragInit()'s.
    const uint32_t master = FindFirstThumb(rom, kMasterPattern);
    const int64_t master_volume = master ? CallLiteral(rom, master, init ? init + kInitMasterCall : 0) : -1;
    if (master_volume >= 0)
    {
        info.master_volume = uint32_t(master_volume);
    }
}

// Lists the modules: from the given table, or the headers that a scan of the ROM finds. A count given for the modules
// applies to a given table and to the game's table that the scan finds. The count leaves out the modules that the table
// doesn't list. Returns the table that the list comes from, or 0 for a list from the scan.
uint32_t ListModules(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info)
{
    std::vector<uint32_t> found;
    uint32_t table = overrides.module_table;
    if (!table)
    {
        found = ScanModules(rom);
        FindModuleTable(rom, found, info.module_table, info.table_count);
        table = overrides.module_count > 0 ? info.module_table : 0;
    }

    if (table)
    {
        const int count = overrides.module_count > 0 ? overrides.module_count : int(kMaxModules);
        for (int i = 0; i < count && rom.Contains(table + 4 * uint32_t(i), 4); i++)
        {
            const uint32_t at = rom.U32(table + 4 * uint32_t(i));
            ModuleInfo module;
            if (!ReadModule(rom, at, module))
            {
                if (overrides.module_count > 0)
                {
                    info.warnings.push_back("entry " + std::to_string(i) + " of the module table, " + Hex(at) +
                                            ", isn't a module the player can read");
                }
                else
                {
                    break;
                }
            }
            info.modules.push_back(at);
        }

        return table;
    }

    // The modules in the order of the game's table, and then any that it doesn't list, in the ROM's order. Without a
    // table, a count given for the modules keeps that many of them.
    for (int i = 0; i < info.table_count; i++)
    {
        info.modules.push_back(rom.U32(info.module_table + 4 * uint32_t(i)));
    }
    for (uint32_t at : found)
    {
        if (std::find(info.modules.begin(), info.modules.end(), at) == info.modules.end())
        {
            info.modules.push_back(at);
        }
    }
    if (overrides.module_count > 0 && info.modules.size() > size_t(overrides.module_count))
    {
        info.modules.resize(size_t(overrides.module_count));
    }

    return 0;
}

// Logs what detection found, with the modules that come from `table`, or from a scan if it's 0.
void LogDetection(const Rom& rom, uint32_t table, DriverInfo& info)
{
    char text[160];
    if (info.version)
    {
        info.log.push_back("version: " + VersionText(rom, info.version));
    }
    std::snprintf(text, sizeof text, "krapPlay(): %s, samples: %s, instruments: %s", Hex(info.play).c_str(),
                  Hex(info.samples).c_str(), Hex(info.instruments).c_str());
    info.log.push_back(text);
    std::snprintf(text, sizeof text, "mixer: %d Hz, %d samples a frame, %s, quality %u, master volume 0x%X",
                  info.mix_rate, info.frame_samples, info.stereo ? "stereo" : "mono", unsigned(info.quality),
                  unsigned(info.master_volume));
    info.log.push_back(text);
    if (info.table_count)
    {
        std::snprintf(text, sizeof text, "module table: %s (%d)", Hex(info.module_table).c_str(), info.table_count);
        info.log.push_back(text);
    }
    const size_t unlisted = info.modules.size() - std::min(info.modules.size(), size_t(info.table_count));
    if (!table && unlisted)
    {
        info.log.push_back(std::to_string(unlisted) + (unlisted == 1 ? " module" : " modules") +
                           (info.table_count ? " that the table doesn't list" : ", which no table lists"));
    }
}

// Lists the songs. A module without markers converts as a whole. One with markers converts song by song, each in the
// player's song mode.
void ListSongs(const Rom& rom, DriverInfo& info)
{
    for (uint32_t at : info.modules)
    {
        ModuleInfo module;
        const int songs = ReadModule(rom, at, module) ? SongsInModule(module) : 0;
        if (songs == 0)
        {
            info.songs.push_back({at, -1});
            continue;
        }

        for (int s = 0; s < songs; s++)
        {
            info.songs.push_back({at, s});
        }

        char text[160];
        std::snprintf(text, sizeof text, "the module at %s separates %d %s with +++", Hex(at).c_str(), songs,
                      songs == 1 ? "song" : "songs");
        info.log.push_back(text);
    }
}

} // namespace

bool ReadModule(const Rom& rom, uint32_t address, ModuleInfo& module)
{
    module = ModuleInfo();
    if (!rom.Contains(address, kModulePatterns))
    {
        return false;
    }

    module.address = address;
    module.channels = rom.U8(address + kModuleChannels);
    module.order_count = rom.U8(address + kModuleOrderCount);
    module.restart = rom.U8(address + kModuleRestart);
    for (uint32_t i = 0; i < 256; i++)
    {
        module.orders[i] = rom.U8(address + kModuleOrders + i);
    }
    for (uint32_t i = 0; i < 32; i++)
    {
        module.channel_pan[i] = rom.S8(address + kModuleChannelPans + i);
    }
    for (uint32_t i = 0; i < 64; i++)
    {
        module.song_starts[i] = rom.U8(address + kModuleSongStarts + i);
    }
    module.global_volume = rom.U8(address + kModuleGlobalVolume);
    module.speed = rom.U8(address + kModuleSpeed);
    module.tempo = rom.U8(address + kModuleTempo);
    const uint8_t flags[3] = {rom.U8(address + kModuleFlags), rom.U8(address + kModuleFlags + 1),
                              rom.U8(address + kModuleFlags + 2)};
    module.instruments = flags[0] != 0;
    module.linear = flags[1] != 0;
    module.fast_slides = flags[2] != 0;
    module.volume_opt = rom.U8(address + kModuleFlags + 3);
    module.amiga_limits = rom.U8(address + kModuleFlags + 4);

    // The header's flags are bytes of 0 or 1, and the pans are within the player's range.
    if (module.channels == 0 || module.channels > 32 || module.order_count == 0 || module.speed == 0)
    {
        return false;
    }
    for (uint8_t f : {flags[0], flags[1], flags[2], module.volume_opt, module.amiga_limits})
    {
        if (f > 1)
        {
            return false;
        }
    }
    for (int c = 0; c < module.channels; c++)
    {
        if (module.channel_pan[size_t(c)] < -64 || module.channel_pan[size_t(c)] > 64)
        {
            return false;
        }
    }

    // The orders name the patterns, which follow the header.
    int patterns = 0;
    for (int i = 0; i < module.order_count; i++)
    {
        const uint8_t o = module.orders[size_t(i)];
        if (o < kOrderSkip)
        {
            patterns = std::max(patterns, o + 1);
        }
    }
    if (patterns == 0 || !rom.Contains(address + kModulePatterns, 4 * uint32_t(patterns)))
    {
        return false;
    }
    for (int p = 0; p < patterns; p++)
    {
        const uint32_t pattern = rom.U32(address + kModulePatterns + 4 * uint32_t(p));
        if (!PlausiblePattern(rom, pattern))
        {
            return false;
        }

        module.patterns.push_back(pattern);
    }

    return true;
}

RowEntry ReadEntry(const Rom& rom, uint8_t flags, uint32_t& at)
{
    // The note is in the first byte's top 7 bits. The second byte holds the instrument's low 8 bits, and the first
    // byte's low bit holds its 9th.
    RowEntry entry;
    entry.channel = flags & kEntryChannel;
    if (flags & kEntryNote)
    {
        const uint8_t b0 = rom.U8(at++);
        entry.instrument = uint16_t(rom.U8(at++) | ((b0 & 1) << 8));
        entry.note = uint8_t(b0 >> 1);
    }
    if (flags & kEntryVolume)
    {
        entry.volume = rom.U8(at++);
    }
    if (flags & kEntryEffect)
    {
        entry.effect = rom.U8(at++);
        entry.effect_op = rom.U8(at++);
    }

    return entry;
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    error.clear();

    // The player's code, which this build of Krawall compiled the same in every game. A game with another build has the
    // library's version string, which names the build.
    const std::string kVersion = kVersionText;
    info.version = FindBytes(rom, std::vector<uint8_t>(kVersion.begin(), kVersion.end()), 1);
    info.play = FindFirstThumb(rom, kPlayPattern);
    const uint32_t table_code = FindFirstThumb(rom, kTablesPattern);
    if (!info.play || !table_code)
    {
        if (info.version)
        {
            error = "found Krawall, but not a build that " + std::string(kProgramName) + " supports (" +
                    VersionText(rom, info.version) + "): it reads the build of 2003/09/01";
        }
        return false;
    }

    if (!ReadTables(rom, table_code, info, error) || !ReadMixRate(rom, info, error))
    {
        return false;
    }
    ReadGameSettings(rom, info);

    const uint32_t table = ListModules(rom, overrides, info);
    if (info.modules.empty())
    {
        error = "found Krawall, but no modules";
        return false;
    }

    LogDetection(rom, table, info);
    ListSongs(rom, info);

    return true;
}

int SongsInModule(const ModuleInfo& module)
{
    int songs = 0;
    bool markers = false;
    bool in_song = false;
    for (int i = 0; i < module.order_count; i++)
    {
        const uint8_t o = module.orders[size_t(i)];
        markers = markers || o == kOrderSkip;
        if (o < kOrderSkip && !in_song)
        {
            songs++;
        }
        in_song = o < kOrderSkip;
    }

    return markers ? std::min(songs, int(module.song_starts.size())) : 0;
}

} // namespace agbsf2::krawall
