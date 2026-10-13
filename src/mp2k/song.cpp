// SPDX-License-Identifier: MIT

#include "mp2k/song.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace agbsf2::mp2k
{
namespace
{

// The lengths of waits and notes in ticks, from the driver's clock table.
constexpr uint8_t kClockTable[49] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52,
                                     54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90, 92, 96};

// The argument bytes of each extended command, or -1 for those that end the track.
constexpr int kXcmdArgs[kXcmdCount] = {-1, 4, 1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 4};

// Maximum number of commands ScanTrack() reads per track.
constexpr int kMaxTrackCommands = 1000000;

// The size of the memory area that kCmdMemAcc reads and writes: a byte offset reaches 256 bytes.
constexpr size_t kMemAccSize = 256;

// Returns true if kCmdMemAcc's conditional operation `op` jumps, for a byte `value` of the memory area and an operand
// `operand`.
bool MemAccJumps(uint8_t op, uint8_t value, uint8_t operand)
{
    switch (op % 6)
    {
    case 0:
        return value == operand;
    case 1:
        return value != operand;
    case 2:
        return value > operand;
    case 3:
        return value >= operand;
    case 4:
        return value <= operand;
    default:
        return value < operand;
    }
}

} // namespace

bool EndsTrack(uint8_t command)
{
    return command == kCmdFine || (command >= 0xB6 && command <= 0xB8) || command == 0xC6 || command == 0xC7 ||
           (command >= 0xC9 && command <= 0xCB);
}

int ClockLength(int index)
{
    return index >= 0 && index < 49 ? kClockTable[index] : 0;
}

bool ReadSongHeader(const Rom& rom, uint32_t address, SongHeader& header)
{
    header = SongHeader();
    if (!rom.Contains(address, 4))
    {
        return false;
    }

    header.address = address;
    header.track_count = rom.U8(address);
    header.priority = rom.U8(address + 2);
    header.reverb = rom.U8(address + 3);
    if (header.track_count == 0)
    {
        return true;
    }

    header.voices = rom.U32(address + 4);
    if (header.track_count > 16 || !rom.Contains(address, 8 + 4 * uint32_t(header.track_count)) ||
        !rom.Contains(header.voices, 12))
    {
        return false;
    }

    for (int t = 0; t < header.track_count; t++)
    {
        const uint32_t track = rom.U32(address + 8 + 4 * uint32_t(t));
        if (!rom.Contains(track))
        {
            return false;
        }

        header.tracks.push_back(track);
    }

    return true;
}

bool ReadVoice(const Rom& rom, uint32_t address, Voice& voice)
{
    voice = Voice();
    if (!rom.Contains(address, 12))
    {
        return false;
    }

    voice.address = address;
    voice.type = rom.U8(address);
    voice.key = rom.U8(address + 1);
    voice.length = rom.U8(address + 2);
    voice.pan_sweep = rom.U8(address + 3);
    voice.wave = rom.U32(address + 4);
    voice.attack = rom.U8(address + 8);
    voice.decay = rom.U8(address + 9);
    voice.sustain = rom.U8(address + 10);
    voice.release = rom.U8(address + 11);
    voice.key_table = rom.U32(address + 8);

    return true;
}

uint32_t SplitVoice(const Rom& rom, const Voice& split, int key)
{
    // A key split picks its voice from its key table, and a drum kit plays voice `key`.
    uint32_t index = uint32_t(key) & 0x7F;
    if (split.type & kVoiceKeySplit)
    {
        if (!rom.Contains(split.key_table + index))
        {
            return 0;
        }

        index = rom.U8(split.key_table + index);
    }

    const uint32_t address = split.wave + 12 * index;
    return rom.Contains(address, 12) ? address : 0;
}

bool ReadWave(const Rom& rom, uint32_t address, Wave& wave)
{
    wave = Wave();
    if (!rom.Contains(address, 16))
    {
        return false;
    }

    wave.address = address;
    wave.type = rom.U16(address);
    wave.loops = (rom.U8(address + 3) & 0xC0) != 0;
    wave.frequency = rom.U32(address + 4);
    wave.loop_start = rom.U32(address + 8);
    wave.size = rom.U32(address + 12);

    // A compressed sample stores each block of 64 points in 33 bytes.
    const uint64_t bytes = wave.type == 0 ? uint64_t(wave.size) : (uint64_t(wave.size) + 63) / 64 * 33;
    return bytes <= 0x2000000 && rom.Contains(wave.Data(), uint32_t(bytes));
}

bool DecodeEvent(const Rom& rom, uint32_t address, uint8_t running, Event& event)
{
    event = Event();
    if (!rom.Contains(address))
    {
        return false;
    }

    // A byte below 0x80 repeats the last command from kCmdVoice up, with that byte as its first argument.
    uint32_t at = address;
    const uint8_t first = rom.U8(address);
    if (first < 0x80)
    {
        if (running < kCmdVoice)
        {
            return false;
        }

        event.command = running;
    }
    else
    {
        event.command = first;
        at++;
    }

    // Find the number of argument bytes. For the commands whose arguments are optional, count the ones that are there.
    const uint8_t c = event.command;
    int args = 0;
    if (c >= kCmdTie || c == kCmdEot)
    {
        const int most = c == kCmdEot ? 1 : 3;
        while (args < most && rom.Contains(at + uint32_t(args)) && rom.U8(at + uint32_t(args)) < 0x80)
        {
            args++;
        }
    }
    else if (c <= kCmdFine || c == kCmdPend || EndsTrack(c))
    {
        args = 0;
    }
    else if (c == kCmdGoto || c == kCmdPatt)
    {
        args = 4;
    }
    else if (c == kCmdRept)
    {
        args = 5;
    }
    else if (c == kCmdMemAcc)
    {
        args = rom.U8(at) >= 6 && rom.U8(at) <= 17 ? 7 : 3;
    }
    else if (c == kCmdPort)
    {
        args = 2;
    }
    else if (c == kCmdXcmd)
    {
        const uint8_t x = rom.U8(at);
        if (x >= kXcmdCount)
        {
            return false;
        }

        args = 1 + (kXcmdArgs[x] > 0 ? kXcmdArgs[x] : 0);
    }
    else if (c >= kCmdPrio && c <= kCmdTune)
    {
        args = 1;
    }

    if (args > 0 && !rom.Contains(at, uint32_t(args)))
    {
        return false;
    }

    event.args = args;
    for (int i = 0; i < args && i < 4; i++)
    {
        event.arg[i] = rom.U8(at + uint32_t(i));
    }
    event.size = uint8_t(at + uint32_t(args) - address);

    if (c == kCmdGoto || c == kCmdPatt)
    {
        event.target = rom.U32(at);
    }
    else if (c == kCmdRept || (c == kCmdMemAcc && args == 7) || (c == kCmdXcmd && args == 5))
    {
        event.target = rom.U32(at + uint32_t(args - 4));
    }

    return true;
}

uint8_t RunningStatus(const Event& event, uint8_t running)
{
    return event.command >= kCmdVoice ? event.command : running;
}

std::string DescribeEvent(const Event& e)
{
    static const std::map<uint8_t, const char*> kNames = {
        {kCmdPrio, "PRIO"},   {kCmdKeySh, "KEYSH"}, {kCmdVoice, "VOICE"}, {kCmdVol, "VOL"},
        {kCmdPan, "PAN"},     {kCmdBend, "BEND"},   {kCmdBendR, "BENDR"}, {kCmdLfoS, "LFOS"},
        {kCmdLfoDl, "LFODL"}, {kCmdMod, "MOD"},     {kCmdModT, "MODT"},   {kCmdTune, "TUNE"}};
    static const char* const kXcmdNames[kXcmdCount] = {"xxx",   "xWAVE", "xTYPE", "xxx",   "xATTA", "xDECA", "xSUST",
                                                       "xRELE", "xIECV", "xIECL", "xLENG", "xSWEE", "xWAIT", "x0D"};

    char b[96];
    const uint8_t c = e.command;
    if (c >= kCmdWait && c <= 0xB0)
    {
        std::snprintf(b, sizeof b, "W%02d", ClockLength(c - kCmdWait));
    }
    else if (c >= kCmdTie)
    {
        std::snprintf(b, sizeof b, "N%02d", ClockLength(c - kCmdTie));
        std::string text = c == kCmdTie ? "TIE" : b;
        const char* const kParts[3] = {" key ", " vel ", " gate+"};
        for (int i = 0; i < e.args; i++)
        {
            text += kParts[i] + std::to_string(e.arg[i]);
        }

        return text;
    }
    else if (c == kCmdEot)
    {
        return e.args ? "EOT key " + std::to_string(e.arg[0]) : "EOT";
    }
    else if (c == kCmdGoto || c == kCmdPatt)
    {
        std::snprintf(b, sizeof b, "%s 0x%08X", c == kCmdGoto ? "GOTO" : "PATT", unsigned(e.target));
    }
    else if (c == kCmdRept)
    {
        std::snprintf(b, sizeof b, "REPT %u 0x%08X", unsigned(e.arg[0]), unsigned(e.target));
    }
    else if (c == kCmdMemAcc)
    {
        std::snprintf(b, sizeof b, "MEMACC op %u byte %u value %u", unsigned(e.arg[0]), unsigned(e.arg[1]),
                      unsigned(e.arg[2]));
        if (e.args == 7)
        {
            char jump[32];
            std::snprintf(jump, sizeof jump, " jump 0x%08X", unsigned(e.target));

            return std::string(b) + jump;
        }
    }
    else if (c == kCmdTempo)
    {
        std::snprintf(b, sizeof b, "TEMPO %u (%u BPM)", unsigned(e.arg[0]), unsigned(e.arg[0]) * 2);
    }
    else if (c == kCmdPort)
    {
        std::snprintf(b, sizeof b, "PORT register %02X value %02X", unsigned(e.arg[0]), unsigned(e.arg[1]));
    }
    else if (c == kCmdXcmd && e.args == 5)
    {
        // The 4-byte argument of kXcmdWave, a sample's address, or of kXcmdOffset, a point in the sample.
        std::snprintf(b, sizeof b, e.arg[0] == kXcmdWave ? "XCMD %s 0x%08X" : "XCMD %s %u", kXcmdNames[e.arg[0]],
                      unsigned(e.target));
    }
    else if (c == kCmdXcmd)
    {
        std::string text = std::string("XCMD ") + kXcmdNames[e.arg[0]];
        for (int i = 1; i < e.args; i++)
        {
            text += " " + std::to_string(e.arg[i]);
        }

        return text;
    }
    else if (c == kCmdPend)
    {
        return "PEND";
    }
    else if (EndsTrack(c))
    {
        if (c == kCmdFine)
        {
            return "FINE";
        }

        std::snprintf(b, sizeof b, "unused command %02X, which ends the track", unsigned(c));
    }
    else if (kNames.count(c))
    {
        std::snprintf(b, sizeof b, "%s %u", kNames.at(c), unsigned(e.arg[0]));
    }
    else
    {
        std::snprintf(b, sizeof b, "command %02X", unsigned(c));
    }

    return b;
}

TrackLayout ScanTrack(const Rom& rom, uint32_t address)
{
    TrackLayout layout;
    std::array<uint8_t, kMemAccSize> memory = {};
    std::array<uint32_t, 3> stack = {};
    int level = 0;
    int repeats = 0;
    uint8_t running = 0;
    uint64_t tick = 0;

    // The tick at which each command was first played, by its address and the patterns it was called from.
    std::map<std::tuple<uint32_t, int, uint32_t>, uint64_t> first;

    // The keys of the ties that no end of tie has released yet, and the track's last key, which a note, tie or end of
    // tie without one uses.
    std::multiset<uint8_t> ties;
    uint8_t key = 0;

    for (int n = 0; n < kMaxTrackCommands; n++)
    {
        const auto at = std::make_tuple(address, level, level ? stack[size_t(level - 1)] : 0);
        first.emplace(at, tick);

        Event e;
        if (!DecodeEvent(rom, address, running, e) || EndsTrack(e.command))
        {
            break;
        }

        running = RunningStatus(e, running);
        address += e.size;
        const uint8_t c = e.command;
        if (c >= kCmdWait && c <= 0xB0)
        {
            tick += uint64_t(ClockLength(c - kCmdWait));
            continue;
        }
        if (c == kCmdXcmd && e.arg[0] == kXcmdWait)
        {
            tick += uint64_t(e.arg[1] | (e.arg[2] << 8));
            continue;
        }
        if (c == kCmdXcmd && kXcmdArgs[e.arg[0]] < 0)
        {
            break;
        }

        // A command other than a wait plays at this tick, and a note of a set length lasts its length plus its third
        // argument.
        const uint64_t length = c > kCmdTie ? uint64_t(ClockLength(c - kCmdTie) + (e.args >= 3 ? e.arg[2] : 0)) : 0;
        layout.last_command = std::max(layout.last_command, tick + length);

        // A note, tie or end of tie can give the track a new key. An end of tie releases one of the ties on its key, as
        // the driver releases the newest note it's playing there.
        if ((c >= kCmdTie || c == kCmdEot) && e.args >= 1)
        {
            key = e.arg[0];
        }
        if (c == kCmdTie)
        {
            ties.insert(key);
        }
        else if (c == kCmdEot && ties.count(key))
        {
            ties.erase(ties.find(key));
        }

        if (c == kCmdPatt)
        {
            if (level >= 3)
            {
                break;
            }

            stack[size_t(level++)] = address;
            address = e.target;
        }
        else if (c == kCmdPend && level > 0)
        {
            address = stack[size_t(--level)];
        }
        else if (c == kCmdGoto || (c == kCmdRept && e.arg[0] == 0) ||
                 (c == kCmdMemAcc && e.args == 7 &&
                  MemAccJumps(e.arg[0], memory[e.arg[1]], e.arg[0] >= 12 ? memory[e.arg[2]] : e.arg[2])))
        {
            // An unconditional jump back to a command played before is the track's loop.
            const auto found = first.find(std::make_tuple(e.target, level, level ? stack[size_t(level - 1)] : 0));
            if (c != kCmdMemAcc && found != first.end())
            {
                layout.loops = true;
                layout.loop_start = found->second;
                layout.loop_end = tick;
                return layout;
            }

            address = e.target;
        }
        else if (c == kCmdRept && ++repeats < e.arg[0])
        {
            address = e.target;
        }
        else if (c == kCmdRept)
        {
            repeats = 0;
        }
        else if (c == kCmdMemAcc && e.arg[0] < 6)
        {
            // Operations 0-2 combine the byte with the value, and 3-5 with another byte of the area.
            uint8_t& byte = memory[e.arg[1]];
            const uint8_t value = e.arg[0] >= 3 ? memory[e.arg[2]] : e.arg[2];
            byte = e.arg[0] % 3 == 0 ? value : e.arg[0] % 3 == 1 ? uint8_t(byte + value) : uint8_t(byte - value);
        }
    }

    // The track's end releases the notes it's still playing, which cuts a note short, or ends a tie.
    layout.end = tick;
    layout.last_command = ties.empty() ? std::min(layout.last_command, tick) : tick;

    return layout;
}

} // namespace agbsf2::mp2k
