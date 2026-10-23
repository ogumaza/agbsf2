// SPDX-License-Identifier: MIT

#include "quintet/song.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "files.h"
#include "quintet/driver.h"
#include "rom.h"

namespace agbsf2::quintet
{
namespace
{

// The most commands DumpSong() lists for a channel.
constexpr int kMaxListedCommands = 20000;

// A song header's bytes: the song's length, and the offset of each channel's data.
constexpr uint32_t kHeaderSize = 2 + 2 * kChannels;

// The deepest repeat the driver keeps.
constexpr size_t kMaxRepeatDepth = 8;

// The names of the pitches, from C.
constexpr const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string Format(const char* format, int a, int b = 0, int c = 0, int d = 0)
{
    char text[160];
    std::snprintf(text, sizeof text, format, a, b, c, d);

    return text;
}

} // namespace

bool ReadSongHeader(const Rom& rom, uint32_t address, SongHeader& header)
{
    header = SongHeader();
    header.address = address;
    header.length = rom.U16(address) & ~1u;
    if (header.length < kHeaderSize || !rom.Contains(address, header.length))
    {
        return false;
    }

    for (uint32_t c = 0; c < header.channels.size(); c++)
    {
        const uint32_t offset = rom.U16(address + 2 + 2 * c);
        if (offset < kHeaderSize || offset >= header.length)
        {
            return false;
        }

        header.channels[c] = address + offset;
    }

    return true;
}

Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, int channel)
{
    Command cmd;
    const uint8_t b = rom.U8(address);
    const int arg = rom.U8(address + 1);
    const int sarg = int8_t(arg);
    const bool pcm = channel >= kPcmA;

    // A note: the low nibble is the pitch, 12 to play again or 13 to rest, and the high nibble picks the length.
    const int pitch = b & 15;
    const int length = b >> 4;
    if (pitch <= 13 && length <= 12)
    {
        const int value = rom.U8(info.length_table + uint32_t(length));
        cmd.note = true;
        cmd.ticks = value ? uint32_t(kWholeNote / value) : 0;
        const std::string len = Format("%d ticks (1/%d)", int(cmd.ticks), value);
        if (pitch <= 11)
        {
            cmd.text = std::string("note ") + kNoteNames[pitch] + ", " + len;
        }
        else if (pitch == 12)
        {
            cmd.text = (pcm ? "the sample at its own rate, " : "play again, ") + len;
        }
        else
        {
            cmd.text = "rest, " + len;
        }

        return cmd;
    }

    // The commands with arguments.
    struct Argument
    {
        uint8_t command;
        uint32_t size;
    };

    static constexpr Argument kSizes[] = {
        {0x0E, 2}, {0x1F, 2}, {0x2E, 2}, {0x2F, 2}, {0x3E, 2}, {0x4F, 2}, {0x5E, 2}, {0x7F, 2}, {0x8E, 2}, {0x8F, 2},
        {0x9E, 2}, {0x9F, 2}, {0xAE, 2}, {0xAF, 2}, {0xBE, 2}, {0xCE, 2}, {0xCF, 2}, {0xD0, 3}, {0xD2, 6}, {0xD5, 2},
        {0xD7, 2}, {0xDA, 2}, {0xDB, 2}, {0xDC, 2}, {0xDD, 2}, {0xDE, 4}, {0xDF, 4}, {0xE2, 6}, {0xE4, 2}, {0xE5, 2},
        {0xE6, 3}, {0xE7, 4}, {0xE8, 2}, {0xE9, 2}, {0xEA, 2}, {0xEC, 2}, {0xED, 2}, {0xF1, 2}};
    for (const Argument& a : kSizes)
    {
        if (a.command == b)
        {
            cmd.size = a.size;
        }
    }

    cmd.end = b == 0xFF;

    auto at = [&](uint32_t i)
    {
        return int(rom.U8(address + i));
    };

    switch (b)
    {
    case 0x0E:
        cmd.text = Format("sweep shift %d", arg);
        break;
    case 0x0F:
        cmd.text = "sweep up";
        break;
    case 0x1E:
        cmd.text = "sweep down";
        break;
    case 0x1F:
        cmd.text = Format("sweep time %d", arg);
        break;
    case 0x2E:
        cmd.text = arg == 0x63 ? "no sound length" : Format("sound length %d", arg);
        break;
    case 0x2F:
        cmd.text = Format("duty %d", arg);
        break;
    case 0x3E:
        cmd.text = Format("envelope step %d", arg);
        break;
    case 0x3F:
        cmd.text = "envelope up";
        break;
    case 0x4E:
        cmd.text = "envelope down";
        break;
    case 0x4F:
        cmd.text = Format("octave %d", arg);
        break;
    case 0x5E:
        cmd.text = Format("volume %d", arg);
        break;
    case 0x5F:
        cmd.text = "volume up";
        break;
    case 0x6E:
        cmd.text = "volume down";
        break;
    case 0x6F:
        cmd.text = "octave up";
        break;
    case 0x7E:
        cmd.text = "octave down";
        break;
    case 0x7F:
        cmd.text = Format(channel == kSquare1 ? "tempo %d, for every channel" : "tempo %d", arg);
        break;
    case 0x8E:
        cmd.text = Format("noise clock shift %d", arg);
        break;
    case 0x8F:
        cmd.text = Format("noise width %d", arg);
        break;
    case 0x9E:
        cmd.text = Format("noise divider %d", arg);
        break;
    case 0x9F:
        cmd.text = Format("wave size %d", arg);
        break;
    case 0xAE:
        cmd.text = Format("wave bank %d", arg);
        break;
    case 0xAF:
        cmd.text = Format("wave channel on %d", arg);
        break;
    case 0xBE:
        cmd.text = Format("wave 75%% volume %d", arg);
        break;
    case 0xBF:
        cmd.text = "loop point";
        break;
    case 0xCE:
        cmd.text = Format("PSG master volume, left: NR50 bits 0x%02X", arg);
        break;
    case 0xCF:
        cmd.text = Format("PSG master volume, right: NR50 bits 0x%02X", arg);
        break;
    case 0xD0:
        cmd.text = Format(pcm ? "FIFO outputs: keep SOUNDCNT_H bits 0x%02X00, set 0x%02X00"
                              : "PSG outputs: keep NR51 bits 0x%02X, set 0x%02X",
                          at(1), at(2));
        break;
    case 0xD1:
        cmd.text = "dot: the note before lasts half as long again";
        break;
    case 0xD2:
        cmd.text = Format("volume envelope: %d%% to %d%% over %d frames, then to %d%%", at(1), at(2), at(4), at(3)) +
                   Format(" over %d frames", at(5));
        break;
    case 0xD3:
        cmd.text = "volume envelope off";
        break;
    case 0xD4:
        cmd.text = "tie: the next note, if it has the same pitch, or else a slide to it";
        break;
    case 0xD5:
        cmd.text = Format("detune %d", pcm ? sarg * 10 : sarg);
        break;
    case 0xD6:
        cmd.text = "repeat from here";
        break;
    case 0xD7:
        // The driver's pass count wraps around. A count of 0 makes 256 passes.
        cmd.text = Format("play the repeat %d times", arg ? arg : 256);
        break;
    case 0xDA:
        cmd.text = Format("wave %d into bank %d", arg & 0x7F, arg >> 7);
        break;
    case 0xDB:
        cmd.text = arg ? Format("noise macro %d", arg) : "no noise macro";
        break;
    case 0xDC:
    case 0xDD:
        cmd.text = Format("sample %d", arg);
        break;
    case 0xDE:
        cmd.text = Format(channel == kWave ? "wave bank %d for %d frames of a note, then %d"
                                           : "duty %d for %d frames of a note, then %d",
                          at(2), at(1), at(3));
        break;
    case 0xDF:
        cmd.text = Format("LFO: after %d frames, a period of %d frames and a depth of %d", at(1), at(2), at(3));
        break;
    case 0xE0:
        cmd.text = "LFO off";
        break;
    case 0xE1:
    case 0xEB:
        cmd.text = "no duty or wave bank switch";
        break;
    case 0xE2:
        cmd.text = Format("pitch envelope: %d to %d over %d frames, then to %d", int8_t(at(1)), int8_t(at(2)), at(4),
                          int8_t(at(3))) +
                   Format(" over %d frames", at(5));
        break;
    case 0xE3:
        cmd.text = "pitch envelope off";
        break;
    case 0xE4:
        cmd.text = Format("transpose %d", sarg);
        break;
    case 0xE5:
        cmd.text = Format("slides take %d frames", arg);
        break;
    case 0xE6:
        cmd.text = Format("detune %d", int16_t(rom.U16(address + 1)));
        break;
    case 0xE7:
        // A range of 0, 0 and 0 keeps the start of the last range that the FIFO played since DC or DD.
        if (at(1) == 0 && at(2) == 0 && at(3) == 0)
        {
            cmd.text = "play the sample once, from the last range's start, or from its start after DC or DD";
        }
        else
        {
            cmd.text = at(3) == at(2)
                           ? Format("play %d%% to %d%% of the sample", at(1), at(2))
                           : Format("play %d%% to %d%% of the sample, then loop from %d%%", at(1), at(2), at(3));
        }
        break;
    case 0xE8:
        cmd.text = Format("SOUNDBIAS resolution %d", arg);
        break;
    case 0xE9:
        cmd.text = Format("second transpose %d", sarg);
        break;
    case 0xEA:
        cmd.text = Format("wave bank 0 for %d frames of a note, then 1", arg);
        break;
    case 0xEC:
        cmd.text = Format("LFO shape %d", arg);
        break;
    case 0xED:
        cmd.text = Format("LFO shape, with bits 0x%02X", arg);
        break;
    case 0xEE:
        cmd.text = "transpose up";
        break;
    case 0xEF:
        cmd.text = "transpose down";
        break;
    case 0xF0:
        cmd.text = !TraitsOf(info.revision).banks ? "nothing"
                   : channel == kNoise            ? "switch to the other bank of noise macros"
                                                  : "switch to the other bank of samples";
        break;
    case 0xF1:
        cmd.text = Format("volume %+d", sarg);
        break;
    case 0xFF:
        cmd.text = "end, or back to the loop point";
        break;
    default:
        cmd.text = "nothing";
        break;
    }

    return cmd;
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    SongHeader h;
    const uint32_t address = SongAddress(info, song);
    if (!address || !ReadSongHeader(rom, address, h))
    {
        error = "invalid song";
        return false;
    }

    std::string text;
    char line[320];
    const int effect = EffectIndex(info, song);
    const std::string name = effect >= 0 ? "song " + std::to_string(song) + ", sound effect " + std::to_string(effect)
                                         : "song " + std::to_string(song);
    std::snprintf(line, sizeof line,
                  "; %s (%s) %s\n; header 0x%08X: %u bytes\n; columns: address, tick, bytes, command\n"
                  "; each channel is listed as the driver plays it, repeats included, up to its end or its loop\n",
                  rom.Title().c_str(), rom.GameCode().c_str(), name.c_str(), unsigned(h.address), unsigned(h.length));
    text += line;

    static constexpr const char* kChannelNames[kChannels] = {"square 1", "square 2", "wave", "noise", "PCM A", "PCM B"};
    for (int c = 0; c < kChannels; c++)
    {
        std::snprintf(line, sizeof line, "\n; ---- channel %d (%s) at 0x%08X ----\n", c, kChannelNames[c],
                      unsigned(h.channels[size_t(c)]));
        text += line;

        uint32_t address = h.channels[size_t(c)];
        uint64_t tick = 0;
        uint32_t dot_ticks = 0;                        // the length of the note just before, which a dot adds half of
        int tie_pitch = -1;                            // the pitch of the note just before, which D4 can tie to
        std::vector<std::pair<uint32_t, int>> repeats; // each open repeat's start and the passes left
        for (int n = 0; n < kMaxListedCommands; n++)
        {
            // A dot or a tie only counts right after a note.
            Command cmd = DecodeCommand(rom, info, address, c);
            const uint8_t b = rom.U8(address);
            const uint8_t next = rom.U8(address + 1);
            const bool tie = b == 0xD4 && tie_pitch >= 0 && (next & 15) == tie_pitch && (next >> 4) <= 12;
            if (b == 0xD1 && dot_ticks == 0)
            {
                cmd.text = "nothing: a dot that doesn't follow a note";
            }
            else if (b == 0xD4)
            {
                cmd.text = tie ? "tie: the next note adds to this one" : "slide into the next note";
            }

            const std::string bytes = HexBytes(rom, address, cmd.size);
            std::snprintf(line, sizeof line, "0x%08X %8llu  %-18s %s\n", unsigned(address),
                          static_cast<unsigned long long>(tick), bytes.c_str(), cmd.text.c_str());
            text += line;
            if (cmd.end)
            {
                break;
            }

            // Follow the channel as the driver does: notes take up time, a dot after a note adds half of it again, and
            // a repeat goes back to its start until its passes run out.
            address += cmd.size;
            if (cmd.note)
            {
                tick += cmd.ticks;
                dot_ticks = cmd.ticks;
                tie_pitch = b & 15;
                continue;
            }
            if (b == 0xD1 && dot_ticks)
            {
                tick += dot_ticks / 2;
                dot_ticks = 0;
                continue;
            }
            if (tie)
            {
                dot_ticks = 0;
                continue;
            }

            dot_ticks = 0;
            tie_pitch = -1;
            if (b == 0xD6 && repeats.size() < kMaxRepeatDepth)
            {
                repeats.emplace_back(address, 0);
            }
            else if (b == 0xD7 && !repeats.empty())
            {
                int& left = repeats.back().second;
                if (left == 0)
                {
                    left = rom.U8(address - 1) ? rom.U8(address - 1) : 256;
                }
                if (--left > 0)
                {
                    address = repeats.back().first;
                }
                else
                {
                    repeats.pop_back();
                }
            }
        }
    }

    return WriteTextFile(path, text, error);
}

} // namespace agbsf2::quintet
