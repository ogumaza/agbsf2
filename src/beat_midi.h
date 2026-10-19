// SPDX-License-Identifier: MIT

// The MIDI timing of a song whose beat a BeatGrid works out: the stretch of frames that a conversion covers, the ticks
// of the frames, and the conductor track.

#pragma once

#include <cstdint>
#include <string>

#include "beat_grid.h"
#include "midi.h"

namespace agbsf2
{

// The stretch of a song that a conversion covers, in frames: its loop, if it loops, and its end.
struct FramePlan
{
    bool loops = false;
    uint32_t loop_start = 0; // the frame that the loop starts on
    uint32_t loop_end = 0;   // the frame on which the song goes back to the loop's start
    uint32_t end = 0;        // the frame on which the conversion ends
};

// The MIDI ticks to a quarter note of a song timed by a BeatGrid. Several fall in each frame at the tempos that songs
// have. A change within a frame, such as a step of square 1's frequency sweep, then gets a tick of its own.
constexpr uint16_t kBeatQuarterTicks = 480;

// Returns the MIDI tick of a place `quarters` quarter notes into a song, or of the song's start for a place before it.
uint32_t QuarterTick(double quarters);

// The MIDI ticks of a song's frames: on the song's beat, as a BeatGrid works it out, or with frame timing, on the
// frames the driver plays them in.
class BeatTicks
{
public:
    // Keeps a reference to `grid`, which has to outlive this.
    BeatTicks(const BeatGrid& grid, bool frame_timing);

    // Returns the tick of a place in the song on `frame`, such as its end: on the beat if it's near it.
    uint32_t Song(double frame) const;

    // Returns the tick of a marker on `frame`, such as a loop's start. It comes no later than any channel's events from
    // the frame on.
    uint32_t Marker(double frame) const;

    // Returns the tick of channel `c`'s event on `frame`, with the channel's notes moved onto the beat, and no later
    // than `end`.
    uint32_t Channel(int c, double frame, uint32_t end) const;

    // Returns the grid that times the song.
    const BeatGrid& Grid() const
    {
        return grid_;
    }

private:
    const BeatGrid& grid_;
    bool frame_timing_;
};

// Writes a conductor track: the title, the description `about`, a time signature of 4/4 and the tempo map of the grid
// that `ticks` follows. A song whose `plan` loops gets the loop's markers, and the tempo at the loop's start again
// after the marker. The loop's end can have another tempo. Without the repeat, a player that keeps its tempo when it
// jumps back would play the loop's start at that tempo. The track ends at tick `end`.
void WriteBeatConductor(MidiTrack& conductor, const std::string& title, const std::string& about,
                        const BeatTicks& ticks, const FramePlan& plan, uint32_t end);

} // namespace agbsf2
