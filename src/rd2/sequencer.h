// SPDX-License-Identifier: MIT

// A model of the driver playing a sequence: the music player, its tracks and the voices they play, frame by frame, as
// the driver's per-frame routine runs them.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gba.h"
#include "rd2/driver.h"
#include "rom.h"

namespace agbsf2::rd2
{

// The voices: the mixer's sample voices, then one for each of the PSG's channels.
constexpr int kPsgVoices = 4;
constexpr int kVoices = kSampleVoices + kPsgVoices;

// The tracks a player can have, and the driver's tracks for all its players.
constexpr int kPlayerTracks = 10;
constexpr int kTrackSlots = 24;

// The deepest F4 can nest: the driver keeps 3 return addresses.
constexpr int kMaxDepth = 3;

// The mixer's rate and the samples it mixes each frame.
constexpr uint32_t kMixRate = 10512;
constexpr uint32_t kFrameSamples = 176;

// The number of units a track counts in each tick, and the tempo a player starts at. Each frame takes the tempo off a
// track's count, so at a tempo of 150 a tick lasts a frame.
constexpr uint32_t kTickUnits = 150;
constexpr uint16_t kStartTempo = 150;

// The region types, which are also the voices' types.
constexpr uint8_t kSampleType = 0;
constexpr uint8_t kSquare1Type = 1;
constexpr uint8_t kSquare2Type = 2;
constexpr uint8_t kWaveType = 3;
constexpr uint8_t kNoiseType = 4;

// The pitch index that plays a sample at its rate, a square at middle C and the wave at C3.
constexpr int kUnityIndex = 0x30;

// The last pitch index in the driver's tables. For a note above them, the driver reads the entry after it.
constexpr int kLastIndex = 0x77;

// The note that drums and instruments with a sample for each key play, whatever the key.
constexpr uint8_t kFixedNote = 0x30;

// The pan that plays a voice on both sides alike.
constexpr uint8_t kCentre = 0x40;

// The priority and the echo send that a sound effect's tracks start with.
constexpr uint8_t kEffectPriority = 12;
constexpr uint8_t kEffectEcho = 0x7F;

// The states of a voice, as the driver keeps them in the voice's byte 0x01.
constexpr uint8_t kVoiceFree = 0;
constexpr uint8_t kVoicePlaying = 1;
constexpr uint8_t kVoiceReleased = 2;

// The driver's record of a voice, with each field's offset. The links are voice and track numbers, where the driver
// keeps addresses.
struct Voice
{
    uint8_t type = kSampleType; // 0x00
    uint8_t state = kVoiceFree; // 0x01
    int track = -1;             // 0x04: the track slot that plays it
    uint8_t priority = 0;       // 0x08
    uint8_t note = 0;           // 0x09: the note as the track gave it, before its transpose
    uint8_t velocity = 0;       // 0x0A
    uint32_t base_pitch = 0;    // 0x0C: a sample's pitch, a square or wave's frequency setting, or a noise index
    uint32_t pitch = 0;         // 0x10: the pitch with the frame's slide, bend and LFO
    uint32_t volume = 0;        // 0x14
    uint16_t frames = 0;        // 0x18: frames left before the release
    uint8_t echo = 0;           // 0x1A: the track's echo send
    uint8_t own_pan = 0;        // 0x1B: 1 if the pan is a drum's rather than its track's
    uint8_t pan = 0;            // 0x1C: the drum's pan, or the track's after the release
    uint32_t lfo_phase = 0;     // 0x20
    uint32_t lfo_delay = 0;     // 0x24
    uint32_t slide_delay = 0;   // 0x2C
    uint32_t slide_frames = 0;  // 0x30
    int32_t slide = 0;          // 0x34: the pitch the slide has added so far
    int32_t slide_end = 0;      // 0x38: the pitch it adds by its end
    int32_t slide_step = 0;     // 0x3C
    int32_t level = 0;          // 0x40: the envelope's level, 0x7FFF at full
    int32_t target = 0;         // 0x44: the level the envelope's segment ends at
    uint16_t segment = 0;       // 0x48: the segment's frames left
    int16_t level_step = 0;     // 0x4A
    uint32_t envelope = 0;      // 0x4C: the envelope's points
    uint8_t point = 0;          // 0x50: the point the segment heads for
    uint32_t region = 0;        // 0x54: the region it plays, or 0 for the driver's region of a sample for each key
    uint8_t release = 0;        // 0x58
    uint32_t sample = 0;        // 0x5C: a sample voice's sample
    uint32_t position = 0;      // 0x60: a sample voice's position in 1/256 points, or a PSG voice's frames played
    uint32_t psg = 0;           // 0x64: a square's duty or a noise's width, or a table of them, or a wave
    int prev = -1;              // 0x68: the sample voices' lists
    int next = -1;              // 0x6C
    int track_prev = -1;        // 0x70: the voices of its track
    int track_next = -1;        // 0x74
    uint32_t id = 0;            // the model's number for the note it plays, counted from 1
    uint32_t note_pitch = 0;    // the pitch of the note, without its slide, bend or LFO
};

// The driver's record of a track, with each field's offset.
struct Track
{
    uint32_t position = 0;                      // 0x00: the next byte to read
    uint32_t samples = 0;                       // 0x04: the table of the bank's samples
    bool active = false;                        // 0x08: true while a player has it
    int voices = -1;                            // 0x0C: the first of its voices, the newest
    uint16_t lfo_delay = 0;                     // 0x10: E5
    uint32_t lfo_rate = 0x22;                   // 0x14: E6
    uint32_t lfo_depth = 0;                     // 0x18: E7
    uint8_t slide = 0;                          // 0x1C: 1 while the next note slides
    uint8_t slide_flags = 0;                    // 0x1D: Dx's low nibble
    uint8_t slide_note = 0;                     // 0x1E
    uint16_t slide_delay = 0;                   // 0x20
    uint16_t slide_length = 0;                  // 0x22: the slide's length in 256ths of the note's
    std::array<uint32_t, kMaxDepth> stack = {}; // 0x24: F4's return addresses
    int depth = 0;                              // 0x30
    int32_t counter = 0;                        // 0x34: the time left before the next command, in units of 1/150 tick
    uint16_t bank = 0;                          // 0x40: C7's slot in the player's list of banks
    uint16_t instrument = 0;                    // 0x42: C2
    uint16_t length = 0x7F;                     // 0x44: the notes' length, in ticks
    uint16_t wait = 0;                          // 0x46: C0's wait
    uint8_t velocity = 0x7F;                    // 0x48
    uint8_t legato = 0;                         // 0x49: C5 and C6
    uint8_t mute = 0;                           // 0x4A
    uint8_t pan = kCentre;                      // 0x4B: C3
    uint8_t echo = 0;                           // 0x4C: E3
    uint8_t volume = 0x80;                      // 0x4D: E0
    uint8_t volume2 = 0x80;                     // 0x4E
    int8_t bend = 0;                            // 0x4F: E1
    uint8_t bend_range = 2;                     // 0x50: E2
    uint8_t transpose = 0;                      // 0x51: E9
    uint8_t priority = 3;                       // 0x52: C4
    uint8_t timed_notes = 0;                    // 0x53: C8 and C9, which make notes wait for their length
    int number = -1;                            // the player's number for the track, 0-9
    uint64_t units = 0;                         // the time the track has read up to, in units from the sequence's start
    std::map<uint32_t, uint64_t> seen;          // the time it first read the command at each address
};

// The driver's record of a player, with each field's offset.
struct Player
{
    uint32_t banks = 0;    // 0x00: the sequence's list of banks
    uint32_t sequence = 0; // 0x04
    std::array<int, kPlayerTracks> tracks = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; // 0x08: track slots
    uint16_t tempo = kStartTempo;                                                     // 0x30: E4
    int16_t tempo_adjust = 0; // 0x32: the game's change to the tempo
    uint16_t fade = 0x8000;   // 0x34: the game's fade
    uint8_t volume = 0x80;    // 0x40: EA
    uint8_t volume2 = 0x80;   // 0x41
    bool active = false;      // 0x42
    bool effect = false;      // 0x43: true while it plays a sound effect
};

// An instrument's settings for a note: the region to play, its envelope and its PSG settings.
struct Lookup
{
    bool found = false;
    uint32_t region = 0; // 0 for the driver's region of a sample for each key
    uint32_t envelope = 0;
    uint32_t psg_table = 0;
    uint32_t wave = 0;
    uint8_t drum_pan = 0;
    bool drum = false;       // from a drum kit, which plays it at its own pitch and pan
    bool key_sample = false; // from an instrument with a sample for each key, which plays it at its own pitch
    uint16_t key_sample_index = 0;
    bool missing = false; // the bank has no region for the note: the offset of the instrument, or of a drum kit's or
                          // key split's region, is 0, so the bank's table of offsets is the region
};

// Returns what instrument `instrument` of bank `bank` (an index into the driver's banks) gives a note.
Lookup LookUpInstrument(const Rom& rom, const DriverInfo& info, uint16_t bank, uint16_t instrument, uint8_t note);

// Returns the address of bank `bank`. The offsets of its instruments count from there.
uint32_t BankAddress(const Rom& rom, const DriverInfo& info, uint16_t bank);

// Returns the address of the table of samples of the sample set that bank `bank` plays.
uint32_t SampleTable(const Rom& rom, const DriverInfo& info, uint16_t bank);

// Returns the address of sample `index` of the sample set that bank `bank` plays.
uint32_t SampleAddress(const Rom& rom, const DriverInfo& info, uint16_t bank, uint32_t index);

// Returns the driver's pitch index for `note` on a region with `tuning`: the note less the tuning, plus kUnityIndex,
// kept from 0 up to the entry past the tables.
int PitchIndex(uint8_t note, uint8_t tuning);

// Each returns a field of a region. Region 0 is the driver's region in RAM for instruments with a sample for each key:
// a sample voice's, with no flags and no release, tuned to play kFixedNote at the sample's rate.
uint8_t RegionType(const Rom& rom, uint32_t region);
uint8_t RegionFlags(const Rom& rom, uint32_t region);
uint8_t RegionRelease(const Rom& rom, uint32_t region);
uint8_t RegionTuning(const Rom& rom, uint32_t region);
uint8_t RegionSweep(const Rom& rom, uint32_t region);

// The region flag that leaves the game's change to the tempo out of a note's length.
constexpr uint8_t kNoTempoAdjustFlag = 0x10;

// Returns true if a square or noise region has a table of duties or widths at halfword 2 (flag 0x01). Without one,
// halfword 2 is the duty or width.
bool HasPsgTable(const Rom& rom, uint32_t region);

// Returns the entry for frame `frame` of a note from a square's table of duties or a noise voice's table of widths. The
// table is a halfword count, then a byte for each frame from the note's first. The last entry stays after that.
uint8_t PsgTableEntry(const Rom& rom, uint32_t table, uint32_t frame);

// Returns the last entry of a table of duties or widths. The voice holds it after the others.
uint8_t LastPsgTableEntry(const Rom& rom, uint32_t table);

// Returns the NR43 that the noise table gives pitch index `index`. An index past the table gets the table's last entry.
uint8_t NoiseTableEntry(const Rom& rom, const DriverInfo& info, uint32_t index);

// The target time of a jump to a command that the track hasn't read before.
constexpr uint64_t kUnreadTarget = ~uint64_t(0);

// Something that happened in a frame, for the conversion.
struct Event
{
    enum Kind : uint8_t
    {
        kNote,         // a note on: `voice` plays it, or -1 if it got no voice; `id` numbers it
        kRelease,      // the release of `voice`'s note
        kStop,         // `voice`'s note stopped: it was taken by another note, its sample ended, or it faded out
        kPlayerVolume, // the player's volume (EA) is now `value`
        kPan,          // the track's pan (C3) is now `value`
        kTrackVolume,  // the track's volume (E0) is now `value`
        kJump,         // F0: the track went to `value`
        kTrackStart,   // F8: the track started track `value`
        kTrackEnd,     // the track ended
    };

    // The part of the frame it happened in.
    enum Phase : uint8_t
    {
        kCommands,  // a track's command, at its time
        kPsgVoices, // the PSG voices' update, at the frame's start
        kMixer,     // the mixer, at the frame's end
    };

    Kind kind = kNote;
    Phase phase = kCommands;
    int track = -1; // the player's number for the track, 0-9
    int voice = -1;
    uint32_t id = 0;    // the note's number, counted from 1
    uint64_t units = 0; // the track's time when it read the command, in 1/150 ticks from the sequence's start
    uint32_t value = 0;
    uint64_t target_units = kUnreadTarget; // a jump's: the time the track first read the command it goes to

    // A note's settings.
    uint8_t key = 0; // the note after the track's transpose
    uint8_t velocity = 0;
    uint16_t bank = 0; // the bank the instrument is in
    uint16_t instrument = 0;
    uint32_t length = 0; // in units
    Lookup lookup;
    uint8_t type = 0;    // the voice's type
    uint32_t sample = 0; // a sample voice's sample
    uint32_t psg = 0;    // a PSG voice's setting, as the voice has it
    bool legato = false; // the track plays in legato: its voice keeps the envelope from note to note
};

// A model of the driver that plays a sequence a frame at a time.
class Sequencer
{
public:
    // Plays song `song`: a sequence, or a sound effect after the sequences (see SongEffect()). A sequence plays with
    // the settings of its version `version` (see VersionSettings()). Keeps references to `rom` and `info`. Both have to
    // outlive the sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int song, int version = 0);

    // Returns false if the sequence can't be played.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one frame of the driver: the sequencer, the PSG voices and the mixer's voices.
    void Step();

    // Returns true once the player has stopped.
    bool Ended() const
    {
        return started_ && !player_.active;
    }

    const std::array<Voice, kVoices>& Voices() const
    {
        return voices_;
    }

    const std::array<Track, kTrackSlots>& Tracks() const
    {
        return tracks_;
    }

    const Player& GetPlayer() const
    {
        return player_;
    }

    // Returns the sample voices that play, in the order the mixer takes them.
    std::vector<int> ActiveOrder() const;

    // Returns the last frame's events and register writes.
    const std::vector<Event>& Events() const
    {
        return events_;
    }

    const std::vector<RegisterWrite>& Writes() const
    {
        return writes_;
    }

    // Returns the sound registers from 0x04000060 on: 0x40 bytes, with the values last written.
    const std::array<uint8_t, 0x40>& Registers() const
    {
        return registers_;
    }

    // Returns the PSG channels that the last frame started again: bit c for channel c, from 0 for square 1 to 3 for the
    // noise.
    uint8_t Restarted() const
    {
        return restarted_;
    }

    // Returns the time at which the next frame starts, in units from the sequence's start: the tempo of each frame so
    // far, added up.
    uint64_t Elapsed() const
    {
        return elapsed_;
    }

    // Returns the player's tempo with the game's change to it, at least 0. Each frame adds it to Elapsed().
    int FrameTempo() const
    {
        return std::max(0, int(player_.tempo) + player_.tempo_adjust);
    }

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    void Start();
    void StartEffect();
    void Warn(const std::string& text);

    // The sequencer.
    void StartTrack(int slot, uint32_t position, int number, uint64_t units);
    int AllocateTrack() const;
    void FreeTrack(int slot);
    void ReleaseTrackVoices(int slot);
    bool RunTrack(int slot);
    void PlayNote(int slot, uint8_t op);
    void StartTrackFrom(int slot, uint32_t at);
    uint32_t ReadVarLength(Track& t);
    void NoteOn(int slot, uint8_t note, uint8_t velocity, uint32_t length);
    uint32_t NoteFrames(uint32_t region, uint32_t length) const;
    int TakeVoice(int slot, uint8_t given, const Lookup& lk);
    void StartSlide(Track& t, Voice& x, uint8_t note, uint8_t tuning, uint32_t frames);
    void SetVoiceSound(const Track& t, Voice& x, const Lookup& lk);
    void SetBank(Track& t, uint16_t bank);
    uint16_t BankOf(const Track& t) const;
    void StopPlayer();

    // The voices.
    void SetSampleVoices(int count);
    int AllocateVoice(uint8_t voice_class, uint8_t priority);
    void Unlink(int v, int& head);
    void PushFront(int v, int& head);
    void InsertActive(int v);
    void LinkToTrack(int slot, int v);
    void UnlinkFromTrack(int v);
    const Track& TrackOf(const Voice& v) const;
    void Release(int v);
    void StopVoice(int v);
    uint32_t PitchStep(uint8_t type, uint8_t note, uint8_t tuning) const;
    int32_t EnvelopeStep(Voice& v);
    uint32_t FramePitch(Voice& v);
    uint32_t Bend(const Voice& v, const Track& t, uint32_t pitch) const;
    uint32_t ApplyLfo(Voice& v, const Track& t, uint32_t pitch);
    uint32_t SampleLevel(Voice& v);
    uint8_t PsgEnvelope(Voice& v, bool panned);
    uint32_t PsgLevel(const Voice& v, bool panned) const;
    void UpdatePsgVoices();
    void WritePan(int channel, uint8_t pan);
    void FadeReleasedWave(int index, uint8_t pan);
    uint8_t FrameDuty(const Voice& v) const;
    void WriteChannel(const Voice& v, uint32_t pitch, uint8_t envelope, uint8_t duty);
    void StartPsgVoice(Voice& v, uint8_t envelope);
    uint8_t FirstDuty(const Voice& v) const;
    uint8_t NoiseShape(uint32_t pitch) const;
    void UpdateSampleVoices();
    bool MixVoice(Voice& v, uint32_t step);

    // The sound registers, and the bits of them that the driver reads back.
    void Write(uint32_t address, uint8_t size, uint32_t value);
    uint32_t ReadBack(uint32_t address, uint8_t size) const;

    const Rom& rom_;
    const DriverInfo& info_;
    const RevisionTraits& traits_; // TraitsOf(info_.revision)
    int sequence_ = 0;
    int version_ = 0;
    const Effect* effect_ = nullptr; // the sound effect that it plays, or null for a sequence
    bool valid_ = false;
    bool started_ = false;
    Player player_;
    std::array<Track, kTrackSlots> tracks_;
    std::array<Voice, kVoices> voices_;
    int free_ = -1;           // the first free sample voice
    int active_ = -1;         // the first sample voice that plays
    uint16_t key_sample_ = 0; // the sample in the driver's region of a sample for each key
    uint32_t current_wave_ = 0;
    std::array<uint8_t, 0x40> registers_ = {}; // 0x04000060 on, as last written
    uint8_t restarted_ = 0;                    // bit c: the frame started PSG channel c again
    uint64_t elapsed_ = 0;
    uint32_t next_id_ = 1;
    Event::Phase phase_ = Event::kCommands;
    uint64_t command_units_ = 0; // the time of the command that runs
    std::vector<Event> events_;
    std::vector<RegisterWrite> writes_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::rd2
