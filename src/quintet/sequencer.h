// SPDX-License-Identifier: MIT

// A model of the driver's per-frame routine for a song: its 6 channels, their effects and the sound registers it
// writes, frame by frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gba.h"
#include "quintet/driver.h"
#include "quintet/song.h"
#include "rom.h"

namespace agbsf2::quintet
{

// The frames that a channel's effects have run, in Channel::envelope_time, bend_time and lfo_time, after a rest: the
// effects wait for the next note.
constexpr uint16_t kAfterRest = 0xFFFF;

// The driver's record of a channel. The comments give each field's offset in the A revision's record. The J revision's
// has the two bank flags at 0x0C and 0x0D, and the fields from the transposes to the tempo 2 bytes on and the rest 4
// bytes on.
struct Channel
{
    uint32_t loop = 0;                         // 0x00: the byte FF goes back to, or 0 if it ends the channel
    uint32_t position = 0;                     // 0x04: the next byte to read, or 0 once the channel has ended
    uint8_t octave = 2;                        // 0x08
    uint8_t pcm_start = 0;                     // 0x09: E7's start, loop end and loop start, in % of a sample
    uint8_t pcm_end = 0;                       // 0x0A
    uint8_t pcm_loop = 0;                      // 0x0B
    int8_t transpose = 0;                      // 0x0C: E4, EE and EF, in semitones
    int8_t transpose2 = 0;                     // 0x0D: E9
    uint8_t volume = 0;                        // 0x0E: 0-15, 0-3 on the wave channel (0-4 in J), 0-1 on a PCM channel
    uint16_t pitch_from = 0x3FF;               // 0x10: the pitch a note starts at, before any slide
    uint16_t pitch = 0x3FF;                    // 0x12: the note's pitch
    uint16_t pitch_out = 0;                    // 0x14: the pitch with the frame's effects
    uint16_t detune = 0;                       // 0x16: D5 and E6, a signed value
    uint16_t countdown = 0;                    // 0x18: frames to the next note, a signed value
    uint16_t tempo = 90;                       // 0x1A
    uint32_t ticks = 0;                        // 0x1C: ticks played since the start or the last loop or tempo
    uint32_t frames = 0;                       // 0x20: the frames those ticks take
    uint8_t switch_value = 0;                  // 0x24: DE's duty or wave bank at the start of a note
    uint8_t switch_to = 0;                     // 0x25: the duty or bank it switches to
    uint8_t switch_delay = 0;                  // 0x26: frames before it switches, or 0 for no switch
    uint8_t switch_count = 0;                  // 0x27
    uint8_t sweep_shift = 0;                   // 0x28: NR10
    uint8_t sweep_down = 1;                    // 0x29
    uint8_t sweep_time = 0;                    // 0x2A
    uint8_t length = 0x3F;                     // 0x2B: the sound length setting
    uint8_t duty = 2;                          // 0x2C
    uint8_t envelope_step = 0;                 // 0x2D: the hardware envelope's step time
    uint8_t envelope_up = 1;                   // 0x2E
    uint8_t level = 0;                         // 0x2F: the volume of the note playing, 0 for a rest
    uint8_t sounding = 1;                      // 0x30: 0 after a rest
    uint8_t noise_shift = 0;                   // 0x31: NR43
    uint8_t noise_width = 0;                   // 0x32
    uint8_t noise_ratio = 0;                   // 0x33
    uint8_t wave_size = 0;                     // 0x34: NR30
    uint8_t wave_bank = 0;                     // 0x35
    uint8_t wave_on = 1;                       // 0x36
    uint8_t wave_75 = 0;                       // 0x37: NR32's 75% volume bit
    uint8_t wave = 0;                          // 0x38: the wave in bank 0, or on a PCM channel the sample
    uint8_t wave_high = 0x80;                  // 0x39: the wave in bank 1, with bit 7 set
    uint8_t length_on = 0;                     // 0x3A: the sound length is enabled
    uint8_t envelope_on = 0;                   // 0x3B: D2's volume envelope
    uint16_t envelope_time = 0;                // 0x3C: frames since the note started, kAfterRest after a rest
    std::array<uint8_t, 5> envelope = {};      // 0x3E: start, peak and end levels in %, attack and decay frames
    uint8_t bend_on = 0;                       // 0x43: E2's pitch envelope
    uint16_t bend_time = 0;                    // 0x44
    std::array<int8_t, 5> bend = {};           // 0x46: start, middle and end offsets, then the two times
    uint8_t lfo_on = 0;                        // 0x4B: DF's LFO
    uint8_t lfo_shape = 0;                     // 0x4C: EC and ED; bit 7 inverts it
    uint16_t lfo_time = 0;                     // 0x4E: kAfterRest after a rest
    uint16_t lfo_delay = 0;                    // 0x50
    uint16_t lfo_period = 0;                   // 0x52
    uint16_t lfo_depth = 0;                    // 0x54
    uint8_t slide_next = 0;                    // 0x56: D4, so the next note slides from the last
    uint8_t sliding = 0;                       // 0x57
    uint16_t slide_frames = 2;                 // 0x58: E5
    uint16_t slide_time = 0;                   // 0x5A
    std::array<uint32_t, 8> repeat_start = {}; // 0x5C: D6's positions
    std::array<uint8_t, 8> repeat_count = {};  // 0x7C: D7's counters
    uint8_t repeat_depth = 0;                  // 0x84
    uint32_t macro = 0;                        // 0x88: the noise macro's next byte, or 0
    uint32_t macro_start = 0;                  // 0x8C: DB's macro, which each note starts
    uint8_t macro_hold = 0;                    // 0x90: frames to hold each setting, less 1
    uint8_t macro_count = 0;                   // 0x91
    uint8_t sample_bank = 0;                   // the J revision's bank of PCM samples, 0 for music
    uint8_t macro_bank = 0;                    // its bank of noise macros
};

// The driver's record of a FIFO's sample.
struct Fifo
{
    uint32_t next_start = 0;    // 0x00: the byte the next start plays from
    uint32_t sample = 0;        // 0x04: the sample's data
    uint32_t loop = 0;          // 0x08: the byte it loops back to, or 0 if it doesn't loop
    uint32_t end = 0;           // 0x0C: the bytes it plays before it stops (or loops)
    uint32_t length = 0;        // 0x10: the sample's bytes, less 64, less 0.6%
    uint32_t base_rate = 0;     // 0x14: the sample's rate in Hz
    uint32_t rate = 0;          // 0x18: the rate it plays at
    uint32_t position = ~0u;    // 0x1C: the bytes played × 60, or 0xFFFFFFFF while it's stopped
    uint32_t loop_position = 0; // 0x20: that count at the loop start
};

// A musical event on a channel, which the conversion follows.
struct Event
{
    enum Kind : uint8_t
    {
        kNote,      // a note at `index`
        kPlayAgain, // note 12: the channel plays its pitch again, or a PCM channel its sample at the sample's rate
        kRest,      // note 13
        kLoopPoint, // BF
        kLoop,      // FF, back to the loop point
        kEnd,       // FF without a loop point: the channel has ended
        kHold,      // FF at its loop point: the channel reads nothing more, and its note goes on
        kTempo,     // 7F
    };

    Kind kind = kNote;
    uint8_t channel = 0;
    int index = 0;      // a note's index into the pitch table: octave × 12 + pitch + transposes
    uint64_t tick = 0;  // the channel's ticks since the song started, counting every pass through a loop
    uint32_t frame = 0; // the frame it happened in
};

// The sound registers that the driver's register image holds, in order: SOUND1CNT_L to SOUNDCNT_H.
inline constexpr uint32_t kImageRegisters[12] = {0x04000060, 0x04000062, 0x04000064, 0x04000068,
                                                 0x0400006C, 0x04000070, 0x04000072, 0x04000074,
                                                 0x04000078, 0x0400007C, 0x04000080, 0x04000082};

// The indexes of the image's halfwords. Hardware::registers holds the halfwords in the same order.
constexpr int kNr10 = 0;       // SOUND1CNT_L
constexpr int kNr11 = 1;       // SOUND1CNT_H: NR11 and NR12
constexpr int kNr13 = 2;       // SOUND1CNT_X: NR13 and NR14
constexpr int kNr21 = 3;       // SOUND2CNT_L: NR21 and NR22
constexpr int kNr23 = 4;       // SOUND2CNT_H: NR23 and NR24
constexpr int kNr30 = 5;       // SOUND3CNT_L
constexpr int kNr31 = 6;       // SOUND3CNT_H: NR31 and NR32
constexpr int kNr33 = 7;       // SOUND3CNT_X: NR33 and NR34
constexpr int kNr41 = 8;       // SOUND4CNT_L: NR41 and NR42
constexpr int kNr43 = 9;       // SOUND4CNT_H: NR43 and NR44
constexpr int kNr50 = 10;      // SOUNDCNT_L: NR50 and NR51
constexpr int kSoundcntH = 11; // SOUNDCNT_H: the PSG's share of the mix, and the FIFOs' volumes and outputs

// Each PSG channel's halfwords: the one with its length and envelope (on the wave channel, its length and volume), and
// the one with its frequency (on the noise channel, its clock) and the bit that restarts it.
inline constexpr int kPsgEnvelope[4] = {kNr11, kNr21, kNr31, kNr41};
inline constexpr int kPsgControl[4] = {kNr13, kNr23, kNr33, kNr43};

// The bit of a PSG channel's control halfword that restarts the channel: bit 7 of NR14, NR24, NR34 or NR44.
constexpr uint16_t kRestart = 0x8000;

// The sound hardware after a frame, as the driver has left it.
struct Hardware
{
    std::array<uint16_t, 12> registers = {};              // the last values written to the registers of kImageRegisters
    uint8_t restarted = 0;                                // bit c: PSG channel c was restarted in the frame
    std::array<std::array<uint8_t, 16>, 2> wave_ram = {}; // the wave RAM's two banks
};

// Returns the CPU cycles between the samples of a FIFO that the driver plays at `rate` Hz, as its timer setting gives
// them.
uint32_t SampleCycles(uint32_t rate);

// Returns the frames that a FIFO at `rate` plays from `position`, the driver's count of its bytes × 60, before the
// driver's update at the end of a frame stops or loops it. The driver does that when the next frame would reach `end`.
uint32_t FifoFrames(uint32_t position, uint32_t rate, uint32_t end);

// Plays a song one frame at a time, as the driver's per-frame routine does.
class Sequencer
{
public:
    // Sets up `song` as the driver's play routine does after the game's sound init. `rom` and `info` have to outlive
    // the sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int song);

    // Returns true if the song's header was read.
    bool Valid() const
    {
        return valid_;
    }

    // Returns the register writes that starting the song made.
    const std::vector<RegisterWrite>& StartWrites() const
    {
        return start_writes_;
    }

    // Runs one frame of the driver's per-frame routine. Returns the register writes it made, in order.
    const std::vector<RegisterWrite>& Step();

    // Returns the channels' musical events in the last frame, in the order the driver ran them.
    const std::vector<Event>& Events() const
    {
        return events_;
    }

    // Returns the sound hardware's state after the last frame.
    const Hardware& GetHardware() const
    {
        return hardware_;
    }

    // Returns true once every channel has ended.
    bool Ended() const;

    const Channel& GetChannel(int c) const
    {
        return channels_[size_t(c)];
    }

    const Fifo& GetFifo(int f) const
    {
        return fifos_[size_t(f)];
    }

    // Returns problems found in the song's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // The driver's routines, each named after what it does.
    void ResetChannel(Channel& ch);
    void ResetPsg();
    void ReadNotes(int c);
    void RunCommand(int c, uint8_t command);
    void ChannelEffects(int c);
    void PcmEffects(int c);
    void VolumeEnvelope(int c);
    void PitchEnvelope(int c);
    void Lfo(int c);
    void AddPitch(int c, int16_t offset);
    void Slide(int c);
    void NoiseMacro(int c);
    void Switch(int c);
    void UpdateImage(int c);
    void Output();
    void UpdateFifos();
    uint16_t PsgFrequency(const Channel& ch, int note) const;
    uint16_t PcmPitch(const Channel& ch, int note) const;
    void LoadWave(int c, uint8_t wave);
    void SelectMacro(Channel& ch, uint8_t macro);
    void SelectSample(int c, uint8_t sample);
    uint32_t SampleAddress(int bank, uint8_t sample);
    void PcmNoteOn(int c);
    void LoadSample(int f, uint32_t sample);
    void PlaySample(uint16_t pitch, int start, int end, int loop, int f);
    void SetRate(uint16_t pitch, int f);
    void SetRange(int start, int end, int loop, int f);
    void StartFifo(uint32_t data, int f);
    void StopFifo(int f);
    void BendFifo(uint16_t pitch, int f);
    void SetTimer(int f, uint32_t value);
    void NextRandom();
    void Reseed();

    // Follows FF on channel `c`: back to the loop point, a hold at the loop point itself, or the channel's end if it
    // has no loop point. Returns false if the channel reads nothing more in this frame.
    bool FollowEnd(int c);

    // Starts note `note` on channel `c`: the pitch and the level, and on a PCM channel the sample's start or stop. Note
    // 12 plays again. Note 13 rests. A note after D4 slides from the note before.
    void StartNote(int c, int note);

    // Returns the ticks that a note of length index `length` lasts with its dot and the notes that D4 ties to it. Moves
    // the channel past the dot and the ties.
    uint16_t NoteTicks(Channel& ch, int note, int length);

    // Writes a hardware register, as the driver does.
    void Write(uint32_t address, int size, uint32_t value);

    // Adds an event of channel `c` at its tick count.
    void AddEvent(int c, Event::Kind kind, int index = 0);

    void Warn(const std::string& message);

    const Rom& rom_;
    const DriverInfo& info_;
    const RevisionTraits& traits_;
    bool valid_ = false;
    std::array<Channel, kChannels> channels_;
    std::array<Fifo, 2> fifos_;
    std::array<uint16_t, 12> image_ = {};  // the registers the channels set, from SOUND1CNT_L to SOUNDCNT_H
    std::array<uint16_t, 12> shadow_ = {}; // the values last written to those registers
    std::array<uint8_t, 2> loaded_waves_ = {};
    std::array<uint8_t, 2> loaded_samples_ = {};
    uint32_t random_seed_ = 1;
    uint8_t random_value_ = 0;
    uint8_t random_high_ = 0;
    uint16_t soundcnt_h_ = 0; // the hardware's SOUNDCNT_H, as it reads
    uint16_t soundbias_ = 0x0200;
    uint8_t nr30_ = 0;      // the hardware's NR30, as it reads
    uint8_t wave_code_ = 0; // the wave channel's last NR32 volume code
    std::array<uint16_t, 2> dma_control_ = {};
    std::array<uint32_t, 2> pending_timers_ = {}; // the J revision's timer settings for the end of the frame
    std::vector<RegisterWrite> start_writes_;
    std::vector<RegisterWrite> writes_;
    std::array<uint64_t, kChannels> ticks_ = {}; // each channel's ticks since the song started
    std::array<bool, kChannels> held_ = {};      // the channels that have stopped at FF on their loop point
    uint32_t frame_ = 0;
    std::vector<Event> events_;
    Hardware hardware_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::quintet
