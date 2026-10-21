// SPDX-License-Identifier: MIT

// A model of Ubisoft Milan's driver as it plays a piece of music: the sequencer, the mixer's voices that play the kit's
// samples, and the PSG's notes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gba.h"
#include "rom.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{

// The longest that a song plays: an hour. The model stops there, and so does a conversion.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

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
    uint8_t envelope = 0; // the volume: a square's or the noise's NRx2, with its envelope, or the wave's NR32
    uint8_t control = 0;  // a noise note's NR43
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

    // Returns the piece of music that the sequencer read when it started the song.
    const MusicPiece& Piece() const
    {
        return piece_;
    }

    // Returns the frames on which each sequence of the piece started, in order, the first on frame 0.
    const std::vector<uint32_t>& SequenceStarts() const
    {
        return sequence_starts_;
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
        int index = 0; // the place in the frequency table of its last note's key
        int note = -1; // the note that sounds, or -1
    };

    void SequencerStep();
    void SoundUpdate();
    void MixerRun();
    void RunCommand(int status);
    void NoteOn(int channel, int key, int velocity);
    void NoteOff(int channel, int key);
    void KitNoteOn(int key, int velocity);
    void KitNoteOff();
    void NoiseNoteOn(int key, int velocity);
    void NoiseNoteOff(int key);
    void PsgNoteOn(int channel, int key, int velocity);
    void PsgNoteOff(int channel, int key);
    void Program(int channel, int program);
    Note NewNote(int channel, int key, int velocity, int program) const;
    void EndNote(int index, uint32_t frame);
    void Stop(const std::string& why);
    void Warn(const std::string& message);
    uint32_t NextSequence(int ahead) const;
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

    // The next sequence's countdown and commands, or 0. From an end command to the sound update, it holds the address
    // of the resource that the game's callback gave the track.
    uint32_t next_ = 0;

    bool active_ = false;

    // The game's flag that the piece plays. The game's callback clears it once the track stops.
    bool piece_playing_ = false;

    bool ending_ = false; // an end command was read

    // A sequence ended. The game's sound update notes it.
    bool changed_ = false;

    int index_ = 0; // the sequence of the piece that plays

    // The kit, its voices and the PSG.
    int kit_program_ = 0;
    bool kit_voice_fixed_ = false;
    int kit_voice_ = 0;
    std::array<Voice, kVoiceCount> voices_ = {};
    std::array<PsgChannel, kPsgChannels> psg_ = {};

    std::vector<Note> notes_;
    std::vector<uint32_t> sequence_starts_;
    std::vector<RegisterWrite> writes_;
    std::vector<std::string> warnings_;
};

} // namespace agbsf2::ubimilan
