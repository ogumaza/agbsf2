// SPDX-License-Identifier: MIT

// The commands of Ubisoft Milan's sequences, and the listing of a piece's sequences that --dump writes.

#include "ubimilan/song.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "files.h"
#include "rom.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{
namespace
{

// Returns the listing's text for the command with the status byte `status`. Its arguments start at `arguments`.
std::string CommandText(const Rom& rom, int status, uint32_t arguments)
{
    char text[96];
    switch (status & 0xF0)
    {
    case 0x80:
    case 0x90:
        std::snprintf(text, sizeof text, "%02X %s channel %d key %d velocity %d\n", unsigned(status),
                      (status & 0xF0) == 0x90 ? "note on" : "note off", status & 0xF, rom.U8(arguments),
                      rom.U8(arguments + 1));
        break;

    case 0xC0:
        std::snprintf(text, sizeof text, "%02X program channel %d: %d\n", unsigned(status), status & 0xF,
                      rom.U8(arguments));
        break;

    case 0xE0:
        std::snprintf(text, sizeof text, "%02X nothing\n", unsigned(status));
        break;

    case 0xF0:
        if (status == kEndCommand)
        {
            return "FC end\n";
        }
        [[fallthrough]];

    default:
        std::snprintf(text, sizeof text, "%02X unknown, on which the game resets\n", unsigned(status));
        break;
    }

    return text;
}

// Returns the frames from a command to the next that a countdown of `wait` puts between them: wait + 1, or none for a
// countdown of 0.
uint32_t CountdownFrames(uint32_t wait)
{
    return wait ? wait + 1 : 0;
}

} // namespace

int ArgumentBytes(int status)
{
    switch (status & 0xF0)
    {
    case 0x80:
    case 0x90:
        return 2;
    case 0xC0:
        return 1;
    case 0xE0:
        return 0;
    default:
        return status == kEndCommand ? 0 : -1;
    }
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    MusicPiece piece;
    if (!ReadMusic(rom, info, song, piece))
    {
        error = "its list of sequences can't be read";
        return false;
    }

    // Each sequence's commands, with the frame each plays on from the sequence's start: a countdown of n puts the next
    // command n + 1 frames later, or on the same frame if it's 0. The piece's first sequence starts before the first
    // frame's step, so its first countdown puts its first command n frames in.
    std::string out;
    char line[160];
    std::snprintf(line, sizeof line, "music %d: resource %d at 0x%08X, flags %d (%s)\n", song, info.songs[size_t(song)],
                  unsigned(piece.address), piece.flags, piece.Loops() ? "loops" : "plays once");
    out += line;
    for (size_t i = 0; i < piece.sequences.size(); i++)
    {
        const int id = piece.sequences[i];
        const uint32_t address = ResourceAddress(rom, info, id);
        std::snprintf(line, sizeof line, "\nsequence %d at 0x%08X\n", id, unsigned(address));
        out += line;
        if (!address || rom.U8(address) != kSequence)
        {
            out += "  not a sequence\n";
            continue;
        }

        uint32_t at = address + kSequenceHeader;
        uint32_t wait = rom.U16(at);
        at += 2;
        uint32_t frame = i == 0 ? wait : CountdownFrames(wait);
        while (rom.Contains(at, 1))
        {
            const int status = rom.U8(at);
            std::snprintf(line, sizeof line, "  %6u  0x%08X  ", unsigned(frame), unsigned(at));
            out += line;
            out += CommandText(rom, status, at + 1);
            const int arguments = ArgumentBytes(status);
            if (status == kEndCommand || arguments < 0)
            {
                break;
            }

            // The arguments, then the next command's countdown.
            at += 1 + uint32_t(arguments);
            wait = rom.U16(at);
            at += 2;
            frame += CountdownFrames(wait);
        }
    }

    return WriteTextFile(path, out, error);
}

} // namespace agbsf2::ubimilan
