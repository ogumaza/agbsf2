// SPDX-License-Identifier: MIT

// The model of Ubisoft Milan's driver. It follows the game's code: the sequencer's step that the VBlank runs, the sound
// update in the game's main loop, which frees the voices whose samples ended and moves the piece on to its next
// sequence, the mixer's progress through each voice's sample, and the writes that the PSG's notes make.

#include "ubimilan/sequencer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "gba.h"
#include "rom.h"
#include "ubimilan/driver.h"
#include "ubimilan/song.h"
#include "warnings.h"

namespace agbsf2::ubimilan
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

// An instrument's flags: modulation, a pitch mode, and a note that doesn't start the channel again.
constexpr uint16_t kModulation = 1;
constexpr uint16_t kPitchMode = 2;
constexpr uint16_t kLegato = 4;

// Returns the place of a key in the PSG's frequency table: 36 keys down on the squares and 12 on the wave channel, as a
// byte.
int KeyIndex(int channel, int key)
{
    if (channel == kWaveChannel)
    {
        return (key - 12) & 0xFF;
    }

    return (key - 36) & 0xFF;
}

// Divides as the game's library does: towards 0, with 0 for a divisor of 0.
int32_t Divide(int32_t a, int32_t b)
{
    return b ? int32_t(int64_t(a) / int64_t(b)) : 0;
}

// Returns the warning for the unknown status byte `status` at `address`.
std::string UnknownCommand(int status, uint32_t address)
{
    char text[96];
    std::snprintf(text, sizeof text, "command %02X at 0x%08X, on which the game resets", unsigned(status),
                  unsigned(address));

    return text;
}

} // namespace

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song, bool kit) : rom_(rom), info_(info), kit_on_(kit)
{
    if (!ReadMusic(rom, info, song, piece_))
    {
        return;
    }

    if (piece_.sample >= 0)
    {
        StartSamplePiece(song);
        return;
    }

    const uint32_t first = ResourceAddress(rom, info, piece_.sequences[0]);
    if (!first || rom.U8(first) != kSequence || !rom.Contains(first + kSequenceHeader, 2))
    {
        return;
    }

    valid_ = true;

    // The piece's start: a voice for the kit if the piece keeps one, the PSG's envelopes silenced, the kit's voice
    // stopped, then the track on the first sequence, with the next one queued.
    kit_voice_fixed_ = piece_.KeepsKitVoice();
    if (kit_voice_fixed_)
    {
        kit_voice_ = kVoiceCount - 1;
        voices_[size_t(kit_voice_)].busy = true;
    }

    for (uint32_t address : {kNr11, kNr21, kNr31, kNr41})
    {
        Write(address, 2, 0);
    }

    wait_ = rom.U16(first + kSequenceHeader);
    pointer_ = first + kSequenceHeader + 2;
    const uint32_t next = NextSequence(1);
    next_ = next ? next + kSequenceHeader : 0;
    active_ = true;
    piece_playing_ = true;
    sequence_starts_.push_back(0);
}

void Sequencer::StartSamplePiece(int song)
{
    GameSample sample;
    if (!ReadSample(rom_, piece_.address, sample))
    {
        return;
    }

    valid_ = true;

    // The game's sound event starts the sample on a voice at full volume, with no sequence.
    kit_voice_fixed_ = true;
    kit_voice_ = kVoiceCount - 1;
    Voice& voice = voices_[size_t(kit_voice_)];
    voice.busy = true;
    StartVoice(voice, sample, kFullVolume);

    Note n = NewNote(kKitChannel, kFirstSampleKey + song - info_.piece_count, kFullVolume, 0);
    n.sample = piece_.address;
    voice.note = int(notes_.size());
    notes_.push_back(n);
    sequence_starts_.push_back(0);
}

void Sequencer::NextSamplePass(Voice& voice)
{
    // The voice goes on looping. A file that loops on its markers needs a note for each pass. Each note starts on the
    // frame nearest the moment that the pass starts.
    const uint64_t cycles = (runs_ + 1) * kRunOutput * OutputCycles(info_);
    const uint32_t frame = uint32_t((cycles + kFrameCycles / 2) / kFrameCycles);
    EndNote(voice.note, frame);

    // The next pass goes on from the loop's start. In a sample with points before its loop, that's another key.
    Note n = notes_[size_t(voice.note)];
    n.on = frame;
    n.ended = false;
    n.off = 0;
    GameSample sample;
    if (ReadSample(rom_, piece_.address, sample) && sample.loop_start && !n.skip)
    {
        n.key += kLoopKeyOffset;
        n.skip = sample.loop_start;
    }
    voice.note = int(notes_.size());
    notes_.push_back(n);
    sequence_starts_.push_back(frame);
}

uint32_t Sequencer::NextSequence(int ahead) const
{
    // The sequence `ahead` places after the one that plays, wrapping round if the piece loops; a piece of one sequence
    // plays it again if it loops.
    const int count = int(piece_.sequences.size());
    const bool loops = piece_.Loops();
    if (count == 1)
    {
        return loops ? ResourceAddress(rom_, info_, piece_.sequences[0]) : 0;
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

    return ResourceAddress(rom_, info_, piece_.sequences[size_t(index)]);
}

std::vector<RegisterWrite> Sequencer::Step()
{
    if (frame_ < kMaxFrames)
    {
        SequencerStep();
        SoundUpdate();

        // The mixer's runs before the next frame: 16 points of output each, at Timer 0's rate.
        const uint64_t run_cycles = kRunOutput * OutputCycles(info_);
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
        PsgUpdate();
        return;
    }

    // Commands until the countdown isn't 0. An end command makes the track take the next sequence, if there's one, and
    // the game's callback gives the track the sequence after it, as a resource that the sound update corrects. The
    // PSG's update follows them, even in the frame in which the piece stops.
    while (active_ && !wait_)
    {
        if (ending_)
        {
            ending_ = false;
            if (next_)
            {
                wait_ = rom_.U16(next_);
                pointer_ = next_ + 2;
                sequence_starts_.push_back(frame_);
            }
            else
            {
                active_ = false;
                piece_playing_ = false;
            }
            next_ = active_ ? NextSequence(2) : 0;
            changed_ = true;
            continue;
        }

        if (!rom_.Contains(pointer_, 1))
        {
            Stop("its commands run out of the ROM");
            return;
        }
        RunCommand(rom_.U8(pointer_++));
    }
    PsgUpdate();
}

void Sequencer::RunCommand(int status)
{
    // A channel's command, then the next command's countdown. On channel 9, a note's or a program's first byte decides
    // where it goes: keys up to 11 play the PSG's noise, programs up to 11 go to the PSG, and the rest go to the kit.
    // The end has no countdown, and any other status makes the game reset.
    const int channel = status & 0xF;
    switch (status & 0xF0)
    {
    case 0x80:
        NoteOff(channel, rom_.U8(pointer_));
        break;

    case 0x90:
        NoteOn(channel, rom_.U8(pointer_), rom_.U8(pointer_ + 1));
        break;

    case 0xC0:
        Program(channel, rom_.U8(pointer_));
        break;

    case 0xE0:
        break;

    case 0xF0:
        if (status == kEndCommand)
        {
            ending_ = true;
            return;
        }
        [[fallthrough]];

    default:
        Stop(UnknownCommand(status, pointer_ - 1));
        return;
    }

    pointer_ += uint32_t(ArgumentBytes(status));
    wait_ = rom_.U16(pointer_);
    pointer_ += 2;
}

// The kit's notes and note-offs play only while the game's switch of the kit is on. In The Mummy, the channel command
// tests the switch at 0x08004942 and 0x08004982.
void Sequencer::NoteOn(int channel, int key, int velocity)
{
    if (channel < kPsgChannels)
    {
        PsgNoteOn(channel, key, velocity);
    }
    else if (PlaysNoise(channel, key))
    {
        NoiseNoteOn(key, velocity);
    }
    else if (PlaysKit(channel, key) && kit_on_)
    {
        KitNoteOn(key, velocity);
    }
}

void Sequencer::NoteOff(int channel, int key)
{
    if (channel < kPsgChannels)
    {
        PsgNoteOff(channel, key);
    }
    else if (PlaysNoise(channel, key))
    {
        NoiseNoteOff(key);
    }
    else if (PlaysKit(channel, key) && kit_on_)
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

    // The PSG takes channel 9's programs up to 11 and stores them past the end of its three channels' records. They
    // change nothing that the music plays.
    if (channel >= kPsgChannels)
    {
        return;
    }

    // The wave channel's program loads its wave into both banks of the wave RAM, when it changes.
    PsgChannel& psg = psg_[size_t(channel)];
    if (channel == kWaveChannel && program != psg.program && info_.wave_table)
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

Note Sequencer::NewNote(int channel, int key, int velocity, int program) const
{
    Note n;
    n.channel = channel;
    n.key = key;
    n.velocity = velocity;
    n.program = program;
    n.on = frame_;

    return n;
}

void Sequencer::KitNoteOn(int key, int velocity)
{
    // The kit's entry for the key, moved on by the program, names the sample.
    const int index = kit_program_ + key;
    const uint16_t entry = info_.kit ? rom_.U16(info_.kit + 2 * uint32_t(index)) : kNoResource;
    const uint32_t address = entry != kNoResource ? ResourceAddress(rom_, info_, entry) : 0;
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

    Voice& voice = voices_[size_t(v)];
    if (voice.note >= 0)
    {
        EndNote(voice.note, frame_);
    }
    StartVoice(voice, sample, velocity);

    Note n = NewNote(kKitChannel, key, velocity, kit_program_);
    n.sample = address;
    voice.note = int(notes_.size());
    notes_.push_back(n);
}

void Sequencer::StartVoice(Voice& voice, const GameSample& sample, int velocity)
{
    // The voice starts the sample at the note's velocity; a sample that doesn't loop keeps the last loop's points.
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

void Sequencer::NoiseNoteOn(int key, int velocity)
{
    if (!info_.noise_table)
    {
        return;
    }

    // The key's entry in the noise table. Its second byte is ORed with the velocity, to NR42.
    const uint32_t entry = info_.noise_table + 4 * uint32_t(key);
    const uint8_t envelope = uint8_t(rom_.U8(entry + 1) | velocity);
    Write(kNr41, 2, rom_.U8(entry) | (envelope << 8));
    Write(kNr43, 2, rom_.U16(entry + 2));

    Note n = NewNote(kKitChannel, key, velocity, kit_program_);
    n.envelope = envelope;
    n.control = rom_.U8(entry + 2);
    notes_.push_back(n);
}

void Sequencer::NoiseNoteOff(int key)
{
    // The noise isn't silenced: its envelope and length run on. The model and the MIDI file end the note here.
    for (size_t i = notes_.size(); i-- > 0;)
    {
        if (notes_[i].channel == kKitChannel && notes_[i].key == key && !notes_[i].ended)
        {
            EndNote(int(i), frame_);
            break;
        }
    }
}

void Sequencer::PsgNoteOn(int channel, int key, int velocity)
{
    if (!info_.instrument_tables[size_t(channel)] || !info_.frequency_table)
    {
        return;
    }

    // A note continues from the one before if the channel plays and the instrument has legato. Otherwise it starts the
    // channel. Either way the channel stores the new key, and the next note-off has to match it.
    PsgChannel& psg = psg_[size_t(channel)];
    const int index = KeyIndex(channel, key);
    Note n = NewNote(channel, key, velocity, psg.program);
    const uint32_t flags = info_.instrument_tables[0] + 16 * uint32_t(psg.program + 30 * channel) + 12;
    if (psg.playing && (rom_.U16(flags) & kLegato))
    {
        PsgLegato(channel, index);
        n.legato = true;
        if (psg.note >= 0)
        {
            n.duty = notes_[size_t(psg.note)].duty;
            n.envelope = notes_[size_t(psg.note)].envelope;
        }
    }
    else
    {
        PsgStart(channel, index, velocity);
        psg.playing = true;
        const uint32_t inst = Instrument(channel, psg.program);
        n.duty = channel == 0 ? rom_.U8(inst + 1) : channel == 1 ? rom_.U8(inst) : 0;
        n.envelope = uint8_t(velocity | (channel == 0 ? rom_.U8(inst + 2) : channel == 1 ? rom_.U8(inst + 1) : 0));
    }
    psg.index = index;

    if (psg.note >= 0)
    {
        EndNote(psg.note, frame_);
    }
    psg.note = int(notes_.size());
    notes_.push_back(n);
}

uint32_t Sequencer::Instrument(int channel, int program) const
{
    return info_.instrument_tables[size_t(channel)] + 16 * uint32_t(program & 0xFF);
}

uint16_t Sequencer::Frequency(int index) const
{
    return rom_.U16(info_.frequency_table + 2 * uint32_t(index));
}

uint16_t Sequencer::FineFrequency(int32_t cents) const
{
    return info_.fine_table ? rom_.U16(info_.fine_table + 2 * uint32_t(cents)) : 0;
}

void Sequencer::PsgStart(int channel, int index, int velocity)
{
    // The instrument's bytes and the velocity, which is ORed into the envelope. The wave channel's DAC comes on, and
    // its velocity is its volume. The last byte for the channel goes above the frequency setting, in NRx4.
    PsgChannel& psg = psg_[size_t(channel)];
    const uint32_t inst = Instrument(channel, psg.program);
    uint32_t frequency_register = 0;
    uint32_t high = 0;
    if (channel == 0)
    {
        Write(kNr10, 2, rom_.U8(inst));
        Write(kNr11, 2, rom_.U8(inst + 1) | (uint32_t(velocity | rom_.U8(inst + 2)) << 8));
        frequency_register = kNr13;
        high = uint32_t(rom_.U8(inst + 3)) << 8;
    }
    else if (channel == 1)
    {
        Write(kNr21, 2, rom_.U8(inst) | (uint32_t(velocity | rom_.U8(inst + 1)) << 8));
        frequency_register = kNr23;
        high = uint32_t(rom_.U8(inst + 2)) << 8;
    }
    else
    {
        Write(kNr30, 2, 0xA0);
        Write(kNr31, 2, rom_.U8(inst) | (uint32_t(velocity) << 8));
        frequency_register = kNr33;
        high = uint32_t(rom_.U8(inst + 1)) << 8;
    }

    // The modulation starts with its delay. Without the fine table, the model leaves out the modulation and the pitch
    // mode, as detection warns.
    const uint16_t kept = info_.fine_table ? 0xFFFF : uint16_t(~(kModulation | kPitchMode));
    const uint16_t flags = rom_.U16(inst + 12) & kept;
    Modulation& mod = psg.modulation;
    if (flags & kModulation)
    {
        mod.value = rom_.U16(inst + 4);
        mod.step = Divide(int32_t(uint32_t(rom_.U16(inst + 6)) << 16), int32_t(rom_.U16(inst + 8)) * 2);
        mod.depth = rom_.U16(inst + 6);
        mod.state = 1;
    }
    else
    {
        mod.state = 0;
    }

    // The pitch mode's first note starts at its key. A later one glides there from the note before, or from where a
    // glide has reached.
    PitchMode& pitch = psg.pitch;
    const int32_t target = 100 * index;
    if (!(flags & kPitchMode))
    {
        Write(frequency_register, 2, (high | Frequency(index)) & 0xFFFF);
        pitch.state = 0;
        return;
    }

    switch (pitch.state)
    {
    case 0:
        Write(frequency_register, 2, (high | Frequency(index)) & 0xFFFF);
        pitch.frames = rom_.U16(inst + 10);
        pitch.state = 1;
        pitch.previous = pitch.current = pitch.target = target;
        break;

    case 1:
        Write(frequency_register, 2, (high | Frequency(psg.index)) & 0xFFFF);
        Glide(pitch, inst, target);
        break;

    case 2:
    case 3:
        Write(frequency_register, 2, (high | FineFrequency(pitch.current)) & 0xFFFF);
        Glide(pitch, inst, target);
        break;

    default:
        break;
    }
}

void Sequencer::Glide(PitchMode& pitch, uint32_t inst, int32_t target) const
{
    // A glide to the note's place in cents, over the instrument's frames. A glide to the place of the last target
    // keeps its step at 0. The pitch then stays where it is unless it's below the target.
    if (pitch.state == 1)
    {
        pitch.state = 2;
    }
    else
    {
        pitch.previous = pitch.target;
    }

    pitch.target = target;
    if (target == pitch.previous)
    {
        pitch.step = 0;
        return;
    }

    pitch.frames = rom_.U16(inst + 10);
    pitch.step = Divide(target - pitch.current, pitch.frames);
    pitch.state = 2;
}

void Sequencer::PsgLegato(int channel, int index)
{
    // A legato note writes only the frequency setting of its key, without a restart, or glides there in the pitch
    // mode. On the wave channel, the pitch-mode check reads all the flags. A wave instrument with legato therefore
    // always uses the glide code. Without a glide state, the frequency stays.
    PsgChannel& psg = psg_[size_t(channel)];
    const uint32_t inst = Instrument(channel, psg.program);
    const uint16_t flags = rom_.U16(info_.instrument_tables[0] + 16 * uint32_t(psg.program + 30 * channel) + 12);
    const bool pitch_mode = channel == kWaveChannel ? flags != 0 : (flags & kPitchMode) != 0;
    if (!pitch_mode)
    {
        Write(channel == 0 ? kNr13 : channel == 1 ? kNr23 : kNr33, 2, Frequency(index));
    }
    else if (psg.pitch.state >= 1 && psg.pitch.state <= 3)
    {
        Glide(psg.pitch, inst, 100 * index);
    }
}

void Sequencer::PsgNoteOff(int channel, int key)
{
    // A note-off silences the channel if its key is the one that the last note set. GCC warns that a negative channel
    // would index past psg_. at() checks the index.
    PsgChannel& psg = psg_.at(size_t(channel));
    if (psg.index != KeyIndex(channel, key))
    {
        return;
    }

    psg.playing = false;
    Write(channel == 0 ? kNr11 : channel == 1 ? kNr21 : kNr31, 2, 0);
    if (psg.note >= 0)
    {
        EndNote(psg.note, frame_);
        psg.note = -1;
    }
}

void Sequencer::PsgUpdate()
{
    // Each frame after the commands, the sequencer moves the modulation and the glide of each PSG channel on. If
    // either sets a place in cents, the channel gets the frequency setting at that place in the fine table, without a
    // restart. The modulation alone moves the place of the channel key. The modulation and the glide continue after a
    // note-off.
    static constexpr uint32_t kFrequencyRegisters[kPsgChannels] = {kNr13, kNr23, kNr33};
    for (int c = 0; c < kPsgChannels; c++)
    {
        PsgChannel& psg = psg_[size_t(c)];
        int32_t mod = 0;
        int32_t glide = 0;
        Modulation& m = psg.modulation;
        if (m.state)
        {
            switch (m.state)
            {
            case 1:
                if (m.value == 0)
                {
                    m.state = 2;
                }
                else
                {
                    m.value--;
                }
                break;

            case 2:
            case 3:
                m.value = int32_t(uint32_t(m.value) + uint32_t(m.step));
                if ((m.value >> 16) > m.depth)
                {
                    m.state = 4;
                }
                break;

            case 4:
                m.value = int32_t(uint32_t(m.value) - uint32_t(m.step));
                if (int64_t(m.value >> 16) + m.depth < 0)
                {
                    m.state = 3;
                }
                break;

            default:
                break;
            }
            mod = m.state == 1 ? 0 : int16_t(uint32_t(m.value) >> 16);
        }

        PitchMode& p = psg.pitch;
        if (p.state)
        {
            if (p.state == 2)
            {
                const bool reached = p.step > 0 ? p.current >= p.target : p.current <= p.target;
                if (reached)
                {
                    p.current = p.target;
                    p.state = 3;
                }
                else
                {
                    p.current += p.step;
                }
            }
            glide = p.current;
        }

        if (!mod && !glide)
        {
            continue;
        }

        const int32_t cents = glide + mod + (glide ? 0 : 100 * psg.index);
        Write(kFrequencyRegisters[c], 2, FineFrequency(cents));
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
    if (!piece_playing_ || !changed_)
    {
        return;
    }

    changed_ = false;
    const uint32_t next = NextSequence(2);
    if (next && rom_.U8(next) == kSequence)
    {
        next_ = next + kSequenceHeader;
    }
    else if (active_)
    {
        Warn("the piece's sequence after next isn't a sequence, on which the game resets");
        next_ = 0;
    }

    const int count = int(piece_.sequences.size());
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
                if (piece_.sample >= 0)
                {
                    NextSamplePass(voice);
                }
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
    WarnOnce(warnings_, message);
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

} // namespace agbsf2::ubimilan
