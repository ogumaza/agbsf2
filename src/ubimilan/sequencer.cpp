// SPDX-License-Identifier: MIT

// The model of Ubisoft Milan's driver. It follows the game's code: the sequencer's step that the VBlank runs, the sound
// update in the game's main loop, which frees the voices whose samples ended and moves the piece on to its next
// sequence, the mixer's progress through each voice's sample, and the writes that the PSG's notes make.

#include "ubimilan/sequencer.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace supergbamidi::ubimilan
{
namespace
{

// The PSG's registers that the driver writes.
constexpr uint32_t kNr10 = 0x04000060;
constexpr uint32_t kNr11 = 0x04000062;
constexpr uint32_t kNr13 = 0x04000064;
constexpr uint32_t kNr21 = 0x04000068;
constexpr uint32_t kNr23 = 0x0400006C;
constexpr uint32_t kNr30 = 0x04000070;
constexpr uint32_t kNr31 = 0x04000072;
constexpr uint32_t kNr33 = 0x04000074;
constexpr uint32_t kNr41 = 0x04000078;
constexpr uint32_t kNr43 = 0x0400007C;
constexpr uint32_t kWaveRam = 0x04000090;

// The longest that a song plays: an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// An instrument's flags: modulation, a pitch mode, and a note that doesn't start the channel again.
constexpr uint16_t kModulation = 1;
constexpr uint16_t kPitchMode = 2;
constexpr uint16_t kLegato = 4;

// Returns the place of a key in the PSG's frequency table: 36 keys down on the squares and 12 on the wave channel, as
// a byte.
int KeyIndex(int channel, int key)
{
    if (channel == 0 || channel == 1)
    {
        return (key - 36) & 0xFF;
    }

    return channel == 2 ? (key - 12) & 0xFF : key;
}

} // namespace

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song) : rom_(rom), info_(info)
{
    if (!ReadMusic(rom, info, song, piece_))
    {
        return;
    }

    const uint32_t first = ResourceAddress(rom, info, piece_.segments[0]);
    if (!first || rom.U8(first) != kSequence || !rom.Contains(first + 4, 2))
    {
        return;
    }

    valid_ = true;

    // The piece's start: a voice for the kit if the piece keeps one, the PSG's envelopes silenced, the kit's voice
    // stopped, then the track on the first sequence, with the next one queued.
    kit_voice_fixed_ = piece_.flags & 8;
    if (kit_voice_fixed_)
    {
        kit_voice_ = kVoiceCount - 1;
        voices_[size_t(kit_voice_)].busy = true;
    }

    for (uint32_t address : {kNr11, kNr21, kNr31, kNr41})
    {
        Write(address, 2, 0);
    }

    wait_ = rom.U16(first + 4);
    pointer_ = first + 6;
    const uint32_t next = NextSegment(1);
    next_ = next ? next + 4 : 0;
    active_ = true;
    playing_ = true;
    segment_starts_.push_back(0);
}

uint32_t Sequencer::NextSegment(int ahead) const
{
    // The sequence `ahead` places after the one that plays, wrapping round if the piece loops; a piece of one sequence
    // plays it again if it loops.
    const int count = int(piece_.segments.size());
    const bool loops = piece_.flags & 1;
    if (count == 1)
    {
        return loops ? ResourceAddress(rom_, info_, piece_.segments[0]) : 0;
    }

    int index = index_ + ahead;
    if (index >= count)
    {
        if (!loops)
        {
            return 0;
        }
        index -= count;
    }

    return ResourceAddress(rom_, info_, piece_.segments[size_t(index)]);
}

std::vector<RegisterWrite> Sequencer::Step()
{
    if (frame_ < kMaxFrames)
    {
        SequencerStep();
        SoundUpdate();

        // The mixer's runs before the next frame: 16 points of output each, at Timer 0's rate.
        const uint64_t run_cycles = kRunOutput * (0x10000 - info_.timer_reload);
        const uint64_t due = (uint64_t(frame_) + 1) * kFrameCycles / run_cycles;
        while (runs_ < due)
        {
            MixerRun();
            runs_++;
        }
    }
    frame_++;

    // The writes since the last step, which for the first include the piece's start.
    std::vector<RegisterWrite> writes;
    writes.swap(writes_);

    return writes;
}

void Sequencer::SequencerStep()
{
    if (!active_)
    {
        return;
    }
    if (wait_)
    {
        wait_--;
        return;
    }

    // Commands until the countdown isn't 0. An end command makes the track take the next sequence, if there's one,
    // and the game's callback gives the track the sequence after it, as a resource that the sound update corrects.
    while (active_ && !wait_)
    {
        if (ending_)
        {
            ending_ = false;
            if (next_)
            {
                wait_ = rom_.U16(next_);
                pointer_ = next_ + 2;
                segment_starts_.push_back(frame_);
            }
            else
            {
                active_ = false;
                playing_ = false;
            }
            next_ = active_ ? NextSegment(2) : 0;
            changed_ = true;
            continue;
        }

        if (!rom_.Contains(pointer_, 1))
        {
            Stop("its commands run out of the ROM");
            return;
        }
        const int status = rom_.U8(pointer_++);
        if (status == 0xFC)
        {
            ending_ = true;
        }
        else if (status >= 0xF0)
        {
            char text[96];
            std::snprintf(text, sizeof text, "command %02X at 0x%08X, on which the game resets", unsigned(status),
                          unsigned(pointer_ - 1));
            Stop(text);
            return;
        }
        else
        {
            Event(status);
        }
    }
}

void Sequencer::Event(int status)
{
    // A channel's command, then the next command's countdown. A note's or a program's first byte decides where it
    // goes: channel 9's keys and programs up to 11 go to the PSG's noise, its others to the kit.
    const int channel = status & 0xF;
    const int kind = status & 0xF0;
    if (kind == 0x90 || kind == 0x80)
    {
        const int key = rom_.U8(pointer_);
        const int velocity = rom_.U8(pointer_ + 1);
        pointer_ += 2;
        if (kind == 0x90)
        {
            NoteOn(channel, key, velocity);
        }
        else
        {
            NoteOff(channel, key);
        }
    }
    else if (kind == 0xC0)
    {
        Program(channel, rom_.U8(pointer_++));
    }
    else if (kind != 0xE0)
    {
        char text[96];
        std::snprintf(text, sizeof text, "command %02X at 0x%08X, on which the game resets", unsigned(status),
                      unsigned(pointer_ - 1));
        Stop(text);
        return;
    }

    wait_ = rom_.U16(pointer_);
    pointer_ += 2;
}

void Sequencer::NoteOn(int channel, int key, int velocity)
{
    if (channel < kPsgChannels || (channel == kKitChannel && key <= kLastNoiseKey))
    {
        PsgNoteOn(channel, key, velocity);
    }
    else if (channel == kKitChannel)
    {
        KitNoteOn(key, velocity);
    }
}

void Sequencer::NoteOff(int channel, int key)
{
    if (channel < kPsgChannels || (channel == kKitChannel && key <= kLastNoiseKey))
    {
        PsgNoteOff(channel, key);
    }
    else if (channel == kKitChannel)
    {
        KitNoteOff();
    }
}

void Sequencer::Program(int channel, int program)
{
    if (channel == kKitChannel && program > kLastNoiseKey)
    {
        kit_program_ = program;
        return;
    }
    if (channel >= kPsgChannels)
    {
        return;
    }

    // The wave channel's program loads its wave into both banks of the wave RAM, when it changes.
    PsgChannel& psg = psg_[size_t(channel)];
    if (channel == 2 && program != psg.program && info_.wave_table)
    {
        const uint32_t wave = info_.wave_table + 32 * uint32_t(program);
        for (int bank = 0; bank < 2; bank++)
        {
            Write(kNr30, 2, bank ? 0x60 : 0x20);
            for (uint32_t i = 0; i < 16; i++)
            {
                Write(kWaveRam + i, 1, rom_.U8(wave + 16 * uint32_t(bank) + i));
            }
        }
    }
    psg.program = program;
}

void Sequencer::KitNoteOn(int key, int velocity)
{
    // The kit's entry for the key, moved on by the program, names the sample.
    const int index = kit_program_ + key;
    const uint16_t entry = info_.kit ? rom_.U16(info_.kit + 2 * uint32_t(index)) : 0x8000;
    const uint32_t address = entry != 0x8000 ? ResourceAddress(rom_, info_, entry) : 0;
    GameSample sample;
    if (!ReadSample(rom_, address, sample))
    {
        char text[96];
        std::snprintf(text, sizeof text, "channel 9's key %d plays no sample, which the game doesn't check", key);
        Warn(text);
        return;
    }

    // The piece's voice for the kit, or the last voice that no sound has.
    int v = kit_voice_;
    if (!kit_voice_fixed_)
    {
        v = -1;
        for (int i = 0; i < kVoiceCount; i++)
        {
            v = voices_[size_t(i)].busy ? v : i;
        }
        if (v < 0)
        {
            Warn("a note of channel 9 finds no voice, on which the game writes before the voices");
            return;
        }
        voices_[size_t(v)].busy = true;
        kit_voice_ = v;
    }

    // The voice starts the sample at the note's velocity; a sample that doesn't loop keeps the last loop's points.
    Voice& voice = voices_[size_t(v)];
    if (voice.note >= 0)
    {
        EndNote(voice.note, frame_);
    }
    voice.position = sample.data;
    voice.end = sample.data + sample.length;
    voice.loop = sample.loop;
    if (sample.loop)
    {
        voice.loop_start = sample.data + sample.loop_start;
        voice.loop_end = sample.data + sample.loop_end;
    }
    voice.volume = uint32_t(velocity);
    voice.stopped = false;

    Note n;
    n.channel = kKitChannel;
    n.key = key;
    n.velocity = velocity;
    n.program = kit_program_;
    n.on = frame_;
    n.sample = address;
    voice.note = int(notes_.size());
    notes_.push_back(n);
}

void Sequencer::KitNoteOff()
{
    // Whatever its key, a note-off stops the voice of the kit's last note, and frees it.
    Voice& voice = voices_[size_t(kit_voice_)];
    if (voice.note >= 0)
    {
        EndNote(voice.note, frame_);
        voice.note = -1;
    }
    voice.stopped = true;
    voice.busy = false;
}

void Sequencer::PsgNoteOn(int channel, int key, int velocity)
{
    const int index = KeyIndex(channel, key);

    // Channel 9's noise: the second byte of the key's entry is ORed with the velocity, to NR42.
    if (channel == kKitChannel)
    {
        if (!info_.noise_table)
        {
            return;
        }
        const uint32_t entry = info_.noise_table + 4 * uint32_t(index);
        const uint8_t envelope = uint8_t(rom_.U8(entry + 1) | velocity);
        Write(kNr41, 2, rom_.U8(entry) | (envelope << 8));
        Write(kNr43, 2, rom_.U8(entry + 2) | (rom_.U8(entry + 3) << 8));

        Note n;
        n.channel = channel;
        n.key = key;
        n.velocity = velocity;
        n.program = kit_program_;
        n.on = frame_;
        n.envelope = envelope;
        n.control = rom_.U8(entry + 2);
        notes_.push_back(n);
        return;
    }
    if (!info_.instrument_tables[size_t(channel)] || !info_.frequency_table)
    {
        return;
    }

    PsgChannel& psg = psg_[size_t(channel)];
    const uint32_t inst = info_.instrument_tables[size_t(channel)] + 16 * uint32_t(psg.program);
    const uint16_t flags = rom_.U16(inst + 12);
    if (flags & (kModulation | kPitchMode | kLegato))
    {
        char text[128];
        std::snprintf(text, sizeof text,
                      "PSG instrument %d of channel %d has a modulation, a pitch mode or legato, "
                      "which the model leaves out",
                      psg.program, channel);
        Warn(text);
    }

    Note n;
    n.channel = channel;
    n.key = key;
    n.velocity = velocity;
    n.program = psg.program;
    n.on = frame_;

    // The instrument's bytes and the velocity, which is ORed into the envelope, then the key's frequency.
    const uint16_t frequency = rom_.U16(info_.frequency_table + 2 * uint32_t(index));
    if (channel == 0)
    {
        n.duty = rom_.U8(inst + 1);
        n.envelope = uint8_t(rom_.U8(inst + 2) | velocity);
        Write(kNr10, 2, rom_.U8(inst));
        Write(kNr11, 2, n.duty | (n.envelope << 8));
        Write(kNr13, 2, (rom_.U8(inst + 3) << 8) | frequency);
    }
    else if (channel == 1)
    {
        n.duty = rom_.U8(inst);
        n.envelope = uint8_t(rom_.U8(inst + 1) | velocity);
        Write(kNr21, 2, n.duty | (n.envelope << 8));
        Write(kNr23, 2, (rom_.U8(inst + 2) << 8) | frequency);
    }
    else
    {
        n.envelope = uint8_t(velocity);
        n.control = uint16_t(psg.program);
        Write(kNr30, 2, 0xA0);
        Write(kNr31, 2, rom_.U8(inst) | (velocity << 8));
        Write(kNr33, 2, (rom_.U8(inst + 1) << 8) | frequency);
    }

    if (psg.note >= 0)
    {
        EndNote(psg.note, frame_);
    }
    psg.on = true;
    psg.key = index;
    psg.note = int(notes_.size());
    notes_.push_back(n);
}

void Sequencer::PsgNoteOff(int channel, int key)
{
    // The noise isn't silenced: its envelope and length run on. Its note ends here, as the MIDI file has it.
    if (channel >= kPsgChannels)
    {
        for (size_t i = notes_.size(); i-- > 0;)
        {
            if (notes_[i].channel == channel && notes_[i].key == key && !notes_[i].ended)
            {
                EndNote(int(i), frame_);
                break;
            }
        }
        return;
    }

    // A note-off silences the channel if its key is the one that the last note set. GCC warns that a negative channel
    // would index past psg_. at() checks the index.
    PsgChannel& psg = psg_.at(size_t(channel));
    if (psg.key != KeyIndex(channel, key))
    {
        return;
    }
    psg.on = false;
    Write(channel == 0 ? kNr11 : channel == 1 ? kNr21 : kNr31, 2, 0);
    if (psg.note >= 0)
    {
        EndNote(psg.note, frame_);
        psg.note = -1;
    }
}

void Sequencer::SoundUpdate()
{
    // The voices whose samples the mixer has stopped are free again.
    for (Voice& voice : voices_)
    {
        if (voice.busy && voice.stopped)
        {
            voice.busy = false;
        }
    }

    // After an end command, the track's next sequence is the one after the next, in place of the resource that the
    // callback gave it, and the piece moves on, unless it has stopped.
    if (!playing_ || !changed_)
    {
        return;
    }

    changed_ = false;
    const uint32_t next = NextSegment(2);
    if (next && rom_.U8(next) == kSequence)
    {
        next_ = next + 4;
    }
    else if (active_)
    {
        Warn("the piece's sequence after next isn't a sequence, on which the game resets");
        next_ = 0;
    }

    const int count = int(piece_.segments.size());
    index_ = index_ == count - 1 ? 0 : index_ + 1;
}

void Sequencer::MixerRun()
{
    // Each voice moves on 8 points; one that loops goes back to its loop's start once it reaches the loop's end, and
    // one that doesn't stops once it reaches its end, after playing those points.
    for (Voice& voice : voices_)
    {
        if (voice.stopped)
        {
            continue;
        }

        voice.position += kRunPoints;
        if (voice.loop)
        {
            if (voice.position >= voice.loop_end)
            {
                voice.position = voice.loop_start;
            }
        }
        else if (voice.position >= voice.end)
        {
            voice.stopped = true;
            if (voice.note >= 0)
            {
                EndNote(voice.note, frame_ + 1);
                voice.note = -1;
            }
        }
    }
}

void Sequencer::EndNote(int index, uint32_t frame)
{
    Note& n = notes_[size_t(index)];
    if (!n.ended)
    {
        n.ended = true;
        n.off = frame;
    }
}

void Sequencer::Stop(const std::string& why)
{
    Warn(why);
    active_ = false;
}

void Sequencer::Warn(const std::string& message)
{
    if (std::find(warnings_.begin(), warnings_.end(), message) == warnings_.end())
    {
        warnings_.push_back(message);
    }
}

bool Sequencer::Playing() const
{
    if (active_)
    {
        return true;
    }

    for (const Voice& voice : voices_)
    {
        if (!voice.stopped)
        {
            return true;
        }
    }

    return false;
}

std::array<uint32_t, 5> Sequencer::TrackState() const
{
    return {wait_, pointer_, next_, active_ ? 1u : 0u, ending_ ? 1u : 0u};
}

std::array<uint32_t, 6> Sequencer::VoiceState(int v) const
{
    const Voice& voice = voices_[size_t(v)];
    return {voice.position, voice.loop_start, voice.end, voice.loop_end, voice.volume, voice.loop ? 1u : 0u};
}

void Sequencer::Write(uint32_t address, uint8_t size, uint32_t value)
{
    writes_.push_back({address, size, value});
}

} // namespace supergbamidi::ubimilan
