// SPDX-License-Identifier: MIT

// agbsf2's names for seqlib's MIDI file builder, the drum channel, the pitch bend ranges and values that the drivers
// share, and the writer of a channel's track that some drivers share.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "seqlib/midi/builder.h"

#include "song_banks.h"

namespace agbsf2
{

namespace cc = seqlib::midi::cc;

using seqlib::midi::LevelsToControllers;
using MidiFile = seqlib::midi::FileBuilder;
using MidiTrack = seqlib::midi::TrackBuilder;

// The MIDI channel that General MIDI players keep for drums.
constexpr int kDrumChannel = 9;

// Returns channel `n` of the MIDI channels other than the drum channel, from 0: channels 0-8, and then 10 on.
constexpr int MelodicChannel(int n)
{
    return n < kDrumChannel ? n : n + 1;
}

// The widest pitch bend range that a track gets, in semitones.
constexpr int kMaxBendRange = 96;

// The widest bend that a track can leave out, in semitones: half a cent.
constexpr double kSmallBend = 0.005;

// Returns the pitch bend range that covers a track's bends of up to `widest` semitones either way: at least 2
// semitones and at most kMaxBendRange. It's 0 when no bend is wider than `smallest`. The track then writes no bends.
inline int BendRangeFor(double widest, double smallest)
{
    return widest > smallest ? std::clamp(int(std::ceil(widest - 1e-9)), 2, kMaxBendRange) : 0;
}

// Returns the pitch bend value for `bend` semitones with a bend range of `range`.
inline int BendValue(double bend, int range)
{
    return std::clamp(8192 + int(std::lround(bend / range * 8192)), 0, 16383);
}

// Returns the pitch bend value for a pitch offset of `offset` steps, `semitone` of which make a semitone, with a bend
// range of `range` semitones, at least 1. The value is rounded to the nearest, with halves away from 0.
inline int FixedBendValue(int64_t offset, int64_t semitone, int range)
{
    const int64_t scaled = offset * 8192;
    const int64_t unit = int64_t(range) * semitone;
    const int64_t steps = scaled >= 0 ? (scaled + unit / 2) / unit : -((-scaled + unit / 2) / unit);
    return int(std::clamp<int64_t>(8192 + steps, 0, 16383));
}

// Returns the bank and program of the preset numbered `number`. Each bank holds 128 presets.
constexpr BankProgram NumberedPreset(int number)
{
    return {number / 128, number % 128};
}

// Writes a channel's track for a driver whose notes give their program, and their levels and bend frame by frame. The
// track starts with the first note's program, full volume, the centre pan, no expression, reverb or chorus, and, with a
// bend range, no bend. After that, the writer writes each setting where it changes. A player that jumps back to the
// loop's start keeps the settings that the loop's end left. The writer writes each setting again from the first tick
// that it reaches at or after the loop's start. SetProgram(), SetLevels() and SetBend() call Reach() with their tick
// first.
class ChannelWriter
{
public:
    // Starts `track` on MIDI channel `channel` with program `program`. `preset` is the bank and program number that
    // the program plays. `range` is the bend range in semitones, or 0 for a track without bends. `loop_start` is the
    // tick of the loop's start, or 0 if the song doesn't loop or loops from its start.
    ChannelWriter(MidiTrack& track, int channel, int program, BankProgram preset, int range, uint32_t loop_start);

    // Reaches tick `at`. On the first call at or after the loop's start, the writer clears its record of the settings.
    void Reach(uint32_t at);

    // Changes to program `program` at tick `at` if it's another program. `preset` is the bank and program number that
    // the program plays. The bank select comes first if the bank changes too.
    void SetProgram(uint32_t at, int program, BankProgram preset);

    // Sets the pan and expression at tick `at` to those that the levels `left` and `right` give.
    void SetLevels(uint32_t at, double left, double right);

    // Sets the bend at tick `at` to `semitones` if the track has a bend range.
    void SetBend(uint32_t at, double semitones);

    // Writes a note on `key` at full velocity from tick `on` to tick `off`. The expression gives the note's loudness.
    void Note(uint32_t on, uint32_t off, int key);

private:
    MidiTrack& track_;
    int channel_;
    int range_;
    uint32_t loop_start_;
    bool before_loop_;
    int program_; // -1 from the loop's start to the next SetProgram()
    int bank_;    // the bank of `program_`'s preset
    int pan_ = 64;
    int expression_ = 0;
    int bend_ = 8192;
};

} // namespace agbsf2
