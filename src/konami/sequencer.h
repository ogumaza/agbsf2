// SPDX-License-Identifier: MIT

// Frame-accurate models of the Konami GBA sequencer.
//
// Each model follows the driver's per-frame VBlank routine closely enough to reproduce the outputs sent to the PSG and
// mixer. Driver revisions have separate models.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "konami/driver.h"
#include "konami/seqformat.h"
#include "rom.h"

namespace agbsf2::konami
{

// One track's requests to its sound channel in one frame. This is the driver's 12-byte per-track output record, plus
// the note's base pitch.
struct TrackOutput
{
    // Returns true if the track asks its channel for anything this frame.
    bool Any() const
    {
        return trig || flags;
    }

    int16_t pitch = 0;        // 1/32 semitones incl. bend and vibrato (noise track: note number in Ultimate Masters),
                              // or in Dungeon Dice Monsters an entry of a frequency or sample period table, or a period
    uint8_t b2 = 0;           // PSG duty/length byte, wave number, or DS sample low byte
    uint8_t vol = 0;          // volume
    uint8_t trig = 0;         // PSG: a (re)trigger of the channel; DS: a refresh of its volume and pan
    uint8_t flags = 0;        // kOut* bits below
    uint16_t key = 0;         // DS sample index, or in Dungeon Dice Monsters the square channels' envelope (NRx2 bits)
    uint8_t pan_r = 0;        // Ultimate Masters: pan level of the right side (0..63)
    uint8_t pan_l = 0;        // Ultimate Masters: pan level of the left side (0..63)
    uint8_t pan = 0xFF;       // WCT 2004: pan byte, left level << 4 | right level (0..15 each), or in Eternal Duelist
                              // the NR51 bits of a PSG track
    uint8_t pan_start = 0xFF; // WCT 2004: the track's pan byte as the frame started
    int16_t note_pitch = 0;   // the note itself without bend/vibrato (1/32 semitones)
    bool active = false;      // true if the track runs this frame
};

constexpr uint8_t kOutNoteOn = 0x80;  // DS: the start of a new note
constexpr uint8_t kOutStop = 0x40;    // DS: a stop of the voice
constexpr uint8_t kOutPitch = 0x20;   // a pitch change (bend or vibrato)
constexpr uint8_t kOutPan = 0x08;     // a pan change
constexpr uint8_t kOutPsgNote = 0x01; // a PSG note or a volume retrigger

// An echo bus: its settings and the voices routed to it.
struct EchoBus
{
    int feedback = 0;    // F7 value (0..127), feedback = value / 256
    int delay = 0;       // F8 value, delay = value * 32 mixer samples
    uint16_t voices = 0; // bit v: DirectSound voice v (track 4 + v) feeds this bus
};

// One frame, the sequencer's time step: 280896 CPU cycles at 16.78 MHz (59.7275 Hz).
constexpr double kFrameSeconds = 280896.0 / 16777216.0;

// Maximum playback length, including loops (about 30 minutes).
constexpr uint32_t kMaxFrames = 60 * 60 * 30;

// The frames before the looping track's loop point in which another track's loop point still counts for the loop's
// start. The checked songs' loop points are within a frame of each other.
constexpr int kLoopPointWindow = 2;

// Plays a song one frame at a time, as the driver's per-frame routine does.
class Sequencer
{
public:
    // Creates a sequencer for song `song` using the driver revision in `info`. Keeps a reference to `rom`, which must
    // outlive the sequencer.
    static std::unique_ptr<Sequencer> Create(const Rom& rom, const DriverInfo& info, int song);

    virtual ~Sequencer() = default;

    // Returns true if the song's header and data are in the ROM. An invalid song plays nothing.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one frame of the sequencer and returns the 16 track outputs.
    const std::array<TrackOutput, kTracks>& Step();

    // Returns true after a stop command or a zero-delay loop that would hang the driver.
    bool Stopped() const
    {
        return stopped_;
    }

    // Returns true if any track is still running.
    bool AnyTrackActive() const;

    // Returns the number of song loops executed.
    int LoopsDone() const
    {
        return loops_;
    }

    // Returns true if the last Step() looped the song.
    bool LoopedLastFrame() const
    {
        return looped_last_frame_;
    }

    // Returns the frame a loop goes back to, or 0 if it has none. A loop sends every track back to its loop point at
    // once, so that's the earliest frame on which a track passed its loop point, where a player that loops leaves out
    // none of the loop's notes, or 0 if the track that loops the song has no loop point and goes back to its start.
    // Only loop points within kLoopPointWindow frames before the looping track's count. A track whose loop point
    // comes far earlier, such as at its first command, would make a looping player replay the other tracks' intros.
    int LoopStartFrame() const;

    // Returns true if a track's loop point starts with a delay that the first pass skips. The later passes then play
    // that track later than the first does.
    bool LoopPointDelayed() const;

    // Returns echo bus `bus` (0-2): its settings and the voices routed to it.
    const EchoBus& Echo(int bus) const
    {
        return echo_[bus];
    }

    // Returns the PSG's volume on each side, as NR50 holds it: left << 4 | right, 0-7 each. Only the Dungeon Dice
    // Monsters revision's songs change it.
    uint8_t PsgVolume() const
    {
        return psg_volume_;
    }

    // Returns the wave in the wave channel's RAM, which the Dungeon Dice Monsters revision's commands load.
    int LoadedWave() const
    {
        return loaded_wave_;
    }

    // Returns the number of wave loads in the Dungeon Dice Monsters revision, including reloads of the current wave.
    int WaveLoads() const
    {
        return wave_loads_;
    }

    // Returns warnings about the song data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

protected:
    // The outcome of a track's turn in a frame: the next track's turn, or a loop of the song.
    enum class Result
    {
        kContinue,
        kRestart
    };

    // Reads the header of song `song`. The derived class sets `valid_` once it has set its tracks up.
    Sequencer(const Rom& rom, const DriverInfo& info, int song);

    // Runs one frame of `track`.
    virtual Result StepTrack(int track) = 0;

    // Resets the tracks as a song loop does.
    virtual void ResetTracks() = 0;

    // Returns true if `track` is running.
    virtual bool TrackActive(int track) const = 0;

    // Returns the track's current data offset, which changes at loop points.
    virtual uint16_t TrackStart(int track) const = 0;

    // Returns the pitch of the note that `track` plays, without bend or vibrato.
    virtual int16_t NotePitch(int track) const = 0;

    // Adds warning `what` about `track`'s data at `addr`, headed by the frame, the track and the address.
    void Warn(int track, uint32_t addr, const std::string& what);

    // Records the frame on which `track` passes a loop point before the song first loops, for LoopStartFrame(), and the
    // frames of the delay at the loop point's start. The first pass skips that delay.
    void PassLoopPoint(int track, int delay = 0);

    // Clears `track`'s loop point when the track ends before the song first loops, never to restart.
    void EndForGood(int track);

    const Rom& rom_;
    SongHeader header_;
    bool valid_ = false;
    std::array<TrackOutput, kTracks> out_{};
    std::array<EchoBus, 3> echo_{};
    uint8_t psg_volume_ = 0x77;
    int loaded_wave_ = 0;
    int wave_loads_ = 0;
    bool stopped_ = false;

private:
    bool looped_last_frame_ = false;
    int loops_ = 0;
    int loop_track_ = -1;
    std::array<int, kTracks> loop_frame_{}; // the frame each track last passed a loop point in the first pass, or -1
    std::array<int, kTracks> loop_delay_{}; // the delay at the start of that loop point
    uint32_t frame_ = 0;
    std::vector<std::string> warnings_;
};

// The sequencer of the Ultimate Masters revision.
class UltimateMastersSequencer : public Sequencer
{
public:
    UltimateMastersSequencer(const Rom& rom, const DriverInfo& info, int song);

protected:
    Result StepTrack(int track) override;
    void ResetTracks() override;
    bool TrackActive(int track) const override;
    uint16_t TrackStart(int track) const override;
    int16_t NotePitch(int track) const override;

private:
    // The state of one of the song's tracks.
    struct Track
    {
        uint8_t flags = 0;  // 0x80 active, 0x40 restart on loop, 0x20 vibrato, 0x01 started
        uint16_t start = 0; // track offset (moved by F3)
        uint16_t pos = 0;   // read position relative to start
        uint16_t delay = 0; // frames until the next command
        int16_t pitch = 0;  // note pitch (1/32 semitones), noise note for track 3
        uint8_t b2 = 0;     // duty byte / wave / sample low byte
        uint8_t vol = 0;
        uint8_t vib_phase = 0;
        uint8_t vib_depth = 0;
        uint8_t pan_r = 0x3F, pan_l = 0x3F;
        uint16_t key = 0; // DS sample index of the current note
        int16_t bend = 0; // 1/32 semitones
    };

    // A track's next step after a command: read the delay that follows, stop until the next frame, or loop the song.
    enum class Next
    {
        kReadDelay,
        kStop,
        kRestart
    };

    // Runs command `c` of `track`, whose read position is `pos`.
    Next RunCommand(int track, const Command& c, uint32_t& pos);

    // Applies vibrato and outputs the volume after processing a running track's commands.
    void PerFrame(int track);

    bool bend_kept_ = true; // DriverInfo::bend_kept
    std::array<Track, kTracks> tracks_{};
};

// Sequencer for WCT 2004 and the older Rave Master and Eternal Duelist revisions, with their individual differences.
class Wct2004Sequencer : public Sequencer
{
public:
    Wct2004Sequencer(const Rom& rom, const DriverInfo& info, int song);

protected:
    Result StepTrack(int track) override;
    void ResetTracks() override;
    bool TrackActive(int track) const override;
    uint16_t TrackStart(int track) const override;
    int16_t NotePitch(int track) const override;

private:
    // The state of one of the song's tracks.
    struct Track
    {
        uint8_t flags = 0;  // 0x80 active, 0x40 restart on loop, 0x20 vibrato, 0x01 started
        uint16_t start = 0; // track offset (moved by F3 and by calls)
        uint16_t pos = 0;   // read position relative to start
        uint16_t delay = 0; // frames until the next command
        int16_t pitch = 0;  // note pitch (1/32 semitones)
        uint8_t b2 = 0;     // duty byte / wave / sample low byte
        uint8_t vol = 0;    // 0..15
        uint8_t vib_phase = 0;
        uint8_t vib_depth = 0;
        uint8_t attack_rate = 0;  // F4
        uint8_t decay_rate = 0;   // F5
        uint8_t attack = 0;       // attack countdown, 0xFF at a PSG note
        uint8_t decay = 0;        // decay countdown, 0xFF at a PSG note
        uint8_t pan = 0xFF;       // left level << 4 | right level, or in Eternal Duelist a PSG track's NR51 bits
        uint16_t ret = 0;         // position to return to after a call, 0 if there's none
        uint16_t saved_start = 0; // start to return to after a call
        uint8_t count = 0;        // commands left in a call
    };

    // A track's next step after a command: read the delay that follows (after counting the command if a call played
    // it), read it without counting the command, stop until the next frame, or loop the song.
    enum class Next
    {
        kReadDelay,
        kReadDelayUncounted,
        kStop,
        kRestart
    };

    // Runs command `c` of `track`, whose read position is `pos`.
    Next RunCommand(int track, const Command& c, uint32_t& pos);

    // Applies vibrato and PSG attack/decay, then outputs the volume after processing a running track's commands.
    void PerFrame(int track);

    Revision revision_;
    bool eternal_duelist_; // the Eternal Duelist revision
    bool rave_master_;     // the Rave Master revision, or the Eternal Duelist one, which shares its differences
    std::array<Track, kTracks> tracks_{};
};

// Dungeon Dice Monsters sequencer. Pitches index the PSG frequency table (which also holds noise settings) or the
// sample period table, in 1/16 semitones. Playback matches a song requested without a fade-in: master volume stays at
// 16.
class DungeonDiceSequencer : public Sequencer
{
public:
    DungeonDiceSequencer(const Rom& rom, const DriverInfo& info, int song);

protected:
    Result StepTrack(int track) override;
    void ResetTracks() override;
    bool TrackActive(int track) const override;
    uint16_t TrackStart(int track) const override;
    int16_t NotePitch(int track) const override;

private:
    // The state of one of the song's tracks.
    struct Track
    {
        uint8_t flags = 0;        // 0x80 active, 0x40 restart on loop, 0x20 vibrato, 0x04 fade, 0x01 started
        uint16_t start = 0;       // track offset (moved by FA and by calls)
        uint16_t pos = 0;         // read position relative to start
        uint16_t delay = 0;       // frames until the next command
        int16_t pitch = 0;        // an entry of the PSG frequency or sample period table, or a period after F1 or F2
        uint8_t b2 = 0;           // duty byte, wave or sample
        uint8_t vol = 0;          // 0..15
        uint8_t note = 0;         // the note of the last note command, 0 after a sample map note
        uint8_t vib_depth = 0;    // entries of the frequency table's vibrato part
        uint8_t vib_count = 0;    // frames of vibrato, counted on from the song's start
        uint8_t fade = 0;         // frames of a fade left
        uint8_t fade_from = 0;    // the volume a fade started from
        uint16_t ret = 0;         // position to return to after a call, 0 if there's none
        uint16_t saved_start = 0; // start to return to after a call
        uint8_t count = 0;        // commands left in a call
    };

    // A track's next step after a command: read the delay that follows (after counting the command if a call played
    // it), read it without counting the command, stop until the next frame, or loop the song.
    enum class Next
    {
        kReadDelay,
        kReadDelayUncounted,
        kStop,
        kRestart
    };

    // Runs command `c` of `track`, whose read position is `pos`.
    Next RunCommand(int track, const Command& c, uint32_t& pos);

    // Runs a duty or wave command (00-7F). F5 uses the same handler but can pass any byte up to FF.
    void DutyOrWave(int track, uint8_t op);

    // Returns the next track, which some commands modify. On the last track, warns and returns nullptr: the driver
    // would write past the track array into its own variables.
    Track* NextTrack(int track, const Command& c);

    // Applies fades and vibrato, then outputs the volume. Runs even after a track has ended.
    void PerFrame(int track);

    uint32_t sample_map_;     // DriverInfo::sample_map
    uint32_t sample_periods_; // DriverInfo::sample_period_table
    int vibrato_entry_;       // DriverInfo::vibrato_entry
    std::array<Track, kTracks> tracks_{};
};

} // namespace agbsf2::konami
