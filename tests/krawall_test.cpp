// SPDX-License-Identifier: MIT

// Unit tests for Krawall. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "krawall/convert.h"
#include "krawall/driver.h"
#include "krawall/player.h"
#include "krawall/soundfont.h"
#include "midi.h"
#include "music.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"

namespace agbsf2::krawall
{
namespace
{

using test::HasGlobalZoneModulators;
using test::Le16;
using test::Le32;
using test::ReadAll;
using test::ReadMidi;
using test::ReadSf2;

// Addresses in a test cartridge: the code patterns that detection reads, the game's calls of the library's settings,
// the version string, the player's tables, the game's tables of samples and modules, the samples and the modules.
constexpr uint32_t kPlay = kRomBase + 0x100;
constexpr uint32_t kTables = kRomBase + 0x140;
constexpr uint32_t kTimer = kRomBase + 0x180;
constexpr uint32_t kFrame = kRomBase + 0x1C0;
constexpr uint32_t kInit = kRomBase + 0x200;
constexpr uint32_t kQuality = kRomBase + 0x240;
constexpr uint32_t kMaster = kRomBase + 0x280;
constexpr uint32_t kGameInit = kRomBase + 0x2C0;
constexpr uint32_t kVersion = kRomBase + 0x400;
constexpr uint32_t kSineTable = kRomBase + 0x1000;
constexpr uint32_t kRampTable = kRomBase + 0x1080;
constexpr uint32_t kSquareTable = kRomBase + 0x1100;
constexpr uint32_t kRandomTable = kRomBase + 0x1180;
constexpr uint32_t kPeriodTable = kRomBase + 0x1200;
constexpr uint32_t kFineTuneTable = kRomBase + 0x1300;
constexpr uint32_t kLinearTable = kRomBase + 0x1400;
constexpr uint32_t kSampleTable = kRomBase + 0x2000;
constexpr uint32_t kInstrumentTable = kRomBase + 0x2100;
constexpr uint32_t kModuleTable = kRomBase + 0x2200;
constexpr uint32_t kSampleData = kRomBase + 0x3000;
constexpr uint32_t kModules = kRomBase + 0x4000;

// The test samples: one that loops forwards, one that doesn't loop, and one that loops back and forth.
constexpr uint32_t kForwardPoints = 128, kForwardLoop = 32;
constexpr uint32_t kOneShotPoints = 1024;
constexpr uint32_t kBidiPoints = 64, kBidiLoop = 16;

// The effects that the test modules use.
constexpr uint8_t kJump = 4, kBreak = 5, kVolumeSlide = 6, kPortaUp = 15, kOffset = 27, kPan = 34;

// The ticks of a row and the mixer's samples of a tick in the test modules: speed 3, and tempo 125, whose ticks the
// sound timer makes 4 times 245760 / 3000 samples long.
constexpr int kSpeed = 3;
constexpr uint32_t kTickSamples = 4 * (245760 / 3000);

// The rate that the player plays C-4 at with no fine tune: 8363 Hz, from the clock over C-4's period, 1712.
constexpr uint32_t kC4Rate = 14317456 / 1712;

// An event of a test pattern: a row, a channel, and the note, instrument, volume byte, effect and operand, with -1 for
// what the event leaves out.
struct Cell
{
    int row = 0;
    int channel = 0;
    int note = -1;
    int instrument = 0;
    int volume = -1;
    int effect = -1;
    int op = 0;
};

// A test pattern: its rows, and its events.
struct Pattern
{
    int rows = 0;
    std::vector<Cell> cells;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, Krawall's tables,
// and the game's samples and modules.
class Cart : public test::CartImage
{
public:
    explicit Cart(bool code = true) : CartImage(0x10000)
    {
        WriteTables();
        if (code)
        {
            WriteCode();
        }
    }

    void PutString(uint32_t a, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); i++)
        {
            Put8(a + uint32_t(i), uint8_t(s[i]));
        }
    }

    // Writes a module of `channels` channels, at the S3M module's speed and tempo, with `cells` in its one pattern of
    // `rows` rows, and adds it to the game's table of modules if `listed`. Returns its header's address.
    uint32_t Module(int channels, int rows, const std::vector<Cell>& cells, bool listed = true)
    {
        return Module(channels, {{rows, cells}}, listed);
    }

    // Writes a module of `channels` channels, at the S3M module's speed and tempo, whose orders play `patterns` in
    // turn, and adds it to the game's table of modules if `listed`. Returns its header's address.
    uint32_t Module(int channels, const std::vector<Pattern>& patterns, bool listed = true)
    {
        std::vector<uint32_t> addresses;
        uint32_t header = next_module_;
        for (const Pattern& p : patterns)
        {
            addresses.push_back(header);
            header = WritePattern(header, p);
        }

        // The header: an order for each pattern, the pans, the global volume, speed and tempo, and the patterns'
        // addresses.
        Put8(header, uint8_t(channels));
        Put8(header + 1, uint8_t(patterns.size()));
        Put8(header + 2, 0);
        for (uint32_t o = 0; o < 256; o++)
        {
            Put8(header + 3 + o, o < patterns.size() ? uint8_t(o) : kOrderEnd);
        }
        Put8(header + 0x163, 64);
        Put8(header + 0x164, uint8_t(kSpeed));
        Put8(header + 0x165, 125);
        for (size_t i = 0; i < addresses.size(); i++)
        {
            Put32(header + kModulePatterns + 4 * uint32_t(i), addresses[i]);
        }
        next_module_ = (header + kModulePatterns + 4 * uint32_t(patterns.size()) + 0xFF) & ~0xFFu;
        if (listed)
        {
            Put32(kModuleTable + 4 * uint32_t(listed_++), header);
        }

        return header;
    }

    // Makes the module at `header` hold songs separated by +++. Each song plays its patterns in turn.
    void Songs(uint32_t header, const std::vector<std::vector<uint8_t>>& songs)
    {
        std::vector<uint8_t> orders;
        for (size_t s = 0; s < songs.size(); s++)
        {
            if (s > 0)
            {
                orders.push_back(kOrderSkip);
            }
            Put8(header + 0x123 + uint32_t(s), uint8_t(orders.size()));
            orders.insert(orders.end(), songs[s].begin(), songs[s].end());
        }

        Put8(header + 1, uint8_t(orders.size()));
        for (uint32_t o = 0; o < 256; o++)
        {
            Put8(header + 3 + o, o < orders.size() ? orders[o] : kOrderEnd);
        }
    }

private:
    // Writes pattern `p` at `pattern`: an index of every 4th row's data, the rows and the data. Returns the next word's
    // address after it.
    uint32_t WritePattern(uint32_t pattern, const Pattern& p)
    {
        std::vector<uint8_t> data;
        for (int row = 0; row < p.rows; row++)
        {
            if (row % 4 == 0)
            {
                Put16(pattern + 2 * uint32_t(row / 4), uint16_t(data.size()));
            }
            for (const Cell& c : p.cells)
            {
                if (c.row != row)
                {
                    continue;
                }

                const uint8_t follow = uint8_t(c.channel | (c.note >= 0 ? 0x20 : 0) | (c.volume >= 0 ? 0x40 : 0) |
                                               (c.effect >= 0 ? 0x80 : 0));
                data.push_back(follow);
                if (c.note >= 0)
                {
                    data.push_back(uint8_t(c.note << 1 | (c.instrument >> 8)));
                    data.push_back(uint8_t(c.instrument));
                }
                if (c.volume >= 0)
                {
                    data.push_back(uint8_t(c.volume));
                }
                if (c.effect >= 0)
                {
                    data.push_back(uint8_t(c.effect));
                    data.push_back(uint8_t(c.op));
                }
            }
            data.push_back(0);
        }
        for (int i = (p.rows + 3) / 4; i < 16; i++)
        {
            Put16(pattern + 2 * uint32_t(i), Get16(pattern + 2 * uint32_t((p.rows - 1) / 4)));
        }
        Put8(pattern + kPatternRows, uint8_t(p.rows));
        for (size_t i = 0; i < data.size(); i++)
        {
            Put8(pattern + kPatternData + uint32_t(i), data[i]);
        }

        return (pattern + kPatternData + uint32_t(data.size()) + 3) & ~3u;
    }

    // Writes the player's tables: the waves of the vibrato, the periods of the notes from C-0, each octave's half the
    // last's, the fine tunes and the linear frequencies, with the values that detection looks for first. Then the
    // samples and their table.
    void WriteTables()
    {
        const double kPi = std::acos(-1.0);
        const int16_t kRandom[16] = {-28,  185,  162, -32, -161, 170,  -222, -186,
                                     -177, -193, -2,  232, -252, -133, 121,  -127};
        for (uint32_t i = 0; i < 64; i++)
        {
            Put16(kSineTable + 2 * i, uint16_t(int16_t(std::lround(255 * std::sin(2 * kPi * i / 64)))));
            Put16(kRampTable + 2 * i, uint16_t(int16_t(i < 32 ? 8 * i : 8 * (int(i) - 64))));
            Put16(kSquareTable + 2 * i, uint16_t(int16_t(i < 32 ? 255 : -255)));
            Put16(kRandomTable + 2 * i, uint16_t(i < 16 ? kRandom[i] : int16_t(i * 7)));
        }

        const uint16_t kPeriods[12] = {27392, 25855, 24403, 23034, 21741, 20521,
                                       19369, 18282, 17256, 16287, 15373, 14510};
        for (uint32_t n = 0; n < 120; n++)
        {
            Put16(kPeriodTable + 2 * n, uint16_t(kPeriods[n % 12] >> (n / 12)));
        }

        const uint16_t kFineTunes[8] = {34716, 34654, 34591, 34529, 34467, 34405, 34343, 34281};
        for (uint32_t i = 0; i < 64; i++)
        {
            Put16(kFineTuneTable + 2 * i,
                  i < 8 ? kFineTunes[i] : uint16_t(std::lround(32768 * std::pow(2.0, (32.0 - i) / 32 / 12))));
        }

        const uint32_t kLinear[6] = {1096155136, 1097144902, 1098135561, 1099127114, 1100119563, 1101112908};
        for (uint32_t i = 0; i < 768; i++)
        {
            Put32(kLinearTable + 4 * i,
                  i < 6 ? kLinear[i] : uint32_t(std::lround(1096155136 * std::pow(2.0, i / 768.0))));
        }

        // Each sample's header: its loop's length, its end, its rate at C-4, fine tune, relative note, volume, pan,
        // loop and interpolation, then its points.
        struct TestSample
        {
            uint32_t points;
            uint32_t loop_length;
            uint8_t loop;
            uint8_t volume;
        };
        const TestSample kSamples[3] = {
            {kForwardPoints, kForwardLoop, 1, 64}, {kOneShotPoints, 0, 0, 48}, {kBidiPoints, kBidiLoop, 2, 40}};
        uint32_t at = kSampleData;
        for (uint32_t s = 0; s < 3; s++)
        {
            Put32(kSampleTable + 4 * s, at);
            Put32(at, kSamples[s].loop_length);
            Put32(at + 4, at + 0x12 + kSamples[s].points);
            Put32(at + 8, 8363);
            Put8(at + 0xE, kSamples[s].volume);
            Put8(at + 0x10, kSamples[s].loop);
            for (uint32_t i = 0; i < kSamples[s].points; i++)
            {
                Put8(at + 0x12 + i, uint8_t(128 + 100 * std::sin(2 * kPi * i / 16)));
            }
            at = (at + 0x12 + kSamples[s].points + 3) & ~3u;
        }
    }

    // Writes the patterns of krapPlay(), the loads of the tables in the row's routine, the timers' setup,
    // getDmaAddress(), kragInit(), kramQualityMode() and kramSetMasterVol(), the game's calls of the last three, and
    // the version string.
    void WriteCode()
    {
        PutHalfwords(kPlay,
                     {0xB5F0, 0x4657, 0x464E, 0x4645, 0xB4E0, 0x1C0F, 0x4680, 0x4692, 0x1C38, 0x2204, 0x4010, 0x2800});
        PutHalfwords(kTables,
                     {0x0407, 0x4907, 0x0BBA, 0x588F, 0x0071, 0x616F, 0x3902, 0x5A78, 0x4F04, 0x0082, 0x58B8, 0x6128});
        Put32(kTables + 0x20, kInstrumentTable);
        Put32(kTables + 0x24, kSampleTable);
        PutHalfwords(kTimer, {0x4800, 0x4900, 0x4B00, 0x8008, 0x3102, 0x4805, 0x800B, 0x3906, 0x8008});
        Put32(kTimer + 0x20, 0xFC00);
        PutHalfwords(kFrame, {0x4900, 0x238A, 0x0058, 0x680B, 0x181A, 0x600A});
        PutHalfwords(kInit, {0xB510, 0x1C04, 0x2080, 0xF000, 0xF93B, 0x1C20, 0xF000, 0xF800, 0xF000, 0xF800});
        PutHalfwords(kQuality, {0xB530, 0x2210, 0x4002, 0x2A00, 0xD000, 0x4900, 0x2301, 0x700B});
        PutHalfwords(kMaster, {0xB5F0, 0x1C04, 0x0C20, 0x2800, 0xD000, 0x4900});

        // kragInit(1), kramSetMasterVol(0x40080) with the level from a literal, and kramQualityMode(0).
        Put16(kGameInit, 0x2001);
        PutBl(kGameInit + 2, kInit);
        Put16(kGameInit + 6, 0x4806);
        Put32(kGameInit + 0x20, 0x40080);
        PutBl(kGameInit + 8, kMaster);
        Put16(kGameInit + 0xC, 0x2000);
        PutBl(kGameInit + 0xE, kQuality);
        PutString(kVersion, "$Id: Krawall $Date: 2003/09/01 06:51:01 $");
    }

    uint32_t next_module_ = kModules;
    int listed_ = 0;
};

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, DriverOverrides(), info, error);

    return info;
}

// The test module: a note on each channel, a volume slide, a note from a point within its sample, a portamento, and a
// jump back to the start, which loops it every 5 rows.
std::vector<Cell> LoopingCells()
{
    return {
        {0, 0, 49, 1, 0x50, -1, 0},       {0, 1, 53, 2, -1, kPan, 0x20},     {1, 0, -1, 0, -1, kVolumeSlide, 0x04},
        {2, 1, 53, 2, -1, kOffset, 0x01}, {3, 0, -1, 0, -1, kPortaUp, 0x10}, {4, 0, -1, 0, -1, kJump, 0x00},
    };
}

// Detection finds the player by its code, its tables from the code and the values in them, the mixer's settings, the
// game's calls of the library's settings and the modules, in the order of the game's table and then the one it doesn't
// list. A count for the modules applies to the game's table, and leaves out the one it doesn't list. A game with the
// version string of another build gets an error that names the build.
void TestDetection()
{
    Cart cart;
    const uint32_t first = cart.Module(2, 5, LoopingCells());
    const uint32_t second = cart.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    const uint32_t unlisted = cart.Module(1, 4, {{0, 0, 37, 1, -1, -1, 0}}, false);
    const Rom rom = cart.ToRom();
    Cart other(false);
    other.PutString(kVersion, "$Id: Krawall $Id: version.h 8 2005-04-21 12:24:45Z seb $");
    const Rom other_rom = other.ToRom();
    const Rom blank = Cart(false).ToRom();
    DriverInfo info, counted_info, other_info, blank_info;
    std::string error, counted_error, other_error, blank_error;
    DriverOverrides one;
    one.module_count = 1;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);
    const bool counted = DetectDriver(rom, one, counted_info, counted_error);
    const bool found_other = DetectDriver(other_rom, DriverOverrides(), other_info, other_error);
    const bool found_blank = DetectDriver(blank, DriverOverrides(), blank_info, blank_error);

    SEQLIB_CHECK(found && error.empty());
    SEQLIB_CHECK_EQ(info.play, kPlay);
    SEQLIB_CHECK_EQ(info.samples, kSampleTable);
    SEQLIB_CHECK_EQ(info.instruments, kInstrumentTable);
    SEQLIB_CHECK_EQ(info.sine, kSineTable);
    SEQLIB_CHECK_EQ(info.periods, kPeriodTable);
    SEQLIB_CHECK_EQ(info.linear, kLinearTable);
    SEQLIB_CHECK_EQ(info.mix_rate, 16384);
    SEQLIB_CHECK_EQ(info.frame_samples, 276);
    SEQLIB_CHECK(info.stereo);
    SEQLIB_CHECK_EQ(info.quality, 0);
    SEQLIB_CHECK_EQ(info.master_volume, 0x40080);
    SEQLIB_CHECK_EQ(info.module_table, kModuleTable);
    SEQLIB_CHECK_EQ(info.table_count, 2);
    SEQLIB_CHECK(info.modules == std::vector<uint32_t>({first, second, unlisted}));
    SEQLIB_CHECK(counted && counted_info.modules == std::vector<uint32_t>({first}));
    SEQLIB_CHECK(!found_other && other_error ==
                                     "found Krawall, but not a build that agbsf2 supports ($Id: Krawall $Id: "
                                     "version.h 8 2005-04-21 12:24:45Z seb $): it reads the build of 2003/09/01");
    SEQLIB_CHECK(!found_blank && blank_error.empty());
}

// The player starts a note at the rate its period gives and the volume its sample or the volume column gives, a volume
// slide changes the mixer's volume on the ticks between rows, an offset starts a note within its sample, a pan sets the
// sides' volumes, and a portamento up lowers the period on the ticks between rows.
void TestPlayer()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    std::vector<MixChannel> first, second;
    std::vector<uint64_t> times;
    std::vector<int> periods;
    const auto record = [&](uint64_t time)
    {
        times.push_back(time);
        first.push_back(player.MixerOf(player.Channels()[0]));
        second.push_back(player.MixerOf(player.Channels()[1]));
        periods.push_back(player.Channels()[0].period);
    };
    player.OnTick(record);

    while (times.size() < 13)
    {
        player.Frame();
    }

    // The first tick comes a tick's samples after the start, and the rest a tick apart.
    SEQLIB_CHECK(player.Valid());
    SEQLIB_CHECK_EQ(times[0], kTickSamples);
    SEQLIB_CHECK_EQ(times[12] - times[0], 12 * kTickSamples);

    // Row 0: C-4 at volume 64, and E-4 at its sample's volume, 48, with a pan of -32: the left side gets three times
    // the right's level.
    SEQLIB_CHECK_EQ(first[0].inc, int32_t(kC4Rate << 2));
    SEQLIB_CHECK_EQ(first[0].volume, 64);
    SEQLIB_CHECK_EQ(second[0].volume, 48);
    SEQLIB_CHECK_EQ(second[0].pan, -32);
    SEQLIB_CHECK_EQ(second[0].left, (48 * 64 * 96) >> 13);
    SEQLIB_CHECK_EQ(second[0].right, (48 * 64 * 32) >> 13);

    // Row 1 slides the volume down by 4 on its second and third ticks.
    SEQLIB_CHECK_EQ(first[3].volume, 64);
    SEQLIB_CHECK_EQ(first[4].volume, 60);
    SEQLIB_CHECK_EQ(first[5].volume, 56);

    // Row 2 starts E-4 again 256 points into its sample.
    SEQLIB_CHECK(second[6].handle != second[5].handle);
    SEQLIB_CHECK_EQ(second[6].pos - second[6].start, 256);

    // Row 3 lowers the period by 64 on each of its ticks between rows, and the rate goes up with it.
    SEQLIB_CHECK_EQ(periods[9], 1712);
    SEQLIB_CHECK_EQ(periods[10], 1712 - 64);
    SEQLIB_CHECK_EQ(periods[11], 1712 - 128);
    SEQLIB_CHECK_EQ(first[11].inc, int32_t((14317456 / (1712 - 128)) << 2));
}

// The mixer plays a sample to its end, or to its loop's end and back to the loop's start, or with a loop back and
// forth, back to the loop's start and forwards again, and a silent channel moves on by its guess, which for a step of
// less than a point is a point a sample.
void TestMixer()
{
    Cart cart;
    cart.Module(4, 4,
                {{0, 0, 49, 1, -1, -1, 0}, {0, 1, 85, 2, -1, -1, 0}, {0, 2, 49, 3, -1, -1, 0}, {0, 3, 49, 2, 0x10}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    bool forward_in_loop = true, bidi_turned = false;
    uint32_t one_shot_status = 1;
    std::vector<uint32_t> silent_points;
    const auto check = [&](uint64_t)
    {
        const MixChannel& f = player.MixerOf(player.Channels()[0]);
        const MixChannel& b = player.MixerOf(player.Channels()[2]);
        const MixChannel& s = player.MixerOf(player.Channels()[3]);
        forward_in_loop = forward_in_loop && f.status == 1 && f.pos < f.end && f.pos >= f.start;
        bidi_turned = bidi_turned || b.inc < 0;
        one_shot_status = player.MixerOf(player.Channels()[1]).status;
        if (s.status == 1)
        {
            silent_points.push_back(s.pos - s.start);
        }
    };
    player.OnTick(check);

    for (int f = 0; f < 40; f++)
    {
        player.Frame();
    }

    SEQLIB_CHECK(forward_in_loop);
    SEQLIB_CHECK(bidi_turned);
    SEQLIB_CHECK_EQ(one_shot_status, 0);
    SEQLIB_CHECK(silent_points.size() >= 2 && silent_points[1] - silent_points[0] == kTickSamples);
}

// A module holds two songs separated by +++: the first plays pattern 0, with a note on C-4, and the second pattern 1,
// with a note an octave up. Each converts on its own in the player's song mode. At its end it goes back to its start,
// and it never plays the other song.
void TestSongs()
{
    Cart cart;
    const uint32_t module = cart.Module(1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 61, 1}}}});
    cart.Songs(module, {{0}, {1}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = test::g_temp.string();
    opt.base_name = "krawall_songs";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{module, 0}, {module, 1}}));
    SEQLIB_CHECK(first.ok && second.ok && !first.silent && !second.silent);
    std::vector<std::set<int>> keys;
    for (const SongReport* s : {&first, &second})
    {
        std::set<int> played;
        for (const auto& e : ReadMidi(s->midi_path).Find(0x90))
        {
            played.insert(e.bytes[1]);
        }
        keys.push_back(played);
    }
    SEQLIB_CHECK(keys[0].size() == 1 && keys[1].size() == 1);
    SEQLIB_CHECK(!keys[0].empty() && !keys[1].empty() && *keys[0].begin() + 12 == *keys[1].begin());
}

// Eight rows per quarter note give a tempo of 126 BPM, within the target range of 105 to 210 BPM. Each player tick is
// one MIDI tick, and the tempo comes from the sample count for eight rows. Each channel's notes are on its MIDI
// channel, on the keys their rates give, with a program for each sample and start offset. The five-row loop, from the
// first row back to it, plays twice; the volume slide becomes CC11 and the portamento bends upwards. SoundFont presets
// preserve sample offsets and C-4 rates, and bidirectional loops contain both the forward and reverse samples.
void TestConversion()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    cart.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}, {3, 0, -1, 0, -1, kJump, 0x00}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = test::g_temp.string();
    opt.base_name = "krawall";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport bidi = ConvertSong(rom, info, 1, opt, nullptr);
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    const test::Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const test::Sf2Records bidi_sf2 = ReadSf2(bidi.sf2_path);

    // The plan: a loop of 15 ticks from the start, played twice.
    constexpr int kBeatTicks = 8 * kSpeed;
    const double kTickSeconds = double(kTickSamples) / 16384;
    SEQLIB_CHECK(sum.ok && bidi.ok);
    SEQLIB_CHECK_EQ(sum.tracks, 2);
    SEQLIB_CHECK(std::fabs(sum.loop_start) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 15 * kTickSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 30 * kTickSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.bpm - 60.0 / (kBeatTicks * kTickSeconds)) < 1e-3);
    SEQLIB_CHECK_EQ(midi.division, kBeatTicks);
    const auto metas = midi.Find(0xFF);
    bool tempo = false, loop_start = false, loop_end = false;
    for (const auto& [tick, bytes] : metas)
    {
        tempo = tempo ||
                (bytes[1] == 0x51 && tick == 0 &&
                 ((bytes[3] << 16) | (bytes[4] << 8) | bytes[5]) == int(std::lround(kBeatTicks * kTickSeconds * 1e6)));
        loop_start = loop_start || (bytes[1] == 0x06 && tick == 0);
        loop_end = loop_end || (bytes[1] == 0x06 && tick == 15);
    }
    SEQLIB_CHECK(tempo && loop_start && loop_end);

    // The notes: C-4 on key 60 at the start of each pass, and E-4 on key 64, again 2 rows on from its point.
    const auto first_on = midi.Find(0x90);
    const auto second_on = midi.Find(0x91);
    SEQLIB_REQUIRE_EQ(first_on.size(), 2u);
    SEQLIB_CHECK(first_on[0].tick == 0 && first_on[0].bytes[1] == 60 && first_on[1].tick == 15);
    SEQLIB_REQUIRE_EQ(second_on.size(), 4u);
    SEQLIB_CHECK(second_on[0].bytes[1] == 64 && second_on[1].tick == 6);
    const auto programs = midi.Find(0xC1);
    SEQLIB_CHECK(programs.size() >= 2 && programs[0].bytes[1] != programs[1].bytes[1]);

    // The volume slide: CC11 for volumes 64, 60 and 56 on its channel.
    std::vector<int> expression;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == cc::kExpression && tick > 0 && tick < 6)
        {
            expression.push_back(bytes[2]);
        }
    }
    int cc10 = 0, cc11_at_60 = 0, cc11_at_56 = 0;
    LevelsToControllers((60 * 64 * 64) >> 13, (60 * 64 * 64) >> 13, cc10, cc11_at_60);
    LevelsToControllers((56 * 64 * 64) >> 13, (56 * 64 * 64) >> 13, cc10, cc11_at_56);
    SEQLIB_CHECK(expression == std::vector<int>({cc11_at_60, cc11_at_56}));

    // The portamento: a bend up on the first channel in row 3.
    bool bent_up = false;
    for (const auto& [tick, bytes] : midi.Find(0xE0))
    {
        bent_up = bent_up || (tick >= 9 && tick < 12 && (bytes[1] | (bytes[2] << 7)) > 8192);
    }
    SEQLIB_CHECK(bent_up);

    // The SoundFont: a preset for each sample and point, and the note from point 256 starting there.
    SEQLIB_CHECK_EQ(sf2.Count("phdr", 38), 4u);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf2));
    bool offset_zone = false;
    for (size_t z = 0; z + 1 < sf2.Count("ibag", 4); z++)
    {
        const auto gens = sf2.ZoneGens(z);
        offset_zone = offset_zone || (gens.count(sf2::kStartAddrsOffset) && gens.at(sf2::kStartAddrsOffset) == 256);
    }
    SEQLIB_CHECK(offset_zone);
    SEQLIB_CHECK_EQ(Le32(sf2.Record("shdr", 46, 0) + 36), kC4Rate);
    SEQLIB_CHECK_EQ(Le32(sf2.Record("shdr", 46, 0) + 28) - Le32(sf2.Record("shdr", 46, 0) + 20),
                    kForwardPoints - kForwardLoop);

    // The sample that loops back and forth plays its loop forwards and then backwards.
    const uint8_t* s = bidi_sf2.Record("shdr", 46, 0);
    SEQLIB_CHECK_EQ(Le32(s + 32) - Le32(s + 28), 2 * kBidiLoop);
    SEQLIB_CHECK_EQ(Le32(s + 32) - Le32(s + 20), kBidiPoints + kBidiLoop);
}

// Returns a MIDI file's time signatures, as the row each starts on, its numerator and its denominator's power of 2.
std::vector<std::vector<uint32_t>> TimeSignatures(const std::string& path)
{
    std::vector<std::vector<uint32_t>> signatures;
    for (const auto& [tick, bytes] : ReadMidi(path).Find(0xFF))
    {
        if (bytes[1] == 0x58)
        {
            signatures.push_back({uint32_t(tick / uint32_t(kSpeed)), bytes[3], bytes[4]});
        }
    }

    return signatures;
}

// Bars follow pattern boundaries, with a short bar when an order ends partway through one. At eight rows per quarter
// note, a 4/4 bar has 32 rows. A break ends the first order after 33 rows, so each pass starts with a one-row pickup.
// The 48-row patterns use 12/8: two bars per pattern, at two rows per eighth note.
void TestBars()
{
    Cart cart;
    cart.Module(1, {{64, {{1, 0, 49, 1}, {32, 0, -1, 0, -1, kBreak, 0x00}}},
                    {64, {{0, 0, 49, 1}, {32, 0, 49, 1}}},
                    {40, {{0, 0, 49, 1}, {32, 0, 49, 1}}}});
    cart.Module(1, 64, {{0, 0, 49, 1}});
    cart.Module(1, {{48, {{0, 0, 49, 1}}}, {48, {{0, 0, 49, 1}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = test::g_temp.string();
    opt.base_name = "krawall_bars";

    const SongReport cut = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport full = ConvertSong(rom, info, 1, opt, nullptr);
    const SongReport compound = ConvertSong(rom, info, 2, opt, nullptr);

    // Each pass starts with a one-row pickup, followed by full bars and a one-quarter-note bar at the end of the third
    // pattern. The unbroken 64-row pattern stays in 4/4. The 48-row patterns use 12/8, with four rows per quarter note.
    const std::vector<std::vector<uint32_t>> kPasses = {{0, 1, 5},   {1, 4, 2},   {129, 1, 2},
                                                        {137, 1, 5}, {138, 4, 2}, {266, 1, 2}};
    SEQLIB_CHECK(cut.ok && full.ok && compound.ok);
    SEQLIB_CHECK(TimeSignatures(cut.midi_path) == kPasses);
    SEQLIB_CHECK(TimeSignatures(full.midi_path) == std::vector<std::vector<uint32_t>>({{0, 4, 2}}));
    SEQLIB_CHECK(TimeSignatures(compound.midi_path) == std::vector<std::vector<uint32_t>>({{0, 12, 3}}));
    SEQLIB_CHECK_EQ(ReadMidi(compound.midi_path).division, 4 * kSpeed);
}

// The intro's row pans the channel to the left, and the loop plays a note on its first row and then pans the channel to
// the right, which it keeps when the module jumps back. The first pass's note plays on the left and the later passes'
// on the right, so the marked loop starts at the second pass, and the module plays the first pass once before it.
void TestLoopStartsAtSecondPass()
{
    Cart cart;
    cart.Module(1, {{1, {{0, 0, -1, 0, -1, kPan, 0x00}}},
                    {4, {{0, 0, 49, 1, -1, -1, 0}, {2, 0, -1, 0, -1, kPan, 0x80}, {3, 0, -1, 0, -1, kJump, 0x01}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = test::g_temp.string();
    opt.base_name = "krawall_second";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop runs from the second pass, after the intro's row and the first pass's four, for four rows. The file
    // plays the note in the first pass and in the loop's two passes.
    const double kRowSeconds = kSpeed * double(kTickSamples) / 16384;
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(std::fabs(sum.loop_start - 5 * kRowSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 9 * kRowSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 13 * kRowSeconds) < 1e-9);
    SEQLIB_REQUIRE_EQ(ReadMidi(sum.midi_path).Find(0x90).size(), 3u);
}

// A module can have up to 20 channels, and --tracks can name each of them. The channels from the 16th on share MIDI
// channels with the first ones.
void TestManyChannels()
{
    Cart cart;
    cart.Module(18, 4, {{0, 0, 49, 1}, {0, 17, 53, 2}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = test::g_temp.string();
    opt.base_name = "krawall_channels";
    ConvertSettings first_only = opt;
    first_only.track_mask = 1;
    ConvertSettings last_only = opt;
    last_only.track_mask = 1u << 17;
    ConvertSettings first_sixteen = opt;
    first_sixteen.track_mask = 0xFFFF;

    const SongReport every = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport chosen = InspectSong(rom, info, 0, first_only);
    const SongReport last = InspectSong(rom, info, 0, last_only);
    const SongReport sixteen = InspectSong(rom, info, 0, first_sixteen);

    SEQLIB_CHECK(every.ok && chosen.ok && last.ok && sixteen.ok);
    SEQLIB_CHECK_EQ(every.tracks, 2);
    SEQLIB_CHECK_EQ(chosen.tracks, 1);
    SEQLIB_CHECK_EQ(last.tracks, 1);
    SEQLIB_CHECK_EQ(sixteen.tracks, 1);

    // The 18th channel's E-4 is on key 64 of MIDI channel 3, after the 15 channels that the first 15 take, in each of
    // the loop's two passes.
    const auto notes = ReadMidi(every.midi_path).Find(0x92);
    SEQLIB_CHECK(notes.size() == 2 && notes[0].bytes[1] == 64 && notes[1].bytes[1] == 64);
}

// --dump lists the header, the orders and the rows, --trace gives the records that change each frame, and Music names
// Krawall.
void TestMusic()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    std::string error;
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::string dump = (test::g_temp / "krawall_dump.txt").string();
    const std::string trace = (test::g_temp / "krawall_trace.txt").string();

    const bool dumped = music && music->DumpSong(0, dump, error);
    std::FILE* out = test::OpenTempFile("krawall_trace.txt");
    std::vector<std::string> warnings;
    const bool traced = music && out && music->Trace(0, 3, out, warnings);
    if (out)
    {
        std::fclose(out);
    }
    const std::vector<uint8_t> dump_bytes = ReadAll(dump);
    const std::vector<uint8_t> trace_bytes = ReadAll(trace);
    const std::string dump_text(dump_bytes.begin(), dump_bytes.end());
    const std::string trace_text(trace_bytes.begin(), trace_bytes.end());

    SEQLIB_CHECK(music && music->Log().front() == "Krawall");
    SEQLIB_CHECK(music && music->SongCount() == 1);
    SEQLIB_CHECK(dumped && traced);
    SEQLIB_CHECK(dump_text.find("2 channels, 1 orders, restart at order 0, speed 3, tempo 125") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  0 | c0 C-4 i1 v64 | c1 E-4 i2 pan 20\n") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  4 | c0 jump 00\n") != std::string::npos);
    SEQLIB_CHECK(trace_text.rfind("0 g 4080", 0) == 0);
    SEQLIB_CHECK(trace_text.find("\n1 c0 ") != std::string::npos);
    SEQLIB_CHECK(trace_text.find("\n1 m0 ") != std::string::npos);
}

// A sample of 20 points that loops forwards over its last 6 gets its loop repeated 6 times: 36 points, the first length
// of kMinLoop points or more.
void TestShortLoop()
{
    Cart cart;
    constexpr uint32_t kHeader = kRomBase + 0xF000;
    cart.Put32(kHeader + kSampleLoopLength, 6);
    cart.Put32(kHeader + kSampleEnd, kHeader + kSamplePoints + 20);
    cart.Put32(kHeader + 8, 8363);
    cart.Put8(kHeader + kSampleLoop, 1);
    for (uint32_t i = 0; i < 20; i++)
    {
        cart.Put8(kHeader + kSamplePoints + i, uint8_t(i * 9));
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    InstrumentSet set;
    set.Program(kHeader, 0, 0);
    const Sf2File sf = set.Build(rom, info);
    SEQLIB_REQUIRE_EQ(sf.samples.size(), 1);
    SEQLIB_CHECK(test::LengthenedLoop(sf.samples[0], 14, 6));
}

} // namespace

void RunTests()
{
    TestDetection();
    TestPlayer();
    TestMixer();
    TestSongs();
    TestConversion();
    TestBars();
    TestLoopStartsAtSecondPass();
    TestManyChannels();
    TestMusic();
    TestShortLoop();
}

} // namespace agbsf2::krawall
