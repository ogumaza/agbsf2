// SPDX-License-Identifier: MIT

// A model of AlphaDream's sound driver that plays one song on one player, frame by frame, as the per-frame routine and
// the mixer do. docs/alphadream.md describes the driver.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "alphadream/driver.h"
#include "psg.h"
#include "rom.h"

namespace agbsf2::alphadream
{

// A track's flags, in the first halfword of its record.
namespace flag
{
constexpr uint16_t kUsed = 0x01;
constexpr uint16_t kMuted = 0x02; // a lower player holds the track's channel
constexpr uint16_t kRests = 0x20; // a rest's length follows each note
constexpr uint16_t kTie = 0x40;   // the note goes on into the next one
constexpr uint16_t kNote = 0x80;  // a note sounds
constexpr uint16_t kPitchLfo = 0x100;
constexpr uint16_t kPitchLfoOn = 0x200;
constexpr uint16_t kPitchLfoDown = 0x400;
constexpr uint16_t kLevelLfo = 0x800;
constexpr uint16_t kLevelLfoOn = 0x1000;
constexpr uint16_t kLevelLfoDown = 0x2000;
} // namespace flag

// A square LFO's settings and its count of frames.
struct Lfo
{
    uint8_t depth = 0;
    uint8_t period = 0;
    uint8_t delay = 0;
    uint8_t count = 0;
};

// A track's record of 32 bytes, as the driver keeps it.
struct TrackState
{
    uint16_t flags = 0;
    uint8_t tempo = 0;
    uint8_t count = 0; // the tempo's running count
    uint32_t data = 0; // the track's data in the ROM
    uint16_t at = 0;   // the next byte's offset from the data
    uint8_t wait = 0;  // ticks to the next event
    uint8_t key = 0;
    uint8_t instrument = 0;
    uint8_t volume = 0;
    uint8_t pan = 0;
    uint8_t bend_range = 0;
    int8_t bend = 0;
    int8_t fine = 0;
    uint8_t channel = 0;
    uint8_t player = 0;
    Lfo level_lfo;
    Lfo pitch_lfo;
    std::array<uint8_t, 4> loops{};

    // Returns the record's bytes as the driver keeps them.
    std::array<uint8_t, 32> Bytes() const;
};

// A sample channel's record of 16 bytes, at 0x03001B50 in A88E. The status is 0 when the channel is off, 0x80 for a
// note that the mixer hasn't started, 0x81 to 0x83 for the attack, the decay and the sustain, and the same with bit 6
// set for the release.
struct SampleChannel
{
    uint8_t status = 0;
    uint8_t level = 0;   // the envelope's level, 0-255
    uint16_t sample = 0; // the region's sample
    uint32_t position = 0;
    uint8_t right = 0; // the right side's volume, 0-127
    uint8_t left = 0;
    uint16_t pitch = 0; // in 256ths of a semitone, from key 0
    std::array<uint8_t, 4> envelope{};

    // Returns the record's bytes without the position, as driver_emu.py's trace gives them.
    std::array<uint8_t, 12> TraceBytes() const;
};

// A PSG channel's record of 12 bytes, from 0x4C8 in the driver's state, and its sides. The state is 0 when the channel
// is off, 1 and 2 for the attack, 3 for the decay, 4 for the sustain, and 5 and 6 for the release.
struct PsgChannel
{
    std::array<uint8_t, 4> envelope{};
    uint8_t volume = 0; // 0-15
    uint8_t count = 0;  // the frames since the driver last set the hardware's envelope, up to 255
    uint8_t duty = 0;   // the region's sample: the duty, or the wave pattern's number
    uint8_t state = 0;
    uint16_t frequency = 0; // the frequency setting, or the noise's NR43
    uint8_t written = 0;    // the envelope setting (NRx2) that the driver last kept
    bool left = true;
    bool right = true;

    // Returns the record's bytes as the driver keeps them, as driver_emu.py's trace gives them.
    std::array<uint8_t, 12> TraceBytes() const;
};

// The PSG's registers after a frame, as the driver writes them (0x0819AC84, 0x0819AF18).
struct PsgFrame
{
    std::array<uint16_t, 4> control{};   // NRx1 and NRx2: the duty, and the envelope, or the wave's NR31 and NR32
    std::array<uint16_t, 4> frequency{}; // NRx3 and NRx4, or the noise's NR43 and NR44
    uint8_t restarted = 0;               // a bit for each channel that the frame restarted
};

// A note as the model plays it: from the frame on which a channel starts it to the frame on which its track releases
// it, or another note or the song's end stops it. A note of a square or the noise lasts until the hardware's envelope
// falls silent instead.
struct Note
{
    int track = 0;
    uint32_t on = 0;
    uint32_t off = 0;
    uint32_t released = UINT32_MAX; // the frame on which the track lets the note go, or UINT32_MAX if it never does
    bool ended = false;
    int key = 0; // the key that the note starts on
    int instrument = 0;
    Region region;
};

// A setting of a track that changes how its note sounds, on the frame it changes.
struct Change
{
    int track = 0;
    uint32_t frame = 0;
    double pitch = 0; // semitones from the note's key
    double left = 0;  // with a full-scale FIFO sample at 128
    double right = 0;
};

// Plays a song on one player, frame by frame.
class Sequencer
{
public:
    // Keeps references to `rom` and `info`, which have to outlive this. Starts `song` on player 1, as the game starts
    // its music.
    Sequencer(const Rom& rom, const DriverInfo& info, const Song& song);

    // Runs a frame: the tracks, then the mixer's envelopes.
    void Step();

    // Returns the frames run.
    uint32_t Frame() const
    {
        return frame_;
    }

    // Returns true while a track plays or a channel sounds.
    bool Playing() const;

    // Returns true once every track that plays on has gone back by a goto `passes` times.
    bool Looped(int passes) const;

    // Returns the last frame on which a track went back by a goto for the pass `pass`, from 0, or 0 if none did.
    uint32_t LoopFrame(int pass) const;

    // Returns the frames on which each track went back by a goto.
    const std::array<std::vector<uint32_t>, kTracks>& GotoFrames() const
    {
        return goto_frames_;
    }

    const TrackState& Track(int t) const
    {
        return tracks_[size_t(t)];
    }

    const SampleChannel& Channel(int c) const
    {
        return samples_[size_t(c)];
    }

    const PsgChannel& Psg(int c) const
    {
        return psg_[size_t(c)];
    }

    // Returns the hardware's volume (0-15) on PSG channel `c` at the start of the last frame, after the driver's
    // writes, or -1 if the channel is off. The note's level for the frame is the mean of the volumes at the frame's
    // start and end.
    int PsgVolume(int c) const
    {
        return psg_volumes_[size_t(c)];
    }

    const std::vector<Note>& Notes() const
    {
        return notes_;
    }

    const std::vector<Change>& Changes() const
    {
        return changes_;
    }

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

    // Returns the player's volume, as the driver keeps it.
    uint16_t PlayerVolume() const
    {
        return player_volume_;
    }

private:
    uint8_t Next(TrackState& t);
    void StepTrack(int index);
    void NoteOn(int index);
    void NoteOff(int index);
    void UpdatePitch(int index, bool square = false);
    void UpdateLevels(int index);
    void RunEnvelopes();
    void RunPsgEnvelope(int channel);
    void RunWave();
    void RunPsg();
    bool PsgSounds(int channel) const;
    void Record(int index);
    void EndNote(int index, uint32_t frame);
    void LetGo(int index, uint32_t frame);

    const Rom& rom_;
    const DriverInfo& info_;
    std::array<TrackState, kTracks> tracks_{};
    std::array<SampleChannel, kSampleTracks> samples_{};
    std::array<PsgChannel, kTracks - kSampleTracks> psg_{};
    PsgFrame registers_; // the PSG's registers as the driver last wrote them
    std::array<PsgModel, kTracks - kSampleTracks> psg_models_{PsgModel(0), PsgModel(1), PsgModel(2), PsgModel(3)};
    std::array<int, kTracks - kSampleTracks> psg_volumes_{-1, -1, -1, -1};
    std::array<double, kTracks - kSampleTracks> psg_levels_{}; // the mean volume over the last frame
    std::array<int, kTracks> open_{}; // the index in notes_ of each track's sounding note, or -1
    std::array<int, kTracks> held_{}; // the index in notes_ of the note that each track holds, or -1
    std::array<std::vector<uint32_t>, kTracks> goto_frames_{};
    std::array<Change, kTracks> last_{};
    std::vector<Note> notes_;
    std::vector<Change> changes_;
    std::vector<std::string> warnings_;
    uint32_t frame_ = 0;
    uint32_t divisor_ = kDefaultDivisor;
    uint16_t player_volume_ = 0xFFFF;
};

} // namespace agbsf2::alphadream
