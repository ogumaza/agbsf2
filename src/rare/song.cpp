// SPDX-License-Identifier: MIT

#include "rare/song.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "music.h"
#include "rom.h"

namespace agbsf2::rare
{
namespace
{

// The argument bytes after each command byte in the kChannelNibble format. The kChannelByte format adds the channel
// byte to the commands that have a channel.
constexpr uint8_t kArgBytes[kCommandCount] = {3, 1, 2, 3, 2, 2, 2, 2, 1, 1, 2, 0, 0};

bool HasChannel(uint8_t command)
{
    return command >= kCmdNoteOnB && command <= kCmdBend;
}

} // namespace

bool ReadTuneHeader(const Rom& rom, uint32_t address, TuneHeader& header)
{
    header = TuneHeader();
    if (!rom.Contains(address, 20))
    {
        return false;
    }

    header.address = address;
    header.track_count = rom.U32(address);
    header.ticks_per_quarter = rom.U32(address + 4);
    header.track_list = rom.U32(address + 8);
    header.program_map = rom.U32(address + 12);
    header.instruments = rom.U32(address + 16);
    if (header.track_count < 1 || header.track_count > 16 || header.ticks_per_quarter < 1 ||
        header.ticks_per_quarter > 0x7FFF || !rom.Contains(header.track_list, header.track_count * 4) ||
        !rom.Contains(header.program_map, 128) || !rom.Contains(header.instruments, 4))
    {
        return false;
    }

    for (uint32_t t = 0; t < header.track_count; t++)
    {
        const uint32_t track = rom.U32(header.track_list + 4 * t);
        if (!rom.Contains(track))
        {
            return false;
        }

        header.tracks.push_back(track);
    }

    return true;
}

bool DecodeEvent(const Rom& rom, uint32_t address, Format format, Event& event)
{
    event = Event();
    if (!rom.Contains(address))
    {
        return false;
    }

    // Split the command byte, and in the kChannelByte format read the channel byte.
    const uint8_t first = rom.U8(address);
    uint32_t at = address + 1;
    if (format == Format::kChannelNibble)
    {
        event.command = first & 0x0F;
        event.channel = first >> 4;
    }
    else
    {
        event.command = first;
    }
    if (event.command >= kCommandCount)
    {
        return false;
    }
    if (format == Format::kChannelByte && HasChannel(event.command))
    {
        event.channel = rom.U8(at++) & 0x0F;
    }

    const uint32_t args = kArgBytes[event.command];
    if (!rom.Contains(at, args == 0 ? 1 : args))
    {
        return false;
    }

    event.size = uint8_t(at + args - address);
    event.a = rom.U8(at);
    event.b = rom.U8(at + 1);
    switch (event.command)
    {
    case kCmdTempo:
    case kCmdDelay3:
        event.value = rom.U8(at) | (rom.U8(at + 1) << 8) | (uint32_t(rom.U8(at + 2)) << 16);
        break;
    case kCmdDelay1:
        event.value = rom.U8(at);
        break;
    case kCmdDelay2:
    case kCmdBend:
        event.value = rom.U16(at);
        break;
    default:
        break;
    }

    return true;
}

std::string DescribeEvent(const Event& e)
{
    char b[96];
    switch (e.command)
    {
    case kCmdTempo:
        std::snprintf(b, sizeof b, "tempo %u us per quarter (%.2f BPM)", unsigned(e.value),
                      e.value ? 60000000.0 / e.value : 0.0);
        break;
    case kCmdDelay1:
    case kCmdDelay2:
    case kCmdDelay3:
        std::snprintf(b, sizeof b, "wait %u", unsigned(e.value));
        break;
    case kCmdNoteOnB:
    case kCmdNoteOn:
        std::snprintf(b, sizeof b, "note on ch %d key %d vel %d", e.channel, e.a, e.b);
        break;
    case kCmdNoteOff:
        std::snprintf(b, sizeof b, "note off ch %d key %d", e.channel, e.a);
        break;
    case kCmdControl:
        std::snprintf(b, sizeof b, "controller ch %d #%d = %d", e.channel, e.a, e.b);
        break;
    case kCmdProgram:
        std::snprintf(b, sizeof b, "program ch %d %d", e.channel, e.a);
        break;
    case kCmdPressure:
        std::snprintf(b, sizeof b, "pressure ch %d (ignored)", e.channel);
        break;
    case kCmdBend:
        std::snprintf(b, sizeof b, "pitch bend ch %d %d", e.channel, int(e.value) - 0x2000);
        break;
    case kCmdEnd:
        std::snprintf(b, sizeof b, "end of track");
        break;
    default:
        std::snprintf(b, sizeof b, "nothing");
        break;
    }

    return b;
}

TrackLayout ScanTrack(const Rom& rom, uint32_t address, Format format)
{
    TrackLayout layout;
    uint64_t tick = 0;
    bool loop_started = false;
    for (int n = 0; n < kMaxTrackCommands; n++)
    {
        Event e;
        if (!DecodeEvent(rom, address, format, e))
        {
            break;
        }

        address += e.size;
        if (IsDelay(e.command))
        {
            tick += e.value;
            continue;
        }
        if (e.command == kCmdEnd)
        {
            break;
        }

        layout.last_command = tick;
        if (e.command == kCmdControl && e.a == kCtrlLoopStart)
        {
            layout.loop_start = tick;
            loop_started = true;
        }
        else if (e.command == kCmdControl && e.a == kCtrlLoopEnd && loop_started)
        {
            layout.loops = true;
            layout.loop_end = tick;
            return layout;
        }
    }

    layout.end = tick;

    return layout;
}

Instrument InstrumentAt(const Rom& rom, uint32_t address)
{
    Instrument inst;
    inst.type = rom.U32(address);
    inst.loop_mode = rom.U32(address + 4);
    inst.rate = rom.U32(address + 8);
    inst.root_key = rom.U32(address + 12);
    inst.start = rom.U32(address + 16);
    inst.loop_length = rom.U32(address + 20);
    inst.end = rom.U32(address + 24);
    inst.key_map = rom.U32(address + 28);
    inst.key_table = rom.U32(address + 32);
    inst.attack = rom.U32(address + 36);
    inst.decay = rom.U32(address + 40);
    inst.sustain = rom.U32(address + 44);
    inst.release = rom.U32(address + 48);
    inst.fine_tune = rom.S32(address + 52);
    inst.bend_range = rom.S32(address + 56);
    inst.vibrato_rate = rom.U32(address + 60);
    inst.vibrato_depth = rom.S32(address + 64);

    return inst;
}

bool ReadInstrument(const Rom& rom, uint32_t address, Instrument& inst)
{
    inst = Instrument();
    if (!rom.Contains(address, 68))
    {
        return false;
    }

    inst = InstrumentAt(rom, address);

    return true;
}

bool SampleValid(const Rom& rom, const Instrument& inst)
{
    if (!inst.IsSample() || inst.end <= inst.start || inst.end - inst.start > 0x1000000 ||
        !rom.Contains(inst.start, inst.end - inst.start + 1) || inst.rate == 0 || inst.rate > 1000000)
    {
        return false;
    }

    return !inst.Loops() || (inst.loop_length > 0 && inst.loop_length <= inst.end - inst.start);
}

uint32_t SplitInstrument(const Rom& rom, const Instrument& split, int key)
{
    if (key < 0 || key > 127 || !rom.Contains(split.key_map, 128))
    {
        return 0;
    }

    const uint8_t index = rom.U8(split.key_map + uint32_t(key));
    if (index == 0xFF || !rom.Contains(split.key_table + 4u * index, 4))
    {
        return 0;
    }

    return rom.U32(split.key_table + 4u * index);
}

} // namespace agbsf2::rare
