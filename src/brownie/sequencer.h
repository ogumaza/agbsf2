// SPDX-License-Identifier: MIT

// A model of the driver's per-frame routine and of how its samples play, frame by frame: its sound channels, the sound
// registers it writes and its fades, and in the Sword of Mana revision its mixer's progress through each of its 5
// voices' samples, or in the Magical Vacation revision the FIFOs' samples and the timers that play them.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "brownie/driver.h"
#include "gba.h"
#include "rom.h"

namespace agbsf2::brownie
{

// The steps of the Magical Vacation revision's rate table for each semitone.
constexpr int kRateSteps = 12;

// The points of each FIFO that the mixer makes each time it runs.
constexpr int kMixPoints = 16;

// The sound registers that Sequencer::Registers() returns: from SOUND1CNT_L up to the end of the wave RAM.
constexpr uint32_t kIoNr10 = 0x04000060;      // SOUND1CNT_L: square 1's sweep, and the first of the sound registers
constexpr uint32_t kIoNr30 = 0x04000070;      // SOUND3CNT_L: the wave channel's bank and on bit
constexpr uint32_t kIoSoundcntL = 0x04000080; // NR50 and NR51
constexpr uint32_t kIoSoundcntH = 0x04000082; // the FIFOs' and the PSG's mix
constexpr uint32_t kIoNr52 = 0x04000084;      // SOUNDCNT_X: the sound's on bit
constexpr uint32_t kIoWaveRam = 0x04000090;
constexpr uint32_t kIoSoundEnd = 0x040000A0; // just past the wave RAM

// Each PSG channel's NRx1 and NRx2, and its NRx3 and NRx4. NRx3 and NRx4 hold the channel's frequency setting.
inline constexpr uint32_t kIoControlRegisters[4] = {0x04000062, 0x04000068, 0x04000072, 0x04000078};
inline constexpr uint32_t kIoFrequencyRegisters[4] = {0x04000064, 0x0400006C, 0x04000074, 0x0400007C};

// The bits of a channel's flags.
constexpr uint8_t kFlagOn = 0x01;     // the channel is in use
constexpr uint8_t kFlagEffect = 0x02; // a sound effect has the music channel's PSG channel
constexpr uint8_t kFlagPaused = 0x80; // the channel was in use when a fade out stopped everything

// The driver's record of a sound channel: 0x38 bytes, with each field's offset. The fields have a PSG channel's names.
// A sample channel uses some of them for other things. The accessors give those uses their names.
struct Channel
{
    // A sample channel's 0x24: the frames to the level's next change.
    uint8_t RampWait() const
    {
        return duty;
    }

    void SetRampWait(uint8_t value)
    {
        duty = value;
    }

    // A sample channel's 0x25: its volume with the envelope's level.
    uint8_t EnvelopeVolume() const
    {
        return sweep;
    }

    void SetEnvelopeVolume(uint8_t value)
    {
        sweep = value;
    }

    // A sample channel's 0x2A and 0x2B: its right and left levels.
    uint8_t RightLevel() const
    {
        return pan_on;
    }

    void SetRightLevel(uint8_t value)
    {
        pan_on = value;
    }

    uint8_t LeftLevel() const
    {
        return pan_off;
    }

    void SetLeftLevel(uint8_t value)
    {
        pan_off = value;
    }

    // In the Magical Vacation revision, a sample channel's 0x14, 0x1C and 0x20: its sample's start and loop, and the
    // reload value of the timer that plays its note.
    uint32_t SampleStart() const
    {
        return samples;
    }

    void SetSampleStart(uint32_t value)
    {
        samples = value;
    }

    uint32_t SampleLoop() const
    {
        return wave;
    }

    void SetSampleLoop(uint32_t value)
    {
        wave = value;
    }

    uint16_t TimerReload() const
    {
        return frequency;
    }

    void SetTimerReload(uint16_t value)
    {
        frequency = value;
    }

    uint8_t flags = 0;        // 0x00
    uint8_t wait = 0;         // 0x01: frames to the next read of the channel's data
    uint8_t step_frames = 0;  // 0x02: frames to the envelope's next step, or 0 for none
    uint8_t loop_count = 0;   // 0x03: F4's count
    uint32_t position = 0;    // 0x04: the next byte of the channel's data
    uint32_t loop_start = 0;  // 0x08: where F5 goes back to
    uint32_t return_to = 0;   // 0x0C: where F7 returns to
    uint32_t subroutines = 0; // 0x10: the places F6 calls and F8 jumps to
    uint32_t samples = 0;     // 0x14: E1's sample set, or in the Magical Vacation revision its sample's start
    uint32_t sample_end = 0;  // 0x18: the Magical Vacation revision: E1's sample's end
    uint32_t wave = 0;        // 0x1C: FC's wave, or in the Magical Vacation revision a sample channel's loop
    uint16_t frequency = 0;   // 0x20: a PSG channel's frequency setting, for noise NR43, or in the Magical Vacation
                              // revision a sample channel's timer reload value
    uint16_t control = 0;     // 0x22: NRx1 in the low byte and NRx2 in the high, as last set
    uint8_t duty = 0;         // 0x24: F1's NRx1, or on a sample channel the frames to the level's next change
    uint8_t sweep = 0;        // 0x25: FA's NR10, or on a sample channel its volume with the envelope
    uint8_t volume = 0;       // 0x26: F2's
    uint8_t detune = 0;       // 0x27: F9's, added to the frequency setting
    uint8_t level = 0;        // 0x28: the envelope's level
    uint8_t length = 0;       // 0x29: the frames that each note or rest lasts
    uint8_t pan_on = 0;       // 0x2A: the PSG channel's bits of NR51, or a sample channel's right level
    uint8_t pan_off = 0;      // 0x2B: the bits of NR51 the channel clears, or a sample channel's left level
    uint8_t transpose = 0;    // 0x2C: F3's, a signed value
    uint8_t ramp = 0;         // 0x2D: a sample channel's change in level each step, down with bit 7 set, or in the
                              // Magical Vacation revision bit 3
    uint8_t ramp_frames = 0;  // 0x2E: the frames between those changes
    uint8_t length_set = 0;   // 0x2F: EE's set of lengths
    uint32_t envelope = 0;    // 0x30: F0's envelope, which each note starts
    uint32_t step = 0;        // 0x34: the envelope's next step
};

// The mixer's record of a voice: 16 bytes.
struct Voice
{
    uint8_t mode = 0;   // 0x00: bit 0 plays each point twice, bit 1 four times, bit 2 eight times
    uint8_t mode2 = 0;  // 0x01: a copy of the mode
    uint8_t right = 0;  // 0x02
    uint8_t left = 0;   // 0x03
    uint32_t point = 0; // 0x04: the next point to play
    uint32_t end = 0;   // 0x08: the end of the sample
    uint32_t loop = 0;  // 0x0C: where the sample goes back to at its end, or the end if it doesn't loop
};

// The keys of each octave of a sample set: the mixer plays the first 12 keys' samples at an eighth of its rate, the
// next 12 at a quarter and the 12 after them at half, and the rest at its rate.
constexpr int kOctaveKeys = 12;
constexpr int kSampleOctaves = 4;

// Returns the key that a sample note plays: keys from 64 play key 47.
inline int SampleKey(int key)
{
    return (key & 0xC0) ? 47 : key;
}

// Returns the octave of a sample note's key, 0-3, which the mixer plays at an eighth of its rate up to its full rate.
inline int SampleOctave(int key)
{
    return std::min(key / kOctaveKeys, kSampleOctaves - 1);
}

// A FIFO's sample in the Magical Vacation revision, as the driver keeps it: 16 bytes.
struct FifoVoice
{
    uint32_t point = 0;  // 0x00: the next 4 points to give the FIFO
    uint32_t end = 0;    // 0x04: the end of the sample
    uint32_t loop = 0;   // 0x08: where the sample goes back to at its end, or the end if it doesn't loop
    uint32_t volume = 0; // 0x0C: the interrupt scales each point by (volume + 1) / 256
};

// A FIFO and the timer that plays it, in the Magical Vacation revision.
struct Fifo
{
    bool running = false; // the timer runs
    uint16_t reload = 0;  // the timer's reload value, which sets the rate
    uint64_t next = 0;    // the CPU cycle of the timer's next overflow
    int count = 0;        // the points the FIFO holds
    bool dma = false;     // the FIFO's DMA is on, so that its requests for more points interrupt
    int owner = -1;       // the channel whose note last started the timer
};

// The driver's other variables, kept as the driver keeps them, since some of its stores span several of them. In the
// Sword of Mana revision, they're 48 bytes from the request queue on. In the Magical Vacation revision, they're the 12
// bytes of the queue, and then the 32 from SOUNDCNT_L's copy on, which comes after the FIFOs' samples.
using Globals = std::array<uint8_t, 48>;

// The offsets in Globals of the variables the driver uses.
constexpr int kQueueRead = 0x00;     // the queue's next request to start, in bytes
constexpr int kQueueWrite = 0x01;    // where the queue's next request goes
constexpr int kQueue = 0x02;         // 8 requests of a halfword, 0 for none
constexpr int kMixerMode = 0x12;     // E2's: 0 mixes voices 0 and 1, and anything else all 5
constexpr int kMaster = 0x14;        // the master level, 0-255
constexpr int kFadeStep = 0x15;      // the master level's change at each step of a fade
constexpr int kFadeOutFrames = 0x16; // a halfword: the frames left of a fade out, which stops everything at its end
constexpr int kFadeOutTimer = 0x18;  // a halfword: frames to its next step, in 256ths
constexpr int kFadeOutPeriod = 0x1A; // a halfword: the frames between its steps, in 256ths
constexpr int kFadeInTimer = 0x1C;   // a halfword, as for the fade out
constexpr int kFadeInPeriod = 0x1E;  // a halfword
constexpr int kSoundcntLCopy = 0x20; // a halfword: SOUNDCNT_L as the driver last set it
constexpr int kFadeInFrames = 0x2C;  // the frames left of a fade in

// The Magical Vacation revision's variables, where they differ: the queue is the same, but it holds 4 requests.
constexpr int kVacationSoundcntLCopy = 0x0C;
constexpr int kVacationMaster = 0x20;
constexpr int kVacationGlobals = 44; // the bytes of Globals that the revision has

// A musical event on a channel, which the conversion follows.
struct Event
{
    enum Kind : uint8_t
    {
        kNote,    // a note starts
        kRest,    // EC or ED
        kRelease, // E0: the envelope goes on from the release envelope
        kHold,    // FE: the note goes on
        kLoop,    // F8 jumps back to a place the channel has read before
        kEnd,     // FF
    };

    Kind kind = kNote;
    uint8_t channel = 0;
    uint8_t key = 0;      // kNote: the key after the transpose, or a noise note's byte
    uint32_t frame = 0;   // the frame it happened in
    uint32_t first = 0;   // kLoop: the frame in which the channel first read the place it goes back to
    uint32_t samples = 0; // kNote on a sample channel: the channel's sample set, or in the Magical Vacation revision
                          // its sample's start
};

// Plays a sound one frame at a time, as the driver's per-frame routine and its mixer do.
class Sequencer
{
public:
    // Sets up the driver as its init leaves it, and queues `song`, which it starts at the end of the first frame. `rom`
    // and `info` have to outlive the sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int song);

    // Returns the record of a channel as the driver keeps it, for traces.
    static std::array<uint8_t, 0x38> RecordBytes(const Channel& c);

    // Returns the record of a voice as the mixer keeps it, for traces.
    static std::array<uint8_t, 0x10> VoiceBytes(const Voice& v);

    // Returns the number of channels that the revision's per-frame routine runs.
    int ChannelCount() const
    {
        return layout_.channels;
    }

    // Returns the number of the revision's voices: the mixer's, or the FIFOs'.
    int VoiceCount() const
    {
        return layout_.voices;
    }

    // Returns voice `v` as the driver keeps it, for traces: a mixer voice, or a FIFO's sample.
    std::array<uint8_t, 0x10> VoiceRecord(int v) const;

    // Returns the number of bytes of Globals that the revision has.
    size_t GlobalsSize() const
    {
        return vacation_ ? kVacationGlobals : sizeof(Globals);
    }

    // Returns true if the song's entry was read.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one frame of the per-frame routine, and then until the next frame, the mixer once for each 16 points the
    // FIFOs play, or in the Magical Vacation revision, the interrupts that give the FIFOs their samples' points.
    // Returns the frame's writes to the sound registers, in order.
    const std::vector<RegisterWrite>& Step();

    // Returns the channels' musical events in the last frame, in the order the driver ran them.
    const std::vector<Event>& Events() const
    {
        return events_;
    }

    // Returns true while a channel is in use, or the mixer or a FIFO's timer runs.
    bool Playing() const;

    // Returns true if the mixer ran in the last frame.
    bool Mixing() const
    {
        return mix_start_ >= 0;
    }

    const Channel& GetChannel(int c) const
    {
        return channels_[size_t(c)];
    }

    const Voice& GetVoice(int v) const
    {
        return voices_[size_t(v)];
    }

    // Returns the voices as the last frame's routine left them, which the mixer then played.
    const std::array<Voice, kVoiceCount>& VoicesBeforeMix() const
    {
        return voices_before_mix_;
    }

    // Returns the FIFOs and their samples as the last frame's routine left them, before the interrupts that gave the
    // FIFOs more points, in the Magical Vacation revision.
    const std::array<Fifo, kFifoCount>& FifosBeforeInterrupts() const
    {
        return fifos_before_;
    }

    const std::array<FifoVoice, kFifoCount>& FifoVoicesBeforeInterrupts() const
    {
        return fifo_voices_before_;
    }

    const Globals& GetGlobals() const
    {
        return globals_;
    }

    // Returns the set of lengths that channel `c` last took a length from: EE's, or in the Magical Vacation revision,
    // which has no EE, the length command's group of 16.
    uint8_t LengthSet(int c) const
    {
        return vacation_ ? length_groups_[size_t(c)] : channels_[size_t(c)].length_set;
    }

    // Returns the sound registers from SOUND1CNT_L to the wave RAM's end, as last written: 0x40 bytes.
    const std::array<uint8_t, 0x40>& Registers() const
    {
        return registers_;
    }

    // Returns the PSG channels that the last frame started again (bit c for channel c).
    uint8_t Restarted() const
    {
        return restarted_;
    }

    // Returns problems found in the sound's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // The per-frame routine's parts, in the order it runs them.
    void RunChannels();
    void ReadChannel(int c);
    bool RunCommand(int c, uint8_t command, uint32_t& at);
    void PsgNote(int c, uint32_t frequency);
    void SampleNote(int c, int key);
    void FifoNote(int c, int key);
    void PsgEnvelopeStep(int c);
    void SampleEnvelopeStep(int c);
    void FifoEnvelopeStep(int c);
    void TakeSampleStep(int c, uint32_t at);
    void TakeFifoStep(int c, uint32_t at);
    void RampStep(int c);
    void FifoRampStep(int c);
    void SetVoiceLevels(int c);
    void EndChannel(int c);
    void LoadWave(uint32_t wave);
    void Resume();
    void Fade();
    void FadeStep(int timer, int period, bool up);
    void StopAll();
    void Requests();
    void StartSound(int sound);
    void Mix();
    void RunFifos();
    void Interrupt(int x);

    // Queues a sound, as the driver's request routine does.
    void Request(int sound);

    // Returns the offset in Globals of the driver's copy of SOUNDCNT_L.
    int SoundcntLCopy() const
    {
        return vacation_ ? kVacationSoundcntLCopy : kSoundcntLCopy;
    }

    // Returns the CPU cycles between overflows of the timer that plays FIFO x.
    uint64_t Period(int x) const
    {
        return 0x10000 - uint64_t(fifos_[size_t(x)].reload);
    }

    uint8_t Get8(int offset) const;
    uint16_t Get16(int offset) const;
    void Set8(int offset, uint32_t value);
    void Set16(int offset, uint32_t value);
    void Set32(int offset, uint32_t value);

    // Writes a hardware register as the driver does. The sound registers are kept and listed. The DMA's and Timer 0's
    // control registers decide when the mixer runs, and the timers, the DMA and the FIFOs decide when the Magical
    // Vacation revision's interrupts give the FIFOs more points. Writes anywhere else, such as the PSG rest's on a
    // sample channel past the end of the driver's table of registers, change nothing the model follows.
    void Write(uint32_t address, int size, uint32_t value);

    // Returns the address of a PSG channel's NRx1 and NRx2 from the driver's table, as the driver reads it. A sample
    // channel reads past the table's end.
    uint32_t ControlRegister(int c) const;

    // Returns the address of a PSG channel's NRx3 and NRx4, with the frequency setting, in the same way.
    uint32_t FrequencyRegister(int c) const;

    // Notes that channel `c` read the command at `address`, in this frame if it hasn't before.
    void Visit(int c, uint32_t address);

    void AddEvent(int c, Event::Kind kind, uint8_t key = 0);
    void Warn(const std::string& message);

    const Rom& rom_;
    const DriverInfo& info_;
    const RevisionLayout& layout_;
    const bool vacation_; // the Magical Vacation revision
    bool valid_ = false;
    std::array<Channel, kRecordCount> channels_ = {};
    std::array<Voice, kVoiceCount> voices_ = {};
    std::array<Voice, kVoiceCount> voices_before_mix_ = {};
    std::array<FifoVoice, kFifoCount> fifo_voices_ = {};
    std::array<FifoVoice, kFifoCount> fifo_voices_before_ = {};
    std::array<Fifo, kFifoCount> fifos_ = {};
    std::array<Fifo, kFifoCount> fifos_before_ = {};
    std::array<uint8_t, kChannelCount> length_groups_ = {}; // the Magical Vacation revision's, for LengthSet()
    Globals globals_ = {};
    std::array<uint8_t, 0x40> registers_ = {};                        // from SOUND1CNT_L
    std::array<std::map<uint32_t, uint32_t>, kChannelCount> visited_; // each command a channel read, and its frame

    // The hardware the driver sets up for the FIFOs.
    uint16_t dma_control_ = 0;   // DMA1CNT_H
    uint16_t timer_control_ = 0; // TM0CNT_H
    uint16_t interrupts_ = 0;    // IE
    uint64_t mix_cycles_ = 0;    // CPU cycles between the mixer's runs
    uint64_t now_ = 0;           // the CPU cycle that the driver's code runs at

    // The per-frame routine's r5, which the PSG rest writes to the frequency setting. Each frame starts with 0.
    uint32_t r5_ = 0;

    bool returned_ = false; // E3 has ended the frame's routine
    uint8_t restarted_ = 0;
    uint32_t frame_ = 0;
    int64_t mix_start_ = -1; // the frame the mixer started to run in, or -1 while it doesn't run
    uint64_t mixed_ = 0;     // the mixer's runs since then
    std::vector<RegisterWrite> writes_;
    std::vector<Event> events_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::brownie
