// SPDX-License-Identifier: MIT

// A model of the driver's per-frame routine: its tracks, channels, note slots, envelopes and mixer, frame by frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rare/driver.h"
#include "rare/song.h"
#include "rom.h"

namespace agbsf2::rare
{

// The number of MIDI channels that the tracks play on.
constexpr int kChannels = 16;

// The states of a note slot.
constexpr uint8_t kSlotFree = 0x10;
constexpr uint8_t kSlotOn = 0x11;
constexpr uint8_t kSlotReleased = 0x12;

// The phases of a slot's envelope.
constexpr uint8_t kPhaseAttackStart = 0;  // sets up the attack
constexpr uint8_t kPhaseAttack = 1;       // rises to the full level
constexpr uint8_t kPhaseDecayStart = 2;   // sets up the decay
constexpr uint8_t kPhaseDecay = 3;        // falls to the sustain level
constexpr uint8_t kPhaseSustain = 4;      // holds the sustain level
constexpr uint8_t kPhaseReleaseStart = 5; // sets up the release
constexpr uint8_t kPhaseRelease = 6;      // falls to 0

// An envelope's full level.
constexpr int32_t kFullLevel = 0x80000;

// The full level in the units of SustainLevel(): kFullLevel / 16. The driver works out the attack's and the decay's
// steps in these units with its 16-bit division, then multiplies them by 16.
constexpr uint32_t kFullSustain = 0x8000;

// The level below which a decay stops a note.
constexpr int32_t kAudibleLevel = 0x1000;

// Returns the number of steps of a note's attack. The attack reaches the full level after `steps` + 1 frames. A
// channel's `setting` from controller 20 gives the steps. A setting from 0x80 up leaves the instrument's `attack`, in
// which values above 99 count as 99.
uint32_t AttackSteps(uint32_t attack, uint8_t setting);

// Returns a note's sustain level, from 0 to kFullSustain. A channel's `setting` from controller 22 gives it in 1/128ths
// of the full level. A setting from 0x80 up leaves the instrument's `sustain` of 0-99: sustain / 99 of the full level,
// worked out in 8.8 fixed point with the driver's 16-bit division.
uint32_t SustainLevel(uint32_t sustain, uint8_t setting);

// The song time that the driver counts for each frame, in 1/256 microseconds: 1/60 s, as it works it out.
constexpr int64_t kFrameTime = 4266666;

// One semitone in the model's pitch offsets. They count semitones in 32.32 fixed point.
constexpr int64_t kSemitone = int64_t(1) << 32;

// One cent in the model's pitch offsets: kSemitone / 100, rounded.
constexpr int64_t kCent = 0x028F5C29;

// What the mixer did with a note slot's voice in the last frame.
enum class MixResult : uint8_t
{
    kIdle,    // the slot had no voice
    kMixed,   // the mixer took the voice
    kSkipped, // the mixer left the voice out, after taking as many voices as its limit allows
    kSilent,  // the mixer left the voice out, as its level was 0
};

// A note slot. Each channel has the same number, and a note plays in one until its envelope or its sample ends.
struct Slot
{
    uint8_t state = kSlotFree;
    uint8_t loop_mode = 0;
    uint8_t key = 0;       // the key that started it, which a note off matches
    uint8_t velocity = 0;  // the note's velocity + 1
    uint8_t phase = 0;     // the envelope's phase, such as kPhaseAttack
    uint8_t pitch_key = 0; // the key it plays at: the key, or a drum kit instrument's root key
    int8_t channel = 0;
    uint32_t step = 0;     // the sample step the mixer used this frame, in 1/2^23 bytes per output sample
    uint32_t position = 0; // the address of the sample byte it plays
    uint32_t fraction = 0; // 23 bits
    int32_t level = 0;     // the envelope's level
    int32_t level_step = 0;
    int32_t sustain = 0;
    uint32_t instrument = 0; // the instrument it plays
    MixResult mix = MixResult::kIdle;
};

// A channel's settings.
struct ChannelState
{
    uint32_t instrument = 0; // the instrument its last program change chose, or 0
    int program = -1;
    uint32_t bend = 0x2000;
    uint32_t modulation = 0; // controller 1
    uint32_t volume = 0x7F;  // controller 7
    uint32_t vibrato_phase = 0;
    bool mono = false;
    EnvelopeSettings envelope; // a program change sets them back
};

// Something the model did in a frame, for the MIDI conversion: a track's command, or the start of an envelope phase
// that took settings from controllers 20-23.
struct Action
{
    enum Kind : uint8_t
    {
        kNoteOn,
        kNoteDropped, // a note on that found no free slot
        kNoteOff,
        kProgram,
        kVolume,
        kBend,
        kTempo,
        kEnvelope, // a phase of a slot's envelope started with settings from controllers 20-23
    };

    Kind kind = kNoteOn;
    uint8_t track = 0;
    uint8_t channel = 0;
    uint8_t a = 0, b = 0;      // key and velocity, program, controller value
    uint32_t value = 0;        // tempo
    uint64_t tick = 0;         // the track's tick count when it happened, counting every pass through a loop
    uint32_t frame = 0;        // the frame it happened on
    int slot = -1;             // kNoteOn, kEnvelope: the slot the note plays in; kNoteOff: the slot it released, or -1
    bool playable = true;      // kNoteOn: false if its instrument has no sample for the key, so it makes no sound
    int program = -1;          // kNoteOn: the channel's program
    uint32_t instrument = 0;   // kNoteOn, kProgram: the channel's instrument
    EnvelopeSettings envelope; // kEnvelope: the settings the phase took, and 0xFF for the others
};

// Plays a tune one frame at a time, as the driver's per-frame routine does.
class Sequencer
{
public:
    // Sets up `tune` as the driver does when it starts a tune after its init. The game then holds the channels in
    // `silenced` at a level of 0 (see SilencedChannels()). `rom` and `info` have to outlive the sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int tune, uint16_t silenced = 0);

    // Returns true if the tune's header was read.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one frame of the driver's per-frame routine. Returns the frame's actions, in order.
    const std::vector<Action>& Step();

    // Returns true once every track has reached its end in the same frame, so that the driver stopped the tune.
    bool Ended() const
    {
        return ended_;
    }

    int TrackCount() const
    {
        return int(tracks_.size());
    }

    // Returns track `t`'s tick count, which counts every pass through a loop.
    uint64_t TrackTick(int t) const
    {
        return tracks_[size_t(t)].tick;
    }

    // Returns true once track `t` has reached its end, or a command it can't read.
    bool TrackDone(int t) const
    {
        return tracks_[size_t(t)].done;
    }

    // Returns the number of note slots: DriverInfo::slots_per_channel for each channel.
    int SlotCount() const
    {
        return int(slots_.size());
    }

    const Slot& GetSlot(int index) const
    {
        return slots_[size_t(index)];
    }

    const ChannelState& Channel(int channel) const
    {
        return channels_[size_t(channel)];
    }

    // Returns a channel's pitch offset from its bend and vibrato, in semitones as 32.32 fixed point.
    int64_t PitchOffset(int channel) const;

    // Returns problems found in the tune's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    struct Track
    {
        uint32_t position = 0;
        int64_t counter = 0; // song time to its next command, in 1/256 microseconds; it runs commands while negative
        uint32_t loop = 0;   // the position after its last loop start, or 0
        uint64_t tick = 0;
        bool done = false; // reached its end, or a command it can't read
    };

    // Runs the commands of track `t` that are due. Returns true if it's at its end.
    bool RunTrack(int t);

    // Returns an action of kind `kind` for event `e` of track `t`, at the track's tick.
    Action TrackAction(Action::Kind kind, int t, const Event& e) const;

    // Converts a delay of `ticks` to song time, as the driver does.
    int64_t DelayTime(uint32_t command, uint32_t ticks) const;

    void NoteOn(int t, const Event& e);
    void NoteOff(int t, const Event& e);
    void Control(int t, const Event& e, Track& track);
    void Program(int t, const Event& e);

    void UpdateVibrato();
    void UpdateEnvelope(int index);

    // Sets up the release of slot `index`, which plays `inst`, and takes its first frame's step.
    void StartRelease(int index, const Instrument& inst);

    // Adds a kEnvelope action for the phase that slot `index` starts, unless every setting in `used` leaves the
    // instrument's. The conversion reads these actions to give each note the settings that its phases took.
    void AddEnvelopeAction(int index, const EnvelopeSettings& used);

    void Mix();
    void MixVoice(Slot& slot);
    void Advance(Slot& slot);

    // Returns a voice's sample step: its pitch as the driver works it out, in 1/2^23 bytes per output sample.
    uint32_t PitchStep(const Slot& slot) const;

    void Warn(const std::string& message);

    const Rom& rom_;
    const DriverInfo& info_;
    uint16_t silenced_; // the channels at a level of 0
    bool valid_ = false;
    TuneHeader header_;
    std::vector<Track> tracks_;
    std::array<ChannelState, kChannels> channels_;
    std::vector<Slot> slots_;
    uint32_t tempo_ = 0;
    int64_t frame_time_ = 1; // the time that each frame takes off the tracks' counters, set by the first tempo
    uint32_t frame_ = 0;
    bool ended_ = false;
    std::vector<Action> actions_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::rare
