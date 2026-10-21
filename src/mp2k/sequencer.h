// SPDX-License-Identifier: MIT

// A model of MP2K's per-frame routine: its tracks, sound channels, envelopes and the mixer's progress through each
// sample, frame by frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mp2k/driver.h"
#include "mp2k/song.h"
#include "rom.h"

namespace agbsf2::mp2k
{

// The bits of a channel's status.
constexpr uint8_t kStatusStart = 0x80;   // a note that has just started
constexpr uint8_t kStatusStop = 0x40;    // the note's release
constexpr uint8_t kStatusSpecial = 0x20; // a reversed or compressed sample that the mixer has set up
constexpr uint8_t kStatusLoop = 0x10;    // a sample that loops
constexpr uint8_t kStatusEcho = 0x04;    // the echo after the release
constexpr uint8_t kStatusEnvelope = 0x03;
constexpr uint8_t kStatusOn = kStatusStart | kStatusStop | kStatusEcho | kStatusEnvelope;

// The envelope's phases, in the low 2 bits of the status.
constexpr uint8_t kPhaseAttack = 3;
constexpr uint8_t kPhaseDecay = 2;
constexpr uint8_t kPhaseSustain = 1;
constexpr uint8_t kPhaseRelease = 0;

// The number of the first PSG channel, after the DirectSound channels.
constexpr int kFirstPsgChannel = kMaxDirectChannels;

// The number of sound channels: the DirectSound channels and the four PSG channels.
constexpr int kChannelCount = kMaxDirectChannels + 4;

// The driver's tick: each frame adds the tempo to a counter. Each 150 in the counter is a tick.
constexpr int kTickCounter = 150;

// The tempo before a song sets one, in the driver's units: quarter notes a minute at 60 frames a second.
constexpr int kDefaultTempo = 150;

// A sound channel: one of the DirectSound mixer's, or one of the four PSG channels.
struct Channel
{
    uint8_t status = 0;
    uint8_t type = 0; // the voice's type
    uint8_t right = 0;
    uint8_t left = 0;
    uint8_t attack = 0;
    uint8_t decay = 0;
    uint8_t sustain = 0;
    uint8_t release = 0;
    uint8_t key = 0; // the key it plays at, before the track's key shift and bend
    uint8_t envelope = 0;
    uint8_t echo_volume = 0;
    uint8_t echo_length = 0;
    uint8_t gate = 0; // the ticks left before the note's release, or 0 for a tied note
    uint8_t midi_key = 0;
    uint8_t velocity = 0;
    uint8_t priority = 0;
    int8_t rhythm_pan = 0;
    uint32_t frequency = 0; // a DirectSound channel's rate in Hz, or the PSG channel's frequency setting
    uint32_t wave = 0;      // the sample, or the PSG channel's duty, wave pattern or noise type
    int track = -1;
    int prev = -1; // the track's list of channels, newest first
    int next = -1;

    // A DirectSound channel's progress through its sample. Camelot's mixer plays a sample of no length as a synth
    // voice, whose count holds the pulse wave's duty or the saw wave's filter, and whose fraction is the wave's phase.
    int32_t count = 0;     // the points left before the sample's end
    uint32_t fraction = 0; // 23 bits
    uint32_t position = 0; // the address of the point it plays, or the point's number in a compressed sample
    bool synth = false;

    // A PSG channel's envelope and output settings.
    uint8_t goal = 0;    // the envelope's full level, 0-15
    uint8_t counter = 0; // counts before the envelope's next step
    uint8_t sustain_goal = 0;
    uint8_t pan = 0;
    uint8_t pan_mask = 0;

    // The conversion's view of the channel: its note has got louder than silence.
    bool audible = false;
};

// Something a track or a sound channel did, for the MIDI conversion.
struct Action
{
    enum Kind : uint8_t
    {
        kNoteOn,
        kNoteDropped, // a note that found no channel, or whose voice plays nothing
        kRelease,     // the release of a channel's note
        kNoteEnd,     // the end of a channel's note: its sound ended, or a later note took the channel
        kAudible,     // the first time a channel's note got louder than silence
        kVoice,       // value = the voice
        kLevels,      // left and right = the levels that the track's volume and pan give each side
        kPitch,       // value = the pitch offset in 256ths of a semitone
        kTempo,       // value = the tempo, in quarter notes a minute at 60 frames a second
    };

    Kind kind = kNoteOn;
    uint8_t track = 0;
    uint8_t key = 0;       // kNoteOn, kNoteDropped: the key
    uint8_t velocity = 0;  // kNoteOn, kNoteDropped: the velocity
    uint8_t play_key = 0;  // kNoteOn: the key the note plays at, after a drum kit
    int32_t value = 0;     // kVoice, kPitch and kTempo: the setting
    uint8_t left = 0;      // kLevels: the level on the left, 0-255
    uint8_t right = 0;     // kLevels: the level on the right
    int32_t old_tempo = 0; // kTempo: the tempo before it, which the rest of this tick's progress counted at
    int32_t counter = 0;   // kTempo: the progress towards the next tick, in the tempo's units, after this one
    uint64_t tick = 0;     // the player's tick count when it happened
    uint32_t frame = 0;    // the frame it happened on
    int channel = -1;      // kNoteOn, kRelease, kNoteEnd, kAudible: the channel
    int program = -1;      // kNoteOn: the track's voice
    uint32_t voice = 0;    // kNoteOn: the voice that plays the note, after a key split or drum kit
    bool sounded = true;   // kNoteEnd: false if the note ended before the mixer started it
    bool modified = false; // kNoteOn: extended commands changed the track's voice, or start notes partway in
};

// Plays a song one frame at a time, as the driver's per-frame routine does.
class Sequencer
{
public:
    // Sets up `song` as the driver does when the game starts it on its music player after the driver's init. `rom` and
    // `info` have to outlive the sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int song);

    // Returns true if the song's header was read and has tracks.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one frame of the driver's per-frame routine. Returns what the tracks and channels did in it, in order.
    const std::vector<Action>& Step();

    // From tick `tick` on, has each track report its levels where it first sets its volume or pan. The track reports
    // its pitch where it first sets that. A command sets them, and so does clearing the LFO's swing. The reports come
    // even where the settings stay the same. A player that jumps back to a loop's start keeps the settings that the
    // loop's end left. The game does the same until the loop sets them again.
    void RestateFrom(uint64_t tick)
    {
        restate_from_ = tick;
    }

    // Returns true once every track has ended.
    bool TracksEnded() const
    {
        return (status_ & kPlayerStopped) != 0;
    }

    // Returns true once every track has ended and every channel is silent.
    bool Ended() const;

    // Returns the number of ticks the player has played.
    uint64_t Tick() const
    {
        return clock_;
    }

    int TrackCount() const
    {
        return int(tracks_.size());
    }

    // Returns the number of DirectSound channels the mixer uses.
    int DirectChannels() const
    {
        return info_.max_channels;
    }

    const Channel& GetChannel(int index) const
    {
        return channels_[size_t(index)];
    }

    // Returns problems found in the song's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // The state of a track: the driver's MusicPlayerTrack.
    struct Track
    {
        uint8_t flags = 0;
        uint8_t wait = 0;
        uint8_t pattern_level = 0;
        uint8_t repeats = 0;
        uint8_t gate = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint8_t running = 0;
        int8_t key_m = 0;
        uint8_t pit_m = 0;
        int8_t key_shift = 0;
        int8_t tune = 0;
        int8_t bend = 0;
        uint8_t bend_range = 0;
        uint8_t vol_mr = 0;
        uint8_t vol_ml = 0;
        uint8_t vol = 0;
        uint8_t vol_x = 0;
        int8_t pan = 0;
        int8_t mod_m = 0;
        uint8_t mod = 0;
        uint8_t mod_t = 0;
        uint8_t lfo_speed = 0;
        uint8_t lfo_speed_c = 0;
        uint8_t lfo_delay = 0;
        uint8_t lfo_delay_c = 0;
        uint8_t priority = 0;
        uint8_t echo_volume = 0;
        uint8_t echo_length = 0;
        int chan = -1; // the newest of its channels
        Voice tone;    // its copy of its voice
        uint16_t timer = 0;
        uint32_t offset = 0; // the point that its notes start from in their samples
        uint32_t position = 0;
        std::array<uint32_t, 3> stack = {};

        // The conversion's view of the track.
        int voice = -1;
        bool modified = false; // an extended command changed its copy of its voice
        int last_left = -1, last_right = -1, last_pitch = 0x7FFFFFFF;
        uint8_t sets = 0;    // the settings it set in the tick, changed or not
        uint8_t restate = 0; // the settings it reports where it next sets them
    };

    // A track's levels on each side.
    struct SideLevels
    {
        uint8_t left = 0;
        uint8_t right = 0;
    };

    // The player's status once every track has ended.
    static constexpr uint32_t kPlayerStopped = 0x80000000u;

    // Runs a tick of track `t`: counts down its notes, runs its commands that are due and moves its LFO on.
    void RunTrack(int t);

    // Runs one command of track `t`. Returns false if the track has ended.
    bool RunCommand(int t);

    void StartTrack(Track& track);
    void Note(int t, int length_index);
    void EndTie(int t);
    void EndTrack(int t);
    void ExtendedCommand(int t);
    void MemAcc(int t);
    void Jump(Track& track);
    void ClearModulation(Track& track);

    // Returns the levels that a track's volume and pan give each side. The LFO's swing counts when it modulates them.
    // The driver's TrkVolPitSet computes the same.
    static SideLevels TrackLevels(const Track& track);

    // Recalculates a track's volumes and pitch, as the driver's TrkVolPitSet does, if its flags ask for it.
    void UpdateTrack(Track& track);

    // Sets a channel's volumes from its velocity and its track's volumes.
    void SetChannelVolume(Channel& c, const Track& track) const;

    // Sets a channel's frequency from its key and its track's pitch.
    void SetChannelPitch(Channel& c, const Track& track) const;

    // Updates the channels of tracks whose volume or pitch changed in the frame.
    void UpdateChannels();

    // Returns the channel a note of priority `priority` on track `t` takes, or -1 if there's none it can take.
    int FindChannel(int t, int psg, int priority) const;

    void LinkChannel(int index, int t);
    void UnlinkChannel(int index);

    // Returns the channel a DirectSound note takes in Camelot's version of the driver, or -1 if there's none it can
    // take.
    int FindCamelotChannel(int t, int priority) const;

    void RunPsg();
    void RunDirect();
    void MixChannel(Channel& c, const Wave& wave);

    // Moves a synth voice of Camelot's mixer on by a frame.
    void MixSynth(Channel& c) const;

    // Records the track's levels and pitch for the conversion if they changed, or if it set them for the first time
    // from RestateFrom()'s tick on.
    void ReportTrack(int t);

    // Returns a track's pitch in 256ths of a semitone: its tune, bend and key shift, and the LFO's swing when it
    // modulates the pitch.
    static int32_t TrackPitch(const Track& track);

    void AddAction(Action action);

    // Releases the note that track `t` plays on `channel`.
    void ReleaseNote(int t, int channel);

    void EndNote(int channel, bool sounded);

    // Records that a channel's note is audible, the first time its envelope is above 0.
    void CheckAudible(int channel);

    void Warn(const std::string& message);

    const Rom& rom_;
    const DriverInfo& info_;
    bool valid_ = false;
    SongHeader header_;
    std::vector<Track> tracks_;
    std::array<Channel, kChannelCount> channels_ = {};

    // The music player's state.
    uint32_t status_ = 0;
    uint8_t priority_ = 0;
    uint64_t clock_ = 0;
    uint16_t tempo_d_ = kDefaultTempo;
    uint16_t tempo_u_ = 0x100;
    uint16_t tempo_i_ = kDefaultTempo;
    uint16_t tempo_c_ = 0;
    std::array<uint8_t, kMemAccSize> memory_ = {};

    uint8_t c15_ = 0; // the PSG envelopes' frame counter, 14 down to 0
    uint32_t frame_ = 0;
    uint64_t restate_from_ = ~uint64_t(0);
    std::vector<Action> actions_;
    std::vector<std::string> warnings_;
};

// Returns the rate in Hz at which a sample whose header gives `frequency` (1024 times its rate for key 60) plays `key`,
// raised by `fine`/256 of a semitone, as the driver's MidiKeyToFreq works it out.
uint32_t KeyToFrequency(uint32_t frequency, int key, int fine);

// Returns the rate as KeyToFrequency() does, as Camelot's version of MidiKeyToFreq works it out. It shifts the step to
// the next semitone down by the octaves, where the driver shifts both semitones' rates and takes the difference, so the
// two can round differently.
uint32_t CamelotKeyToFrequency(uint32_t frequency, int key, int fine);

// Returns the rate as KeyToFrequency() does, or as CamelotKeyToFrequency() does in Camelot's version of the driver.
uint32_t SampleFrequency(const DriverInfo& info, uint32_t frequency, int key, int fine);

// Returns the frequency setting of PSG channel `channel` (1-4) for `key`, raised by `fine`/256 of a semitone, as the
// driver's MidiKeyToCgbFreq works it out: the 11-bit register value for channels 1-3, or a noise setting for 4.
uint32_t KeyToPsgFrequency(int channel, int key, int fine);

// Returns how far the phase of a synth voice of Camelot's mixer moves on for each point of the mixer's output, at
// `frequency` Hz. The phase moves 8 times as far as a sample's position would. A cycle of the wave therefore lasts 64
// of a sample's points. The step wraps around at 32 bits.
inline uint32_t SynthStep(uint32_t frequency, uint32_t step_scale)
{
    return (frequency * step_scale) << 3;
}

// Returns the point of a synth voice's saw wave at `phase`. The wave rises from -112 to 112 over a cycle. It rises 3
// for every 4 of the phase's 256 steps and jumps 32 halfway.
inline int32_t SawPoint(uint32_t phase)
{
    return int32_t(phase >> 24) - 0x70 - int32_t((phase >> 26) & 0x1F);
}

// Returns the saw wave's filter output for `point` after the output `last`: the point plus half the last output.
inline int32_t FilterSaw(int32_t point, int32_t last)
{
    return point + (last >> 1);
}

// Returns how far the triangle wave that sets a pulse wave's duty moves on each frame: the fourth byte of the synth
// voice's data at `data`, at the top of a word.
inline uint32_t DutyStep(const Rom& rom, uint32_t data)
{
    return uint32_t(rom.U8(data + 3)) << 24;
}

} // namespace agbsf2::mp2k
