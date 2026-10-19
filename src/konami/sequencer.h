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

// One track's requests to its sound channel in one frame. This holds the driver's per-track output record and the
// note's base pitch. The record has 12 bytes, or 8 in the Eternal Duelist revision.
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

// The Eternal Duelist revision's flag for a bend or a vibrato step. It replaces every other flag in the record, even
// kOutNoteOn.
constexpr uint8_t kOutEternalDuelistPitch = 0x01;

// The Dungeon Dice Monsters revision's flags.
constexpr uint8_t kOutDungeonDiceNote = 0x01;   // a note, a rest or a pitch for the channel or the voice
constexpr uint8_t kOutDungeonDicePeriod = 0x40; // a sample track's pitch: a timer period for the voice's FIFO

// The trig byte that a stop or the end of a track writes, in every revision but Dungeon Dice Monsters.
constexpr uint8_t kTrigStop = 0x40;

// An echo bus: its settings and the voices routed to it.
struct EchoBus
{
    int feedback = 0;    // F7 value (0..127), feedback = value / 256
    int delay = 0;       // F8 value, delay = value * 32 mixer samples
    uint16_t voices = 0; // bit v: DirectSound voice v (track 4 + v) feeds this bus
};

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

    // Returns true if another of the song's tracks follows `track`. Otherwise it returns false and adds a warning that
    // command `c` is ignored. On the last track, `c` would write into the driver's variables.
    bool HasNextTrack(int track, const Command& c);

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

// The bits of a track's flags in the driver's track records.
constexpr uint8_t kTrackOn = 0x80;       // the track runs
constexpr uint8_t kTrackRestarts = 0x40; // a song loop runs the track again
constexpr uint8_t kTrackVibrato = 0x20;  // vibrato is on
constexpr uint8_t kTrackFade = 0x04;     // Dungeon Dice Monsters: the track fades out
constexpr uint8_t kTrackStarted = 0x01;  // the track has had its first frame

// A sequencer that keeps the state of each track in a `TrackState`. The constructor of each derived class calls
// StartTracks() and then ResetTracks(). This class's constructor can't call ResetTracks(): the derived class's members
// aren't set while it runs.
template <typename TrackState>
class TrackSequencer : public Sequencer
{
protected:
    using Track = TrackState;

    // A track's next step after a command: read the delay that follows (after counting the command if a call played
    // it), read it without counting the command, stop until the next frame, or loop the song.
    enum class Next
    {
        kReadDelay,
        kReadDelayUncounted,
        kStop,
        kRestart
    };

    TrackSequencer(const Rom& rom, const DriverInfo& info, int song) : Sequencer(rom, info, song)
    {
    }

    bool TrackActive(int track) const override
    {
        return tracks_[track].flags & kTrackOn;
    }

    uint16_t TrackStart(int track) const override
    {
        return tracks_[track].start;
    }

    int16_t NotePitch(int track) const override
    {
        return tracks_[track].pitch;
    }

    // Starts each of the song's tracks active, restartable by a song loop and at its offset in the song's header.
    void StartTracks()
    {
        for (int t = 0; t < header_.tracks; t++)
        {
            tracks_[t].flags = kTrackOn | kTrackRestarts;
            tracks_[t].start = header_.offsets[t];
        }
    }

    // Runs a frame of `track`, a track that's on. Revision `revision` gives the format of its data. On the track's
    // first frame, FirstFrame() runs and the delay before the first command is read. On a later frame, the delay counts
    // down. Once a delay is done, the commands up to the next delay run. Unless the track stops or the song loops,
    // PerFrame() runs last.
    Result RunTrack(int track, Revision revision);

    std::array<Track, kTracks> tracks_{};

private:
    // Outputs what track `t` plays on its first frame through its output `o`.
    virtual void FirstFrame(Track& t, TrackOutput& o) = 0;

    // Runs command `c` of `track`, whose read position is `pos`.
    virtual Next RunCommand(int track, const Command& c, uint32_t& pos) = 0;

    // Counts a command that a call plays, in a revision with calls. After the call's last command, the track's start
    // and read position go back to those before the call.
    virtual void CountInCall(Track&, uint32_t&)
    {
    }

    // Does the work of a frame of `track` that follows its commands.
    virtual void PerFrame(int track) = 0;
};

// The state of one of a song's tracks in the Ultimate Masters revision.
struct UltimateMastersTrack
{
    uint8_t flags = 0;  // kTrack* bits
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

// The sequencer of the Ultimate Masters revision.
class UltimateMastersSequencer : public TrackSequencer<UltimateMastersTrack>
{
public:
    UltimateMastersSequencer(const Rom& rom, const DriverInfo& info, int song);

protected:
    Result StepTrack(int track) override;
    void ResetTracks() override;

private:
    // Outputs a rest.
    void FirstFrame(Track& t, TrackOutput& o) override;

    // Runs command `c` of `track`, whose read position is `pos`. The revision has no calls to count commands in.
    Next RunCommand(int track, const Command& c, uint32_t& pos) override;

    // Applies vibrato and outputs the volume after processing a running track's commands.
    void PerFrame(int track) override;

    bool bend_kept_ = true; // DriverInfo::bend_kept
};

// The state of one of a song's tracks in the WCT 2004, Rave Master and Eternal Duelist revisions.
struct Wct2004Track
{
    uint8_t flags = 0;  // kTrack* bits
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

// Sequencer for WCT 2004 and the older Rave Master and Eternal Duelist revisions, with their individual differences.
class Wct2004Sequencer : public TrackSequencer<Wct2004Track>
{
public:
    Wct2004Sequencer(const Rom& rom, const DriverInfo& info, int song);

protected:
    Result StepTrack(int track) override;
    void ResetTracks() override;

private:
    // Clears the PSG's attack and decay countdowns and outputs a rest.
    void FirstFrame(Track& t, TrackOutput& o) override;

    // Runs command `c` of `track`, whose read position is `pos`.
    Next RunCommand(int track, const Command& c, uint32_t& pos) override;

    // Eternal Duelist keeps the return position after a call's last command.
    void CountInCall(Track& t, uint32_t& pos) override;

    // Applies vibrato and PSG attack/decay, then outputs the volume after processing a running track's commands.
    void PerFrame(int track) override;

    Revision revision_;
    bool eternal_duelist_; // the Eternal Duelist revision
};

// The state of one of a song's tracks in the Dungeon Dice Monsters revision.
struct DungeonDiceTrack
{
    uint8_t flags = 0;        // kTrack* bits, kTrackFade among them
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

// Dungeon Dice Monsters sequencer. Pitches index the PSG frequency table (which also holds noise settings) or the
// sample period table, in 1/16 semitones. Playback matches a song requested without a fade-in: master volume stays at
// 16.
class DungeonDiceSequencer : public TrackSequencer<DungeonDiceTrack>
{
public:
    DungeonDiceSequencer(const Rom& rom, const DriverInfo& info, int song);

protected:
    Result StepTrack(int track) override;
    void ResetTracks() override;

private:
    // Outputs a rest at volume 0.
    void FirstFrame(Track& t, TrackOutput& o) override;

    // Runs command `c` of `track`, whose read position is `pos`.
    Next RunCommand(int track, const Command& c, uint32_t& pos) override;

    // The driver keeps the return position after a call's last command.
    void CountInCall(Track& t, uint32_t& pos) override;

    // Runs a duty or wave command (00-7F). F5 uses the same handler but can pass any byte up to FF.
    void RunDutyOrWave(int track, uint8_t op);

    // Returns the next track, which some commands modify. On the last track, warns and returns nullptr: the driver
    // would write past the track array into its own variables.
    Track* NextTrack(int track, const Command& c);

    // Applies fades and vibrato, then outputs the volume. Runs even after a track has ended.
    void PerFrame(int track) override;

    uint32_t sample_map_;     // DriverInfo::sample_map
    uint32_t sample_periods_; // DriverInfo::sample_period_table
    int vibrato_entry_;       // DriverInfo::vibrato_entry
};

} // namespace agbsf2::konami
