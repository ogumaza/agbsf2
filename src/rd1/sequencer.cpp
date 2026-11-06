// SPDX-License-Identifier: MIT
// A frame-by-frame model of a player of Nintendo R&D1's MIDI-file sound driver.

#include "rd1/sequencer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rd1/driver.h"
#include "rd1/song.h"
#include "rd1/soundfont.h"
#include "rom.h"

namespace agbsf2::rd1
{

namespace
{

// The most notes that a track can queue in a frame before it plays them.
constexpr size_t kQueueSize = 20;

// The markers that start and end the loop.
constexpr char kLoopStartMarker = '[';
constexpr char kLoopEndMarker = ']';

uint32_t ReadBigEndian24(const Rom& rom, uint32_t at)
{
    return uint32_t(rom.U8(at)) << 16 | uint32_t(rom.U8(at + 1)) << 8 | rom.U8(at + 2);
}

} // namespace

uint32_t ChannelRecord::Field(int offset, int bit, int width) const
{
    uint32_t word = 0;
    for (int b = 0; b < 4 && offset + b < kChannelSize; b++)
    {
        word |= uint32_t(bytes[size_t(offset + b)]) << (8 * b);
    }
    return (word >> bit) & ((1u << width) - 1);
}

void ChannelRecord::SetField(int offset, int bit, int width, uint32_t value)
{
    uint32_t word = 0;
    const int size = std::min(4, kChannelSize - offset);
    for (int b = 0; b < size; b++)
    {
        word |= uint32_t(bytes[size_t(offset + b)]) << (8 * b);
    }
    const uint32_t mask = ((1u << width) - 1) << bit;
    word = (word & ~mask) | ((value << bit) & mask);
    for (int b = 0; b < size; b++)
    {
        bytes[size_t(offset + b)] = uint8_t(word >> (8 * b));
    }
}

int ChannelRecord::Program() const
{
    return int(Field(0, 2, 7));
}

int ChannelRecord::Volume() const
{
    return int(Field(2, 7, 7));
}

int ChannelRecord::Pan() const
{
    return int(Field(4, 0, 7));
}

int ChannelRecord::Expression() const
{
    return int(Field(4, 7, 7));
}

int ChannelRecord::Modulation() const
{
    return int(Field(4, 14, 7));
}

int ChannelRecord::LfoTarget() const
{
    return int(Field(7, 4, 2));
}

int ChannelRecord::Bend() const
{
    return int(Field(8, 0, 14));
}

int ChannelRecord::ModDepth() const
{
    return bytes[0xC];
}

int ChannelRecord::Swing() const
{
    return int8_t(bytes[0xD]);
}

int ChannelRecord::BendRange() const
{
    return bytes[0xF];
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, const SongRecord& record)
    : rom_(rom), info_(info), record_(record)
{
    if (!ReadSongFile(rom, record.midi, file_, error_))
    {
        return;
    }
    const int channels = info.players[size_t(record.player)].channels;
    channels_.resize(size_t(channels));
    for (ChannelRecord& c : channels_)
    {
        ResetChannel(c);
    }
    bus_volume_ = record.volume;
    priority_ = record.priority & 0xFF;
    for (ChannelRecord& c : channels_)
    {
        SetPriority(c, priority_);
    }

    const int tracks = std::min(int(file_.tracks.size()), channels);
    tracks_.resize(size_t(tracks));
    track_ticks_.assign(size_t(tracks), 0);
    loop_ticks_.assign(size_t(tracks), 0);
    frame_ticks_.assign(size_t(tracks), 0);
    for (int t = 0; t < tracks; t++)
    {
        TrackState& s = tracks_[size_t(t)];
        s.playing = true;
        s.start = file_.tracks[size_t(t)].start;
        uint32_t at = s.start;
        const uint32_t delta = ReadVariable(rom, at);
        s.wait = delta << 8;
        s.at = at;
        s.frame_status = 0;
        s.frame_at = at;
        s.frame_wait = s.wait;
        track_ticks_[size_t(t)] = delta;
    }
    valid_ = true;
}

bool Sequencer::Valid() const
{
    return valid_;
}

const std::string& Sequencer::Error() const
{
    return error_;
}

// A channel's priority: CC33 plus the song's priority, in u16 +0xA. WarioWare keeps 8 bits of it from bit 5. Rhythm
// Tengoku keeps it from bit 6 (0x0804AD18), in the rest of the halfword.
void Sequencer::SetPriority(ChannelRecord& c, int value) const
{
    if (info_.group_levels)
    {
        c.SetField(0xA, 6, 10, uint32_t(value));
    }
    else
    {
        c.SetField(0xA, 5, 8, uint32_t(value));
    }
}

void Sequencer::ResetChannel(ChannelRecord& c)
{
    c.bytes[0] &= uint8_t(~3u);
    c.SetField(0, 2, 7, 0);
    c.SetField(0, 9, 14, 0);
    c.SetField(2, 7, 7, 100);
    c.bytes[4] = uint8_t((c.bytes[4] & 0x80) | 0x40);
    c.SetField(4, 0, 16, c.Field(4, 0, 16) | 0x3F80);
    c.SetField(6, 5, 7, 0);
    c.SetField(4, 14, 7, 0);
    c.bytes[7] &= uint8_t(~0x30u);
    c.bytes[0xC] = 1;
    c.SetField(0x10, 0, 16, 0x3C00);
    c.SetField(0x12, 0, 16, 0);
    c.bytes[0x14] = 0;
    c.bytes[0x15] = 0;
    c.bytes[0xD] = 0;
    c.SetField(8, 0, 14, 0x2000);
    c.bytes[0xF] = 2;
    SetPriority(c, 0);
    c.bytes[3] &= uint8_t(~0x41u & 0x7Fu);
    c.SetField(0x1A, 0, 16, 0x100);
    c.SetField(0x16, 0, 16, 0x100);
    c.SetField(0x18, 0, 16, 0);
    c.bytes[0x1C] = 0;
    c.bytes[0x1D] = 0;
    c.bytes[0x1E] = 0;
}

const std::vector<SeqEvent>& Sequencer::Step()
{
    events_.clear();
    if (!song_on_)
    {
        return events_;
    }
    RunFade();

    next_step_ = 0;
    loop_start_seen_ = false;
    loop_end_seen_ = false;
    const int tracks = TrackCount();
    for (int t = 0; t < tracks; t++)
    {
        RunTrack(t);
    }
    if (loop_end_seen_)
    {
        loop_passes_++;
        for (int t = 0; t < tracks; t++)
        {
            TrackState& s = tracks_[size_t(t)];
            s.playing = s.loop_playing;
            s.status = s.loop_status;
            s.at = s.loop_at;
            s.wait = s.loop_wait;
            track_ticks_[size_t(t)] = loop_ticks_[size_t(t)];
        }
        SeqEvent e;
        e.kind = SeqEvent::Kind::kLoopEnd;
        e.tick = pass_offset_ + loop_end_tick_;
        events_.push_back(e);
        pass_offset_ += loop_end_tick_ - loop_start_tick_;
        for (int t = 0; t < tracks; t++)
        {
            RunTrack(t);
        }
    }
    if (!loop_armed_)
    {
        if (loop_start_seen_)
        {
            for (int t = 0; t < tracks; t++)
            {
                TrackState& s = tracks_[size_t(t)];
                s.loop_playing = s.playing;
                s.loop_status = s.frame_status;
                s.loop_at = s.frame_at;
                s.loop_wait = s.frame_wait;
                loop_ticks_[size_t(t)] = frame_ticks_[size_t(t)];
            }
            loop_armed_ = true;
            SeqEvent e;
            e.kind = SeqEvent::Kind::kLoopStart;
            e.tick = pass_offset_ + loop_start_tick_;
            events_.push_back(e);
        }
        else
        {
            for (int t = 0; t < tracks; t++)
            {
                TrackState& s = tracks_[size_t(t)];
                s.frame_status = s.status;
                s.frame_at = s.at;
                s.frame_wait = s.wait;
                frame_ticks_[size_t(t)] = track_ticks_[size_t(t)];
            }
        }
    }
    if (next_step_)
    {
        step_ = next_step_;
    }
    bool any = false;
    for (const TrackState& s : tracks_)
    {
        any = any || s.playing;
    }
    song_on_ = any;
    for (int t = 0; t < tracks; t++)
    {
        const TrackState& s = tracks_[size_t(t)];
        if (s.playing)
        {
            position_ = pass_offset_ * 256 + track_ticks_[size_t(t)] * 256 - s.wait;
            break;
        }
    }

    for (ChannelRecord& c : channels_)
    {
        RunLfo(c);
    }
    return events_;
}

void Sequencer::RunFade()
{
    switch (fade_mode_)
    {
    case 1:
        fade_level_ = (fade_level_ + fade_step_) & 0xFFFF;
        if (fade_level_ & 0x8000)
        {
            fade_mode_ = 0;
            fade_level_ = 0x8000;
            fade_step_ = 0;
        }
        break;
    case 2:
    case 3:
        if (fade_level_ < fade_step_)
        {
            fade_level_ = 0;
            if (fade_mode_ == 2)
            {
                fade_mode_ = 0;
                song_on_ = false;
            }
        }
        else
        {
            fade_level_ -= fade_step_;
        }
        break;
    default:
        break;
    }
    bus_volume_ = ((record_.volume * volume_ >> 8) * (fade_level_ >> 8) >> 7) & 0xFF;
}

void Sequencer::RunTrack(int t)
{
    TrackState& s = tracks_[size_t(t)];
    if (!s.playing)
    {
        return;
    }
    queue_.clear();
    while (s.wait < step_)
    {
        if (RunEvent(t))
        {
            s.playing = false;
            SeqEvent e;
            e.kind = SeqEvent::Kind::kTrackEnd;
            e.channel = t;
            e.tick = pass_offset_ + track_ticks_[size_t(t)];
            events_.push_back(e);
            return;
        }
        const uint32_t delta = ReadVariable(rom_, s.at);
        if (delta)
        {
            FlushNotes(t);
        }
        s.wait += delta << 8;
        track_ticks_[size_t(t)] += delta;
    }
    s.wait -= step_;
}

void Sequencer::FlushNotes(int t)
{
    for (const QueuedNote& n : queue_)
    {
        SeqEvent e;
        e.kind = n.velocity ? SeqEvent::Kind::kNoteOn : SeqEvent::Kind::kNoteOff;
        e.channel = n.channel;
        e.key = n.key;
        e.velocity = n.velocity;
        e.tick = pass_offset_ + track_ticks_[size_t(t)];
        events_.push_back(e);

        // A note on that finds an instrument starts the channel's LFO delay again, from CC26.
        ChannelRecord& c = channels_[size_t(n.channel)];
        if (n.velocity && !(c.bytes[0] & 1))
        {
            const InstrumentRecord program =
                ReadInstrument(rom_, ProgramInstrument(rom_, info_, record_.bank, c.Program()));
            if (PlayedInstrument(rom_, program, n.key).kind != InstrumentKind::kNone)
            {
                c.bytes[0x15] = c.bytes[0x14];
            }
        }
    }
    queue_.clear();
}

bool Sequencer::RunEvent(int t)
{
    TrackState& s = tracks_[size_t(t)];
    uint32_t at = s.at;
    const uint8_t first = rom_.U8(at);
    if (first & 0x80)
    {
        s.status = first;
        at++;
    }
    const int status = s.status;
    if (status > 0xEF)
    {
        if ((status & 0xF) == 0)
        {
            const uint32_t length = ReadVariable(rom_, at);
            RunSysEx(at);
            at += length;
        }
        else if ((status & 0xF) == 0xF)
        {
            const uint32_t type_at = at;
            uint32_t next = at;
            RunMeta(t, next);
            if (rom_.U8(type_at) == 0x2F)
            {
                channels_[size_t(t)].SetField(0x10, 0, 16, 0);
                s.at = next;
                return true;
            }
            at = next;
        }
        else
        {
            const uint32_t length = ReadVariable(rom_, at);
            at += length;
        }
        s.at = at;
        return false;
    }
    switch (status & 0xF0)
    {
    case 0x80:
    case 0x90:
        if (queue_.size() < kQueueSize)
        {
            queue_.push_back({t & 0xF, rom_.U8(at) & 0x7F, (status & 0xF0) == 0x90 ? rom_.U8(at + 1) & 0x7F : 0});
        }
        at += 2;
        break;
    case 0xA0:
        at += 2;
        break;
    case 0xB0:
        Control(t, rom_.U8(at), rom_.U8(at + 1));
        at += 2;
        break;
    case 0xC0:
        {
            const int program = rom_.U8(at) & 0x7F;
            channels_[size_t(t)].SetField(0, 2, 7, uint32_t(program));
            SeqEvent e;
            e.kind = SeqEvent::Kind::kProgram;
            e.channel = t;
            e.value = program;
            e.tick = pass_offset_ + track_ticks_[size_t(t)];
            events_.push_back(e);
            at += 1;
            break;
        }
    case 0xD0:
        at += 1;
        break;
    case 0xE0:
        {
            channels_[size_t(t)].SetField(8, 0, 14, uint32_t((rom_.U8(at) & 0x7F) | (rom_.U8(at + 1) & 0x7F) << 7));
            SeqEvent e;
            e.kind = SeqEvent::Kind::kSetting;
            e.channel = t;
            e.tick = pass_offset_ + track_ticks_[size_t(t)];
            events_.push_back(e);
            at += 2;
            break;
        }
    default:
        break;
    }
    s.at = at;
    return false;
}

void Sequencer::RunMeta(int t, uint32_t& at)
{
    const uint8_t type = rom_.U8(at++);
    const uint32_t length = ReadVariable(rom_, at);
    const uint32_t data = at;
    at += length;
    if (type == 0x06)
    {
        if (length == 1 && rom_.U8(data) == uint8_t(kLoopStartMarker))
        {
            loop_start_seen_ = true;
            if (!loop_armed_)
            {
                loop_start_tick_ = track_ticks_[size_t(t)];
            }
        }
        if (length == 1 && rom_.U8(data) == uint8_t(kLoopEndMarker) && loop_armed_)
        {
            loop_end_seen_ = true;
            loop_end_tick_ = track_ticks_[size_t(t)];
        }
    }
    else if (type == 0x51)
    {
        const uint32_t tempo = ReadBigEndian24(rom_, data);
        const uint32_t bpm = tempo ? 60000000u / tempo : 0;
        bpm_ = int(bpm & 0x1FF);
        next_step_ = (bpm & 0xFFFF) * uint32_t(speed_) * uint32_t(file_.division) / 3600u;
        SeqEvent e;
        e.kind = SeqEvent::Kind::kTempo;
        e.channel = t;
        e.value = int(next_step_);
        e.tick = pass_offset_ + track_ticks_[size_t(t)];
        events_.push_back(e);
    }
}

void Sequencer::RunSysEx(uint32_t at)
{
    // Type 1 sets the key table: a shift in semitones for each of the 12 keys of an octave.
    if (rom_.U8(at) == 1)
    {
        for (int k = 0; k < 12; k++)
        {
            key_table_[size_t(k)] = int8_t(rom_.U8(at + 1 + uint32_t(k)) - 0x40);
        }
    }
}

void Sequencer::Control(int t, int controller, int value)
{
    ChannelRecord& c = channels_[size_t(t)];
    SeqEvent e;
    e.kind = SeqEvent::Kind::kSetting;
    e.channel = t;
    e.tick = pass_offset_ + track_ticks_[size_t(t)];
    events_.push_back(e);
    switch (controller)
    {
    case 0:
    case 32:
        {
            uint32_t bank = c.Field(0, 9, 14);
            if (controller == 0)
            {
                bank = ((bank & 0x3F80) | (uint32_t(value) << 7)) & 0xFFFF;
            }
            else
            {
                bank = (bank & 0x7F) | uint32_t(value);
            }
            c.SetField(0, 9, 14, bank & 0x3FFF);
            break;
        }
    case 1:
        c.SetField(4, 14, 7, uint32_t(value));
        break;
    case 7:
        c.SetField(2, 7, 7, uint32_t(value));
        break;
    case 10:
        c.SetField(4, 0, 7, uint32_t(value));
        break;
    case 11:
        c.SetField(4, 7, 7, uint32_t(value));
        break;
    case 20:
        c.bytes[0xF] = uint8_t(value);
        break;
    case 21:
        c.SetField(0x10, 0, 16, uint32_t(value) << 8);
        break;
    case 22:
        c.SetField(7, 4, 2, uint32_t(value));
        break;
    case 26:
        c.bytes[0x14] = uint8_t(value);
        break;
    case 33:
        SetPriority(c, value + priority_);
        break;
    case 72:
        c.SetField(3, 6, 1, uint32_t(value));
        break;
    case 75:
        c.bytes[3] = uint8_t((c.bytes[3] & 0x7F) | (value << 7));
        break;
    case 82:
        {
            const uint32_t low = 0x8000u / uint32_t(value + 128);
            const uint32_t high = 0x10000u / uint32_t(256 - value);
            c.SetField(0x16, 0, 16, low);
            c.SetField(0x18, 0, 16, high - low);
            c.SetField(0x1A, 0, 16, 0x100);
            break;
        }
    case 83:
        c.bytes[0x1C] = uint8_t(value);
        break;
    case 84:
        c.bytes[0x1D] = uint8_t(value);
        c.bytes[0x1E] = uint8_t(value);
        break;
    default:
        break;
    }
}

void Sequencer::RunLfo(ChannelRecord& c)
{
    if (c.bytes[0x15])
    {
        c.bytes[0x15]--;
        c.SetField(0x12, 0, 16, 0);
    }
    else
    {
        c.SetField(0x12, 0, 16, c.Field(0x12, 0, 16) + c.Field(0x10, 0, 16));
    }
    const uint32_t phase = c.Field(0x12, 0, 16);
    const int sine = int16_t(rom_.U16(info_.sine_table + 2 * (phase >> 8)));
    c.bytes[0xD] = uint8_t((sine * c.Modulation()) >> 8);
    int lfo_volume = 0x80;
    if (c.LfoTarget() == 1)
    {
        lfo_volume = std::clamp(c.Swing() + 0x80, 0, 0xA0);
    }
    const int level = (bus_volume_ * c.Volume() * c.Expression() * lfo_volume) >> 21;
    if (info_.group_levels)
    {
        c.SetField(8, 14, 8, uint32_t(std::min(level, 0xFF)));
    }
    else
    {
        c.SetField(8, 14, 7, uint32_t(level & 0x7F));
    }
    if (c.bytes[0x1C] && c.bytes[0x1D])
    {
        if (!c.bytes[0x1E])
        {
            c.bytes[0x1E] = c.bytes[0x1D];
        }
        c.bytes[0x1E]--;
    }
}

bool Sequencer::Playing() const
{
    return song_on_;
}

int Sequencer::TrackCount() const
{
    return int(tracks_.size());
}

int Sequencer::ChannelCount() const
{
    return int(channels_.size());
}

int Sequencer::FadeLevel() const
{
    return fade_level_;
}

int Sequencer::FadeStep() const
{
    return fade_step_;
}

int Sequencer::FadeMode() const
{
    return fade_mode_;
}

const TrackState& Sequencer::Track(int t) const
{
    return tracks_[size_t(t)];
}

const ChannelRecord& Sequencer::Channel(int c) const
{
    return channels_[size_t(c)];
}

int Sequencer::Bpm() const
{
    return bpm_;
}

uint32_t Sequencer::TickStep() const
{
    return step_;
}

int Sequencer::Division() const
{
    return file_.division;
}

int Sequencer::ChannelLevel(int c) const
{
    return int(channels_[size_t(c)].Field(8, 14, info_.group_levels ? 8 : 7));
}

int Sequencer::BusPan() const
{
    return bus_pan_;
}

uint64_t Sequencer::Position() const
{
    return position_;
}

int Sequencer::LoopPasses() const
{
    return loop_passes_;
}

const std::vector<std::string>& Sequencer::Warnings() const
{
    return warnings_;
}

} // namespace agbsf2::rd1
