// SPDX-License-Identifier: MIT

#include "brownie/sequencer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "brownie/song.h"
#include "gba.h"
#include "warnings.h"

namespace agbsf2::brownie
{
namespace
{

// The hardware registers the driver writes besides the sound registers.
constexpr uint32_t kIoFifoA = 0x040000A0; // then FIFO B at 0x040000A4
constexpr uint32_t kIoDma1Control = 0x040000C6;
constexpr uint32_t kIoDma2Control = 0x040000D2;
constexpr uint32_t kIoTimer0Reload = 0x04000100;
constexpr uint32_t kIoTimer0Control = 0x04000102;
constexpr uint32_t kIoTimer1Reload = 0x04000104;
constexpr uint32_t kIoTimer1Control = 0x04000106;

// The setting the driver gives both FIFOs' DMA: on, with an interrupt, at the FIFO's request, repeating, a word at a
// time to a fixed address. Bit 10, a word at a time, is the one the driver checks.
constexpr uint16_t kDmaOn = 0xF640;
constexpr uint16_t kDmaWords = 0x0400;
constexpr uint16_t kTimerOn = 0x0080;

// The DMA 1 and DMA 2 interrupts, which run the mixer, or in the Magical Vacation revision feed FIFO A and FIFO B.
constexpr uint16_t kMixerInterrupts = 0x0600;
constexpr uint16_t kFifoAInterrupt = 0x0200;

// The setting the Magical Vacation revision gives a FIFO's DMA, which only makes the FIFO's requests interrupt, since
// the driver never gives the DMA addresses: on, with an interrupt, at the FIFO's request, repeating, a word at a time.
constexpr uint16_t kFifoDmaOn = 0xF600;

// The NRx2 that a PSG note starts with when its envelope's first level is below the channel's volume: volume 0 with
// the direction bit set. It's silent, and it keeps the channel's DAC on. A later step below the volume writes 0.
constexpr uint32_t kSilentNrx2 = 0x08;

// The bytes a FIFO holds, and the most it can hold when it asks for more after playing a point.
constexpr int kFifoBytes = 32;
constexpr int kFifoRequest = 16;

// The sounds that start only when the noise channel is free.
constexpr int kNoiseSound1 = 0x3B;
constexpr int kNoiseSound2 = 0x3D;

// The sound the driver queues when E3 brings the sound back.
constexpr int kResumeSound = 0x36;

// The most commands a channel reads in a frame, so that data that never reaches a note can't hang the model, and the
// most channels a sound starts.
constexpr int kMaxCommands = 10000;
constexpr int kMaxSoundChannels = 64;

std::string HexByte(int v)
{
    char b[8];
    std::snprintf(b, sizeof b, "%02X", unsigned(v & 0xFF));

    return b;
}

// Returns a sample channel's volume with its envelope's level: (volume + 1) × level / 256, rounded down. The record
// holds it at 0x25.
uint8_t VolumeAtLevel(const Channel& ch)
{
    return uint8_t(((ch.volume + 1u) * ch.level) >> 8);
}

// Sets voice `v`'s sides as a note and a fade's step set them: from sample channel `ch`'s volume with its envelope's
// level, the master level `master` and the pan, rounded down once. SetVoiceLevels() rounds twice.
void SetNoteSides(Voice& v, const Channel& ch, uint32_t master)
{
    const uint32_t scaled = ch.EnvelopeVolume() * (master + 1);
    v.right = uint8_t(((ch.RightLevel() + 1u) * scaled) >> 16);
    v.left = uint8_t(((ch.LeftLevel() + 1u) * scaled) >> 16);
}

} // namespace

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : rom_(rom), info_(info), layout_(LayoutOf(info.revision)), vacation_(info.revision == Revision::kMagicalVacation)
{
    if (vacation_)
    {
        // The Magical Vacation revision's init empties the FIFOs, sets the mix, sets SOUNDCNT_L before it turns the
        // sound off and on, which clears it again, clears the PSG channels' registers, writes 0x8000 to 0x0400007A
        // rather than to NR44, and stops the timers. The channels and the queue start clear.
        Write(kIoSoundcntH, 2, 0xC80E);
        Write(kIoSoundcntH, 2, 0x730E);
        Write(kIoSoundcntL, 2, 0x77);
        Set16(kVacationSoundcntLCopy, 0x77);
        Write(kIoNr52, 2, 0);
        Write(kIoNr52, 2, 0x8F);
        for (const uint32_t address :
             {kIoControlRegisters[0], kIoControlRegisters[1], kIoControlRegisters[3], kIoControlRegisters[2], kIoNr10})
        {
            Write(address, 2, 0);
        }
        for (const uint32_t address :
             {kIoFrequencyRegisters[0], kIoFrequencyRegisters[1], kIoFrequencyRegisters[2], 0x0400007Au})
        {
            Write(address, 2, 0x8000);
        }
        Write(kIoTimer0Control, 2, 0);
        Write(kIoTimer1Control, 2, 0);
        Set32(kVacationMaster, 0xFF);
    }
    else
    {
        // The init resets the sound, the channels, the queue, the voices and the master level, and stops the FIFOs.
        Write(kIoNr52, 2, 0);
        Write(kIoNr52, 2, 0x8F);
        Write(kIoTimer0Control, 2, 0);
        Write(kIoSoundcntH, 2, 0x8802);
        Write(kIoSoundcntH, 2, 0x210E);
        Write(kIoSoundcntL, 2, 0x77);
        Set16(kSoundcntLCopy, 0x77);
        Set32(kMaster, 0xFF);
        Write(kIoDma1Control, 2, 0);
        Write(kIoDma2Control, 2, 0);
    }

    mix_cycles_ = uint64_t(kMixPoints) * info.point_cycles;
    std::vector<SongChannel> channels;
    valid_ = mix_cycles_ > 0 && ReadSong(rom, info, song, channels);
    if (valid_)
    {
        Request(song);
    }
}

std::array<uint8_t, 0x38> Sequencer::RecordBytes(const Channel& c)
{
    std::array<uint8_t, 0x38> b = {};
    b[0x00] = c.flags;
    b[0x01] = c.wait;
    b[0x02] = c.step_frames;
    b[0x03] = c.loop_count;
    xsflib::StoreLe32(&b[0x04], c.position);
    xsflib::StoreLe32(&b[0x08], c.loop_start);
    xsflib::StoreLe32(&b[0x0C], c.return_to);
    xsflib::StoreLe32(&b[0x10], c.subroutines);
    xsflib::StoreLe32(&b[0x14], c.samples);
    xsflib::StoreLe32(&b[0x18], c.sample_end);
    xsflib::StoreLe32(&b[0x1C], c.wave);
    xsflib::StoreLe16(&b[0x20], c.frequency);
    xsflib::StoreLe16(&b[0x22], c.control);
    b[0x24] = c.duty;
    b[0x25] = c.sweep;
    b[0x26] = c.volume;
    b[0x27] = c.detune;
    b[0x28] = c.level;
    b[0x29] = c.length;
    b[0x2A] = c.pan_on;
    b[0x2B] = c.pan_off;
    b[0x2C] = c.transpose;
    b[0x2D] = c.ramp;
    b[0x2E] = c.ramp_frames;
    b[0x2F] = c.length_set;
    xsflib::StoreLe32(&b[0x30], c.envelope);
    xsflib::StoreLe32(&b[0x34], c.step);

    return b;
}

std::array<uint8_t, 0x10> Sequencer::VoiceBytes(const Voice& v)
{
    std::array<uint8_t, 0x10> b = {};
    b[0] = v.mode;
    b[1] = v.mode2;
    b[2] = v.right;
    b[3] = v.left;
    xsflib::StoreLe32(&b[4], v.point);
    xsflib::StoreLe32(&b[8], v.end);
    xsflib::StoreLe32(&b[12], v.loop);

    return b;
}

std::array<uint8_t, 0x10> Sequencer::VoiceRecord(int v) const
{
    if (!vacation_)
    {
        return VoiceBytes(voices_[size_t(v)]);
    }

    const FifoVoice& f = fifo_voices_[size_t(v)];
    std::array<uint8_t, 0x10> b = {};
    xsflib::StoreLe32(&b[0], f.point);
    xsflib::StoreLe32(&b[4], f.end);
    xsflib::StoreLe32(&b[8], f.loop);
    xsflib::StoreLe32(&b[12], f.volume);

    return b;
}

const std::vector<RegisterWrite>& Sequencer::Step()
{
    writes_.clear();
    events_.clear();
    returned_ = false;
    restarted_ = 0;
    r5_ = 0;

    // The per-frame routine: the channels, then the fades and a request, unless E3 has ended it. Only the game's fade
    // out routine starts a fade in the Magical Vacation revision, which has no E3, so the model has no fades for it.
    now_ = uint64_t(frame_) * kFrameCycles;
    RunChannels();
    if (!returned_)
    {
        if (!vacation_)
        {
            Fade();
        }
        Requests();
    }

    if (vacation_)
    {
        fifos_before_ = fifos_;
        fifo_voices_before_ = fifo_voices_;
        RunFifos();
        frame_++;

        return writes_;
    }

    // The FIFOs play while their DMA and Timer 0 run, and the mixer makes 16 more points of each whenever they've
    // played the last 16, counting from the frame they started in.
    voices_before_mix_ = voices_;
    if ((dma_control_ & 0x8000) && (timer_control_ & kTimerOn))
    {
        if (mix_start_ < 0)
        {
            mix_start_ = int64_t(frame_);
            mixed_ = 0;
        }

        const uint64_t due = (uint64_t(frame_) - uint64_t(mix_start_) + 1) * kFrameCycles / mix_cycles_;
        for (; mixed_ < due; mixed_++)
        {
            Mix();
        }
    }
    else
    {
        mix_start_ = -1;
    }

    frame_++;

    return writes_;
}

bool Sequencer::Playing() const
{
    for (int c = 0; c < layout_.channels; c++)
    {
        if (channels_[size_t(c)].flags & kFlagOn)
        {
            return true;
        }
    }

    return vacation_ ? fifos_[0].running || fifos_[1].running : mix_start_ >= 0;
}

// The per-frame routine's loop over the channels: each one counts down its wait, and reads its data when the wait runs
// out; otherwise it counts down to its envelope's next step and, on a sample channel, to its level's next step.
void Sequencer::RunChannels()
{
    for (int c = 0; c < layout_.channels && !returned_; c++)
    {
        Channel& ch = channels_[size_t(c)];
        if (!(ch.flags & kFlagOn))
        {
            continue;
        }

        const uint8_t wait = uint8_t(ch.wait - 1);
        if (wait == 0)
        {
            ReadChannel(c);
            continue;
        }

        ch.wait = wait;
        if (ch.step_frames != 0)
        {
            const uint8_t frames = uint8_t(ch.step_frames - 1);
            if (frames == 0)
            {
                if (c < kFirstSampleChannel)
                {
                    PsgEnvelopeStep(c);
                }
                else if (vacation_)
                {
                    FifoEnvelopeStep(c);
                }
                else
                {
                    SampleEnvelopeStep(c);
                }
                continue;
            }

            ch.step_frames = frames;
        }

        if (c >= kFirstSampleChannel && ch.RampWait() != 0)
        {
            const uint8_t frames = uint8_t(ch.RampWait() - 1);
            if (frames == 0)
            {
                if (vacation_)
                {
                    FifoRampStep(c);
                }
                else
                {
                    RampStep(c);
                }
                continue;
            }

            ch.SetRampWait(frames);
        }
    }
}

// Reads the channel's data up to its next note, rest or hold, or a command that ends its frame.
void Sequencer::ReadChannel(int c)
{
    Channel& ch = channels_[size_t(c)];
    uint32_t at = ch.position;
    for (int n = 0; n < kMaxCommands; n++)
    {
        Visit(c, at);
        const uint8_t b = rom_.U8(at++);
        if (b < 0x80)
        {
            ch.position = at;
            if (KindOf(info_.revision, c) == ChannelKind::kNoise)
            {
                // A noise note's byte gives NR43, without the transpose.
                AddEvent(c, Event::kNote, b);
                PsgNote(c, NoiseNr43(b));
                return;
            }

            const int key = ((ch.transpose & 0x80) ? b - (0x100 - ch.transpose) : b + ch.transpose) & 0x7F;
            AddEvent(c, Event::kNote, uint8_t(key));
            if (c >= kFirstSampleChannel)
            {
                events_.back().samples = ch.samples;
                if (vacation_)
                {
                    FifoNote(c, key);
                }
                else
                {
                    SampleNote(c, key);
                }
            }
            else
            {
                PsgNote(c, uint32_t(rom_.U16(info_.frequency_table + 2 * uint32_t(key))) + ch.detune);
            }
            return;
        }
        if (b < 0xE0)
        {
            // The Magical Vacation revision has no length sets. There, LengthSet() gives the group of 16 lengths, one
            // for each tempo, that the command reads from.
            ch.length = NoteLength(rom_, info_, b, ch.length_set);
            length_groups_[size_t(c)] = uint8_t((b & 0x7F) >> 4);
            continue;
        }

        if (!RunCommand(c, b, at))
        {
            return;
        }
    }

    Warn("channel " + std::to_string(c) + " reads commands without reaching a note, so it was ended");
    ch.flags = uint8_t(ch.flags & ~kFlagOn);
    AddEvent(c, Event::kEnd);
}

// Runs command `command` of channel `c`, whose next byte is at `at`. Returns false if the command ends the channel's
// reading for the frame.
bool Sequencer::RunCommand(int c, uint8_t command, uint32_t& at)
{
    // In the Magical Vacation revision, E2-E9 are F2-F9 again, and EF is FF. A command that does nothing has no
    // argument either.
    command = CommandOf(info_.revision, command);
    if (IsNoOp(info_.revision, command))
    {
        return true;
    }

    Channel& ch = channels_[size_t(c)];
    switch (command)
    {
    case 0xE0:
        {
            // The release: the envelope goes on from the release envelope at the next frame.
            const uint8_t index = rom_.U8(at++);
            ch.step = rom_.U32(info_.envelope_table + 4 * uint32_t(index));
            ch.step_frames = 1;
            ch.position = at;
            ch.wait = ch.length;
            AddEvent(c, Event::kRelease);
            return false;
        }

    case 0xE1:
        {
            // A sample set, or in the Magical Vacation revision a sample: its start, end and loop.
            const uint32_t entry = rom_.U32(info_.sample_table + 4 * uint32_t(rom_.U8(at++)));
            if (!vacation_)
            {
                ch.samples = entry;
                return true;
            }

            ch.SetSampleStart(rom_.U32(entry));
            ch.sample_end = rom_.U32(entry + 4);
            ch.SetSampleLoop(rom_.U32(entry + 8));
            return true;
        }

    case 0xE2:
        {
            // The mixer's mode: 0 mixes voices 0 and 1 only, and clears the flags of the other voices' channels and of
            // the 14th record.
            const uint8_t mode = rom_.U8(at++);
            Set8(kMixerMode, mode);
            for (int k = kFirstSampleChannel + kMode0Voices; mode == 0 && k < kRecordCount; k++)
            {
                channels_[size_t(k)].flags = 0;
            }
            return true;
        }

    case 0xE3:
        ch.position = at;
        Resume();
        return false;

    case 0xE8:
    case 0xE9:
        {
            // A sound effect gives the music channel's PSG channel back, or takes it.
            if (c < kFirstEffectChannel)
            {
                Warn("channel " + std::to_string(c) + " has command " + HexByte(command) +
                     ", which changes memory before the channels");
                return true;
            }

            Channel& music = channels_[size_t(c - kFirstEffectChannel)];
            music.flags =
                command == 0xE8 ? uint8_t(music.flags & (kFlagOn | kFlagPaused)) : uint8_t(music.flags | kFlagEffect);
            return true;
        }

    case 0xEA:
        {
            // A sample channel's pan: the right side's level in the low nibble and the left's in the high, each filled
            // out with 0xF unless it's 0.
            const uint8_t pan = rom_.U8(at++);
            const uint32_t right = ((pan & 0x0Fu) << 4) + 0x0F;
            const uint32_t left = (pan & 0xF0u) + 0x0F;
            ch.SetRightLevel(uint8_t((right & 0xF0) ? right : 0));
            ch.SetLeftLevel(uint8_t((left & 0xF0) ? left : 0));
            return true;
        }

    case 0xEC:
        {
            // A sample channel's rest silences its voice, or in the Magical Vacation revision stops the timer of the
            // FIFO of the channel's number & 1, unless a sound effect has the channel's PSG channel or FIFO.
            ch.position = at;
            ch.step_frames = 0;
            ch.wait = ch.length;
            if (vacation_)
            {
                if (!(ch.flags & kFlagEffect))
                {
                    Write(c & 1 ? kIoTimer1Control : kIoTimer0Control, 2, 0);
                }
                AddEvent(c, Event::kRest);
                return false;
            }

            r5_ = uint32_t(c & 7);
            if ((c & 7) < kVoiceCount)
            {
                voices_[size_t(c & 7)] = Voice();
            }
            else
            {
                Warn("channel " + std::to_string(c) + " has command EC, which changes the mixer's buffers");
            }
            AddEvent(c, Event::kRest);
            return false;
        }

    case 0xED:
        {
            // A PSG channel's rest turns its DAC off, and writes the routine's r5 to the frequency setting.
            ch.position = at;
            ch.step_frames = 0;
            ch.wait = ch.length;
            ch.control = uint16_t(ch.control & 0xFF);
            if (!(ch.flags & kFlagEffect))
            {
                Write(ControlRegister(c), 2, 0);
                r5_ ^= 0x8000;
                Write(FrequencyRegister(c), 2, r5_);
            }
            AddEvent(c, Event::kRest);
            return false;
        }

    case 0xEE:
        ch.length_set = rom_.U8(at++);
        return true;

    case 0xF0:
        ch.envelope = rom_.U32(info_.envelope_table + 4 * uint32_t(rom_.U8(at++)));
        return true;

    case 0xF1:
        ch.duty = rom_.U8(at++);
        return true;

    case 0xF2:
        ch.volume = rom_.U8(at++);
        return true;

    case 0xF3:
        ch.transpose = rom_.U8(at++);
        return true;

    case 0xF4:
        ch.loop_count = rom_.U8(at++);
        ch.loop_start = at;
        return true;

    case 0xF5:
        {
            const int count = ch.loop_count - 1;
            ch.loop_count = uint8_t(count);
            if (count != 0)
            {
                at = ch.loop_start;
            }
            return true;
        }

    case 0xF6:
        {
            const uint32_t target = rom_.U32(ch.subroutines + 4 * uint32_t(rom_.U8(at++)));
            ch.return_to = at;
            at = target;
            return true;
        }

    case 0xF7:
        at = ch.return_to;
        return true;

    case 0xF8:
        {
            // A jump back to a place the channel has read before is its loop.
            at = rom_.U32(ch.subroutines + 4 * uint32_t(rom_.U8(at++)));
            const auto it = visited_[size_t(c)].find(at);
            if (it != visited_[size_t(c)].end())
            {
                AddEvent(c, Event::kLoop);
                events_.back().first = it->second;
            }
            return true;
        }

    case 0xF9:
        ch.detune = rom_.U8(at++);
        return true;

    case 0xFA:
        ch.sweep = rom_.U8(at++);
        return true;

    case 0xFB:
        ch.sweep = 0;
        return true;

    case 0xFC:
        ch.wave = rom_.U32(info_.wave_table + 4 * uint32_t(rom_.U8(at++)));
        if (!(ch.flags & kFlagEffect))
        {
            LoadWave(ch.wave);
            r5_ = 0;
        }
        return true;

    case 0xFD:
        {
            // A PSG channel's pan: its bits of NR51 for the right side in the low nibble and the left in the high,
            // which the driver shifts to the channel's bits.
            uint32_t pan = rom_.U8(at++);
            uint32_t mask = 0x11;
            for (int k = 0; k < (c & 3); k++)
            {
                pan <<= 1;
                mask <<= 1;
            }

            mask ^= 0xFF;
            ch.pan_on = uint8_t(pan);
            ch.pan_off = uint8_t(mask);
            if (!(ch.flags & kFlagEffect))
            {
                mask = (mask << 8) + 0xFF;
                const uint32_t value = (Get16(SoundcntLCopy()) & mask) | (pan << 8);
                Set16(SoundcntLCopy(), value);
                Write(kIoSoundcntL, 2, value);
            }
            r5_ = mask;
            return true;
        }

    case 0xFE:
        ch.position = at;
        ch.wait = ch.length;
        AddEvent(c, Event::kHold);
        return false;

    case 0xFF:
        AddEvent(c, Event::kEnd);
        EndChannel(c);
        return false;

    default:
        // The commands that do nothing have returned above.
        return true;
    }
}

// Starts a PSG note: the frequency setting, the envelope's first step, and the registers, which start the channel.
void Sequencer::PsgNote(int c, uint32_t frequency)
{
    Channel& ch = channels_[size_t(c)];
    ch.frequency = uint16_t(frequency);
    ch.wait = ch.length;
    ch.step_frames = rom_.U8(ch.envelope);
    const uint8_t level = rom_.U8(ch.envelope + 1);
    ch.step = ch.envelope + 2;

    const uint32_t envelope = level < ch.volume ? kSilentNrx2 : uint32_t(level - ch.volume);
    const uint32_t control = ch.duty + (envelope << 8);
    ch.control = uint16_t(control);
    if (!(ch.flags & kFlagEffect))
    {
        if (c % kFirstEffectChannel == 0)
        {
            Write(kIoNr10, 2, ch.sweep);
        }
        Write(ControlRegister(c), 2, control);

        // The Magical Vacation revision flips bit 15 of the frequency setting where the other sets it, and writes it
        // once rather than twice.
        frequency = vacation_ ? frequency ^ 0x8000 : frequency | 0x8000;
        Write(FrequencyRegister(c), 2, frequency);
        if (!vacation_)
        {
            Write(FrequencyRegister(c), 2, frequency);
        }
    }

    r5_ = frequency;
}

// Starts a sample note: the semitone's sample from the channel's set, played at the key's octave, the envelope's first
// step, and the FIFOs if they've stopped.
void Sequencer::SampleNote(int c, int key)
{
    Channel& ch = channels_[size_t(c)];
    key = SampleKey(key);
    const int octave = SampleOctave(key);
    static constexpr uint8_t kModes[4] = {4, 2, 1, 0};
    Voice& v = voices_[size_t(c & 7)];
    v.mode = kModes[octave];
    v.mode2 = v.mode;
    const uint32_t entry = rom_.U32(ch.samples + 4 * uint32_t(key - kOctaveKeys * octave));
    v.point = rom_.U32(entry);
    v.end = rom_.U32(entry + 4);
    v.loop = rom_.U32(entry + 8);

    // The envelope's first step sets the level, and the voice's sides take it with the master level and the pan.
    ch.wait = ch.length;
    TakeSampleStep(c, ch.envelope);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    const uint32_t master = Get8(kMaster);
    SetNoteSides(v, ch, master);

    // The routine's r5 is left with the volume times the master level plus 1. A note that starts the FIFOs' DMA leaves
    // IE there instead.
    r5_ = ch.EnvelopeVolume() * (master + 1);
    if (!(dma_control_ & kDmaWords))
    {
        Write(kIoDma1Control, 2, 0);
        Write(kIoDma2Control, 2, 0);
        interrupts_ = uint16_t(interrupts_ | kMixerInterrupts);
        r5_ = interrupts_;
        Write(kIoDma1Control, 2, kDmaOn);
        Write(kIoDma2Control, 2, kDmaOn);
    }
    Write(kIoTimer0Control, 2, kTimerOn);
}

// A sample note in the Magical Vacation revision: the timer reload value for the key and the detune, which is signed
// and in 12ths of a semitone, and the envelope's first step. Unless a sound effect has the FIFO, the note stops the
// FIFO's timer, gives the FIFO the sample's first 4 points as they are, sets the FIFO's sample from the channel's, and
// starts the timer again at the new rate, with the FIFO's DMA, whose requests interrupt.
void Sequencer::FifoNote(int c, int key)
{
    Channel& ch = channels_[size_t(c)];
    const int index = key * kRateSteps + int(int8_t(ch.detune));
    ch.SetTimerReload(rom_.U16(info_.rate_table + 2 * uint32_t(index)));
    ch.wait = ch.length;
    TakeFifoStep(c, ch.envelope);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    if (ch.flags & kFlagEffect)
    {
        return;
    }

    const int x = c & 1;
    const uint32_t timer = x ? kIoTimer1Control : kIoTimer0Control;
    Write(timer, 2, 0);
    Write(kIoFifoA + 4 * uint32_t(x), 2, rom_.U16(ch.SampleStart()));
    Write(kIoFifoA + 4 * uint32_t(x) + 2, 2, rom_.U16(ch.SampleStart() + 2));
    FifoVoice& v = fifo_voices_[size_t(x)];
    v.point = ch.SampleStart() + 4;
    v.end = ch.sample_end;
    v.loop = ch.SampleLoop();
    v.volume = ch.EnvelopeVolume();
    interrupts_ = uint16_t(interrupts_ | (kFifoAInterrupt << x));
    Write(x ? kIoTimer1Reload : kIoTimer0Reload, 2, ch.TimerReload());
    Write(x ? kIoDma2Control : kIoDma1Control, 2, kFifoDmaOn);
    fifos_[size_t(x)].owner = c;
    Write(timer, 2, kTimerOn);

    r5_ = interrupts_;
}

// A PSG envelope's next step: its frames and NRx2, less the channel's volume, which starts the channel again. The
// Magical Vacation revision keeps the new NRx1 and NRx2 only while the channel has its PSG channel.
void Sequencer::PsgEnvelopeStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.step_frames = rom_.U8(ch.step);
    ch.level = rom_.U8(ch.step + 1);
    ch.step += 2;

    const int envelope = ch.level - ch.volume;
    const uint32_t control = ch.duty + (uint32_t(envelope < 0 ? 0 : envelope) << 8);
    if (!vacation_ || !(ch.flags & kFlagEffect))
    {
        ch.control = uint16_t(control);
    }
    if (!(ch.flags & kFlagEffect))
    {
        Write(ControlRegister(c), 2, control);
        Write(FrequencyRegister(c), 2, ch.frequency | 0x8000u);
    }
}

// A sample envelope's next step. The voice's sides take its level.
void Sequencer::SampleEnvelopeStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    TakeSampleStep(c, ch.step);
    SetVoiceLevels(c);
}

// A sample envelope's next step in the Magical Vacation revision. The FIFO's sample takes the volume with the master
// level, unless a sound effect has the FIFO.
void Sequencer::FifoEnvelopeStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    TakeFifoStep(c, ch.step);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    if (!(ch.flags & kFlagEffect))
    {
        r5_ = ch.EnvelopeVolume() * uint32_t(Get8(kVacationMaster));
        fifo_voices_[size_t(c & 1)].volume = r5_ >> 8;
    }
}

// Takes the sample envelope's step at `at`: its frames, the change in level each frame and the level.
void Sequencer::TakeSampleStep(int c, uint32_t at)
{
    Channel& ch = channels_[size_t(c)];
    ch.step_frames = rom_.U8(at);
    ch.SetRampWait(1);
    ch.ramp_frames = 1;
    ch.ramp = rom_.U8(at + 1);
    ch.level = rom_.U8(at + 2);
    ch.step = at + 3;
}

// Takes the sample envelope's step at `at` in the Magical Vacation revision: its frames, the frames between its changes
// in level and the change in the high and low nibbles of a byte, and the level.
void Sequencer::TakeFifoStep(int c, uint32_t at)
{
    Channel& ch = channels_[size_t(c)];
    ch.step_frames = rom_.U8(at);
    const uint8_t ramp = rom_.U8(at + 1);
    ch.ramp_frames = uint8_t(ramp >> 4);
    ch.SetRampWait(ch.ramp_frames);
    ch.ramp = uint8_t(ramp & 0x0F);
    ch.level = rom_.U8(at + 2);
    ch.step = at + 3;
}

// A sample channel's change in level, which stops at 0 and 255.
void Sequencer::RampStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.SetRampWait(ch.ramp_frames);
    if (ch.ramp == 0)
    {
        return;
    }

    const int change = ch.ramp & 0x7F;
    int level = (ch.ramp & 0x80) ? ch.level - change : ch.level + change;
    level = level < 0 ? 0 : level > 255 ? 255 : level;
    ch.level = uint8_t(level);
    SetVoiceLevels(c);
}

// A sample channel's change in level in the Magical Vacation revision, by its low 3 bits, down with bit 3 set, which
// stops at 0 and 255. The FIFO's sample takes the volume with the master level plus 1, where the envelope's step uses
// the master level itself.
void Sequencer::FifoRampStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.SetRampWait(ch.ramp_frames);
    if ((ch.ramp & 0x0F) == 0)
    {
        return;
    }

    const int change = ch.ramp & 7;
    int level = (ch.ramp & 8) ? ch.level - change : ch.level + change;
    level = level < 0 ? 0 : level > 255 ? 255 : level;
    ch.level = uint8_t(level);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    if (!(ch.flags & kFlagEffect))
    {
        r5_ = ch.EnvelopeVolume() * (Get8(kVacationMaster) + 1u);
        fifo_voices_[size_t(c & 1)].volume = r5_ >> 8;
    }
}

// Sets a sample channel's volume with its envelope's level, and its voice's sides from that, the master level and the
// pan. Each step rounds down.
void Sequencer::SetVoiceLevels(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    const uint32_t scaled = (ch.EnvelopeVolume() * (Get8(kMaster) + 1u)) >> 8;
    Voice& v = voices_[size_t(c & 7)];
    v.right = uint8_t(((ch.RightLevel() + 1u) * scaled) >> 8);
    v.left = uint8_t(((ch.LeftLevel() + 1u) * scaled) >> 8);
    r5_ = v.left;
}

// FF: the channel ends. A PSG channel falls silent, unless a sound effect has it; a sound effect gives its PSG channel
// back to the music, which plays on as it was; and a sample channel's voice falls silent, which stops the FIFOs if
// voices 0 and 1 seem to be silent. In the Magical Vacation revision, a sample channel stops its FIFO's timer, unless a
// sound effect has the FIFO, and a sound effect's sample channel gives the FIFO back to the music's, whose sample stays
// stopped until its next note.
void Sequencer::EndChannel(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.flags = uint8_t(ch.flags & ~kFlagOn);
    if (c >= kFirstSampleChannel && vacation_)
    {
        if (c >= kFirstEffectSampleChannel)
        {
            Channel& music = channels_[size_t(c - (kFirstEffectSampleChannel - kFirstSampleChannel))];
            music.flags = uint8_t(music.flags & ~kFlagEffect);
        }
        else if (ch.flags & kFlagEffect)
        {
            return;
        }

        Write(c & 1 ? kIoTimer1Control : kIoTimer0Control, 2, 0);
        return;
    }
    if (c >= kFirstSampleChannel)
    {
        voices_[size_t(c & 7)] = Voice();

        // The driver tests only the low byte of the two voices' next points.
        if (((voices_[0].point | voices_[1].point) & 0xFF) == 0)
        {
            Write(kIoTimer0Control, 2, 0);
            Write(kIoDma1Control, 2, 0);
            Write(kIoDma2Control, 2, 0);
        }
        r5_ = voices_[1].point;
        return;
    }

    if (c >= kFirstEffectChannel)
    {
        Channel& music = channels_[size_t(c - kFirstEffectChannel)];
        music.flags = uint8_t(music.flags & ~kFlagEffect);
        if (music.flags & kFlagOn)
        {
            if ((c & 3) == kWaveChannel)
            {
                LoadWave(music.wave);
            }

            const uint32_t mask = (uint32_t(music.pan_off) << 8) + 0xFF;
            const uint32_t value = (Get16(SoundcntLCopy()) & mask) | (uint32_t(music.pan_on) << 8);
            Set16(SoundcntLCopy(), value);
            Write(kIoSoundcntL, 2, value);
            r5_ = vacation_ ? mask : music.control;
            Write(ControlRegister(c), 2, music.control);
            Write(FrequencyRegister(c), 2, music.frequency ^ 0x8000u);
            if (c == kFirstEffectChannel)
            {
                Write(kIoNr10, 2, music.sweep);
            }
            return;
        }
    }
    else if (ch.flags & kFlagEffect)
    {
        return;
    }

    Write(ControlRegister(c), 2, 0);
    Write(FrequencyRegister(c), 2, 0x8000);
}

// Copies a wave's 16 bytes to the wave RAM, with the wave channel switched to the other bank while it does.
void Sequencer::LoadWave(uint32_t wave)
{
    Write(kIoNr30, 2, 0x40);
    for (uint32_t i = 0; i < 16; i += 2)
    {
        Write(kIoWaveRam + i, 2, rom_.U16(wave + i));
    }
    Write(kIoNr30, 2, 0x80);
}

// E3: the sound comes back after a fade out, fading in over 64 frames, and the driver queues sound 0x36, through a
// different wrap of the queue's index, and ends its routine for the frame.
void Sequencer::Resume()
{
    Set8(kMaster, 0);
    Set16(kFadeInFrames, 0x40);
    Set8(kFadeStep, 4);
    Set16(kFadeInTimer, 0x100);
    Set16(kFadeInPeriod, 0x100);
    Write(kIoSoundcntL, 2, 0);
    for (Channel& ch : channels_)
    {
        ch.flags = (ch.flags & (kFlagOn | kFlagPaused)) ? kFlagOn : 0;
    }
    Write(kIoTimer0Control, 2, kTimerOn);

    const uint8_t at = Get8(kQueueWrite);
    Set16(kQueue + at, kResumeSound);
    Set8(kQueueWrite, uint8_t((at + 2) & 7));
    returned_ = true;
}

// The fade in, or else the fade out, takes its next step when its timer runs out. Only the game's fade routine sets
// kFadeOutFrames. Neither the model nor driver_emu.py runs that routine. The model never fades out.
void Sequencer::Fade()
{
    if (Get8(kFadeInFrames) != 0)
    {
        const uint8_t frames = uint8_t(Get8(kFadeInFrames) - 1);
        Set8(kFadeInFrames, frames);
        if (frames != 0)
        {
            FadeStep(kFadeInTimer, kFadeInPeriod, true);
            return;
        }

        // The fade in's end sets NR50 to full, and writes 0xFF to address 0, where it does nothing.
        const uint32_t value = (Get16(kSoundcntLCopy) & 0xFF00) | 0x77;
        Set16(kSoundcntLCopy, value);
        Write(kIoSoundcntL, 2, value);
        return;
    }

    if (Get16(kFadeOutFrames) != 0)
    {
        const uint16_t frames = uint16_t(Get16(kFadeOutFrames) - 1);
        Set16(kFadeOutFrames, frames);
        if (frames != 0)
        {
            FadeStep(kFadeOutTimer, kFadeOutPeriod, false);
        }
        else
        {
            StopAll();
        }
    }
}

// A step of a fade: the master level goes up or down by the step, and so do NR50 and the sample channels' voices.
void Sequencer::FadeStep(int timer, int period, bool up)
{
    const uint32_t count = uint32_t(Get16(timer)) - 0x100;
    if (count & 0xFF00)
    {
        Set16(timer, count);
        return;
    }

    Set16(timer, count + Get16(period));
    int master = Get8(kMaster);
    const int step = Get8(kFadeStep);
    master = up ? (master + step > 0xFF ? 0xFF : master + step) : (master < step ? 0 : master - step);
    Set8(kMaster, uint32_t(master));

    const uint32_t nr50 = uint32_t(master >> 5) * 0x11;
    const uint32_t value = (Get16(kSoundcntLCopy) & 0xFF00) | nr50;
    Set16(kSoundcntLCopy, value);
    Write(kIoSoundcntL, 2, value);
    for (int v = 0; v < kVoiceCount; v++)
    {
        const Channel& ch = channels_[size_t(kFirstSampleChannel + v)];
        if (ch.flags & kFlagOn)
        {
            SetNoteSides(voices_[size_t(v)], ch, uint32_t(master));
        }
    }
}

// The end of a fade out: the sound and the FIFOs stop, and each channel in use is marked to come back with E3.
void Sequencer::StopAll()
{
    Set8(kMaster, 0);
    Write(kIoNr52, 2, 0);
    Write(kIoNr52, 2, 0x8F);
    Write(kIoTimer0Control, 2, 0);
    Write(kIoSoundcntH, 2, 0x8802);
    Write(kIoSoundcntH, 2, 0x210E);
    Write(kIoSoundcntL, 2, 0x77);
    Set16(kSoundcntLCopy, 0x77);

    // The driver clears the queue's indexes and first four requests with the last record's new flags.
    uint32_t flags = 0;
    for (Channel& ch : channels_)
    {
        flags = (ch.flags & (kFlagOn | kFlagPaused)) ? kFlagPaused : 0;
        ch.flags = uint8_t(flags);
    }
    Set16(kQueueRead, flags);
    Set32(kQueue, flags);
    Set32(kQueue + 4, flags);
    Set32(kMaster, 0xFF);
}

// Starts the next request in the queue, if there is one. The Magical Vacation revision's queue holds 4 requests, and it
// starts the next one if the queue's offsets differ, whatever it is.
void Sequencer::Requests()
{
    const uint8_t read = Get8(kQueueRead);
    if (vacation_)
    {
        if (read == Get8(kQueueWrite))
        {
            return;
        }

        const uint16_t sound = Get16(kQueue + read);
        Set16(kQueue + read, 0);
        Set8(kQueueRead, uint8_t((read + 2) & layout_.queue_mask));
        StartSound(sound);
        return;
    }

    const uint16_t sound = Get16(kQueue + read);
    if (sound == 0)
    {
        if (read != Get8(kQueueWrite))
        {
            Set32(kQueueRead, 0);
        }
        return;
    }

    Set16(kQueue + read, 0);
    Set8(kQueueRead, uint8_t((read + 2) & layout_.queue_mask));
    if (sound == kNoiseSound1 || sound == kNoiseSound2)
    {
        // These wait for the noise channel to be silent, or to have a few frames left of its note.
        const uint32_t nr41 = ControlRegister(kNoiseChannel) - kIoNr10;
        const uint32_t value = nr41 + 1 < registers_.size() ? xsflib::LoadLe16(&registers_[nr41]) : 0;
        const Channel& noise = channels_[kNoiseChannel];
        if ((value & 0xF000) || ((noise.flags & ~kFlagPaused) && noise.wait < 3))
        {
            return;
        }
    }

    StartSound(sound);
}

// Starts each channel that a sound's entry lists, with its data, its subroutines and the channel's pan, and gives each
// sound effect's PSG channel to the sound effect.
void Sequencer::StartSound(int sound)
{
    uint32_t at = SongAddress(rom_, info_, sound);
    for (int n = 0; n < kMaxSoundChannels; n++, at += 12)
    {
        const uint32_t c = rom_.U32(at);
        if (c == 0xFF)
        {
            return;
        }
        if (c >= uint32_t(layout_.records))
        {
            Warn("sound " + std::to_string(sound) + " lists channel " + std::to_string(c) + ", which doesn't exist");
            return;
        }

        Channel& ch = channels_[c];
        ch.flags = uint8_t(ch.flags | kFlagOn);
        ch.position = rom_.U32(at + 4);
        ch.subroutines = rom_.U32(at + 8);
        ch.duty = ch.sweep = ch.volume = ch.detune = 0;
        ch.level = ch.length = ch.pan_on = ch.pan_off = 0;
        ch.transpose = ch.ramp = ch.ramp_frames = ch.length_set = 0;
        ch.wait = 1;
        ch.pan_on = rom_.U8(info_.pan_table + c);
        ch.pan_off = rom_.U8(info_.pan_table + c + 12);
        if (c >= uint32_t(kFirstEffectChannel) && c < uint32_t(kFirstSampleChannel))
        {
            Channel& music = channels_[c - kFirstEffectChannel];
            music.flags = uint8_t(music.flags | kFlagEffect);
        }
        else if (vacation_ && c >= uint32_t(kFirstEffectSampleChannel))
        {
            Channel& music = channels_[c - (kFirstEffectSampleChannel - kFirstSampleChannel)];
            music.flags = uint8_t(music.flags | kFlagEffect);
        }
    }

    Warn("sound " + std::to_string(sound) + " lists too many channels");
}

// The mixer's progress: each voice it plays takes the next 16 points of its sample, or 8, 4 or 2 that it repeats, after
// going back to its loop at its end, and a voice at the end of a sample without a loop stays silent. With the mixer's
// mode 0, it plays voices 0 and 1 only.
void Sequencer::Mix()
{
    interrupts_ = uint16_t(interrupts_ & ~kMixerInterrupts);
    dma_control_ = kDmaOn;
    const int voices = Get8(kMixerMode) == 0 ? kMode0Voices : kVoiceCount;
    for (int i = 0; i < voices; i++)
    {
        Voice& v = voices_[size_t(i)];
        uint32_t point = v.point;
        if (point == v.end)
        {
            point = v.loop;
            if (point == v.end)
            {
                continue;
            }
        }

        v.point = point + ((v.mode & 1) ? 8 : (v.mode & 2) ? 4 : (v.mode & 4) ? 2 : 16);
    }
    interrupts_ = uint16_t(interrupts_ | kMixerInterrupts);
}

// The FIFOs in the Magical Vacation revision: at each overflow of a FIFO's timer until the next frame, the FIFO plays a
// point, and if it then holds 16 or fewer, it asks for more, which interrupts while its DMA is on.
void Sequencer::RunFifos()
{
    const uint64_t end = (uint64_t(frame_) + 1) * kFrameCycles;
    for (int x = 0; x < kFifoCount; x++)
    {
        Fifo& f = fifos_[size_t(x)];
        while (f.running && f.next < end)
        {
            now_ = f.next;
            f.count = f.count > 0 ? f.count - 1 : 0;
            if (f.count <= kFifoRequest && f.dma && (interrupts_ & (kFifoAInterrupt << x)))
            {
                Interrupt(x);
            }
            f.next += Period(x);
        }
    }
}

// The DMA 1 or DMA 2 interrupt in the Magical Vacation revision, which gives FIFO x the next 4 points of its sample,
// scaled by its volume. At the end of the sample, it goes on from the loop, or at the end of one that doesn't loop, it
// stops the timer and gives the FIFO nothing more.
void Sequencer::Interrupt(int x)
{
    FifoVoice& v = fifo_voices_[size_t(x)];
    v.point += 4;
    if (v.point == v.end)
    {
        if (v.end == v.loop)
        {
            Write(x ? kIoTimer1Control : kIoTimer0Control, 2, 0);
            return;
        }
        v.point = v.loop;
    }

    Fifo& f = fifos_[size_t(x)];
    f.count = f.count + 4 < kFifoBytes ? f.count + 4 : kFifoBytes;
}

void Sequencer::Request(int sound)
{
    const uint8_t at = Get8(kQueueWrite);
    Set16(kQueue + at, uint32_t(sound));
    Set8(kQueueWrite, uint8_t((at + 2) & layout_.queue_mask));
}

uint8_t Sequencer::Get8(int offset) const
{
    return offset >= 0 && size_t(offset) < globals_.size() ? globals_[size_t(offset)] : 0;
}

uint16_t Sequencer::Get16(int offset) const
{
    return uint16_t(Get8(offset) | (Get8(offset + 1) << 8));
}

void Sequencer::Set8(int offset, uint32_t value)
{
    if (offset >= 0 && size_t(offset) < globals_.size())
    {
        globals_[size_t(offset)] = uint8_t(value);
    }
    else
    {
        Warn("the driver writes past its variables");
    }
}

void Sequencer::Set16(int offset, uint32_t value)
{
    Set8(offset, value);
    Set8(offset + 1, value >> 8);
}

void Sequencer::Set32(int offset, uint32_t value)
{
    Set16(offset, value);
    Set16(offset + 2, value >> 16);
}

void Sequencer::Write(uint32_t address, int size, uint32_t value)
{
    value &= size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (address >= kIoNr10 && address + uint32_t(size) <= kIoSoundEnd)
    {
        writes_.push_back({address, uint8_t(size), value});
        for (int i = 0; i < size; i++)
        {
            registers_[address - kIoNr10 + uint32_t(i)] = uint8_t(value >> (8 * i));
        }

        // A write to NRx3 and NRx4 with bit 15 set starts a PSG channel again, and SOUNDCNT_H's bits 11 and 15 empty
        // FIFO A and FIFO B.
        for (int c = 0; c < 4; c++)
        {
            if (address == kIoFrequencyRegisters[c] && size == 2 && (value & 0x8000))
            {
                restarted_ = uint8_t(restarted_ | (1 << c));
            }
        }
        if (address == kIoSoundcntH && size == 2)
        {
            if (value & 0x0800)
            {
                fifos_[0].count = 0;
            }
            if (value & 0x8000)
            {
                fifos_[1].count = 0;
            }
        }
    }
    else if (address >= kIoFifoA && address < kIoFifoA + 8)
    {
        Fifo& f = fifos_[(address - kIoFifoA) / 4];
        f.count = std::min(f.count + size, kFifoBytes);
    }
    else if (address == kIoTimer0Control || address == kIoTimer1Control)
    {
        // A timer that starts runs from its reload value, so that its first overflow comes a period later.
        const int x = address == kIoTimer1Control ? 1 : 0;
        Fifo& f = fifos_[size_t(x)];
        if (x == 0)
        {
            timer_control_ = uint16_t(value);
        }
        if ((value & kTimerOn) && !f.running)
        {
            f.running = true;
            f.next = now_ + Period(x);
        }
        else if (!(value & kTimerOn))
        {
            f.running = false;
        }
    }
    else if (address == kIoTimer0Reload || address == kIoTimer1Reload)
    {
        fifos_[address == kIoTimer1Reload ? 1 : 0].reload = uint16_t(value);
    }
    else if (address == kIoDma1Control || address == kIoDma2Control)
    {
        if (address == kIoDma1Control)
        {
            dma_control_ = uint16_t(value);
        }
        fifos_[address == kIoDma2Control ? 1 : 0].dma = (value & 0x8000) != 0;
    }
}

uint32_t Sequencer::ControlRegister(int c) const
{
    return rom_.U32(info_.control_registers + 4 * uint32_t(c));
}

uint32_t Sequencer::FrequencyRegister(int c) const
{
    return rom_.U32(info_.frequency_registers + 4 * uint32_t(c));
}

void Sequencer::Visit(int c, uint32_t address)
{
    visited_[size_t(c)].emplace(address, frame_);
}

void Sequencer::AddEvent(int c, Event::Kind kind, uint8_t key)
{
    Event e;
    e.kind = kind;
    e.channel = uint8_t(c);
    e.key = key;
    e.frame = frame_;
    events_.push_back(e);
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace agbsf2::brownie
