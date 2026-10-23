// SPDX-License-Identifier: MIT

#include "rare/driver.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "arm.h"
#include "rare/song.h"
#include "rom.h"
#include "thumb.h"

namespace agbsf2::rare
{
namespace
{

// Returns an approximation of the driver's fade table, for a ROM in which detection doesn't find the table. A decay or
// release with index i takes entry i + 1 frames, or none if the entry is 0. The entries fall by about 10% an index from
// 254 at index 0 to 88 at index 10, then by about 3% an index to 7 at index 90, and then in a straight line to 0 at
// index 99, which follows the driver's table to within about 10%. It's worked out in 16.16 fixed point, so it's the
// same everywhere.
std::array<uint8_t, 100> ApproximateFadeTable()
{
    std::array<uint8_t, 100> table = {};
    uint32_t entry = 254u << 16;
    for (uint32_t i = 0; i < 100; i++)
    {
        if (i >= 90)
        {
            entry = (7 * (99 - i) << 16) / 9;
        }

        table[i] = uint8_t((entry + 0x8000) >> 16);
        entry = uint32_t((uint64_t(entry) * (i < 10 ? 58945u : 63495u)) >> 16);
    }

    return table;
}

// A mix rate that the driver's rate routine knows: the samples it mixes a frame and the pitch scale it uses.
struct RateSetting
{
    int rate;
    int samples_per_frame;
    uint32_t scale;
};

// The rates that the rate routine knows.
constexpr RateSetting kRateSettings[] = {
    {10512, 176, 0x018F0064}, {13379, 224, 0x01397FC3}, {18157, 304, 0x00E70086}, {21024, 352, 0x00C78032}};

// The rate that detection assumes where the init sets a rate that the rate routine doesn't know.
constexpr const RateSetting& kAssumedRate = kRateSettings[1];

// Maximum number of tunes to read during detection.
constexpr int kMaxTunes = 1024;

// The ARM instructions that detection looks for in the driver's code, which the init copies to IWRAM.
constexpr uint32_t kReadCommand = 0xE4DB0001;  // ldrb r0, [fp], #1
constexpr uint32_t kDispatch = 0xE79C0100;     // ldr r0, [ip, r0, lsl #2]
constexpr uint32_t kChannelShift = 0xE1A01220; // lsr r1, r0, #4
constexpr uint32_t kCommandMask = 0xE3C000F0;  // bic r0, r0, #0xF0
constexpr uint32_t kPitchIndex = 0xE0862103;   // add r2, r6, r3, lsl #2
constexpr uint32_t kSineIndex = 0xE0866101;    // add r6, r6, r1, lsl #2
constexpr uint32_t kFadeLookup = 0xE7D21001;   // ldrb r1, [r2, r1]
constexpr uint32_t kLdrPcMask = 0xFFFFF000;    // ldr rX, [pc, #imm12], without the offset
constexpr uint32_t kLdrR6Pc = 0xE59F6000;
constexpr uint32_t kLdrR4Pc = 0xE59F4000;
constexpr uint32_t kLdrR2Pc = 0xE59F2000;
constexpr uint32_t kLoadR4 = 0xE5944000;    // ldr r4, [r4]
constexpr uint32_t kCompareR2 = 0xE3520000; // cmp r2, #imm8, without the operand
constexpr uint32_t kFreeState = 0xE3A03010; // mov r3, #0x10, the state of a free slot
constexpr uint32_t kStoreR0 = 0xE5C03000;   // strb r3, [r0]
constexpr uint32_t kScaleR4 = 0xE0040493;   // mul r4, r3, r4
constexpr uint32_t kShiftR4 = 0xE1A043A4;   // lsr r4, r4, #7
constexpr uint32_t kTestR4 = 0xE3540000;    // cmp r4, #0

// The size of a note slot in the driver's RAM.
constexpr uint32_t kSlotSize = 0x28;

// Any branch, and any branch taken when the last comparison was equal.
constexpr ArmPattern kAnyBranch = {0xEA000000, 0xFF000000};
constexpr ArmPattern kAnyBranchIfEqual = {0x0A000000, 0xFF000000};

// Donkey Kong Country 3's routine that fades Enchanted Riverbank between its arrangements, at 0x08085CE0. The game
// calls it as a level starts and in every frame of the level. While tune 12 plays, the routine works out the state in
// r8: 0 in the player's swimming states, and 1 back on land or without a Kong. It then picks the channels to fade out
// and those to fade in: mov r1, r8; cmp r1, #0; beq; movs r2, #first out; movs r5, #last out; movs r0, #first in; mov
// ip, r0; movs r3, #last in; b. The beq goes to the same moves for the state under water, with r1 in place of r0. The
// routine checks the tune before that: ldr r0, [r0]; cmp r0, #tune; bne; movs r0, #1; mov r8, r0.
constexpr const char* kWaterAbovePattern = "4641 2900 D0xx 22xx 25xx 20xx 4684 23xx E0xx";
constexpr const char* kWaterUnderPattern = "22xx 25xx 21xx 468C 23xx";
constexpr const char* kWaterTunePattern = "6800 28xx D1xx 2001 4680";

// The furthest that the routine's check of the tune comes before its choice of channels, in bytes.
constexpr uint32_t kWaterTuneReach = 0x100;

// Returns true if the halfword is `ldr r<reg>, [pc, #imm]`.
bool IsLdrLiteral(uint16_t h, int reg)
{
    return (h >> 8) == (0x48 | reg);
}

// Returns true if a Thumb BL instruction starts at `at`.
bool IsBl(const Rom& rom, uint32_t at)
{
    return (rom.U16(at) & 0xF800) == 0xF000 && (rom.U16(at + 2) & 0xF800) == 0xF800;
}

// Returns true if the Thumb code at `at` matches `pattern`.
bool MatchesThumb(const Rom& rom, uint32_t at, const std::vector<ThumbPattern>& pattern)
{
    for (size_t k = 0; k < pattern.size(); k++)
    {
        const uint32_t address = at + 2 * uint32_t(k);
        if (!rom.Contains(address, 2) || (rom.U16(address) & pattern[k].mask) != pattern[k].value)
        {
            return false;
        }
    }

    return true;
}

// Returns the channels from `first` to `last` as a mask, or 0 if they aren't a range of channels.
uint16_t ChannelRange(uint32_t first, uint32_t last)
{
    if (first > last || last >= 16)
    {
        return 0;
    }

    return uint16_t((2u << last) - (1u << first));
}

// Returns the channels of the Thumb moves at `first` and `last`: movs rX, #channel.
uint16_t MovedRange(const Rom& rom, uint32_t first, uint32_t last)
{
    return ChannelRange(rom.U16(first) & 0xFFu, rom.U16(last) & 0xFFu);
}

// Finds the game's routine that fades a tune between two arrangements (see kWaterAbovePattern). Returns the tune and
// the channels of each arrangement from the routine's code, or an empty WaterTune if the game has none.
WaterTune FindWaterTune(const Rom& rom)
{
    const std::vector<ThumbPattern> under_moves = ParseThumbPattern(kWaterUnderPattern);
    const std::vector<ThumbPattern> tune_check = ParseThumbPattern(kWaterTunePattern);
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kWaterAbovePattern)))
    {
        // The beq at at + 4 goes to the moves for the state under water.
        const uint32_t under = at + 8 + uint32_t(int32_t(int8_t(rom.U16(at + 4) & 0xFF)) * 2);
        if (!MatchesThumb(rom, under, under_moves))
        {
            continue;
        }

        // Each state fades in the channels that the other fades out.
        const uint16_t above_in = MovedRange(rom, at + 10, at + 14);
        const uint16_t under_in = MovedRange(rom, under + 4, under + 8);
        if (!above_in || !under_in || (above_in & under_in) || MovedRange(rom, at + 6, at + 8) != under_in ||
            MovedRange(rom, under, under + 2) != above_in)
        {
            continue;
        }

        for (uint32_t back = 2; back <= kWaterTuneReach; back += 2)
        {
            if (MatchesThumb(rom, at - back, tune_check))
            {
                return {int(rom.U16(at - back + 2) & 0xFF), above_in, under_in};
            }
        }
    }

    return {};
}

// Returns a range of channels from FindWaterTune() as text: "0-3", or "4" for one channel. The range can't be empty.
std::string ChannelsText(uint16_t channels)
{
    const int first = std::countr_zero(channels);
    const int last = 15 - std::countl_zero(channels);

    return first == last ? std::to_string(first) : std::to_string(first) + "-" + std::to_string(last);
}

// The driver's init, as found from its copy of the driver's code to IWRAM.
struct InitCode
{
    uint32_t start = 0; // the routine's first instruction
    uint32_t copy = 0;  // the copy's first instruction
    uint32_t code = 0;  // the code it copies, in ROM
    uint32_t code_size = 0;
};

// Finds the init's copy of the code: ldr r1, =source; ldr r2, =destination; ldr r3, =&size; ldr r3, [r3];
// ldr r4, =limit; cmp r3, r4. Returns false if there's none.
bool FindInitCopy(const Rom& rom, InitCode& init)
{
    for (uint32_t at : FindThumb(rom, ParseThumbPattern("49xx 4Axx 4Bxx 681B 4Cxx 42A3")))
    {
        const uint32_t code = ThumbLiteral(rom, at);
        const uint32_t size_at = ThumbLiteral(rom, at + 4);
        const uint32_t code_size = rom.U32(size_at);
        if (!rom.Contains(size_at, 4) || code_size < 0x100 || code_size > 0x8000 || !rom.Contains(code, code_size))
        {
            continue;
        }

        // The routine starts with push {r4-r7, lr} a little before the copy.
        uint32_t start = at;
        while (start > at - 0x100 && rom.U16(start) != 0xB5F0)
        {
            start -= 2;
        }

        init = {rom.U16(start) == 0xB5F0 ? start : at, at, code, code_size};
        return true;
    }

    return false;
}

// Reads the table setup that follows the copy: ldr r0, =tune table variable; ldr r1, =tune table; str r1, [r0]; the
// same for the sound effects; ldr r0, =mix rate; bl set rate; movs r0, #voices; bl set voices. Returns false if it
// isn't there.
bool ReadTableSetup(const Rom& rom, const InitCode& init, DriverInfo& info)
{
    for (uint32_t at = init.copy + 12; at < init.copy + 0x60; at += 2)
    {
        if (!IsLdrLiteral(rom.U16(at), 0) || !IsLdrLiteral(rom.U16(at + 2), 1) || rom.U16(at + 4) != 0x6001 ||
            !IsLdrLiteral(rom.U16(at + 6), 0) || !IsLdrLiteral(rom.U16(at + 8), 1) || rom.U16(at + 10) != 0x6001 ||
            !IsLdrLiteral(rom.U16(at + 12), 0) || !IsBl(rom, at + 14) || (rom.U16(at + 18) >> 8) != 0x20 ||
            !IsBl(rom, at + 20))
        {
            continue;
        }

        info.tune_table = ThumbLiteral(rom, at + 2);
        info.mix_rate = int(ThumbLiteral(rom, at + 12));
        info.voice_limit = rom.U16(at + 18) & 0xFF;

        return true;
    }

    return false;
}

// Finds the table that a `ldr rX, =table` just before the instruction `use` loads, in the driver's code. Returns 0 if
// there's none.
uint32_t FindArmTable(const Rom& rom, const InitCode& init, uint32_t load, uint32_t use)
{
    const ArmPattern pattern[] = {{load, kLdrPcMask}, {use}};
    const uint32_t at = FindArm(rom, init.code, init.code + init.code_size, pattern);
    return at ? ArmLiteral(rom, at) : 0;
}

// Returns true if the driver's code has the instructions of `pattern` one after another.
bool HasArmCode(const Rom& rom, const InitCode& init, std::initializer_list<ArmPattern> pattern)
{
    const std::span<const ArmPattern> instructions(pattern.begin(), pattern.size());
    return FindArm(rom, init.code, init.code + init.code_size, instructions) != 0;
}

// Reads the format and tables from the driver's code. Returns false if its command dispatch isn't there.
bool ReadDriverCode(const Rom& rom, const InitCode& init, DriverInfo& info)
{
    bool found = false;
    for (uint32_t at = init.code; at + 16 <= init.code + init.code_size; at += 4)
    {
        if (rom.U32(at) != kReadCommand)
        {
            continue;
        }

        if (rom.U32(at + 4) == kDispatch)
        {
            info.format = Format::kChannelByte;
            found = true;
            break;
        }
        if (rom.U32(at + 4) == kChannelShift && rom.U32(at + 8) == kCommandMask && rom.U32(at + 12) == kDispatch)
        {
            info.format = Format::kChannelNibble;
            found = true;
            break;
        }
    }
    if (!found)
    {
        return false;
    }

    // The note on reads the size of a channel's slots from a word in ROM: ldr r4, =size; ldr r4, [r4]; ldr r3, =slots;
    // add r3, r3, r4.
    const uint32_t channel_size_at = FindArmTable(rom, init, kLdrR4Pc, kLoadR4);
    const uint32_t channel_size = rom.U32(channel_size_at);
    if (rom.Contains(channel_size_at, 4) && channel_size % kSlotSize == 0 && channel_size >= kSlotSize &&
        channel_size <= 16 * kSlotSize)
    {
        info.slots_per_channel = int(channel_size / kSlotSize);
    }
    else
    {
        info.warnings.push_back("the number of notes a channel can play wasn't found, so 6 is assumed");
    }

    info.pitch_table = FindArmTable(rom, init, kLdrR6Pc, kPitchIndex);
    info.sine_table = FindArmTable(rom, init, kLdrR6Pc, kSineIndex);
    if (!rom.Contains(info.pitch_table - 64 * 4, 129 * 4) || !rom.Contains(info.sine_table, 257 * 4))
    {
        info.pitch_table = info.sine_table = 0;
        info.warnings.push_back(
            "the driver's pitch and vibrato tables weren't found, so --trace can't follow the "
            "voices' positions exactly");
    }

    const uint32_t fade = FindArmTable(rom, init, kLdrR2Pc, kFadeLookup);
    if (rom.Contains(fade, 100))
    {
        for (uint32_t i = 0; i < 100; i++)
        {
            info.fade_table[i] = rom.U8(fade + i);
        }
    }
    else
    {
        info.warnings.push_back("the driver's fade table wasn't found, so the envelopes' fades are approximate");
    }

    // The controller handler of Donkey Kong Country 3's revision compares the controller with 20 to 23 in turn:
    // cmp r2, #20; beq; cmp r2, #21; beq; and so on.
    info.envelope_controllers = HasArmCode(rom, init,
                                           {{kCompareR2 | kCtrlAttack},
                                            kAnyBranchIfEqual,
                                            {kCompareR2 | kCtrlDecay},
                                            kAnyBranchIfEqual,
                                            {kCompareR2 | kCtrlSustain},
                                            kAnyBranchIfEqual,
                                            {kCompareR2 | kCtrlRelease}});

    // When the routine that moves the notes on takes a sample past its end, it stores the free state in loop modes 3
    // and 1: mov r3, #0x10; strb r3, [r0]; b; mov r3, #0x10; strb r3, [r0]. The older revisions store mode 1's through
    // r8, which doesn't hold the slot's address.
    info.frees_loop_once = HasArmCode(rom, init, {{kFreeState}, {kStoreR0}, kAnyBranch, {kFreeState}, {kStoreR0}});

    // The mixer of Donkey Kong Country 3's revision scales a note's level by its channel's level, and leaves the note
    // out when that gives 0: mul r4, r3, r4; lsr r4, r4, #7; cmp r4, #0; beq.
    info.skips_silent_notes = HasArmCode(rom, init, {{kScaleR4}, {kShiftR4}, {kTestR4}, kAnyBranchIfEqual});

    return true;
}

// Returns how many tunes the table at `table` has: entries up to the first that isn't a tune header.
int CountTunes(const Rom& rom, uint32_t table)
{
    int count = 0;
    TuneHeader header;
    while (count < kMaxTunes && rom.Contains(table + 4u * uint32_t(count), 4) &&
           ReadTuneHeader(rom, rom.U32(table + 4u * uint32_t(count)), header))
    {
        count++;
    }

    return count;
}

// Returns the number of commands that every track of the table's tunes decodes to in `format`, up to each track's end,
// or -1 if a track has a command the format doesn't know or doesn't end within kMaxTrackCommands commands.
long long CommandsInFormat(const Rom& rom, uint32_t table, int count, Format format)
{
    long long total = 0;
    for (int t = 0; t < count; t++)
    {
        TuneHeader header;
        ReadTuneHeader(rom, rom.U32(table + 4u * uint32_t(t)), header);
        for (uint32_t address : header.tracks)
        {
            Event e;
            int n = 0;
            for (; n < kMaxTrackCommands; n++)
            {
                if (!DecodeEvent(rom, address, format, e))
                {
                    return -1;
                }
                if (e.command == kCmdEnd)
                {
                    break;
                }

                address += e.size;
            }
            if (n == kMaxTrackCommands)
            {
                return -1;
            }

            total += n;
        }
    }

    return total;
}

// Infers the command format from the tracks when the driver's code is unavailable. Every track must reach its end. If
// both formats pass, prefer the one that decodes more commands: the wrong format often reads an argument as an end.
bool GuessFormat(const Rom& rom, DriverInfo& info)
{
    const long long byte = CommandsInFormat(rom, info.tune_table, info.tune_count, Format::kChannelByte);
    const long long nibble = CommandsInFormat(rom, info.tune_table, info.tune_count, Format::kChannelNibble);
    if (byte < 0 && nibble < 0)
    {
        return false;
    }

    info.format = nibble > byte ? Format::kChannelNibble : Format::kChannelByte;
    info.warnings.push_back(std::string("the driver's code wasn't found; its tracks read as ") +
                            (info.format == Format::kChannelNibble ? "channel-in-command" : "channel-byte") +
                            " commands");

    return true;
}

} // namespace

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    info.fade_table = ApproximateFadeTable();

    // Find the init, and from it the tune table, the mixer settings and the driver's code.
    InitCode init;
    bool code = false;
    if (FindInitCopy(rom, init) && ReadTableSetup(rom, init, info))
    {
        info.init = init.start;
        code = ReadDriverCode(rom, init, info);
        if (!code)
        {
            info.warnings.push_back("the driver's init was found, but not its command reader");
        }
    }

    if (overrides.tune_table)
    {
        info.tune_table = overrides.tune_table;
    }
    if (info.tune_table == 0)
    {
        error.clear();
        return false;
    }

    info.tune_count = overrides.tune_count ? overrides.tune_count : CountTunes(rom, info.tune_table);
    if (info.tune_count == 0)
    {
        error = "the tune table at " + Hex(info.tune_table) + " has no valid tunes";
        return false;
    }

    if (!code && !GuessFormat(rom, info))
    {
        error = "the tracks of the tunes at " + Hex(info.tune_table) + " can't be read";
        return false;
    }

    // The rate routine knows four rates.
    bool known_rate = false;
    for (const RateSetting& r : kRateSettings)
    {
        if (r.rate == info.mix_rate)
        {
            info.samples_per_frame = r.samples_per_frame;
            info.rate_scale = r.scale;
            known_rate = true;
        }
    }
    if (!known_rate)
    {
        info.warnings.push_back("the init sets a mix rate of " + std::to_string(info.mix_rate) +
                                " Hz, which the driver doesn't know; " + std::to_string(kAssumedRate.rate) +
                                " Hz is assumed");
        info.mix_rate = kAssumedRate.rate;
        info.samples_per_frame = kAssumedRate.samples_per_frame;
        info.rate_scale = kAssumedRate.scale;
    }

    if (info.voice_limit < 1 || info.voice_limit > 101)
    {
        info.warnings.push_back("the init sets a limit of " + std::to_string(info.voice_limit) +
                                " voices; 8 is assumed");
        info.voice_limit = 8;
    }

    // Donkey Kong Country 3's code picks the arrangement of a tune by the water, through the channels' levels. Only
    // that revision of the driver has the levels.
    if (info.skips_silent_notes)
    {
        info.water = FindWaterTune(rom);
        if (info.water.tune >= info.tune_count)
        {
            info.water = WaterTune();
        }
    }

    // Report what was found.
    if (info.init)
    {
        info.log.push_back("driver init at " + Hex(info.init) + ", " +
                           (info.format == Format::kChannelNibble ? "channel in each command byte"
                                                                  : "channel in a byte after each command"));
        info.log.push_back("mixer: " + std::to_string(info.mix_rate) + " Hz, up to " +
                           std::to_string(info.voice_limit) + " voices, " + std::to_string(info.slots_per_channel) +
                           " notes a channel");
        if (info.envelope_controllers)
        {
            info.log.push_back("controllers 20-23 set a channel's attack, decay, sustain and release");
        }
        if (info.water.tune >= 0)
        {
            info.log.push_back("the game's code plays tune " + std::to_string(info.water.tune) + " on channels " +
                               ChannelsText(info.water.above) + " above water and " + ChannelsText(info.water.under) +
                               " under water");
        }
    }
    info.log.push_back("tune table at " + Hex(info.tune_table) + ": " + std::to_string(info.tune_count) + " tune" +
                       (info.tune_count == 1 ? "" : "s"));

    return true;
}

uint32_t TuneAddress(const Rom& rom, const DriverInfo& info, int tune)
{
    if (tune < 0 || tune >= info.tune_count)
    {
        return 0;
    }

    return rom.U32(info.tune_table + 4u * uint32_t(tune));
}

uint16_t SilencedChannels(const DriverInfo& info, int tune, bool under_water)
{
    if (tune != info.water.tune)
    {
        return 0;
    }

    return under_water ? info.water.above : info.water.under;
}

int FadeFrames(const DriverInfo& info, uint32_t index)
{
    return index < info.fade_table.size() ? FadeEntryFrames(info.fade_table[index]) : 0;
}

int FadeEntryFrames(uint32_t entry)
{
    return entry ? int(entry) + 1 : 0;
}

int FadeFrames(const DriverInfo& info, uint32_t index, uint8_t setting)
{
    return setting < 0x80 ? FadeEntryFrames(setting) : FadeFrames(info, index);
}

} // namespace agbsf2::rare
