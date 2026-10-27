// SPDX-License-Identifier: MIT

#include "krawall/song.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "files.h"
#include "krawall/driver.h"
#include "rom.h"

namespace agbsf2::krawall
{
namespace
{

// The effects' names, as the player's table of effects lists them.
constexpr const char* kEffectNames[] = {
    "no effect",
    "speed",
    "bpm",
    "speed/bpm",
    "jump",
    "break",
    "volume slide (S3M)",
    "volume slide (XM)",
    "fine volume down",
    "fine volume up",
    "porta down (XM)",
    "porta down (S3M)",
    "fine porta down",
    "extra fine porta down",
    "porta up (XM)",
    "porta up (S3M)",
    "fine porta up",
    "extra fine porta up",
    "volume",
    "porta to note",
    "vibrato",
    "tremor",
    "arpeggio",
    "volume slide + vibrato",
    "volume slide + porta",
    "channel volume",
    "channel volume slide",
    "offset",
    "pan slide",
    "retrig",
    "tremolo",
    "fine vibrato",
    "global volume",
    "global volume slide",
    "pan",
    "panbrello",
    "mark",
    "glissando",
    "vibrato wave",
    "tremolo wave",
    "panbrello wave",
    "fine pattern delay (none)",
    "old pan (none)",
    "pattern loop",
    "note cut",
    "note delay",
    "pattern delay (none)",
    "envelope position (none)",
    "high offset (none)",
    "volume slide + vibrato (XM)",
    "volume slide + porta (XM)",
};

// The volume column's effects, by the column's high digit from 6.
constexpr const char* kColumnNames[] = {
    "volume slide down", "volume slide up", "fine volume down", "fine volume up", "vibrato speed", "vibrato", "pan",
    "pan slide left",    "pan slide right", "porta to note",
};

constexpr const char* kNoteNames[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

// Returns a note of a pattern as its name: the note from C-0 in the pattern, before the sample's relative note.
std::string NoteName(int note)
{
    if (note == kNoteOff)
    {
        return "off";
    }

    return kNoteNames[(note - 1) % 12] + std::to_string((note - 1) / 12);
}

} // namespace

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    ModuleInfo module;
    if (song < 0 || size_t(song) >= info.songs.size() ||
        !ReadModule(rom, info.build, info.songs[size_t(song)].module, module))
    {
        error = "module " + std::to_string(song) + " can't be read";
        return false;
    }

    // A song of a module that holds several lists the whole module. The first line names the module by its place in the
    // list.
    std::string out;
    char line[256];
    const int place = int(std::find(info.modules.begin(), info.modules.end(), module.address) - info.modules.begin());
    if (info.songs[size_t(song)].song >= 0)
    {
        std::snprintf(line, sizeof line, "Song %d of ", info.songs[size_t(song)].song);
        out += line;
    }
    std::snprintf(line, sizeof line,
                  "%s %d at 0x%08X: %d channels, %d orders, restart at order %d, speed %d, tempo %d, global volume "
                  "%d, %s, %s periods%s\n",
                  out.empty() ? "Module" : "module", place, unsigned(module.address), module.channels,
                  module.order_count, module.restart, module.speed, module.tempo, module.global_volume,
                  module.instruments ? "instruments" : "samples", module.linear ? "linear" : "Amiga",
                  module.fast_slides ? ", fast volume slides" : "");
    out += line;

    // The orders, with +++ for a marker between songs, and the channels' pans.
    out += "Orders:";
    for (int i = 0; i < module.order_count; i++)
    {
        const uint8_t o = module.orders[size_t(i)];
        out += o == kOrderSkip ? " +++" : o == kOrderEnd ? " ---" : " " + std::to_string(o);
    }
    out += "\nPans:";
    for (int c = 0; c < module.channels; c++)
    {
        out += " " + std::to_string(module.channel_pan[size_t(c)]);
    }
    out += "\n";

    // Each pattern's rows: a channel's note and instrument, its volume column and its effect, whichever the row gives.
    for (size_t p = 0; p < module.patterns.size(); p++)
    {
        const uint32_t pattern = module.patterns[p];
        const int rows = int(PatternRows(rom, info.build, pattern));
        std::snprintf(line, sizeof line, "\nPattern %d at 0x%08X, %d rows\n", int(p), unsigned(pattern), rows);
        out += line;

        uint32_t at = PatternData(info.build, pattern);
        for (int row = 0; row < rows && rom.Contains(at); row++)
        {
            std::snprintf(line, sizeof line, "%3d", row);
            std::string text = line;
            for (uint8_t flags = rom.U8(at++); flags && rom.Contains(at); flags = rom.U8(at++))
            {
                const RowEntry entry = ReadEntry(rom, info.build, flags, at);
                std::snprintf(line, sizeof line, " | c%d", entry.channel);
                text += line;
                if (entry.note)
                {
                    text += " " + NoteName(entry.note);
                }
                if (entry.instrument)
                {
                    text += " i" + std::to_string(entry.instrument);
                }
                if (flags & kEntryVolume)
                {
                    if (entry.volume <= 0x60)
                    {
                        text += " v" + std::to_string(entry.volume - 0x10);
                    }
                    else
                    {
                        std::snprintf(line, sizeof line, " %s %d", kColumnNames[(entry.volume - 0x60) >> 4],
                                      (entry.volume - 0x60) & 0xF);
                        text += line;
                    }
                }
                if (flags & kEntryEffect)
                {
                    const uint8_t effect = entry.effect;
                    std::snprintf(line, sizeof line, " %s %02X",
                                  effect < std::size(kEffectNames) ? kEffectNames[effect] : "unknown effect",
                                  entry.effect_op);
                    text += line;
                }
            }
            out += text + "\n";
        }
    }

    return WriteTextFile(path, out, error);
}

} // namespace agbsf2::krawall
