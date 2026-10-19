// SPDX-License-Identifier: MIT

#include "konami/seqformat.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "psg.h"

namespace agbsf2::konami
{
namespace
{

// Decodes the command at `addr` in the Ultimate Masters revision. `c` already holds the address and the opcode.
Command DecodeUltimateMasters(const Rom& rom, uint32_t addr, int track, Command c)
{
    const uint8_t op = c.opcode;
    if (op >= 0xFD)
    {
        if (op == 0xFF)
        {
            c.op = Op::kJump;
            c.value = rom.U8(addr + 1);
            c.length = 2;
        }
        else
        {
            c.op = op == 0xFE ? Op::kEndTrack : Op::kEndTrackHard;
        }
    }
    else if (op >= 0xF0)
    {
        c.length = 2;
        c.value = rom.U8(addr + 1);
        switch (op)
        {
        case 0xF0:
            c.op = Op::kPan;
            break;

        case 0xF1:
            c.op = Op::kVibrato;
            break;

        case 0xF2:
            // F2 bb bends by bb - 0x40. A byte above 7F starts a longer bend with the next one: F2 xh ll bends by
            // 0xhll - 0x400, and the high nibble x is ignored.
            c.op = Op::kPitchBend;
            if (c.value <= 0x7F)
            {
                c.value -= 0x40;
            }
            else
            {
                c.value = (((c.value & 0x0F) << 8) | rom.U8(addr + 2)) - 0x400;
                c.length = 3;
            }
            break;

        case 0xF3:
            c.op = Op::kLoopPoint;
            c.value = 0;
            c.length = IsPsgTrack(track) ? 1 : 3;
            break;

        case 0xF7:
            c.op = Op::kEchoFeedback;
            break;

        case 0xF8:
            c.op = Op::kEchoDelay;
            break;

        case 0xF9:
            c.op = Op::kEchoRoute;
            break;

        default:
            c.op = Op::kUnknown;
            c.length = 1;
            break;
        }
    }
    else if (op >= 0xE0)
    {
        c.op = Op::kRest;
    }
    else if (op >= 0xC0)
    {
        uint32_t p = addr + 1;
        c.vol = (op & 8) ? rom.U8(p++) : (op & 7);
        if (op >= 0xD0)
        {
            c.op = Op::kPsgNote;
            c.note = rom.U8(p++);
        }
        else
        {
            c.op = Op::kVolume;
        }

        c.length = p - addr;
    }
    else if (op >= 0xA0)
    {
        uint32_t p = addr + 1;
        c.vol = (op & 8) ? rom.U8(p++) : (op & 7);
        c.sample = rom.U8(p++);
        if (op >= 0xB0)
        {
            c.semitone = int8_t(rom.U8(p++));
        }

        c.length = p - addr;

        // Sample bytes above EF select an extended mode with no known use in this driver revision.
        c.op = c.sample <= 0xEF ? Op::kNote : Op::kUnknown;
    }
    else if (op >= 0x90)
    {
        c.op = Op::kUnknown;
    }
    else
    {
        c.op = Op::kDuty;
        c.value = DutyByteFor(op);
    }

    return c;
}

// Decodes the command at `addr` in the WCT 2004, Rave Master or Eternal Duelist revision. `c` already holds the address
// and the opcode. The Rave Master revision has no F6-FC, which do nothing there, and the Eternal Duelist revision reads
// F4-FC as F1. In both, F3 carries no extra bytes on sample tracks, 00-7F do nothing, and 80-8F store a duty of 0-3 as
// it is, where WCT 2004 shifts it into bits 6-7. Eternal Duelist's F0 sets a PSG track's NR51 bits, and a sample
// track's volume and the next one's.
Command DecodeWct2004(const Rom& rom, uint32_t addr, int track, Revision revision, Command c)
{
    const bool eternal_duelist = revision == Revision::kEternalDuelist;
    const bool rave_master_commands = RaveMasterCommands(revision);
    const uint8_t op = c.opcode;
    if (rave_master_commands && (op <= 0x7F || (!eternal_duelist && op >= 0xF6 && op <= 0xFC)))
    {
        c.op = Op::kNop;
        return c;
    }

    if (op >= 0xFD)
    {
        // FD ends the track until the song loops, FE loops the song and FF stops it.
        if (op == 0xFD)
        {
            c.op = Op::kEndTrack;
        }
        else
        {
            c.op = Op::kJump;
            c.value = op == 0xFE;
        }
    }
    else if (op >= 0xF0)
    {
        c.length = 2;
        c.value = rom.U8(addr + 1);
        switch (eternal_duelist && op >= 0xF4 ? 0xF1 : op)
        {
        case 0xF0:
            c.op = !eternal_duelist ? Op::kPanLevels : IsPsgTrack(track) ? Op::kPsgPan : Op::kPairVolumes;
            break;

        case 0xF1:
            c.op = Op::kVibrato;
            break;

        case 0xF2:
            c.op = Op::kPitchBend;
            c.value -= 0x40;
            break;

        case 0xF3:
            c.op = Op::kLoopPoint;
            c.value = 0;
            c.length = IsPsgTrack(track) || rave_master_commands ? 1 : 3;
            break;

        case 0xF4:
            c.op = Op::kAttack;
            break;

        case 0xF5:
            c.op = Op::kDecay;
            break;

        case 0xF6:
            c.op = Op::kInstrument;
            break;

        case 0xF7:
            c.op = Op::kEchoFeedback;
            break;

        case 0xF8:
            c.op = Op::kEchoDelay;
            break;

        case 0xF9:
            c.op = Op::kEchoRoute;
            break;

        case 0xFA:
            c.op = Op::kVolumeScale;
            break;

        default:
            c.op = Op::kNop;
            c.value = 0;
            c.length = 1;
            break;
        }
    }
    else if (op >= 0xE0)
    {
        c.op = Op::kRest;
    }
    else if (op >= 0xD0)
    {
        c.op = Op::kPsgNote;
        c.vol = op & 15;
        c.note = rom.U8(addr + 1);
        c.length = 2;
    }
    else if (op >= 0xC0)
    {
        c.op = Op::kVolume;
        c.vol = op & 15;
    }
    else if (op >= 0xA0)
    {
        c.op = Op::kNote;
        c.vol = op & 15;
        c.sample = rom.U8(addr + 1);
        c.length = 2;
        if (op >= 0xB0)
        {
            c.semitone = int8_t(rom.U8(addr + 2));
            c.length = 3;
        }
    }
    else if (op >= 0x90)
    {
        c.op = Op::kCall;
        c.value = rom.U16(addr + 1);
        c.count = rom.U8(addr + 3);
        c.length = 4;
    }
    else
    {
        const Command set = DutyOrWave(op, revision);
        c.op = set.op;
        c.value = set.value;
    }

    return c;
}

// Decodes the command at `addr` in the Dungeon Dice Monsters revision. `c` already holds the address and the opcode. A
// sample note's byte is an entry of the driver's sample map, where the sequencer looks the sample up. FA's two bytes
// are a delay and a command that only a song loop reads, and F5's last byte is a duty or wave command that the call
// starts with, and also the delay that follows the call.
Command DecodeDungeonDice(const Rom& rom, uint32_t addr, Command c)
{
    const uint8_t op = c.opcode;
    const int nibble = op & 15;
    if (op >= 0xF0)
    {
        switch (op)
        {
        case 0xFF:
        case 0xFE:
            // FE loops the song and FF stops it.
            c.op = Op::kJump;
            c.value = op == 0xFE;
            break;

        case 0xFD:
            c.op = Op::kEndTrack;
            break;

        case 0xFC:
            c.op = Op::kRest;
            break;

        case 0xFB:
            c.op = Op::kRelease;
            break;

        case 0xFA:
            c.op = Op::kLoopPoint;
            c.length = 3;
            break;

        case 0xF9:
        case 0xF8:
            c.op = Op::kFade;
            break;

        case 0xF7:
        case 0xF6:
            // F7 sets a track flag and F6 clears it. The driver never reads the flag.
            c.op = Op::kNop;
            break;

        case 0xF5:
        case 0xF4:
            c.op = Op::kCall;
            c.value = rom.U16(addr + 1);
            c.count = rom.U8(addr + 3);
            c.length = 4;
            break;

        case 0xF3:
            c.op = Op::kPitchBend;
            c.value = rom.U8(addr + 1) - 32;
            c.length = 2;
            break;

        case 0xF2:
        case 0xF1:
            c.op = Op::kSampleBend;
            c.value = rom.U8(addr + 1) - 32;
            c.length = 2;
            break;

        default:
            c.op = Op::kPsgVolume;
            c.value = rom.U8(addr + 1);
            c.length = 2;
            break;
        }
    }
    else if (op >= 0x90 && !(op >= 0xC0 && op <= 0xDF))
    {
        // Ex nn plays PSG note nn, Bx nn sample map entry nn, Ax nn the same on the next track, and 9x nn the track's
        // sample at note nn, each at volume x.
        c.op = op >= 0xE0 ? Op::kPsgNote : op >= 0xB0 ? Op::kNote : op >= 0xA0 ? Op::kNextNote : Op::kSampleAtNote;
        c.vol = nibble;
        c.length = 2;
        if (c.op == Op::kPsgNote || c.op == Op::kSampleAtNote)
        {
            c.note = rom.U8(addr + 1);
        }
        else
        {
            c.sample = rom.U8(addr + 1);
        }
    }
    else if (op >= 0xC0)
    {
        c.op = op >= 0xD0 ? Op::kSetVolume : Op::kVolume;
        c.vol = nibble;
    }
    else if (op >= 0x80)
    {
        c.op = Op::kVibrato;
        c.value = nibble;
    }
    else
    {
        // 70-7B load a wave and 7C-7F set a duty of 0-3. 00-6F store the opcode itself as the duty byte. On a sample
        // track that's the sample.
        const Command set = DutyOrWave(op, Revision::kDungeonDiceMonsters);
        c.op = set.op;
        c.value = set.value;
    }

    return c;
}

} // namespace

const char* RevisionName(Revision r)
{
    switch (r)
    {
    case Revision::kWct2004:
        return "WCT 2004";
    case Revision::kRaveMaster:
        return "Rave Master";
    case Revision::kEternalDuelist:
        return "Eternal Duelist";
    case Revision::kDungeonDiceMonsters:
        return "Dungeon Dice Monsters";
    default:
        return "Ultimate Masters";
    }
}

int TrackCount(Revision r)
{
    switch (r)
    {
    case Revision::kRaveMaster:
        return 12;
    case Revision::kEternalDuelist:
        return 10;
    case Revision::kDungeonDiceMonsters:
        return 8;
    default:
        return kTracks;
    }
}

bool RaveMasterCommands(Revision r)
{
    return r == Revision::kRaveMaster || r == Revision::kEternalDuelist;
}

bool OlderOutputStage(Revision r)
{
    return r == Revision::kWct2004 || r == Revision::kRaveMaster || r == Revision::kEternalDuelist;
}

bool RecordHasPan(Revision r)
{
    return r != Revision::kEternalDuelist && r != Revision::kDungeonDiceMonsters;
}

uint32_t SongEntrySize(Revision r)
{
    return 4 + 2 * uint32_t(TrackCount(r));
}

uint32_t ReadDelay(const Rom& rom, uint32_t addr, Revision revision, uint32_t& delay)
{
    const uint8_t b = rom.U8(addr);
    if (revision == Revision::kEternalDuelist || revision == Revision::kDungeonDiceMonsters)
    {
        delay = b <= 0xEF ? b : (uint32_t(b & 0x0F) << 8) | rom.U8(addr + 1);
        return b <= 0xEF ? 1 : 2;
    }
    if (b <= 0xDF)
    {
        delay = b;
        return 1;
    }
    if (revision != Revision::kUltimateMasters)
    {
        delay = (uint32_t(b & 0x1F) << 8) | rom.U8(addr + 1);
        return 2;
    }
    if (b <= 0xEF)
    {
        delay = (uint32_t(b & 0x0F) << 8) | rom.U8(addr + 1);
        return 2;
    }

    delay = rom.U16(addr + 1);

    return 3;
}

Command DecodeCommand(const Rom& rom, uint32_t addr, int track, Revision revision)
{
    Command c;
    c.addr = addr;
    c.length = 1;
    if (!rom.Contains(addr))
    {
        return c;
    }

    c.opcode = rom.U8(addr);

    switch (revision)
    {
    case Revision::kUltimateMasters:
        return DecodeUltimateMasters(rom, addr, track, c);
    case Revision::kDungeonDiceMonsters:
        return DecodeDungeonDice(rom, addr, c);
    default:
        return DecodeWct2004(rom, addr, track, revision, c);
    }
}

Command DutyOrWave(uint8_t opcode, Revision revision)
{
    const int nibble = opcode & 15;
    Command c;
    if (revision == Revision::kDungeonDiceMonsters)
    {
        c.op = opcode > 0x6F && nibble <= 11 ? Op::kWave : Op::kDuty;
        c.value = opcode <= 0x6F ? opcode : nibble <= 11 ? nibble : (nibble & 3) << 6;
        return c;
    }

    c.op = nibble <= 3 ? Op::kDuty : Op::kWave;
    c.value = nibble > 3 ? nibble - 4 : RaveMasterCommands(revision) ? nibble : nibble << 6;

    return c;
}

bool CallSetsFirst(uint8_t opcode, Revision revision)
{
    return revision == Revision::kDungeonDiceMonsters ? opcode == 0xF5 : opcode != 0x9F;
}

bool ReadSongHeader(const Rom& rom, uint32_t song_table, int song, Revision revision, SongHeader& out)
{
    const uint32_t size = SongEntrySize(revision);
    const uint32_t e = song_table + uint32_t(song) * size;
    if (!rom.Contains(e, size))
    {
        return false;
    }

    out = SongHeader();
    out.base = rom.U32(e);
    out.tracks = TrackCount(revision);
    for (int t = 0; t < out.tracks; t++)
    {
        out.offsets[t] = rom.U16(e + 4 + 2 * uint32_t(t));
    }

    return true;
}

bool WalkTrack(const Rom& rom, const SongHeader& song, int track, Revision revision,
               const std::function<void(const Command&, uint32_t frame)>& visit, uint32_t max_commands,
               bool follow_calls)
{
    uint32_t start = song.offsets[track];
    uint32_t pos = 0;
    uint32_t frame = 0;

    // The call that the walk follows: the start and position to go back to, and the commands left to play.
    uint32_t saved_start = 0;
    uint32_t ret = 0;
    int call_commands = 0;

    auto read_delay_at = [&]()
    {
        Command d;
        d.op = Op::kDelay;
        d.addr = song.base + start + pos;
        d.length = ReadDelay(rom, d.addr, revision, d.delay);
        visit(d, frame);
        pos += d.length;
        frame += d.delay;

        return rom.Contains(d.addr, d.length);
    };

    if (!read_delay_at())
    {
        return false;
    }

    for (uint32_t n = 0; n < max_commands; n++)
    {
        const Command c = DecodeCommand(rom, song.base + start + pos, track, revision);
        visit(c, frame);

        switch (c.op)
        {
        case Op::kUnknown:
            return false;

        case Op::kEndTrack:
        case Op::kEndTrackHard:
        case Op::kJump:
            return true;

        case Op::kLoopPoint:
            start = (start + pos + c.length) & 0xFFFF;
            pos = 0;
            break;

        case Op::kCall:
            {
                // 90-9E and F5 count setting the duty or the wave as the call's first command. A count of 0 plays none
                // of the fragment. 9F and F4 play 256 commands for 0.
                const int count = c.count == 0 && !CallSetsFirst(c.opcode, revision) ? 256 : c.count;
                // A call of no commands only sets the duty or the wave. Inside another call, it also ends that call's
                // count. The driver does the same.
                if (!follow_calls || count == 0)
                {
                    call_commands = 0;
                    pos += c.length;
                    break;
                }

                // The count below takes the call itself as one of its commands.
                saved_start = start;
                ret = pos + c.length;
                call_commands = count + 1;
                start = song.offsets[track];
                pos = uint32_t(c.value);
                break;
            }

        default:
            pos += c.length;
            break;
        }

        // Each command counts towards the call's commands, and the track goes back after the last one.
        if (call_commands > 0 && --call_commands == 0)
        {
            start = saved_start;
            pos = ret;
        }

        if (!read_delay_at())
        {
            return false;
        }
    }

    return false;
}

std::string Describe(const Command& c, int track, Revision revision)
{
    const bool dungeon_dice = revision == Revision::kDungeonDiceMonsters;
    const char* const next = c.opcode & 1 ? " of the next track" : "";
    char buf[96];
    switch (c.op)
    {
    case Op::kDelay:
        std::snprintf(buf, sizeof buf, "delay %u", c.delay);
        break;

    case Op::kDuty:
        if (track == 0 || track == 1)
        {
            std::snprintf(buf, sizeof buf, "duty       %s", kDutyNames[(c.value >> 6) & 3]);
        }
        else if (track == 2 && !dungeon_dice)
        {
            std::snprintf(buf, sizeof buf, "wave       %d", c.value);
        }
        else if (!IsPsgTrack(track) && dungeon_dice)
        {
            std::snprintf(buf, sizeof buf, "sample     %d", c.value);
        }
        else
        {
            std::snprintf(buf, sizeof buf, "duty byte  %02X (no effect here)", unsigned(c.value));
        }
        break;

    case Op::kWave:
        std::snprintf(buf, sizeof buf, "wave       %d", c.value);
        break;

    case Op::kNote:
        if (dungeon_dice)
        {
            std::snprintf(buf, sizeof buf, "note       vol=%d sample map entry %d", c.vol, c.sample);
        }
        else
        {
            std::snprintf(buf, sizeof buf, "note       vol=%d sample=%d semitone=%+d", c.vol, c.sample, c.semitone);
        }
        break;

    case Op::kNextNote:
        std::snprintf(buf, sizeof buf, "note       vol=%d sample map entry %d, on the next track", c.vol, c.sample);
        break;

    case Op::kSampleAtNote:
        if (IsPsgTrack(track))
        {
            std::snprintf(buf, sizeof buf, "psg note   vol=%d note=%d, 1/16 semitone up", c.vol, c.note);
        }
        else
        {
            std::snprintf(buf, sizeof buf, "note       vol=%d note=%d of the track's sample", c.vol, c.note);
        }
        break;

    case Op::kVolume:
        std::snprintf(buf, sizeof buf, dungeon_dice ? "restart    vol=%d" : "volume     %d", c.vol);
        break;

    case Op::kSetVolume:
        std::snprintf(buf, sizeof buf, "volume     %d", c.vol);
        break;

    case Op::kPsgNote:
        std::snprintf(buf, sizeof buf, "psg note   vol=%d note=%d", c.vol, c.note);
        break;

    case Op::kRest:
        std::snprintf(buf, sizeof buf, "rest");
        break;

    case Op::kPan:
        std::snprintf(buf, sizeof buf, "pan        %d", c.value);
        break;

    case Op::kPanLevels:
        std::snprintf(buf, sizeof buf, "pan        left %d right %d", c.value >> 4, c.value & 15);
        break;

    case Op::kPsgPan:
        std::snprintf(buf, sizeof buf, "pan        NR51 bits %02X", unsigned(c.value));
        break;

    case Op::kPairVolumes:
        std::snprintf(buf, sizeof buf, "volume     %d, and %d for the next voice", c.value & 15, c.value >> 4);
        break;

    case Op::kPsgVolume:
        std::snprintf(buf, sizeof buf, "psg volume left %d right %d", (c.value >> 4) & 7, c.value & 7);
        break;

    case Op::kVibrato:
        std::snprintf(buf, sizeof buf, "vibrato    depth=%d", dungeon_dice ? c.value : c.value >> 1);
        break;

    case Op::kPitchBend:
        std::snprintf(buf, sizeof buf, "pitch bend %+d/%d semitone", c.value, dungeon_dice ? 16 : 32);
        break;

    case Op::kSampleBend:
        std::snprintf(buf, sizeof buf, "pitch bend %+d/16 semitone%s", c.value, next);
        break;

    case Op::kFade:
        std::snprintf(buf, sizeof buf, "fade out%s", next);
        break;

    case Op::kRelease:
        std::snprintf(buf, sizeof buf, "release");
        break;

    case Op::kLoopPoint:
        std::snprintf(buf, sizeof buf, "loop point");
        break;

    case Op::kAttack:
        std::snprintf(buf, sizeof buf, "attack     rate %d", c.value);
        break;

    case Op::kDecay:
        std::snprintf(buf, sizeof buf, "decay      rate %d", c.value);
        break;

    case Op::kInstrument:
        std::snprintf(buf, sizeof buf, "instrument %d (from a table the game gives)", c.value);
        break;

    case Op::kEchoFeedback:
        std::snprintf(buf, sizeof buf, "echo feedback %d", c.value);
        break;

    case Op::kEchoDelay:
        std::snprintf(buf, sizeof buf, "echo delay %d", c.value);
        break;

    case Op::kEchoRoute:
        if (c.value & 15)
        {
            std::snprintf(buf, sizeof buf, "echo route voice=%d bus=%d", c.value >> 4, (c.value & 15) - 1);
        }
        else
        {
            std::snprintf(buf, sizeof buf, "echo route voice=%d off buses 0 and 1", c.value >> 4);
        }
        break;

    case Op::kVolumeScale:
        std::snprintf(buf, sizeof buf, "volume scale %s", c.value ? "from the game's setting" : "off");
        break;

    case Op::kNop:
        std::snprintf(buf, sizeof buf, "nothing");
        break;

    case Op::kCall:
        {
            std::string action;
            if (CallSetsFirst(c.opcode, revision) && dungeon_dice)
            {
                action = ", first the next byte's command";
            }
            else if (CallSetsFirst(c.opcode, revision))
            {
                // 90-9E set the duty byte or the wave as 80-8E do, and are described the same way, in one line.
                std::string what = Describe(DutyOrWave(c.opcode, revision), track, revision);
                what.erase(std::unique(what.begin(), what.end(), [](char a, char b) { return a == ' ' && b == ' '; }),
                           what.end());
                action = ", " + what;
            }

            std::snprintf(buf, sizeof buf, "call       %d commands at +0x%04X%s", c.count, unsigned(c.value),
                          action.c_str());
            break;
        }

    case Op::kEndTrack:
        std::snprintf(buf, sizeof buf, "end track");
        break;

    case Op::kEndTrackHard:
        std::snprintf(buf, sizeof buf, "end track (no restart)");
        break;

    case Op::kJump:
        std::snprintf(buf, sizeof buf, "%s", c.value ? "loop song" : "stop song");
        break;

    case Op::kUnknown:
        std::snprintf(buf, sizeof buf, "UNKNOWN opcode %02X", c.opcode);
        break;
    }

    return buf;
}

} // namespace agbsf2::konami
