// SPDX-License-Identifier: MIT

#include "brownie/driver.h"

#include <string>
#include <vector>

namespace supergbamidi::brownie
{
namespace
{

// The most entries detection reads from the song table.
constexpr int kMaxSongs = 4096;

// The word that ends a sound's list of channels.
constexpr uint32_t kEndOfChannels = 0xFF;

// How far from the start of the per-frame routine detection looks for its parts, and from a command's routine.
constexpr uint32_t kFrameReach = 0x1000;
constexpr uint32_t kCommandReach = 0x20;

// An ARM instruction to look for: its bits under `mask` have to equal `value`.
struct ArmPattern
{
    uint32_t value;
    uint32_t mask;
};

// Masks for an instruction with any operand of 8 bits, any immediate offset of 12 bits, and any branch offset.
constexpr uint32_t kAny8 = 0xFFFFFF00;
constexpr uint32_t kAny12 = 0xFFFFF000;
constexpr uint32_t kAnyBranch = 0xFF000000;
constexpr uint32_t kExact = 0xFFFFFFFF;

// The start of the per-frame routine: push {r4-r7}, or {r4-r8} in the Magical Vacation revision; mov r7, #0; ldr r1,
// =channels; ldrb r2, [r1]; tst r2, #1; beq; ldrb r2, [r1, #1]; sub r2, r2, #1; cmp r2, #0; beq. It counts down each
// channel's wait.
constexpr ArmPattern kFrameStart[] = {{0xE92D00F0, 0xFFFFFEFF}, {0xE3A07000, kExact}, {0xE59F1000, kAny12},
                                      {0xE5D12000, kExact},     {0xE3120001, kExact}, {0x0A000000, kAnyBranch},
                                      {0xE5D12001, kExact},     {0xE2422001, kExact}, {0xE3520000, kExact},
                                      {0x0A000000, kAnyBranch}};

// The loop over the channels: add r1, r1, #0x38; add r7, r7, #1; teq r7, #channels.
constexpr ArmPattern kChannelLoop[] = {{0xE2811038, kExact}, {0xE2877001, kExact}, {0xE3370000, kAny8}};

// The command dispatch: and r0, r0, #0x1f; ldr r3, =commands; ldr pc, [r3, r0, lsl #2].
constexpr ArmPattern kDispatch[] = {{0xE200001F, kExact}, {0xE59F3000, kAny12}, {0xE793F100, kExact}};

// The start of a sound: ldr r2, =songs; ldr r0, [r2, r1, lsl #2].
constexpr ArmPattern kSongLookup[] = {{0xE59F2000, kAny12}, {0xE7920101, kExact}};

// A length command: ldr r3, =lengths; ldrb r0, [r3, r0].
constexpr ArmPattern kLengthLookup[] = {{0xE59F3000, kAny12}, {0xE7D30000, kExact}};

// A PSG envelope's step: ldr r2, =control registers; ldr r2, [r2, r7, lsl #2]; strh r3, [r2]; ldr r2, =frequency
// registers; ldr r2, [r2, r7, lsl #2].
constexpr ArmPattern kRegisterLookup[] = {
    {0xE59F2000, kAny12}, {0xE7922107, kExact}, {0xE1C230B0, kExact}, {0xE59F2000, kAny12}, {0xE7922107, kExact}};

// A PSG note: ldr r2, =frequencies; lsl r0, r0, #1; ldrh r5, [r2, r0].
constexpr ArmPattern kFrequencyLookup[] = {{0xE59F2000, kAny12}, {0xE1A00080, kExact}, {0xE19250B0, kExact}};

// The start of a channel: ldr r3, =pans; ldrb r4, [r3, r1]!.
constexpr ArmPattern kPanLookup[] = {{0xE59F3000, kAny12}, {0xE7F34001, kExact}};

// A sample note in the Magical Vacation revision: ldr r2, =rates; lsl r0, r0, #1; ldrh r0, [r2, r0].
constexpr ArmPattern kRateLookup[] = {{0xE59F2000, kAny12}, {0xE1A00080, kExact}, {0xE19200B0, kExact}};

// Commands F0, E1 and FC: ldrb r0, [r2], #1; ldr r3, =table; then add r3, r3, r0, lsl #2 for the envelopes, ldr r0,
// [r3, r0, lsl #2] for the sample sets, or ldr r4, [r3, r0, lsl #2] for the waves.
constexpr ArmPattern kEnvelopeLookup[] = {{0xE4D20001, kExact}, {0xE59F3000, kAny12}, {0xE0833100, kExact}};
constexpr ArmPattern kSampleLookup[] = {{0xE4D20001, kExact}, {0xE59F3000, kAny12}, {0xE7930100, kExact}};
constexpr ArmPattern kWaveLookup[] = {{0xE4D20001, kExact}, {0xE59F3000, kAny12}, {0xE7934100, kExact}};

// The init's setting of Timer 0, which sets the FIFOs' rate: ldr r1, =reload; ldr r0, [r1]; strh r0, [r3, #0xa0].
constexpr ArmPattern kTimerSetting[] = {{0xE59F1000, kAny12}, {0xE5910000, kExact}, {0xE1C30AB0, kExact}};

// The commands whose routines hold the envelope, sample set and wave tables.
constexpr int kEnvelopeCommand = 0x10;
constexpr int kSampleCommand = 0x01;
constexpr int kWaveCommand = 0x1C;

// Returns true if `pattern` matches the words at `at`.
template <size_t N>
bool MatchesArm(const Rom& rom, uint32_t at, const ArmPattern (&pattern)[N])
{
    if (!rom.Contains(at, 4 * uint32_t(N)))
    {
        return false;
    }

    for (size_t i = 0; i < N; i++)
    {
        if ((rom.U32(at + 4 * uint32_t(i)) & pattern[i].mask) != pattern[i].value)
        {
            return false;
        }
    }

    return true;
}

// Returns the address of the first match of `pattern` from `from` up to `to`, or 0 if there's none.
template <size_t N>
uint32_t FindArm(const Rom& rom, uint32_t from, uint32_t to, const ArmPattern (&pattern)[N])
{
    for (uint32_t at = from; at + 4 * uint32_t(N) <= to; at += 4)
    {
        if (MatchesArm(rom, at, pattern))
        {
            return at;
        }
    }

    return 0;
}

// Returns the word that the ARM instruction `ldr rX, [pc, #imm12]` at `at` loads.
uint32_t ArmLiteral(const Rom& rom, uint32_t at)
{
    const uint32_t insn = rom.U32(at);
    const uint32_t offset = insn & 0xFFF;
    return rom.U32((insn & 0x00800000) ? at + 8 + offset : at + 8 - offset);
}

// Returns the table that the ldr at index `load` of the first match of `pattern` in the per-frame routine loads, or 0.
template <size_t N>
uint32_t FrameTable(const Rom& rom, uint32_t frame, const ArmPattern (&pattern)[N], int load)
{
    const uint32_t at = FindArm(rom, frame, frame + kFrameReach, pattern);
    return at ? ArmLiteral(rom, at + 4 * uint32_t(load)) : 0;
}

// Returns the table that the routine of command `command` loads with `pattern`, as its second instruction, or 0.
template <size_t N>
uint32_t CommandTable(const Rom& rom, uint32_t commands, int command, const ArmPattern (&pattern)[N])
{
    const uint32_t routine = rom.U32(commands + 4 * uint32_t(command));
    if (!rom.Contains(routine, kCommandReach))
    {
        return 0;
    }

    const uint32_t at = FindArm(rom, routine, routine + kCommandReach, pattern);
    return at ? ArmLiteral(rom, at + 4) : 0;
}

// Returns the number of entries of the song table at `table`: those up to the first whose list of channels isn't in the
// ROM or doesn't end.
int CountSongs(const Rom& rom, DriverInfo& info, uint32_t table)
{
    info.song_table = table;
    int count = 0;
    std::vector<SongChannel> channels;
    while (count < kMaxSongs && rom.Contains(table + 4 * uint32_t(count), 4) && ReadSong(rom, info, count, channels))
    {
        count++;
    }

    return count;
}

} // namespace

const char* RevisionName(Revision r)
{
    return r == Revision::kMagicalVacation ? "Magical Vacation" : "Sword of Mana";
}

int ChannelCount(Revision r)
{
    return r == Revision::kMagicalVacation ? 12 : 13;
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();

    // Find the per-frame routine, whose loop over the channels gives the revision, and the tables it and the command
    // routines read.
    const uint32_t end = kRomBase + uint32_t(rom.Size());
    info.frame_routine = FindArm(rom, kRomBase, end, kFrameStart);
    if (info.frame_routine)
    {
        const uint32_t frame = info.frame_routine;
        const uint32_t loop = FindArm(rom, frame, frame + kFrameReach, kChannelLoop);
        const uint32_t channels = loop ? rom.U32(loop + 8) & 0xFF : 0;
        if (channels == uint32_t(ChannelCount(Revision::kMagicalVacation)))
        {
            info.revision = Revision::kMagicalVacation;
        }
        else if (channels != uint32_t(ChannelCount(Revision::kSwordOfMana)))
        {
            info.frame_routine = 0;
        }
    }
    if (info.frame_routine)
    {
        const uint32_t frame = info.frame_routine;
        info.command_table = FrameTable(rom, frame, kDispatch, 1);
        info.song_table = FrameTable(rom, frame, kSongLookup, 0);
        info.length_table = FrameTable(rom, frame, kLengthLookup, 0);
        info.frequency_table = FrameTable(rom, frame, kFrequencyLookup, 0);
        info.pan_table = FrameTable(rom, frame, kPanLookup, 0);
        info.control_registers = FrameTable(rom, frame, kRegisterLookup, 0);
        info.frequency_registers = FrameTable(rom, frame, kRegisterLookup, 3);
        if (rom.Contains(info.command_table, 32 * 4))
        {
            info.envelope_table = CommandTable(rom, info.command_table, kEnvelopeCommand, kEnvelopeLookup);
            info.sample_table = CommandTable(rom, info.command_table, kSampleCommand, kSampleLookup);
            info.wave_table = CommandTable(rom, info.command_table, kWaveCommand, kWaveLookup);
        }

        // In the Magical Vacation revision, each sample note sets its FIFO's timer from the rate table. Otherwise the
        // init sets Timer 0 near the per-frame routine, and its count gives the FIFOs' rate.
        if (info.revision == Revision::kMagicalVacation)
        {
            info.rate_table = FrameTable(rom, frame, kRateLookup, 0);
        }
        else
        {
            const uint32_t timer =
                FindArm(rom, frame > kFrameReach ? frame - kFrameReach : kRomBase, frame, kTimerSetting);
            const uint32_t reload = timer ? ArmLiteral(rom, timer) : 0;
            if (rom.Contains(reload, 2) && rom.U16(reload) != 0)
            {
                info.point_cycles = 0x10000 - uint32_t(rom.U16(reload));
                info.mix_rate = int(kSecondCycles / info.point_cycles);
            }
            else
            {
                info.warnings.push_back("the FIFOs' rate wasn't found, so 16384 Hz is assumed");
            }
        }
    }

    if (overrides.song_table)
    {
        info.song_table = overrides.song_table;
    }
    if (!info.frame_routine && !overrides.song_table)
    {
        error.clear();
        return false;
    }

    const bool no_rates = info.revision == Revision::kMagicalVacation && !rom.Contains(info.rate_table, 2);
    const char* missing = !rom.Contains(info.command_table, 32 * 4) ? "command"
                          : !info.length_table                      ? "length"
                          : !info.frequency_table                   ? "frequency"
                          : !info.pan_table                         ? "pan"
                          : !info.control_registers                 ? "register"
                          : !info.envelope_table                    ? "envelope"
                          : !info.sample_table                      ? "sample set"
                          : !info.wave_table                        ? "wave"
                          : no_rates                                ? "rate"
                                                                    : nullptr;
    if (missing)
    {
        error = std::string("the driver's ") + missing + " table wasn't found";
        return false;
    }

    info.song_count = overrides.song_count ? overrides.song_count : CountSongs(rom, info, info.song_table);
    if (info.song_count == 0)
    {
        error = "the song table at " + Hex(info.song_table) + " has no sounds";
        return false;
    }

    // Report what was found.
    if (info.frame_routine)
    {
        info.log.push_back(std::string(RevisionName(info.revision)) + " revision of the driver, per-frame routine at " +
                           Hex(info.frame_routine));
    }
    if (info.revision == Revision::kMagicalVacation)
    {
        info.log.push_back("samples: one on each FIFO, at each note's rate, rate table " + Hex(info.rate_table));
    }
    else
    {
        info.log.push_back("mixer: " + std::to_string(info.mix_rate) + " Hz");
    }
    info.log.push_back("song table at " + Hex(info.song_table) + ": " + std::to_string(info.song_count) + " sound" +
                       (info.song_count == 1 ? "" : "s"));
    info.log.push_back("command table " + Hex(info.command_table) + ", length table " + Hex(info.length_table) +
                       ", frequency table " + Hex(info.frequency_table));
    info.log.push_back("envelopes " + Hex(info.envelope_table) + ", sample sets " + Hex(info.sample_table) +
                       ", waves " + Hex(info.wave_table));

    return true;
}

bool ReadSong(const Rom& rom, const DriverInfo& info, int song, std::vector<SongChannel>& channels)
{
    channels.clear();
    if (song < 0 || !rom.Contains(info.song_table + 4 * uint32_t(song), 4))
    {
        return false;
    }

    // A list of the channels that the sound uses: each one's number, sequence and table of subroutines, then 0xFF.
    const int count = ChannelCount(info.revision);
    uint32_t at = rom.U32(info.song_table + 4 * uint32_t(song));
    for (int i = 0; i <= count; i++)
    {
        if (!rom.Contains(at, 4))
        {
            return false;
        }
        if (rom.U32(at) == kEndOfChannels)
        {
            return true;
        }
        if (rom.U32(at) >= uint32_t(count) || !rom.Contains(at, 12) || !rom.Contains(rom.U32(at + 4)))
        {
            return false;
        }

        channels.push_back({int(rom.U32(at)), rom.U32(at + 4), rom.U32(at + 8)});
        at += 12;
    }

    return false;
}

} // namespace supergbamidi::brownie
