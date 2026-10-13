// SPDX-License-Identifier: MIT

// The beat of a song whose driver counts time in frames, worked out from the frames its notes start on.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace agbsf2
{

// Estimates the beat for drivers that store note lengths in whole frames. The composer's tools rounded those lengths: a
// 16th note of 6.5 frames becomes 6 or 7, leaving notes about a frame either side of the beat. BeatGrid finds steady
// passages from note lengths and follows passages whose tempo changes from note to note. It maps frames to quarter
// notes and moves each channel's notes onto the beat.
class BeatGrid
{
public:
    // A tempo: from `quarter` quarter notes into the song, a quarter note lasts `frames` frames.
    struct Tempo
    {
        double quarter = 0;
        double frames = 0;
    };

    // Works out the beat from each channel's events: the frames on which its notes, rests and holds start, in order.
    // The song's first beat is the first event. `hints` are frames at which the tempo may change, such as where the
    // channels switch to other note lengths, or loop back.
    BeatGrid(const std::vector<std::vector<uint32_t>>& channels, std::vector<uint32_t> hints);

    // Returns the place of `frame` in quarter notes from the song's first beat, on the tempo map.
    double Quarters(double frame) const;

    // Returns the place of `frame` on channel `c`, with the channel's notes moved onto the beat.
    double Quarters(int c, double frame) const;

    // Returns the place of `frame` as Quarters() does, moved onto the beat if it's near it.
    double Snap(double frame) const;

    // Returns the place of a marker on `frame`, such as a loop's start or end: the earliest of Snap(frame) and the
    // places of each channel's first event on the frame or after it, so that no event from the frame on comes before
    // the marker.
    double Marker(double frame) const;

    // Returns the tempo map: the tempo from the song's first beat, and each change.
    std::vector<Tempo> Tempos() const;

private:
    // A stretch of the song with a steady beat. It starts on `frame`, `quarter` quarter notes into the song, and lasts
    // `units` units of `unit` frames, `per_quarter` of them to a quarter note, or goes on if it's the last. Its events
    // are moved onto the beat if `snap` is set.
    struct Stretch
    {
        double frame = 0;
        double quarter = 0;
        double unit = 0;
        double per_quarter = 1;
        double units = 1;
        bool snap = false;
    };

    // A channel's event that's on the beat: its frame, and the frames it moves by to be on the beat.
    struct Anchor
    {
        double frame = 0;
        double offset = 0;
    };

    // An event's place on the beat, if it's near enough to it: the frame it moves to, and how far it is in frames from
    // the nearest of its stretch's units, which last `unit` frames, whether or not it's near enough. `free` is set if
    // the song has no beat there.
    struct Place
    {
        bool found = false;
        bool free = false;
        double frame = 0;
        double error = 0;
        double unit = 0;
    };

    // A channel's event with its ornaments, such as the quick notes of a chord played as an arpeggio, which belong to
    // the beat it starts on. It starts on `frame` and lasts until the next one, or has a length of 0 if it's the last.
    struct Group
    {
        uint32_t frame = 0;
        uint32_t length = 0;
    };

    // A part of the song between changes of tempo, from frame `from` to `to`, with the unit of its beat if it's steady.
    struct Part
    {
        uint32_t from = 0;
        uint32_t to = 0;
        double unit = 0;
        bool steady = false;
    };

    // Returns the events of a channel, from their frames in order, with their ornaments.
    static std::vector<Group> Groups(std::vector<uint32_t> frames);

    // Returns the units to a quarter note, a power of 2, that make a quarter note of `unit` frames nearest `quarter`.
    static double PerQuarter(double unit, double quarter);

    // Returns the unit of a part's steady beat, near `unit`, that best fits the places of the events of `groups`.
    static double FitBeat(const std::vector<std::vector<Group>>& groups, const Part& part, double unit);

    // Adds a stretch on `frame` of `units` units of `unit` frames, `per_quarter` to a quarter note.
    void Push(double frame, double unit, double per_quarter, double units, bool snap);

    // Returns the frames in a quarter note of the last stretch, or about 120 beats a minute before the first.
    double LastQuarter() const;

    // Returns the units to a quarter note for a beat of `unit` frames: those of the first beat with that unit, so that
    // a beat that comes back keeps its quarter note, or else the ones nearest the last stretch's quarter.
    double PerQuarterFor(double unit) const;

    // Adds the stretches of a part, which `groups` play. The last part's beat goes on after its end.
    void AddPart(const std::vector<std::vector<Group>>& groups, const Part& part, bool last);

    // Adds a steady beat of `unit` frames from frame `from` to its last note on the beat, `on_beat`, and then stretches
    // that follow the notes from there to `to`.
    void AddBeat(const std::vector<std::vector<Group>>& groups, double from, double on_beat, double to, double unit,
                 double per_quarter);

    // Adds stretches from frame `from` to `to` that follow the notes of the channel that agrees most with the others,
    // one for each note, unless that channel has fewer than `fewest`, or with `gradual`, unless its tempo changes
    // gradually. Returns false if it adds none.
    bool FollowNotes(const std::vector<std::vector<Group>>& groups, uint32_t from, uint32_t to, size_t fewest,
                     bool gradual);

    // Rounds the stretches from `start` quarter notes into the song to a whole number of units, `per_quarter` to a
    // quarter note, or of quarter notes if a unit is longer, by changing the last of them, so that the beat after them
    // stays on the song's grid.
    void WholeUnits(double start, double per_quarter);

    // Returns the notes from frame `from` to `to` of the channel whose notes most often start within a frame of another
    // channel's, each lasting until the next or `to`. A channel behind the beat, such as an echo's, agrees with none of
    // the others.
    static std::vector<Group> ReferenceNotes(const std::vector<std::vector<Group>>& groups, uint32_t from, uint32_t to);

    const Stretch& StretchAt(double frame) const;

    // Returns the place on the beat of an event on `frame`, for a channel `delay` frames behind the beat, if the event
    // is near the beat and the place is after frame `after`. It tries a quarter note, half of one, and so on down to a
    // 32nd note, the longest first, then their triplets.
    Place Nearest(double frame, double delay, double after) const;

    // Finds the events of channel `c` that are on the beat, from its `groups`.
    void PlaceChannel(int c, const std::vector<Group>& groups);

    std::vector<Stretch> stretches_;
    std::vector<std::vector<Anchor>> anchors_;
    std::vector<std::vector<uint32_t>> frames_; // each channel's events, in order
    double quarter_ = 0;                        // where the next stretch starts, in quarter notes

    // The beats so far, as their units and their units to a quarter note: each steady part's, and the first unit of
    // each passage whose notes set the tempo one by one. The stretches of such a passage have its notes' lengths, and
    // one that had a later beat's unit by chance would give that beat its quarter note.
    std::vector<std::pair<double, double>> beats_;
};

} // namespace agbsf2
