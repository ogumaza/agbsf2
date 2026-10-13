// SPDX-License-Identifier: MIT

#include "brownie/song.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "files.h"

namespace agbsf2::brownie
{
namespace
{

// The most commands DumpSong() lists for a channel.
constexpr int kMaxListedCommands = 20000;

constexpr const char* kChannelNames[13] = {
    "square 1",     "square 2",  "wave",      "noise",     "effect square 1", "effect square 2", "effect wave",
    "effect noise", "samples 1", "samples 2", "samples 3", "samples 4",       "samples 5"};

// The Magical Vacation revision's sample channels: the music's on FIFO A and FIFO B, and the sound effects'.
constexpr const char* kVacationSampleNames[4] = {"samples 1", "samples 2", "effect samples 1", "effect samples 2"};

std::string Format(const char* format, int a, int b = 0)
{
    char text[160];
    std::snprintf(text, sizeof text, format, a, b);

    return text;
}

// Returns the command that byte `b` runs in revision `r`: in the Magical Vacation revision, E2-E9 are F2-F9 again and
// EF is FF.
uint8_t CommandOf(Revision r, uint8_t b)
{
    if (r != Revision::kMagicalVacation)
    {
        return b;
    }

    return b >= 0xE2 && b <= 0xE9 ? uint8_t(b + 0x10) : b == 0xEF ? uint8_t(0xFF) : b;
}

} // namespace

Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, int channel)
{
    Command cmd;
    const bool vacation = info.revision == Revision::kMagicalVacation;
    const uint8_t b = CommandOf(info.revision, rom.U8(address));
    const int arg = rom.U8(address + 1);
    if (b < 0x80)
    {
        cmd.waits = true;
        if ((channel & 3) == 3 && channel < 8)
        {
            cmd.text = Format("noise, NR43 %02X", ((b << 4) + (b >> 4)) & 0xFF);
        }
        else
        {
            cmd.text = Format("note %d", b);
        }
        return cmd;
    }
    if (b < 0xE0)
    {
        cmd.text = vacation ? Format("length %d", b & 0x7F) : Format("length %d of the set", b & 0x7F);
        return cmd;
    }

    // The commands with an argument. In the Magical Vacation revision, E0, EA and EE have none, and do nothing.
    static constexpr uint8_t kWithArgument[] = {0xE0, 0xE1, 0xE2, 0xEA, 0xEE, 0xF0, 0xF1, 0xF2,
                                                0xF3, 0xF4, 0xF6, 0xF8, 0xF9, 0xFA, 0xFC, 0xFD};
    for (uint8_t c : kWithArgument)
    {
        cmd.size = c == b ? 2 : cmd.size;
    }
    if (vacation && (b == 0xE0 || b == 0xEA || b == 0xEB || b == 0xEE))
    {
        cmd.size = 1;
        cmd.text = "nothing";
        return cmd;
    }

    switch (b)
    {
    case 0xE0:
        cmd.waits = true;
        cmd.text =
            Format("release with envelope %d (0x%08X)", arg, int(rom.U32(info.envelope_table + 4 * uint32_t(arg))));
        break;

    case 0xE1:
        cmd.text = Format(vacation ? "sample %d (0x%08X)" : "sample set %d (0x%08X)", arg,
                          int(rom.U32(info.sample_table + 4 * uint32_t(arg))));
        break;

    case 0xE2:
        cmd.text = arg ? "mix all 5 voices" : "mix voices 0 and 1 only, and turn channels 10-12 off";
        break;

    case 0xE3:
        cmd.text = "bring the sound back, fading in, and start sound 0x36";
        break;

    case 0xE8:
        cmd.text = "give the PSG channel back to the music";
        break;

    case 0xE9:
        cmd.text = "take the PSG channel from the music";
        break;

    case 0xEA:
        cmd.text = Format("pan: right %d, left %d", arg & 15, arg >> 4);
        break;

    case 0xEC:
        cmd.waits = true;
        cmd.text = vacation ? "rest (stops the FIFO's timer)" : "rest (silences the voice)";
        break;

    case 0xED:
        cmd.waits = true;
        cmd.text = "rest (turns the PSG channel off)";
        break;

    case 0xEE:
        cmd.text = Format("length set %d", arg);
        break;

    case 0xF0:
        cmd.text = Format("envelope %d (0x%08X)", arg, int(rom.U32(info.envelope_table + 4 * uint32_t(arg))));
        break;

    case 0xF1:
        cmd.text = Format("NRx1 %02X", arg);
        break;

    case 0xF2:
        cmd.text = Format("volume %d", arg);
        break;

    case 0xF3:
        cmd.text = Format("transpose %+d", int8_t(arg));
        break;

    case 0xF4:
        cmd.text = Format("repeat start, %d times", arg ? arg : 256);
        break;

    case 0xF5:
        cmd.text = "repeat end";
        break;

    case 0xF6:
        cmd.text = Format("call %d", arg);
        break;

    case 0xF7:
        cmd.text = "return";
        break;

    case 0xF8:
        cmd.text = Format("jump to %d", arg);
        break;

    case 0xF9:
        cmd.text = Format("detune +%d", arg);
        break;

    case 0xFA:
        cmd.text = Format("NR10 sweep %02X", arg);
        break;

    case 0xFB:
        cmd.text = "sweep off";
        break;

    case 0xFC:
        cmd.text = Format("wave %d (0x%08X)", arg, int(rom.U32(info.wave_table + 4 * uint32_t(arg))));
        break;

    case 0xFD:
        cmd.text = std::string("PSG pan: ") + (arg & 0x0F ? "right" : "") + (arg & 0x0F && arg & 0xF0 ? " and " : "") +
                   (arg & 0xF0 ? "left" : "") + (arg ? "" : "off");
        break;

    case 0xFE:
        cmd.waits = true;
        cmd.text = "hold";
        break;

    case 0xFF:
        cmd.end = true;
        cmd.text = "end";
        break;

    default:
        cmd.text = "nothing";
        break;
    }

    return cmd;
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    std::vector<SongChannel> channels;
    if (!ReadSong(rom, info, song, channels))
    {
        error = "invalid sound";
        return false;
    }

    std::string text;
    char line[320];
    std::snprintf(line, sizeof line,
                  "; %s (%s) sound %d\n; channels listed at 0x%08X\n; columns: address, frame, bytes, command\n"
                  "; each channel is listed as the driver plays it, repeats and calls included, up to its end or its "
                  "loop\n",
                  rom.Title().c_str(), rom.GameCode().c_str(), song,
                  unsigned(rom.U32(info.song_table + 4 * uint32_t(song))));
    text += line;

    const bool vacation = info.revision == Revision::kMagicalVacation;
    for (const SongChannel& ch : channels)
    {
        const int c = ch.channel;
        const char* name = vacation && c >= 8 ? kVacationSampleNames[c - 8] : kChannelNames[c];
        std::snprintf(line, sizeof line, "\n; ---- channel %d (%s) at 0x%08X ----\n", c, name, unsigned(ch.data));
        text += line;

        // The driver's state that the listing follows: the length of each note, its repeat and its call, which has one
        // place to return to, and the frame in which the channel first read each command.
        uint32_t address = ch.data;
        uint64_t frame = 1;
        int length = 0, length_set = 0;
        uint32_t loop_start = 0, return_to = 0;
        int loop_count = 0;
        std::map<uint32_t, uint64_t> listed;
        for (int n = 0; n < kMaxListedCommands; n++)
        {
            listed.emplace(address, frame);

            const Command cmd = DecodeCommand(rom, info, address, c);
            std::string bytes;
            for (uint32_t i = 0; i < cmd.size; i++)
            {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%02X ", rom.U8(address + i));
                bytes += hex;
            }

            std::string note = cmd.text;
            const uint8_t b = CommandOf(info.revision, rom.U8(address));
            const uint8_t arg = rom.U8(address + 1);
            if (b >= 0x80 && b < 0xE0)
            {
                length = rom.U8(info.length_table + uint32_t(b & 0x7F) + 16 * uint32_t(length_set));
                note += Format(": %d frames", length ? length : 256);
            }
            std::snprintf(line, sizeof line, "%08X %7llu  %-6s %s\n", unsigned(address),
                          static_cast<unsigned long long>(frame), bytes.c_str(), note.c_str());
            text += line;
            if (cmd.end)
            {
                break;
            }

            // Follow the command as the driver does.
            uint32_t next = address + cmd.size;
            switch (b)
            {
            case 0xE3:
                frame++;
                break;

            case 0xEE:
                // The Magical Vacation revision has one table of lengths, and its EE does nothing.
                length_set = vacation ? 0 : arg;
                break;

            case 0xF4:
                loop_count = arg;
                loop_start = next;
                break;

            case 0xF5:
                loop_count = (loop_count - 1) & 0xFF;
                next = loop_count ? loop_start : next;
                break;

            case 0xF6:
                return_to = next;
                next = rom.U32(ch.subroutines + 4 * uint32_t(arg));
                break;

            case 0xF7:
                next = return_to;
                break;

            case 0xF8:
                next = rom.U32(ch.subroutines + 4 * uint32_t(arg));
                break;

            default:
                break;
            }
            if (cmd.waits)
            {
                frame += uint64_t(length ? length : 256);
            }
            address = next;

            // A jump back to a command the channel has read is its loop.
            if (b == 0xF8 && listed.count(address))
            {
                std::snprintf(line, sizeof line, "; goes back to 0x%08X, frame %llu\n", unsigned(address),
                              static_cast<unsigned long long>(listed[address]));
                text += line;
                break;
            }
        }
    }

    const std::vector<uint8_t> data(text.begin(), text.end());
    return WriteFile(path, data, error);
}

} // namespace agbsf2::brownie
