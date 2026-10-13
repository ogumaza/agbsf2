// SPDX-License-Identifier: MIT

// A model of the driver's per-frame routine: its tracks, channels, note slots, envelopes and mixer, frame by frame.

#pragma once

#include <array>
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

// An envelope's full level.
constexpr int32_t kFullLevel = 0x80000;

// The song time that the driver counts for each frame, in 1/256 microseconds: 1/60 s, as it works it out.
constexpr int64_t kFrameTime = 4266666;

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
    uint8_t phase = 0;     // the envelope's phase: 0 start, 1 attack, 2-3 decay, 4 sustain, 5-6 release
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

// A track action used by the MIDI conversion.
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
    // Sets up `tune` as the driver does when it starts a tune after its init. `rom` and `info` have to outlive the
    // sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int tune);

    // Returns true if the tune's header was read.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one frame of the driver's per-frame routine. Returns what the tracks did in it, in order.
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

    // Converts a delay of `ticks` to song time, as the driver does.
    int64_t DelayTime(uint32_t command, uint32_t ticks) const;

    void NoteOn(int t, const Event& e);
    void NoteOff(int t, const Event& e);
    void Control(int t, const Event& e, Track& track);
    void Program(int t, const Event& e);

    void UpdateVibrato();
    void UpdateEnvelope(int index);
    void Mix();
    void MixVoice(Slot& slot);
    void Advance(Slot& slot);

    // Returns a voice's sample step: its pitch as the driver works it out, in 1/2^23 bytes per output sample.
    uint32_t PitchStep(const Slot& slot) const;

    void Warn(const std::string& message);

    const Rom& rom_;
    const DriverInfo& info_;
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
