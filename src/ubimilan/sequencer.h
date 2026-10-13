// SPDX-License-Identifier: MIT

// A model of Ubisoft Milan's driver as it plays a piece of music: the sequencer, the mixer's voices that play the kit's
// samples, and the PSG's notes.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{

// A write to one of the sound registers.
struct RegisterWrite
{
    uint32_t address = 0;
    uint8_t size = 2;
    uint32_t value = 0;
};

// A note as the driver plays it.
struct Note
{
    int channel = 0;      // the sequence channel
    int key = 0;          // the sequence's key
    int velocity = 0;     // the sequence's velocity
    int program = 0;      // channel 9's offset into its kit, or a PSG channel's instrument
    uint32_t on = 0;      // the frame it starts on
    uint32_t off = 0;     // the frame its sound stops on, if `ended`
    bool ended = false;   // false while it still sounds
    uint32_t sample = 0;  // a kit note's sample resource
    uint8_t duty = 0;     // a square note's NRx1, whose top 2 bits are the duty
    uint8_t envelope = 0; // a PSG note's NRx2: its starting volume, its envelope's direction and its step
    uint16_t control = 0; // a noise note's NR43, or a wave note's program
};

// The driver playing one piece of music, frame by frame.
class Sequencer
{
public:
    // Keeps references to `rom` and `info`, which have to outlive this. Starts song `song` as the game does.
    Sequencer(const Rom& rom, const DriverInfo& info, int song);

    // Returns false if the song can't be played: its list of sequences or its first sequence isn't in the ROM.
    bool Valid() const
    {
        return valid_;
    }

    // Runs a frame as the game does: the sequencer's step at VBlank, the game's sound update in its main loop, then the
    // mixer's runs until the next frame. Returns the frame's writes to the PSG's registers, and for the first frame,
    // the writes that the piece's start made.
    std::vector<RegisterWrite> Step();

    // Returns true while the sequencer plays, or a voice does.
    bool Playing() const;

    // Returns the frames run so far.
    uint32_t Frame() const
    {
        return frame_;
    }

    // Returns the notes started so far, in the order they started.
    const std::vector<Note>& Notes() const
    {
        return notes_;
    }

    // Returns the frames on which each sequence of the piece started, in order, the first on frame 0.
    const std::vector<uint32_t>& SegmentStarts() const
    {
        return segment_starts_;
    }

    // Returns the problems found in the song's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

    // The track as the driver keeps it: the countdown, the next command, the next sequence, whether it plays and
    // whether an end command has been read, as driver_emu.py's trace prints them.
    std::array<uint32_t, 5> TrackState() const;

    // Returns false if the mixer doesn't play voice `v`.
    bool VoicePlays(int v) const
    {
        return !voices_[size_t(v)].stopped;
    }

    // Voice `v` as the mixer keeps it: its next point, its loop's start, its end, its loop's end, its volume and
    // whether it loops, as driver_emu.py's trace prints them.
    std::array<uint32_t, 6> VoiceState(int v) const;

private:
    // A voice of the mixer, and the driver's note of whether a sound has it.
    struct Voice
    {
        bool busy = false;   // a sound has it
        bool stopped = true; // the mixer doesn't play it
        uint32_t position = 0;
        uint32_t end = 0;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        uint32_t volume = 0;
        int note = -1; // the note it plays, or -1
    };

    // A PSG channel's state.
    struct PsgChannel
    {
        int program = 0;
        int key = 0;     // the key's place in the frequency table that its last note set
        bool on = false; // the driver's flag that a note plays
        int note = -1;   // the note that sounds, or -1
    };

    void SequencerStep();
    void SoundUpdate();
    void MixerRun();
    void Event(int status);
    void NoteOn(int channel, int key, int velocity);
    void NoteOff(int channel, int key);
    void KitNoteOn(int key, int velocity);
    void KitNoteOff();
    void PsgNoteOn(int channel, int key, int velocity);
    void PsgNoteOff(int channel, int key);
    void Program(int channel, int program);
    void EndNote(int index, uint32_t frame);
    void Stop(const std::string& why);
    void Warn(const std::string& message);
    uint32_t NextSegment(int ahead) const;
    void Write(uint32_t address, uint8_t size, uint32_t value);

    const Rom& rom_;
    const DriverInfo& info_;
    bool valid_ = false;
    MusicPiece piece_;
    uint32_t frame_ = 0;
    uint64_t runs_ = 0; // the mixer's runs so far

    // The track.
    uint32_t wait_ = 0;
    uint32_t pointer_ = 0;
    uint32_t next_ = 0; // the next sequence's countdown and commands, or 0
    bool active_ = false;
    bool playing_ = false; // the piece plays: the game's flag, which its callback clears once the track stops
    bool ending_ = false;  // an end command was read
    bool changed_ = false; // a sequence ended, which the game's sound update notes
    int index_ = 0;        // the sequence of the piece that plays

    // The kit, its voices and the PSG.
    int kit_program_ = 0;
    bool kit_voice_fixed_ = false;
    int kit_voice_ = 0;
    std::array<Voice, kVoiceCount> voices_ = {};
    std::array<PsgChannel, kPsgChannels> psg_ = {};

    std::vector<Note> notes_;
    std::vector<uint32_t> segment_starts_;
    std::vector<RegisterWrite> writes_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::ubimilan
