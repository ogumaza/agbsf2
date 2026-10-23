// SPDX-License-Identifier: MIT

#include "rd2/song.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "files.h"
#include "rd2/driver.h"
#include "rd2/sequencer.h"
#include "rom.h"

namespace agbsf2::rd2
{
namespace
{

// The most commands DumpSong() lists for each track.
constexpr int kMaxListedCommands = 200000;

std::string NoteName(int note)
{
    static constexpr const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

// Reads a length: one byte, or two with the first's top bit set.
uint32_t VarLength(const Rom& rom, uint32_t address, uint32_t& size)
{
    const uint8_t b = rom.U8(address);
    if (b & 0x80)
    {
        size = 2;
        return uint32_t(b & 0x7F) << 8 | rom.U8(address + 1);
    }

    size = 1;

    return b;
}

// A track that an F8 starts: where it starts, its number in the player, and the F8's address.
struct TrackStart
{
    uint32_t address = 0;
    int number = 0;
    uint32_t from = 0;
};

// Lists the commands of the track at `address` in `sequence`, adding them to `text`. Follows the track as the driver
// does: waits take up time, and so do notes after C8, and calls return. Adds each track that an F8 starts to `starts`.
// With `timed`, notes take up time from the start. A sound effect's track starts that way.
void ListTrack(const Rom& rom, const DriverInfo& info, uint32_t sequence, uint32_t address, bool timed,
               std::string& text, std::vector<TrackStart>& starts)
{
    uint64_t tick = 0;
    uint16_t length = 0x7F;
    uint8_t velocity = 0x7F;
    uint16_t wait = 0;
    std::vector<uint32_t> stack;
    std::set<uint32_t> seen;
    for (int n = 0; n < kMaxListedCommands; n++)
    {
        const Command c = DecodeCommand(rom, info, address, length, velocity, wait);
        const std::string bytes = HexBytes(rom, address, c.size);
        char line[320];
        std::snprintf(line, sizeof line, "0x%08X %8llu  %-12s %s\n", unsigned(address),
                      static_cast<unsigned long long>(tick), bytes.c_str(), c.text.c_str());
        text += line;
        seen.insert(address);

        if (c.has_length && c.note)
        {
            length = uint16_t(c.ticks);
            velocity = c.velocity;
        }
        if (c.has_length && c.wait)
        {
            wait = uint16_t(c.ticks);
        }
        if (c.wait || (c.note && timed))
        {
            tick += c.ticks;
        }
        timed = c.op == 0xC8 ? true : c.op == 0xC9 ? false : timed;

        if (c.end)
        {
            if (stack.empty())
            {
                break;
            }
            address = stack.back();
            stack.pop_back();
            continue;
        }
        if (c.op == 0xF0)
        {
            address = sequence + c.target;
            if (seen.count(address))
            {
                text += "; loops\n";
                break;
            }
            continue;
        }
        if (c.op == 0xF4 && stack.size() < size_t(kMaxDepth))
        {
            stack.push_back(address + c.size);
            address = sequence + c.target;
            continue;
        }
        if (c.op == 0xF8 && c.track < kPlayerTracks)
        {
            starts.push_back({sequence + c.target, c.track, address});
        }

        address += c.size;
    }
}

} // namespace

Command DecodeCommand(const Rom& rom, const DriverInfo& info, uint32_t address, uint16_t length, uint8_t velocity,
                      uint16_t wait)
{
    Command c;
    const uint8_t op = rom.U8(address);
    c.op = op;
    char text[160];
    if (op <= 0xBF)
    {
        c.note = true;
        int note = op;
        if (op >= 0x60)
        {
            uint32_t size = 0;
            length = uint16_t(VarLength(rom, address + 1, size));
            velocity = rom.U8(address + 1 + size);
            c.size = 2 + size;
            c.has_length = true;
            note -= 0x60;
        }

        c.ticks = length;
        c.velocity = velocity;
        std::snprintf(text, sizeof text, "note %d (%s), length %u, velocity %u", note, NoteName(note).c_str(),
                      unsigned(length), unsigned(velocity));
        c.text = text;
        return c;
    }
    if (op == 0xC0 || op == 0xC1)
    {
        c.wait = true;
        if (op == 0xC1)
        {
            uint32_t size = 0;
            wait = uint16_t(VarLength(rom, address + 1, size));
            c.size = 1 + size;
            c.has_length = true;
        }

        c.ticks = wait;
        c.text = "wait " + std::to_string(wait);
        return c;
    }
    if ((op & 0xF0) == 0xD0)
    {
        // A slide for the next note: between it and another note, over a share of the note's frames.
        const int flags = op & 15;
        c.size = (flags & 1) ? 4 : 3;
        const int note = rom.U8(address + 1);
        std::snprintf(text, sizeof text, "slide the next note %s note %d (%s) over %u/256 of it%s%s",
                      (flags & 2) ? "to" : "from", note, NoteName(note).c_str(), unsigned(rom.U8(address + 2)),
                      (flags & 1) ? (", after " + std::to_string(rom.U8(address + 3)) + " frames").c_str() : "",
                      (flags & 4) ? ", and each note after it from the last" : "");
        c.text = text;
        return c;
    }

    auto with = [&](const char* what)
    {
        c.size = 2;
        c.text = std::string(what) + " " + std::to_string(rom.U8(address + 1));
    };

    switch (op)
    {
    case 0xC2:
        with("instrument");
        break;

    case 0xC3:
        with("pan");
        break;

    case 0xC4:
        with("priority");
        break;

    case 0xC5:
        c.text = "release the track's notes; legato on";
        break;

    case 0xC6:
        c.text = "release the track's notes; legato off";
        break;

    case 0xC7:
        with("bank: entry of the sequence's list of banks");
        break;

    case 0xC8:
        c.text = "notes wait for their length";
        break;

    case 0xC9:
        c.text = "notes don't wait";
        break;

    case 0xCA:
        with("call the game's code with");
        break;

    case 0xE0:
        with("volume");
        break;

    case 0xE1:
        c.size = 2;
        c.text = "bend " + std::to_string(rom.S8(address + 1));
        break;

    case 0xE2:
        with("bend range");
        break;

    case 0xE3:
        with("echo");
        break;

    case 0xE4:
        {
            // The Super Mario Advance 2 revision's tempo is a byte.
            uint32_t size = 1;
            const uint32_t tempo =
                TraitsOf(info.revision).byte_tempo ? rom.U8(address + 1) : VarLength(rom, address + 1, size);
            c.size = 1 + size;
            c.text = "tempo " + std::to_string(tempo);
            break;
        }

    case 0xE5:
        with("LFO delay");
        break;

    case 0xE6:
        with("LFO rate");
        break;

    case 0xE7:
        with("LFO depth");
        break;

    case 0xE8:
        c.text = "no slide";
        break;

    case 0xE9:
        with("transpose");
        break;

    case 0xEA:
        // The Super Mario Advance 2 revision has no player volume, and EA is one of the commands that do nothing.
        if (TraitsOf(info.revision).player_volume)
        {
            with("player volume");
        }
        else
        {
            c.text = "nothing";
        }
        break;

    case 0xF0:
    case 0xF4:
        c.size = 3;
        c.target = rom.U16(address + 1);
        std::snprintf(text, sizeof text, "%s 0x%04X", op == 0xF0 ? "jump to" : "call", unsigned(c.target));
        c.text = text;
        break;

    case 0xF8:
        c.size = 4;
        c.track = rom.U8(address + 1);
        c.target = rom.U16(address + 2);
        std::snprintf(text, sizeof text, "start track %u at 0x%04X", unsigned(c.track), unsigned(c.target));
        c.text = text;
        break;

    case 0xFF:
        c.end = true;
        c.text = "return, or end the track";
        break;

    default:
        c.text = "nothing";
        break;
    }

    return c;
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    const Effect* effect = SongEffect(info, song);
    if (song < 0 || song >= SongCount(info) || (effect && !effect->address))
    {
        error = "invalid sequence";
        return false;
    }

    std::string text;
    char line[320];
    std::vector<TrackStart> starts;
    std::set<uint32_t> listed;
    uint32_t sequence = 0;
    if (effect)
    {
        // A sound effect is one track of a sequence. The driver plays it as the player's track 0.
        sequence = info.effect_sequence_addresses[size_t(effect->sequence)];
        std::snprintf(line, sizeof line, "; %s (%s) sound effect %d: track %d of the effects' sequence %d at 0x%08X\n",
                      rom.Title().c_str(), rom.GameCode().c_str(), song, effect->track, effect->sequence,
                      unsigned(sequence));
        text += line;
        text +=
            "; columns: address, tick, bytes, command\n"
            "; the track is listed as the driver plays it, calls included, up to its end or its loop\n"
            "; its notes wait for their length until a C9\n"
            "; a track that F8 starts comes after it, with ticks from its start\n";
        std::snprintf(line, sizeof line, "\n; ---- track 0 at 0x%08X ----\n", unsigned(effect->address));
        text += line;
        listed.insert(effect->address);
        ListTrack(rom, info, sequence, effect->address, true, text, starts);
    }
    else
    {
        sequence = info.sequence_addresses[size_t(song)];
        const int count = rom.S8(sequence);
        std::snprintf(line, sizeof line,
                      "; %s (%s) sequence %d at 0x%08X: %d tracks\n; columns: address, tick, bytes, command\n"
                      "; each track is listed as the driver plays it, calls included, up to its end or its loop\n"
                      "; a track that F8 starts comes after the others, with ticks from its start\n",
                      rom.Title().c_str(), rom.GameCode().c_str(), song, unsigned(sequence), count);
        text += line;
        for (int t = 0; t < count && t < kPlayerTracks; t++)
        {
            const uint32_t offset = rom.U16(sequence + 2 + 2 * uint32_t(t));
            if (offset == 0)
            {
                continue;
            }

            std::snprintf(line, sizeof line, "\n; ---- track %d at 0x%08X ----\n", t, unsigned(sequence + offset));
            text += line;
            listed.insert(sequence + offset);
            ListTrack(rom, info, sequence, sequence + offset, false, text, starts);
        }
    }

    // Then each place that an F8 starts a track at. A place where a listed track starts already is left out. A track
    // listed here can start more.
    for (size_t i = 0; i < starts.size(); i++)
    {
        const TrackStart start = starts[i];
        if (!listed.insert(start.address).second)
        {
            continue;
        }

        std::snprintf(line, sizeof line, "\n; ---- track %d at 0x%08X, started by F8 at 0x%08X ----\n", start.number,
                      unsigned(start.address), unsigned(start.from));
        text += line;
        ListTrack(rom, info, sequence, start.address, effect != nullptr, text, starts);
    }

    return WriteTextFile(path, text, error);
}

} // namespace agbsf2::rd2
