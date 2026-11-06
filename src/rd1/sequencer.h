// SPDX-License-Identifier: MIT
// A frame-by-frame model of a player of Nintendo R&D1's MIDI-file sound driver: its tracks, its channels and their
// LFOs, as the driver keeps them.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rd1/driver.h"
#include "rd1/song.h"
#include "rom.h"

namespace agbsf2::rd1
{

constexpr int kChannelSize = 32;

// A channel's record, byte for byte as the driver keeps it. The accessors name its fields.
struct ChannelRecord
{
    std::array<uint8_t, kChannelSize> bytes = {};

    uint32_t Field(int offset, int bit, int width) const;
    void SetField(int offset, int bit, int width, uint32_t value);

    int Program() const;    // u16 +0 bits 2-8
    int Volume() const;     // CC7: u16 +2 bits 7-13
    int Pan() const;        // CC10: +4 bits 0-6
    int Expression() const; // CC11: u16 +4 bits 7-13
    int Modulation() const; // CC1: u32 +4 bits 14-20
    int LfoTarget() const;  // CC22: +7 bits 4-5; 0 pitch, 1 volume, 2 pan
    int Bend() const;       // u16 +8 bits 0-13; 0x2000 is the centre
    int ModDepth() const;   // +0xC: the semitones that a full LFO swing bends the pitch by
    int Swing() const;      // +0xD, signed: the LFO's swing
    int BendRange() const;  // CC20: +0xF, in semitones
};

// A track's state, as the driver keeps it.
struct TrackState
{
    bool playing = false;      // +0 bit 0
    bool loop_playing = false; // +0 bit 1: `playing` where the loop starts
    int status = 0;            // u16 +0 bits 2-9: the running status
    int loop_status = 0;       // u32 +0 bits 10-17
    int frame_status = 0;      // u16 +2 bits 2-9: the running status at the frame's start
    uint32_t start = 0;        // +4: the track's first event
    uint32_t at = 0;           // +8: the next event
    uint32_t wait = 0;         // +0xC: 256ths of a tick to the next event
    uint32_t loop_at = 0;      // +0x10, +0x14: `at` and `wait` where the loop starts
    uint32_t loop_wait = 0;
    uint32_t frame_at = 0; // +0x18, +0x1C: `at` and `wait` at the frame's start
    uint32_t frame_wait = 0;
};

// What the player did in a frame that the conversion writes.
struct SeqEvent
{
    enum class Kind
    {
        kNoteOn,  // a note-on with a velocity above 0
        kNoteOff, // a note-off, or a note-on with velocity 0
        kProgram,
        kLoopStart,
        kLoopEnd,
        kTrackEnd,
        kTempo,   // a new step in `value`: 256ths of a tick a frame
        kSetting, // a controller or bend that changes what a channel's notes sound like
    };
    Kind kind = Kind::kNoteOn;
    int channel = 0; // the channel: the track's number for notes and the other events
    int key = 0;
    int velocity = 0;
    int value = 0;     // the program
    uint64_t tick = 0; // the MIDI file's tick that the event is at, counting every pass through the loop
};

// One player of the driver playing one song alone.
class Sequencer
{
public:
    // Starts `record` on its player, as the driver's start routine does.
    Sequencer(const Rom& rom, const DriverInfo& info, const SongRecord& record);

    // Returns false if the song's MIDI file can't be read. Error() then says why.
    bool Valid() const;
    const std::string& Error() const;

    // Runs a frame: the fade, the tracks' events that are due, the loop, and each channel's LFO and level. Returns what
    // the frame did.
    const std::vector<SeqEvent>& Step();

    // Returns true while a track of the song plays.
    bool Playing() const;

    // The state after the last Step().
    int TrackCount() const;
    int ChannelCount() const;
    int FadeLevel() const;
    int FadeStep() const;
    int FadeMode() const;
    const TrackState& Track(int t) const;
    const ChannelRecord& Channel(int c) const;
    int Bpm() const;
    uint32_t TickStep() const; // 256ths of a tick that each frame moves the tracks on by
    int Division() const;
    int ChannelLevel(int c) const; // u32 +8 from bit 14: the level that the LFO routine works out each frame
    int BusPan() const;
    uint64_t Position() const; // the frame's place at its end, in 256ths of the MIDI file's ticks, counting every pass
    int LoopPasses() const;    // the times the player has gone back to the loop's start
    const std::vector<std::string>& Warnings() const;

private:
    // Runs track t's events that are due this frame. Returns false once the track has ended.
    void RunTrack(int t);
    // Runs the event at track t's position. Returns true at the track's end.
    bool RunEvent(int t);
    void RunMeta(int t, uint32_t& at);
    void RunSysEx(uint32_t at);
    void Control(int t, int controller, int value);
    void FlushNotes(int t);
    void ResetChannel(ChannelRecord& c);
    void SetPriority(ChannelRecord& c, int value) const;
    void RunLfo(ChannelRecord& c);
    void RunFade();

    const Rom& rom_;
    const DriverInfo& info_;
    SongRecord record_;
    SongFile file_;
    std::string error_;
    bool valid_ = false;

    std::vector<TrackState> tracks_;
    std::vector<ChannelRecord> channels_;
    std::array<int8_t, 12> key_table_ = {};
    int bus_volume_ = 100;
    int bus_pan_ = 0;
    int priority_ = 0;
    int bpm_ = 0;
    uint32_t step_ = 1;
    uint32_t next_step_ = 0;
    int speed_ = 0x100;
    int volume_ = 0x100;
    int fade_mode_ = 0;
    int fade_level_ = 0x8000;
    int fade_step_ = 0;
    bool loop_armed_ = false;
    bool loop_start_seen_ = false;
    bool loop_end_seen_ = false;
    bool song_on_ = true;
    int loop_passes_ = 0;

    struct QueuedNote
    {
        int channel;
        int key;
        int velocity;
    };
    std::vector<QueuedNote> queue_;

    // Ticks: each track's tick within the file, and the pass offset that the loop adds.
    std::vector<uint64_t> track_ticks_;
    std::vector<uint64_t> loop_ticks_;
    std::vector<uint64_t> frame_ticks_;
    uint64_t pass_offset_ = 0;
    uint64_t loop_start_tick_ = 0; // the MIDI file's ticks of the loop's markers
    uint64_t loop_end_tick_ = 0;
    uint64_t position_ = 0;

    std::vector<SeqEvent> events_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::rd1
