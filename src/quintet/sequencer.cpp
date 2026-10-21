// SPDX-License-Identifier: MIT

#include "quintet/sequencer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gba.h"
#include "quintet/driver.h"
#include "quintet/song.h"
#include "rom.h"
#include "warnings.h"

namespace agbsf2::quintet
{
namespace
{

// The registers outside the image that the driver writes: SOUNDBIAS, and the wave RAM's 16 bytes.
constexpr uint32_t kSoundbias = 0x04000088;
constexpr uint32_t kWaveRam = 0x04000090;

// The registers that play a FIFO's samples: the timer that sets its rate, and the DMA channel that feeds it.
struct FifoRegisters
{
    uint32_t timer;       // TMxCNT_L: a word written there sets TMxCNT_H too
    uint32_t dma_source;  // DMAxSAD
    uint32_t dma_dest;    // DMAxDAD
    uint32_t dma_count;   // DMAxCNT_L
    uint32_t dma_control; // DMAxCNT_H
    uint32_t fifo;        // FIFO_A or FIFO_B: the DMA's destination
    uint16_t reset;       // the bit of SOUNDCNT_H that resets the FIFO
};

// FIFO A plays through timer 0 and DMA 1. FIFO B plays through timer 1 and DMA 2.
constexpr FifoRegisters kFifoRegisters[2] = {
    {0x04000100, 0x040000BC, 0x040000C0, 0x040000C4, 0x040000C6, 0x040000A0, 0x0800},
    {0x04000104, 0x040000C8, 0x040000CC, 0x040000D0, 0x040000D2, 0x040000A4, 0x8000}};

// The bits that the hardware returns when its SOUNDCNT_H or NR30 is read.
constexpr uint16_t kSoundcntHRead = 0x770F;
constexpr uint8_t kNr30Read = 0xE0;

// The driver's figure for the CPU clock, from which it works out a FIFO's timer setting.
constexpr uint32_t kTimerClock = 16780000;

// The most commands a channel reads in a frame, so that data that never reaches a note can't hang the model.
constexpr int kMaxCommands = 10000;

// The most bytes of a noise macro read in a frame, so that a macro that never reaches a setting can't hang the model.
constexpr int kMaxMacroBytes = 10000;

// Divides as the game's library does: rounding towards 0, and giving 0 for a division by 0.
int32_t Divide(int32_t a, int32_t b)
{
    return b == 0 ? 0 : a / b;
}

uint32_t DivideUnsigned(uint32_t a, uint32_t b)
{
    return b == 0 ? 0 : a / b;
}

int32_t Remainder(int32_t a, int32_t b)
{
    return b == 0 ? 0 : a % b;
}

// Returns half of `x`, rounded towards 0, as the driver's (x + sign) >> 1 does.
int32_t Half(int32_t x)
{
    return (x + (x < 0 ? 1 : 0)) >> 1;
}

// Returns the timer setting for a FIFO that plays at `rate` Hz: the reload value, and the timer's enable bit.
uint32_t TimerSetting(uint32_t rate)
{
    return (0x10000 - SampleCycles(rate)) | 0x800000;
}

// Returns the rate at which a FIFO plays a sample of `base_rate` Hz at PCM pitch `pitch`: the sample's rate plus the
// rate × pitch / 1000. The division rounds towards 0.
uint32_t PlayRate(uint32_t base_rate, uint16_t pitch)
{
    return base_rate + uint32_t(Divide(int32_t(base_rate) * int16_t(pitch), 1000));
}

// Returns the index of note `note` (0-11, or 12 to play again or 13 to rest) at the channel's octave and transposes.
// The index stays within a pitch table of `entries` entries.
int NoteIndex(const Channel& ch, int note, int entries)
{
    const int index = ch.octave * 12 + note + ch.transpose + ch.transpose2;
    return index < 0 ? 0 : index > entries - 1 ? entries - 1 : index;
}

// Starts the channel's effects again for a note or a "play again". Stops them for a rest (note 13).
void RestartEffects(Channel& ch, int note)
{
    if (note != 13)
    {
        ch.macro = ch.macro_start;
        ch.macro_count = 0;
        ch.macro_hold = 0;
        ch.envelope_time = 0;
        ch.bend_time = 0;
        ch.lfo_time = 0;
        ch.switch_count = ch.switch_delay;
    }
    else
    {
        ch.macro = 0;
        ch.envelope_time = kAfterRest;
        ch.bend_time = kAfterRest;
        ch.lfo_time = kAfterRest;
        ch.switch_count = 0;
    }
}

// Sets the channel's countdown to the frames until its next note. The next note comes `ticks` ticks after this one. The
// channel counts the ticks it has played and the frames they take. A tick takes 37 / tempo frames. Working the frames
// out from the whole count keeps the rounding from adding up. A note lasts at least a frame.
void WaitTicks(Channel& ch, uint16_t ticks)
{
    ch.ticks += uint32_t(int32_t(int16_t(ticks)));
    uint32_t frames = DivideUnsigned(ch.ticks * 37, ch.tempo);
    uint32_t wait = frames - ch.frames;
    if (wait == 0)
    {
        frames++;
        wait = 1;
    }

    ch.frames = frames;
    ch.countdown = uint16_t(wait);
}

} // namespace

uint32_t SampleCycles(uint32_t rate)
{
    return DivideUnsigned(kTimerClock, rate);
}

uint32_t FifoFrames(uint32_t position, uint32_t rate, uint32_t end)
{
    // Each update adds `rate`, and then stops or loops the FIFO if another `rate` would reach `end` × 60.
    const uint64_t target = uint64_t(end) * 60;
    if (rate == 0 || position + 2 * uint64_t(rate) >= target)
    {
        return 0;
    }

    return uint32_t((target - position + rate - 1) / rate - 2);
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : rom_(rom), info_(info), traits_(TraitsOf(info.revision))
{
    SongHeader header;
    if (song < 0 || song >= int(info.song_addresses.size()) ||
        !ReadSongHeader(rom, info.song_addresses[size_t(song)], header))
    {
        return;
    }

    // The game's sound init: the driver's init resets the channels and the PSG, and the routine that sets up the FIFOs
    // stops them. None of that is traced, but it leaves the registers that the driver reads back.
    ResetPsg();
    for (const FifoRegisters& fifo : kFifoRegisters)
    {
        Write(fifo.timer, 4, 0);
        Write(fifo.dma_control, 2, 0);
    }

    // The play routine resets the channels and the PSG, sets the PSG's master volume, and points each channel at its
    // data.
    writes_.clear();
    for (Channel& ch : channels_)
    {
        ResetChannel(ch);
    }

    ResetPsg();
    image_[kNr50] = uint16_t((image_[kNr50] & 0xFF00) | 0x77);

    for (int c = 0; c < kChannels; c++)
    {
        channels_[size_t(c)].position = header.channels[size_t(c)];
    }

    if (traits_.reseeds)
    {
        Reseed();
    }

    start_writes_ = writes_;
    valid_ = true;
}

const std::vector<RegisterWrite>& Sequencer::Step()
{
    writes_.clear();
    events_.clear();
    hardware_.restarted = 0;

    // The music channels read their notes when their countdowns run out, and then apply their effects.
    for (int c = 0; c < kChannels; c++)
    {
        Channel& ch = channels_[size_t(c)];
        if (!ch.position)
        {
            continue;
        }

        ch.countdown--;
        if (int16_t(ch.countdown) <= 0)
        {
            ReadNotes(c);
            UpdateImage(c);
        }

        if (c < kPcmA)
        {
            ChannelEffects(c);
        }
        else
        {
            PcmEffects(c);
        }
    }

    Output();
    UpdateFifos();

    // The J revision sets the timers that the frame's starts and bends left until now.
    for (int f = 0; f < 2; f++)
    {
        if (pending_timers_[size_t(f)])
        {
            Write(kFifoRegisters[size_t(f)].timer, 4, pending_timers_[size_t(f)]);
            pending_timers_[size_t(f)] = 0;
        }
    }

    frame_++;

    return writes_;
}

bool Sequencer::Ended() const
{
    for (const Channel& ch : channels_)
    {
        if (ch.position)
        {
            return false;
        }
    }

    return true;
}

void Sequencer::ResetChannel(Channel& ch)
{
    ch = Channel();
    ch.tempo = traits_.start_tempo;
    ch.envelope_up = traits_.start_envelope_up;
}

void Sequencer::ResetPsg()
{
    for (int r = kNr10; r <= kNr43; r++)
    {
        Write(kImageRegisters[r], 2, 0);
    }

    Write(kImageRegisters[kNr50], 2, 0xFF77);
    Write(kImageRegisters[kSoundcntH], 2, 0xFB0E);
    image_.fill(0);
    image_[kNr50] = 0xFF77;
    image_[kSoundcntH] = 0x730E;
    shadow_ = image_;
    Write(kSoundbias, 2, (soundbias_ & 0x3FFF) | 0x4000);
}

void Sequencer::ReadNotes(int c)
{
    Channel& ch = channels_[size_t(c)];
    for (int commands = 0;; commands++)
    {
        if (commands > kMaxCommands)
        {
            Warn("channel " + std::to_string(c) + " reads commands without reaching a note, so it was ended");
            ch.position = 0;
            AddEvent(c, Event::kEnd);
            return;
        }
        if (rom_.U8(ch.position) == 0xFF && !FollowEnd(c))
        {
            return;
        }

        // A note's low nibble is its pitch, or 12 to play again or 13 to rest, and its high nibble picks its length.
        const uint8_t b = rom_.U8(ch.position++);
        const int note = b & 15;
        const int length = b >> 4;
        if (note > 13 || length > 12)
        {
            RunCommand(c, b);
            continue;
        }

        // A note, a "play again" or a rest: its start, its length with any dot and ties, the effects, the event and the
        // wait for the next note.
        StartNote(c, note);
        const uint16_t ticks = NoteTicks(ch, note, length);
        RestartEffects(ch, note);
        const Event::Kind kind = note <= 11 ? Event::kNote : note == 12 ? Event::kPlayAgain : Event::kRest;
        AddEvent(c, kind, NoteIndex(ch, note, c >= kPcmA ? kPcmPitchEntries : kFrequencyEntries));
        ticks_[size_t(c)] += ticks;
        WaitTicks(ch, ticks);
        return;
    }
}

bool Sequencer::FollowEnd(int c)
{
    // At the loop point itself, FF reads nothing more in this frame or any later one. The channel holds its note.
    Channel& ch = channels_[size_t(c)];
    if (ch.position == ch.loop)
    {
        if (!held_[size_t(c)])
        {
            held_[size_t(c)] = true;
            AddEvent(c, Event::kHold);
        }

        return false;
    }

    // Without a loop point, FF ends the channel.
    if (!ch.loop)
    {
        ch.position = 0;
        ch.level = 0;
        ch.envelope_up = 0;
        ch.envelope_step = 0;
        if (c >= kPcmA)
        {
            StopFifo(c - kPcmA);
        }
        AddEvent(c, Event::kEnd);
        return false;
    }

    // Otherwise FF goes back to the loop point. The channel counts its ticks and frames again from there.
    ch.position = ch.loop;
    ch.frames = 0;
    ch.ticks = 0;
    if (traits_.reseeds)
    {
        Reseed();
    }
    AddEvent(c, Event::kLoop);

    return true;
}

void Sequencer::StartNote(int c, int note)
{
    Channel& ch = channels_[size_t(c)];
    const bool pcm = c >= kPcmA;
    ch.sounding = 1;
    ch.sliding = 0;
    if (note <= 11)
    {
        // After D4 the note slides from the pitch of the note before.
        const uint16_t before = ch.pitch;
        ch.pitch = pcm ? PcmPitch(ch, note) : PsgFrequency(ch, note);
        ch.pitch_from = ch.slide_next ? before : ch.pitch;
        if (pcm)
        {
            PcmNoteOn(c);
        }
        if (ch.slide_next)
        {
            ch.slide_time = 0;
            ch.sliding = 1;
        }

        ch.level = ch.volume;
    }
    else if (note == 12)
    {
        ch.level = ch.volume;
        if (pcm)
        {
            ch.pitch = ch.detune;
            ch.pitch_from = ch.detune;
            PcmNoteOn(c);
        }
    }
    else
    {
        ch.level = 0;
        ch.sounding = 0;
        if (pcm)
        {
            StopFifo(c - kPcmA);
        }
    }

    ch.slide_next = 0;
}

uint16_t Sequencer::NoteTicks(Channel& ch, int note, int length)
{
    // D1 makes the note dotted. D4 followed by a note of the same pitch ties another length to it. The driver adds the
    // lengths up in the channel's countdown. The countdown is a halfword.
    uint16_t ticks = uint16_t(Divide(kWholeNote, rom_.U8(info_.length_table + uint32_t(length))));
    if (rom_.U8(ch.position) == 0xD1)
    {
        ch.position++;
        ticks = uint16_t(ticks + Half(int16_t(ticks)));
    }
    while (rom_.U8(ch.position) == 0xD4)
    {
        const uint8_t next = rom_.U8(ch.position + 1);
        if ((next & 15) != note || (next >> 4) > 12)
        {
            break;
        }

        int32_t add = Divide(kWholeNote, rom_.U8(info_.length_table + uint32_t(next >> 4)));
        ch.position += 2;
        if (rom_.U8(ch.position) == 0xD1)
        {
            ch.position++;
            add += Half(add);
        }

        ticks = uint16_t(ticks + add);
    }

    return ticks;
}

void Sequencer::RunCommand(int c, uint8_t command)
{
    Channel& ch = channels_[size_t(c)];

    auto arg = [&]()
    {
        return rom_.U8(ch.position++);
    };

    switch (command)
    {
    case 0x0E:
        ch.sweep_shift = arg();
        break;

    case 0x0F:
        ch.sweep_down = 0;
        break;

    case 0x1E:
        ch.sweep_down = 1;
        break;

    case 0x1F:
        ch.sweep_time = arg();
        break;

    case 0x2E:
        ch.length = arg();
        ch.length_on = ch.length != 0x63;
        if (ch.length == 0x63)
        {
            ch.length = 0x3F;
        }
        break;

    case 0x2F:
        ch.duty = arg();
        ch.switch_delay = 0;
        break;

    case 0x3E:
        ch.envelope_step = arg();
        break;

    case 0x3F:
        ch.envelope_up = 1;
        break;

    case 0x4E:
        ch.envelope_up = 0;
        break;

    case 0x4F:
        ch.octave = arg();
        break;

    case 0x5E:
        ch.volume = arg();
        break;

    case 0x5F:
        {
            // Volume up, to at most 3 on the wave channel (4 in the J revision), 1 on a PCM channel and 15 on the
            // others.
            const uint8_t wave = uint8_t(traits_.wave_levels - 1);
            const uint8_t limit = c == kWave ? wave : c >= kPcmA ? 1 : 15;
            if (ch.volume != limit)
            {
                ch.volume++;
            }
            break;
        }

    case 0x6E:
        if (ch.volume != 0)
        {
            ch.volume--;
        }
        break;

    case 0x6F:
        if (ch.octave != 6)
        {
            ch.octave++;
        }
        break;

    case 0x7E:
        if (ch.octave != 0)
        {
            ch.octave--;
        }
        break;

    case 0x7F:
        // A tempo on the first channel sets every channel's.
        ch.tempo = arg();
        ch.frames = 0;
        ch.ticks = 0;
        if (c == 0)
        {
            for (int k = 1; k < kChannels; k++)
            {
                Channel& other = channels_[size_t(k)];
                other.tempo = ch.tempo;
                other.frames = 0;
                other.ticks = 0;
            }
        }
        AddEvent(c, Event::kTempo);
        break;

    case 0x8E:
        ch.noise_shift = arg();
        break;

    case 0x8F:
        ch.noise_width = arg();
        break;

    case 0x9E:
        ch.noise_ratio = arg();
        break;

    case 0x9F:
        ch.wave_size = arg();
        break;

    case 0xAE:
        ch.wave_bank = arg();
        break;

    case 0xAF:
        ch.wave_on = arg();
        break;

    case 0xBE:
        ch.wave_75 = arg();
        break;

    case 0xBF:
        ch.loop = ch.position;
        AddEvent(c, Event::kLoopPoint);
        break;

    case 0xCE:
        image_[kNr50] = uint16_t((image_[kNr50] & 0xFF8F) | arg());
        break;

    case 0xCF:
        image_[kNr50] = uint16_t((image_[kNr50] & 0xFFF8) | arg());
        break;

    case 0xD0:
        {
            // The PSG channels' enables on each side, or a FIFO's: keep the bits of the first byte, then set those of
            // the second.
            const uint8_t keep = arg();
            const uint8_t set = arg();
            uint16_t& reg = image_[c < kPcmA ? kNr50 : kSoundcntH];
            reg = uint16_t((((keep << 8) | 0xFF) & reg) | (set << 8));
            break;
        }

    case 0xD2:
        ch.envelope_on = 1;
        for (uint8_t& e : ch.envelope)
        {
            e = arg();
        }
        break;

    case 0xD3:
        ch.envelope_on = 0;
        break;

    case 0xD4:
        ch.slide_next = 1;
        break;

    case 0xD5:
        ch.detune = uint16_t(int8_t(arg()));
        if (c >= kPcmA)
        {
            ch.detune = uint16_t(int16_t(ch.detune) * 10);
        }
        break;

    case 0xD6:
        if (ch.repeat_depth >= ch.repeat_start.size())
        {
            Warn("channel " + std::to_string(c) + " nests its repeats too deeply");
            break;
        }

        ch.repeat_start[ch.repeat_depth] = ch.position;
        ch.repeat_count[ch.repeat_depth] = 0;
        ch.repeat_depth++;
        break;

    case 0xD7:
        {
            if (ch.repeat_depth == 0)
            {
                Warn("channel " + std::to_string(c) + " ends a repeat it didn't start");
                ch.position++;
                break;
            }

            uint8_t& count = ch.repeat_count[ch.repeat_depth - 1];
            if (count == 0)
            {
                count = rom_.U8(ch.position);
            }

            count--;
            if (count == 0)
            {
                ch.position++;
                ch.repeat_depth--;
            }
            else
            {
                ch.position = ch.repeat_start[ch.repeat_depth - 1];
            }
            break;
        }

    case 0xDA:
        LoadWave(c, arg());
        break;

    case 0xDB:
        SelectMacro(ch, arg());
        break;

    case 0xDC:
    case 0xDD:
        SelectSample(c, arg());
        ch.pcm_loop = 0;
        ch.pcm_end = 0;
        ch.pcm_start = 0;
        break;

    case 0xDE:
        ch.switch_delay = arg();
        ch.switch_value = arg();
        ch.switch_to = arg();
        break;

    case 0xDF:
        ch.lfo_on = 1;
        ch.lfo_delay = arg();
        ch.lfo_period = arg();
        ch.lfo_depth = arg();
        break;

    case 0xE0:
        ch.lfo_on = 0;
        break;

    case 0xE1:
    case 0xEB:
        ch.switch_delay = 0;
        break;

    case 0xE2:
        ch.bend_on = 1;
        for (int8_t& b : ch.bend)
        {
            b = int8_t(arg());
        }
        break;

    case 0xE3:
        ch.bend_on = 0;
        break;

    case 0xE4:
        ch.transpose = int8_t(arg());
        break;

    case 0xE5:
        ch.slide_frames = arg();
        break;

    case 0xE6:
        {
            const uint8_t low = arg();
            ch.detune = uint16_t(low | arg() << 8);
            break;
        }

    case 0xE7:
        ch.pcm_start = arg();
        ch.pcm_end = arg();
        ch.pcm_loop = arg();
        break;

    case 0xE8:
        Write(kSoundbias, 2, uint32_t(arg() << 14) | (soundbias_ & 0x3FFF));
        break;

    case 0xE9:
        ch.transpose2 = int8_t(arg());
        break;

    case 0xEA:
        ch.switch_delay = arg();
        ch.switch_value = 0;
        ch.switch_to = 1;
        break;

    case 0xEC:
        ch.lfo_shape = arg();
        break;

    case 0xED:
        ch.lfo_shape = uint8_t((ch.lfo_shape & 0x7F) | arg());
        break;

    case 0xEE:
        ch.transpose++;
        break;

    case 0xEF:
        ch.transpose--;
        break;

    case 0xF1:
        {
            // Add to the volume, keeping it from 0 to 4 on the wave channel in both revisions, to 1 on a PCM channel
            // and to 15 on the others.
            ch.volume = uint8_t(ch.volume + arg());
            const int8_t v = int8_t(ch.volume);
            const int8_t limit = c == kWave ? 4 : c >= kPcmA ? 1 : 15;
            if (v < 0)
            {
                ch.volume = 0;
            }
            else if (v > limit)
            {
                ch.volume = uint8_t(limit);
            }
            break;
        }

    case 0xF0:
        // In the J revision, the noise channel switches between its banks of noise macros, and the others between their
        // banks of samples.
        if (traits_.banks)
        {
            (c == kNoise ? ch.macro_bank : ch.sample_bank) ^= 1;
        }
        break;

    default:
        // D1, D8, D9 and F2 to FE do nothing as commands, and neither does F0 in the A revision.
        break;
    }
}

void Sequencer::UpdateImage(int c)
{
    const Channel& ch = channels_[size_t(c)];
    const uint8_t duty = ch.switch_count ? ch.switch_value : ch.duty;
    const uint16_t frequency = uint16_t(ch.length_on << 14 | ch.pitch_from | kRestart);
    switch (c)
    {
    case kSquare1:
    case kSquare2:
        {
            const int step = ch.sounding ? ch.envelope_step : 0;
            if (c == kSquare1)
            {
                image_[kNr10] = uint16_t(ch.sweep_shift | ch.sweep_down << 3 | ch.sweep_time << 4);
            }

            image_[size_t(kPsgEnvelope[c])] =
                uint16_t(duty << 6 | ch.length | step << 8 | ch.envelope_up << 11 | ch.level << 12);
            image_[size_t(kPsgControl[c])] = frequency;
            break;
        }

    case kWave:
        {
            // A level above the revision's wave levels leaves the volume code that came before.
            if (ch.level < traits_.wave_levels)
            {
                wave_code_ = traits_.wave_codes[ch.level];
            }
            else
            {
                Warn("the wave channel plays at volume " + std::to_string(ch.level) +
                     ", which the driver doesn't handle");
            }

            const uint8_t bank = ch.switch_count ? ch.switch_value : ch.wave_bank;
            image_[kNr30] = uint16_t(ch.wave_size << 5 | bank << 6 | ch.wave_on << 7);
            image_[kNr31] = uint16_t(ch.length << 2 | wave_code_ << 13 | ch.wave_75 << 15);
            image_[kNr33] = frequency;
            break;
        }

    case kNoise:
        image_[kNr41] = uint16_t(ch.length | ch.envelope_step << 8 | ch.envelope_up << 11 | ch.level << 12);
        image_[kNr43] =
            uint16_t(ch.noise_ratio | ch.noise_width << 3 | ch.noise_shift << 4 | ch.length_on << 14 | kRestart);
        break;

    case kPcmA:
        image_[kSoundcntH] = uint16_t((image_[kSoundcntH] & 0xFFFB) | ch.level << 2);
        break;

    default:
        image_[kSoundcntH] = uint16_t((image_[kSoundcntH] & 0xFFF7) | ch.level << 3);
        break;
    }
}

void Sequencer::ChannelEffects(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (!ch.position)
    {
        return;
    }

    if (ch.envelope_on)
    {
        VolumeEnvelope(c);
    }

    ch.pitch_out = ch.pitch;
    if (ch.bend_on)
    {
        PitchEnvelope(c);
    }
    if (ch.lfo_on)
    {
        Lfo(c);
    }
    if (ch.sliding)
    {
        Slide(c);
    }

    if (ch.macro)
    {
        NoiseMacro(c);
    }
    if (ch.switch_count)
    {
        Switch(c);
    }
}

void Sequencer::PcmEffects(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (!ch.position)
    {
        return;
    }

    ch.pitch_out = ch.pitch;
    if (ch.bend_on)
    {
        PitchEnvelope(c);
    }
    if (ch.lfo_on)
    {
        Lfo(c);
    }
    if (ch.sliding)
    {
        Slide(c);
    }

    // Only a channel whose pitch moves sets its FIFO's rate again.
    if (ch.bend_on || ch.lfo_on || ch.sliding)
    {
        BendFifo(ch.pitch_out, c - kPcmA);
    }
}

void Sequencer::VolumeEnvelope(int c)
{
    Channel& ch = channels_[size_t(c)];
    const int32_t t = ch.envelope_time;
    if (traits_.idle_after_rest && t == kAfterRest)
    {
        return;
    }

    const int32_t start = ch.envelope[0];
    const int32_t peak = ch.envelope[1];
    const int32_t end = ch.envelope[2];
    const int32_t attack = ch.envelope[3];
    const int32_t decay = ch.envelope[4];

    // The level goes in straight lines from the start to the peak over the attack, and then to the end over the decay.
    // The A revision then leaves the volume alone, and the J revision goes on applying the end level.
    int32_t level = end;
    if (t <= attack)
    {
        level = start + Divide((peak - start) * t, attack);
    }
    else if (t <= attack + decay)
    {
        level = peak + Divide((end - peak) * (t - attack), decay);
    }
    else if (!traits_.envelope_holds)
    {
        return;
    }

    // It scales the channel's volume. A square or noise channel restarts at the new volume when it changes, and the
    // wave channel takes the nearest of its volume codes: in the J revision, 75% for 3 and 100% for 4, and silence
    // above.
    const int32_t volume = Divide(int8_t(ch.volume) * int32_t(uint16_t(level)), 100);
    const uint16_t bits = uint16_t((volume & 15) << 12);
    if (c == kWave)
    {
        const int wave_level = bits >> 12;
        const uint8_t wave_code =
            wave_level < traits_.wave_levels ? traits_.wave_codes[size_t(wave_level)] : traits_.envelope_wave_above;
        const uint16_t code = uint16_t(wave_code << 13);
        const uint16_t mask = traits_.envelope_wave_mask;
        if ((image_[kNr31] & mask) != code)
        {
            image_[kNr31] = uint16_t((image_[kNr31] & ~mask) | code);
        }
    }
    else
    {
        uint16_t& envelope = image_[size_t(kPsgEnvelope[c])];
        if ((envelope & 0xF000) != bits)
        {
            envelope = uint16_t((envelope & 0x0FFF) | bits);
            image_[size_t(kPsgControl[c])] |= kRestart;
        }
    }

    ch.envelope_time++;
}

void Sequencer::PitchEnvelope(int c)
{
    Channel& ch = channels_[size_t(c)];
    const int32_t t = ch.bend_time;
    if (traits_.idle_after_rest && t == kAfterRest)
    {
        return;
    }

    const int32_t start = ch.bend[0];
    const int32_t middle = ch.bend[1];
    const int32_t end = ch.bend[2];
    const int32_t first = ch.bend[3];
    const int32_t second = ch.bend[4];

    // The offset goes in straight lines from the start to the middle, and then to the end, where it stays.
    int32_t offset = end;
    if (t <= first)
    {
        offset = start + Divide((middle - start) * t, first);
    }
    else if (t <= first + second)
    {
        offset = middle + Divide((end - middle) * (t - first), second);
    }

    AddPitch(c, int16_t(offset));
    ch.bend_time++;
}

void Sequencer::Lfo(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (ch.lfo_time == kAfterRest)
    {
        return;
    }

    if (ch.lfo_time >= ch.lfo_delay)
    {
        // The LFO's phase, 0-255, over its period.
        const int32_t phase = Remainder(ch.lfo_time - ch.lfo_delay, ch.lfo_period);
        uint16_t v = uint16_t(Divide(int32_t(int16_t(phase)) * 256, ch.lfo_period));
        const int16_t s = int16_t(v);
        switch (ch.lfo_shape & 0x7F)
        {
        case 1:
            // Triangle.
            v = s > 0x7F ? uint16_t(0xFF - v) : v;
            break;

        case 2:
            // Rising saw.
            v = uint16_t(Half(s));
            break;

        case 3:
            // Falling saw.
            v = uint16_t(Half(0xFF - s));
            break;

        case 4:
            v = s > 0x7F ? 0x7F : 0;
            break;

        case 5:
            v = s > 0x7F ? 0 : 0x7F;
            break;

        case 6:
            // A new random value each period, in its second half.
            if (s > 0x7F)
            {
                if (!random_high_)
                {
                    random_high_ = 1;
                    NextRandom();
                }
                v = random_value_;
            }
            else
            {
                v = 0;
                random_high_ = 0;
            }
            break;

        case 7:
            // A new random value each half period.
            if ((s > 0x7F) != (random_high_ != 0))
            {
                random_high_ = s > 0x7F;
                NextRandom();
            }
            v = random_value_;
            break;

        default:
            // Sine, also for shapes above 7.
            v = uint16_t(rom_.S8(info_.lfo_table + uint32_t(int32_t(s))));
            break;
        }

        if (ch.lfo_shape & 0x80)
        {
            v = uint16_t(-v);
        }

        int32_t m = int32_t(int16_t(v)) * ch.lfo_depth;
        if (m < 0)
        {
            m += 0x7F;
        }

        AddPitch(c, int16_t(uint16_t(m >> 7)));
    }

    ch.lfo_time++;
}

void Sequencer::AddPitch(int c, int16_t offset)
{
    // On a square or wave channel the pitch is a frequency setting. The offset moves the setting directly and keeps it
    // within the register's range. On a PCM channel the pitch is a PCM pitch. In the A revision, the offset to a PCM
    // pitch counts double. The noise channel's pitch takes the offset in the same way as a PCM pitch. Nothing reads the
    // noise channel's pitch.
    Channel& ch = channels_[size_t(c)];
    if (c <= kWave)
    {
        int32_t f = int16_t(uint16_t(ch.pitch_out + offset));
        f = f < 0 ? 0 : f > 0x7FF ? 0x7FF : f;
        ch.pitch_out = uint16_t(f);
        uint16_t& reg = image_[size_t(kPsgControl[c])];
        reg = uint16_t((reg & 0xF800) | f);
    }
    else
    {
        ch.pitch_out = uint16_t(ch.pitch_out + offset * traits_.pcm_offset_factor);
    }
}

void Sequencer::Slide(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (ch.slide_time <= ch.slide_frames)
    {
        const int32_t from = int16_t(ch.pitch_from);
        const int32_t to = int16_t(ch.pitch);
        const int32_t p = from + Divide((to - from) * int32_t(ch.slide_time), ch.slide_frames);
        if (c <= kWave)
        {
            uint16_t& reg = image_[size_t(kPsgControl[c])];
            reg = uint16_t((reg & 0xF800) | uint32_t(p));
        }
        else if (c >= kPcmA)
        {
            ch.pitch_out = uint16_t(p);
        }
    }

    ch.slide_time++;
}

void Sequencer::NoiseMacro(int c)
{
    Channel& ch = channels_[size_t(c)];
    uint16_t& nr41 = image_[kNr41];
    uint16_t& nr43 = image_[kNr43];
    const uint16_t length = uint16_t(ch.length_on << 14);

    // Hold the setting for a few more frames.
    if (ch.macro_count)
    {
        nr43 = uint16_t(length | rom_.U8(ch.macro) | (nr43 & kRestart));
        if (--ch.macro_count == 0)
        {
            ch.macro++;
        }
        return;
    }

    // FF ends the macro, silencing the channel; FD n holds each setting for n frames; FE v sets the envelope and
    // restarts the channel; any other byte is the frame's NR43 setting.
    for (int bytes = 0; bytes < kMaxMacroBytes; bytes++)
    {
        const uint8_t b = rom_.U8(ch.macro);
        if (b == 0xFF)
        {
            ch.macro = 0;
            nr41 &= 0x0FFF;
            nr43 = uint16_t(length | kRestart);
            return;
        }
        if (b == 0xFD)
        {
            ch.macro_hold = uint8_t(rom_.U8(ch.macro + 1) - 1);
            ch.macro += 2;
            continue;
        }
        if (b == 0xFE)
        {
            nr41 = uint16_t(rom_.U8(ch.macro + 1) << 8 | (nr41 & 0xFF));
            nr43 |= kRestart;
            ch.macro += 2;
            continue;
        }

        nr43 = uint16_t(b | length | (nr43 & kRestart));
        if (ch.macro_hold)
        {
            ch.macro_count = ch.macro_hold;
        }
        else
        {
            ch.macro++;
        }
        return;
    }

    ch.macro = 0;
    Warn("a noise macro never reaches a setting, so it was stopped");
}

void Sequencer::Switch(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (--ch.switch_count != 0)
    {
        return;
    }

    switch (c)
    {
    case kSquare1:
    case kSquare2:
        {
            uint16_t& envelope = image_[size_t(kPsgEnvelope[c])];
            envelope = uint16_t((envelope & 0xFF3F) | ch.switch_to << 6);
            break;
        }

    case kWave:
        image_[kNr30] = uint16_t(ch.wave_size << 5 | ch.switch_to << 6 | ch.wave_on << 7);
        break;

    default:
        break;
    }
}

void Sequencer::Output()
{
    // Each PSG channel's registers are written when the channel restarts, and otherwise when they change. Square 1
    // writes its frequency register three times.
    struct Psg
    {
        int first;
        int count;
        uint16_t enable;
    };

    static constexpr Psg kPsgs[4] = {{kNr10, 3, 0x1100}, {kNr21, 2, 0x2200}, {kNr30, 3, 0x4400}, {kNr41, 2, 0x8800}};
    uint16_t nr50 = uint16_t(image_[kNr50] & 0xFF);
    for (int p = 0; p < 4; p++)
    {
        const Psg& psg = kPsgs[p];
        if (p == kWave)
        {
            // The wave channel's waves go back into the wave RAM if something else has loaded others.
            const Channel& wave = channels_[kWave];
            if (loaded_waves_[0] != wave.wave)
            {
                LoadWave(kWave, wave.wave);
            }
            if (loaded_waves_[1] != wave.wave_high)
            {
                LoadWave(kWave, wave.wave_high);
            }
        }

        const int last = psg.first + psg.count - 1;
        if (image_[size_t(last)] & kRestart)
        {
            for (int r = psg.first; r <= last; r++)
            {
                Write(kImageRegisters[r], 2, image_[size_t(r)]);
            }
            if (p == kSquare1)
            {
                Write(kImageRegisters[last], 2, image_[size_t(last)]);
                Write(kImageRegisters[last], 2, image_[size_t(last)]);
            }

            image_[size_t(last)] &= 0x7FFF;
            for (int r = psg.first; r <= last; r++)
            {
                shadow_[size_t(r)] = image_[size_t(r)];
            }
        }
        else
        {
            for (int r = psg.first; r <= last; r++)
            {
                if (shadow_[size_t(r)] != image_[size_t(r)])
                {
                    Write(kImageRegisters[r], 2, image_[size_t(r)]);
                    shadow_[size_t(r)] = image_[size_t(r)];
                }
            }
        }

        nr50 = uint16_t(nr50 | (image_[kNr50] & psg.enable));
    }

    if (shadow_[kNr50] != nr50)
    {
        Write(kImageRegisters[kNr50], 2, nr50);
        shadow_[kNr50] = nr50;
    }

    // The FIFOs' volumes and enables. The master volume that the game sets would halve or mute them at 4 or less, but
    // the play routine sets it to 7.
    uint16_t soundcnt_h = uint16_t(shadow_[kSoundcntH] & 0xCC03);
    soundcnt_h = uint16_t(soundcnt_h | (image_[kSoundcntH] & 0x0304) | (image_[kSoundcntH] & 0x3008));
    if (shadow_[kSoundcntH] != soundcnt_h)
    {
        Write(kImageRegisters[kSoundcntH], 2, soundcnt_h);
        shadow_[kSoundcntH] = soundcnt_h;
    }
}

void Sequencer::UpdateFifos()
{
    // The driver works out how far each FIFO's sample has played from its rate, and stops or loops it a frame before
    // the end.
    for (int f = 0; f < 2; f++)
    {
        Fifo& fifo = fifos_[size_t(f)];
        if (fifo.position == ~0u)
        {
            continue;
        }

        fifo.position += fifo.rate;
        if (DivideUnsigned(fifo.position + fifo.rate, 60) >= fifo.end)
        {
            StopFifo(f);
            if (fifo.loop)
            {
                fifo.position = fifo.loop_position + fifo.rate;
                StartFifo(fifo.loop, f);
            }
        }
    }
}

uint16_t Sequencer::PsgFrequency(const Channel& ch, int note) const
{
    const int32_t n = NoteIndex(ch, note, kFrequencyEntries);
    int32_t f = int16_t(rom_.U16(info_.frequency_table + uint32_t(n) * 2)) + int16_t(ch.detune);
    f = f < 0 ? 0 : f > 0x7FF ? 0x7FF : f;

    return uint16_t(f);
}

uint16_t Sequencer::PcmPitch(const Channel& ch, int note) const
{
    const int32_t n = NoteIndex(ch, note, kPcmPitchEntries);

    return uint16_t(rom_.U16(info_.pcm_pitch_table + uint32_t(n) * 2) + ch.detune);
}

void Sequencer::LoadWave(int c, uint8_t wave)
{
    if (!info_.waves)
    {
        return;
    }

    if (wave & 0x80)
    {
        channels_[size_t(c)].wave_high = wave;
    }
    else
    {
        channels_[size_t(c)].wave = wave;
    }

    // The wave RAM's writes go to the bank that isn't playing, so the driver selects the other bank, writes the wave
    // and puts NR30 back as it read it, which leaves the channel off.
    const uint8_t nr30 = uint8_t(nr30_ & 0x7F);
    uint8_t select = 0;
    if (wave & 0x80)
    {
        select = nr30 & 0xBF;
        loaded_waves_[1] = wave;
    }
    else
    {
        select = nr30 | 0x40;
        loaded_waves_[0] = wave;
    }

    Write(kImageRegisters[kNr30], 1, select);
    const uint32_t data = info_.waves + (wave & 0x7Fu) * 16 + 2;
    for (uint32_t i = 0; i < 16; i++)
    {
        Write(kWaveRam + i, 1, rom_.U8(data + i));
    }

    Write(kImageRegisters[kNr30], 1, nr30);
}

void Sequencer::SelectMacro(Channel& ch, uint8_t macro)
{
    // The A revision leaves the macro alone without a table, and the J revision clears it.
    const uint32_t table = info_.macros[ch.macro_bank & 1];
    if (!table)
    {
        ch.macro_start = traits_.clears_missing_macro ? 0 : ch.macro_start;
        return;
    }

    ch.macro_start = macro ? table + rom_.U16(table + 2 * uint32_t(macro) + 2) : 0;
}

void Sequencer::SelectSample(int c, uint8_t sample)
{
    // The loaded sample is remembered by its number alone, so a bank switch takes a sample of another number to show.
    const int f = c - kPcmA;
    LoadSample(f, SampleAddress(channels_[size_t(c)].sample_bank & 1, sample));
    channels_[size_t(c)].wave = sample;
    loaded_samples_[size_t(f)] = sample;
}

uint32_t Sequencer::SampleAddress(int bank, uint8_t sample)
{
    const std::vector<uint32_t>& list = info_.sample_addresses[size_t(bank)];
    if (sample < list.size())
    {
        return list[sample];
    }

    Warn("a PCM channel plays sample " + std::to_string(sample) + " of bank " + std::to_string(bank) +
         ", which isn't in the game's sample file");

    return 0;
}

void Sequencer::PcmNoteOn(int c)
{
    const Channel& ch = channels_[size_t(c)];
    const int f = c - kPcmA;
    if (loaded_samples_[size_t(f)] != ch.wave)
    {
        SelectSample(c, ch.wave);
    }

    PlaySample(ch.pitch_from, ch.pcm_start, ch.pcm_end, ch.pcm_loop, f);
}

void Sequencer::LoadSample(int f, uint32_t sample)
{
    StopFifo(f);
    Fifo& fifo = fifos_[size_t(f)];
    if (!sample)
    {
        fifo.sample = 0;
        fifo.next_start = 0;
        return;
    }

    // A sample's header holds its size, header included, and its rate. The driver plays 64 bytes fewer, less 0.6%.
    const uint32_t size = rom_.U32(sample);
    const uint32_t length = DivideUnsigned((size - 0x40) * 994, 1000);
    fifo.end = length;
    fifo.length = length;
    fifo.base_rate = rom_.U16(sample + 4);
    fifo.sample = sample + 6;
    fifo.next_start = sample + 6;
}

void Sequencer::PlaySample(uint16_t pitch, int start, int end, int loop, int f)
{
    StopFifo(f);
    if (!fifos_[size_t(f)].next_start)
    {
        return;
    }

    SetRate(pitch, f);
    SetRange(start, end, loop, f);
    StartFifo(fifos_[size_t(f)].next_start, f);
}

void Sequencer::SetRate(uint16_t pitch, int f)
{
    Fifo& fifo = fifos_[size_t(f)];
    if (!fifo.base_rate)
    {
        return;
    }

    fifo.rate = PlayRate(fifo.base_rate, pitch);
    fifo.end = fifo.length;
}

void Sequencer::SetRange(int start, int end, int loop, int f)
{
    // E7 plays the part of the sample from `start`% to `end`%, then loops back to `loop`% unless that's the end.
    Fifo& fifo = fifos_[size_t(f)];
    if (start == 0 && end == 0 && loop == 0)
    {
        fifo.loop = 0;
        fifo.position = 0;
        return;
    }

    const uint32_t length = fifo.end;
    const uint32_t from = DivideUnsigned(length * uint32_t(start), 100);
    const uint32_t to = DivideUnsigned(length * uint32_t(end), 100);
    const uint32_t back = DivideUnsigned(length * uint32_t(loop), 100);
    fifo.position = from * 60;
    fifo.loop_position = back * 60;
    fifo.end = to;
    fifo.next_start = fifo.sample + from;
    fifo.loop = loop != end ? fifo.sample + back : 0;
}

void Sequencer::StartFifo(uint32_t data, int f)
{
    // Reset the FIFO, fill it with the first 32 bytes, and point its DMA at the rest. The J revision clears the DMA's
    // count and control together.
    const FifoRegisters& registers = kFifoRegisters[size_t(f)];
    Write(kImageRegisters[kSoundcntH], 2, soundcnt_h_ | registers.reset);
    Write(registers.dma_source, 4, data + 32);
    Write(registers.dma_dest, 4, registers.fifo);
    Write(registers.dma_count, traits_.dma_clear_size, 0);
    Write(registers.dma_control, 2, 0xB640);
    SetTimer(f, TimerSetting(fifos_[size_t(f)].rate));
}

void Sequencer::StopFifo(int f)
{
    // DC or DD on a PSG channel gives a FIFO number below 0. Any number but 0 stops FIFO B.
    const size_t fifo = f == 0 ? 0 : 1;
    Write(kFifoRegisters[fifo].timer, 4, 0);
    Write(kFifoRegisters[fifo].dma_control, 2, dma_control_[fifo] & 0x7FFF);
    fifos_[fifo].position = ~0u;
    pending_timers_[fifo] = 0;
}

void Sequencer::BendFifo(uint16_t pitch, int f)
{
    Fifo& fifo = fifos_[size_t(f)];
    fifo.rate = PlayRate(fifo.base_rate, pitch);
    SetTimer(f, TimerSetting(fifo.rate));
}

void Sequencer::SetTimer(int f, uint32_t value)
{
    // The J revision writes the timer at the end of the frame.
    if (traits_.timers_at_frame_end)
    {
        pending_timers_[size_t(f)] = value;
    }
    else
    {
        Write(kFifoRegisters[size_t(f)].timer, 4, value);
    }
}

void Sequencer::NextRandom()
{
    random_seed_ = random_seed_ * 0x41C64E6D + 0x3039;
    random_value_ = uint8_t((random_seed_ >> 16) & 0x7F);
}

void Sequencer::Reseed()
{
    random_seed_ = 1;
    random_value_ = 0;
    random_high_ = 0;
}

void Sequencer::Write(uint32_t address, int size, uint32_t value)
{
    value &= size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
    writes_.push_back({address, uint8_t(size), value});

    // Remember what the registers that the driver reads back would return.
    if (address == kImageRegisters[kSoundcntH])
    {
        soundcnt_h_ = uint16_t(value & kSoundcntHRead);
    }
    else if (address == kSoundbias)
    {
        soundbias_ = uint16_t(value);
    }
    else if (address == kImageRegisters[kNr30])
    {
        nr30_ = uint8_t(value & kNr30Read);
    }
    else if (address == kFifoRegisters[0].dma_control || address == kFifoRegisters[1].dma_control)
    {
        dma_control_[address == kFifoRegisters[0].dma_control ? 0 : 1] = uint16_t(value);
    }

    // Keep the hardware's state for the conversion: the sound registers, the channels the writes restart, and the wave
    // RAM, whose writes go to the bank that isn't playing.
    for (size_t r = 0; r < hardware_.registers.size(); r++)
    {
        if (kImageRegisters[r] != address)
        {
            continue;
        }

        uint16_t& reg = hardware_.registers[r];
        reg = size == 1 ? uint16_t((reg & 0xFF00) | value) : uint16_t(value);
        for (int p = 0; p < 4; p++)
        {
            if (r == size_t(kPsgControl[p]) && (value & kRestart))
            {
                hardware_.restarted |= uint8_t(1 << p);
            }
        }
    }
    if (address >= kWaveRam && address < kWaveRam + 16)
    {
        const size_t bank = ((hardware_.registers[kNr30] >> 6) & 1) ^ 1;
        hardware_.wave_ram[bank][address - kWaveRam] = uint8_t(value);
    }
}

void Sequencer::AddEvent(int c, Event::Kind kind, int index)
{
    Event e;
    e.kind = kind;
    e.channel = uint8_t(c);
    e.index = index;
    e.tick = ticks_[size_t(c)];
    e.frame = frame_;
    events_.push_back(e);
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace agbsf2::quintet
