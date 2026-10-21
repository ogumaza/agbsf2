// SPDX-License-Identifier: MIT

#include "beat_grid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace agbsf2
{
namespace
{

// An event this many frames or fewer after the one before is an ornament of the event that starts its run, such as a
// quick note of an arpeggiated chord. A channel's last event is never an ornament.
constexpr uint32_t kOrnamentFrames = 2;

// Hints within this many frames of the last tempo change are part of that change, such as an echo channel's, which
// comes a few frames after the others'.
constexpr uint32_t kHintFrames = 16;

// The fewest events whose lengths can give a part's beat, or its tempo from note to note.
constexpr size_t kFewestEvents = 8;

// The share of a part's events whose lengths have to be whole numbers of a unit for the part to have a steady beat.
constexpr double kSteady = 0.9;

// The units a beat can have, in twelfths of a frame: from 3 frames to 60.
constexpr int kUnitSteps = 12;
constexpr int kShortestUnit = 3 * kUnitSteps;
constexpr int kLongestUnit = 60 * kUnitSteps;

// A unit that fits nearly as many lengths as the best, within this share, fits as well, so the longest such unit wins.
constexpr double kFitSlack = 0.015;

// The share of a part's lengths that can be triplets of its unit, rather than whole numbers of it.
constexpr double kTripletShare = 0.1;

// Two parts whose units differ by less than this share have the same tempo.
constexpr double kSameTempo = 0.005;

// The quarter note that the song's first tempo is folded nearest to, in frames: about 120 beats a minute.
constexpr double kTypicalQuarter = 30;

// A part's tempo changes gradually, as in a rubato, if this share of its note lengths are within this ratio of the
// next.
constexpr double kGradualRatio = 1.25;
constexpr double kGradualShare = 0.8;

// The notes at the start whose median length gives the first unit, when the tempo changes from note to note.
constexpr size_t kFirstUnitNotes = 9;

// The events after a change of a channel's delay that have to be on the beat with it, and the notes after one off the
// beat that show whether the beat goes on.
constexpr size_t kDelayCheck = 3;

// The notes in a row that have to be on a beat for it to start again after a change of tempo, such as a ritardando.
// About half of all frames are near a unit of a fine beat, so a few notes can be on it by chance.
constexpr size_t kSettleNotes = 6;

// Returns how far, in frames, a length can be from a whole number of units of `unit` frames and still fit them.
double LengthTolerance(double unit)
{
    return std::min(1.5, unit / 4);
}

// Returns how far, in frames, an event can be from a unit, or from a third of two units, and still be on the beat.
double UnitTolerance(double unit)
{
    return std::min(1.5, unit / 4);
}

double ThirdTolerance(double unit)
{
    return std::min(1.0, unit / 5);
}

// Returns true if `length` is within `tolerance` frames of a whole number of `unit`.
bool Whole(double length, double unit, double tolerance)
{
    const double k = std::round(length / unit);
    return k >= 1 && std::fabs(length - k * unit) <= tolerance;
}

// Returns the share of `lengths`, each with its count, that are whole numbers of `unit`, or with `triplets`, of `unit`
// or two thirds of it.
double Fit(const std::map<uint32_t, int>& lengths, double unit, bool triplets)
{
    const double tolerance = LengthTolerance(unit);
    const double third = unit * 2 / 3;
    int total = 0;
    int fitting = 0;
    for (const auto& [length, count] : lengths)
    {
        total += count;
        const bool whole = Whole(length, unit, tolerance) || (triplets && Whole(length, third, LengthTolerance(third)));
        fitting += whole ? count : 0;
    }

    return total ? double(fitting) / total : 0;
}

// Returns the unit that the lengths fitting `unit` are whole numbers of, by least squares.
double Refine(const std::map<uint32_t, int>& lengths, double unit)
{
    for (int pass = 0; pass < 3; pass++)
    {
        const double tolerance = LengthTolerance(unit);
        double sum = 0;
        double squares = 0;
        for (const auto& [length, count] : lengths)
        {
            const double k = std::round(length / unit);
            if (Whole(length, unit, tolerance))
            {
                sum += count * k * length;
                squares += count * k * k;
            }
        }
        if (squares > 0)
        {
            unit = sum / squares;
        }
    }

    return unit;
}

// Returns the longest unit whose whole multiples fit nearly as many of `lengths` as the best candidate's, and sets
// `fit` to the fraction of lengths that fit. Triplets can make that unit a third of the intended one, as 16th-note
// triplets of 6⅔ frames do alongside 16th notes of 10 frames. So where the unit gives a steady beat, a unit three times
// as long is taken instead if its whole multiples and triplets fit nearly as many lengths, and its whole multiples
// alone fit most of them. `fit` then includes the triplets.
double FindUnit(const std::map<uint32_t, int>& lengths, double& fit)
{
    std::vector<double> straight, any;
    double best_straight = 0;
    double best_any = 0;
    for (int i = kShortestUnit; i <= kLongestUnit; i++)
    {
        straight.push_back(Fit(lengths, double(i) / kUnitSteps, false));
        any.push_back(Fit(lengths, double(i) / kUnitSteps, true));
        best_straight = std::max(best_straight, straight.back());
        best_any = std::max(best_any, any.back());
    }

    int chosen = kShortestUnit;
    for (int i = kLongestUnit; i > kShortestUnit; i--)
    {
        if (straight[size_t(i - kShortestUnit)] >= best_straight - kFitSlack)
        {
            chosen = i;
            break;
        }
    }

    const int tripled = 3 * chosen;
    const bool triplets = straight[size_t(chosen - kShortestUnit)] >= kSteady && tripled <= kLongestUnit &&
                          any[size_t(tripled - kShortestUnit)] >= best_any - kFitSlack &&
                          straight[size_t(tripled - kShortestUnit)] >= best_straight - kTripletShare;
    const double unit = Refine(lengths, double(triplets ? tripled : chosen) / kUnitSteps);
    fit = Fit(lengths, unit, triplets);

    return unit;
}

// Returns the shortest note, in units of `unit` frames, `per_quarter` to a quarter note, that events are moved onto the
// beat of: a 32nd note, unless that's shorter than the shortest unit, or for triplets (`share` 2/3), a 16th note, since
// the triplets of shorter notes are so close together that most events would be near one.
double Finest(double unit, double per_quarter, double share)
{
    const double shortest = per_quarter / (share == 1 ? 8 : 4);
    double level = std::max(1.0, per_quarter);
    while (level / 2 >= shortest && level / 2 * unit >= double(kShortestUnit) / kUnitSteps)
    {
        level /= 2;
    }

    return level;
}

// Returns true if beats of units `a` and `b` frames long have the same tempo: the longer is a whole number of the
// shorter, such as the dotted 8th notes of a part that plays only those, three 16th notes of the part before.
bool SameTempo(double a, double b)
{
    const double ratio = std::max(a, b) / std::min(a, b);
    return std::fabs(ratio / std::round(ratio) - 1) < kSameTempo;
}

// Returns the median of `values`, which mustn't be empty.
double Median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());

    return values[values.size() / 2];
}

// The notes of the channel that agrees most with the others in a part with a steady beat of `unit` frames, by their
// frames: which of them fall on the beat, and where the beat starts again after notes that leave it.
class SteadyNotes
{
public:
    SteadyNotes(std::vector<uint32_t> frames, double unit, double eighth_triplet)
        : frames_(std::move(frames)), unit_(unit), eighth_triplet_(eighth_triplet)
    {
    }

    size_t Count() const
    {
        return frames_.size();
    }

    uint32_t Frame(size_t i) const
    {
        return frames_[i];
    }

    // Returns true if a note `frames` from the start of the beat is on a unit, or on an 8th-note triplet.
    bool OnBeat(double frames) const
    {
        return Whole(frames, unit_, UnitTolerance(unit_)) ||
               (eighth_triplet_ >= unit_ && Whole(frames, eighth_triplet_, ThirdTolerance(eighth_triplet_)));
    }

    // Returns how many of the `count` notes from `first` fall on the beat that starts at frame `start`.
    size_t Settled(size_t first, double start, size_t count) const
    {
        size_t fitting = 0;
        for (size_t j = first; j < frames_.size() && j < first + count; j++)
        {
            fitting += OnBeat(frames_[j] - start) ? 1 : 0;
        }

        return fitting;
    }

    // Returns the total distance, in frames, of the notes from `first` to the nearest whole units measured from
    // `start`.
    double Misfit(size_t first, double start) const
    {
        double distance = 0;
        for (size_t j = first; j < frames_.size() && j < first + kSettleNotes; j++)
        {
            const double units = (frames_[j] - start) / unit_;
            distance += std::fabs(units - std::round(units)) * unit_;
        }

        return distance;
    }

    // Returns the note from which the notes after note `i`, which leaves the beat, settle on a beat again, or the note
    // count if they never do. After a ritardando, notes may also fit a fine grid starting at one of its last notes. Of
    // the next few notes from which the following notes settle on the beat, the one that they fit best is chosen.
    size_t Resettle(size_t i) const
    {
        size_t again = i;
        while (again < frames_.size() && Settled(again + 1, frames_[again], kSettleNotes) < kSettleNotes)
        {
            again++;
        }
        if (again == frames_.size())
        {
            return again;
        }

        double closest = Misfit(again + 1, frames_[again]);
        for (size_t j = again + 1; j < frames_.size() && j <= again + kSettleNotes; j++)
        {
            const double distance = Misfit(j + 1, frames_[j]);
            if (Settled(j + 1, frames_[j], kSettleNotes) == kSettleNotes && distance < closest)
            {
                closest = distance;
                again = j;
            }
        }

        return again;
    }

private:
    std::vector<uint32_t> frames_;
    double unit_;
    double eighth_triplet_;
};

} // namespace

std::vector<uint32_t> BeatGrid::Bounds(std::vector<uint32_t> hints, uint32_t start, uint32_t end)
{
    // Hints close together are one change, on the frame that most of them give, which is the beat's, where one channel
    // may be ahead of it or behind it.
    std::sort(hints.begin(), hints.end());
    std::vector<uint32_t> bounds = {start};
    for (size_t i = 0; i < hints.size();)
    {
        size_t j = i;
        uint32_t frame = hints[i];
        size_t most = 0;
        while (j < hints.size() && hints[j] <= hints[i] + kHintFrames)
        {
            const size_t same =
                size_t(std::upper_bound(hints.begin(), hints.end(), hints[j]) - (hints.begin() + long(j)));
            if (same > most)
            {
                most = same;
                frame = hints[j];
            }
            j += same;
        }
        if (frame > bounds.back() + kHintFrames && frame < end)
        {
            bounds.push_back(frame);
        }
        i = j;
    }
    bounds.push_back(end);

    return bounds;
}

std::vector<BeatGrid::Part> BeatGrid::Parts(const std::vector<std::vector<Group>>& groups,
                                            const std::vector<uint32_t>& bounds)
{
    std::vector<Part> parts;
    for (size_t i = 0; i + 1 < bounds.size(); i++)
    {
        Part part = {bounds[i], bounds[i + 1], 0, false};
        std::map<uint32_t, int> lengths;
        size_t count = 0;
        for (const std::vector<Group>& list : groups)
        {
            for (const Group& g : list)
            {
                if (g.frame >= part.from && g.frame < part.to && g.length > 0)
                {
                    lengths[g.length]++;
                    count++;
                }
            }
        }

        double fit = 0;
        part.unit = count >= kFewestEvents ? FindUnit(lengths, fit) : 0;
        part.steady = fit >= kSteady;

        // A part with the tempo of the part before goes on with its beat, as does a part too short to have a beat of
        // its own that fits that beat. A change of beat within a part shows in its notes (see AddPart()).
        if (!parts.empty() && parts.back().steady &&
            (part.steady ? SameTempo(parts.back().unit, part.unit) : Fit(lengths, parts.back().unit, true) >= kSteady))
        {
            parts.back().to = part.to;
            parts.back().unit = std::min(parts.back().unit, part.steady ? part.unit : parts.back().unit);
            continue;
        }
        parts.push_back(part);
    }

    return parts;
}

BeatGrid::BeatGrid(const std::vector<std::vector<uint32_t>>& channels, std::vector<uint32_t> hints)
    : anchors_(channels.size()), frames_(channels)
{
    // Each channel's events with their ornaments, and the song's first and last event.
    std::vector<std::vector<Group>> groups;
    uint32_t start = std::numeric_limits<uint32_t>::max();
    uint32_t end = 0;
    for (std::vector<uint32_t>& frames : frames_)
    {
        std::sort(frames.begin(), frames.end());
        groups.push_back(Groups(frames));
        if (!frames.empty())
        {
            start = std::min(start, frames.front());
            end = std::max(end, frames.back());
        }
    }

    if (start >= end)
    {
        stretches_.push_back({start > end ? 0 : double(start), 0, kTypicalQuarter, 1, 1, false});
        return;
    }

    const std::vector<Part> parts = Parts(groups, Bounds(std::move(hints), start, end));
    for (size_t i = 0; i < parts.size(); i++)
    {
        AddPart(groups, parts[i], i + 1 == parts.size());
    }

    for (size_t c = 0; c < channels.size(); c++)
    {
        PlaceChannel(int(c), groups[c]);
    }
}

double BeatGrid::Quarters(double frame) const
{
    const Stretch& s = StretchAt(frame);
    return s.quarter + (frame - s.frame) / (s.unit * s.per_quarter);
}

double BeatGrid::Quarters(int c, double frame) const
{
    if (c < 0 || size_t(c) >= anchors_.size() || anchors_[size_t(c)].empty())
    {
        return Quarters(frame);
    }

    // Interpolate each event's timing offset linearly between the channel's on-beat events. This moves the intervening
    // events and changes within notes along with them.
    const std::vector<Anchor>& anchors = anchors_[size_t(c)];
    const auto later = [](double f, const Anchor& a)
    {
        return f < a.frame;
    };
    const auto it = std::upper_bound(anchors.begin(), anchors.end(), frame, later);
    if (it == anchors.begin())
    {
        return Quarters(frame + anchors.front().offset);
    }
    if (it == anchors.end())
    {
        return Quarters(frame + anchors.back().offset);
    }

    const Anchor& a = *(it - 1);
    const Anchor& b = *it;
    return Quarters(frame + a.offset + (frame - a.frame) / (b.frame - a.frame) * (b.offset - a.offset));
}

double BeatGrid::Snap(double frame) const
{
    const Stretch& s = StretchAt(frame);
    const double units = (frame - s.frame) / s.unit;
    const double whole = std::round(units);
    if (!s.snap || std::fabs(units - whole) * s.unit > UnitTolerance(s.unit))
    {
        return Quarters(frame);
    }

    return s.quarter + whole / s.per_quarter;
}

double BeatGrid::Marker(double frame) const
{
    double place = Snap(frame);
    for (size_t c = 0; c < frames_.size(); c++)
    {
        const auto next = std::lower_bound(frames_[c].begin(), frames_[c].end(), frame);
        if (next != frames_[c].end())
        {
            place = std::min(place, Quarters(int(c), *next));
        }
    }

    return place;
}

std::vector<BeatGrid::Tempo> BeatGrid::Tempos() const
{
    std::vector<Tempo> tempos;
    for (const Stretch& s : stretches_)
    {
        const double frames = s.unit * s.per_quarter;
        if (tempos.empty() || std::fabs(tempos.back().frames - frames) > 1e-9 * frames)
        {
            tempos.push_back({s.quarter, frames});
        }
    }

    return tempos;
}

std::vector<BeatGrid::Group> BeatGrid::Groups(std::vector<uint32_t> frames)
{
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());

    std::vector<Group> groups;
    size_t i = 0;
    while (i < frames.size())
    {
        size_t j = i;
        while (j + 2 < frames.size() && frames[j + 1] - frames[j] <= kOrnamentFrames)
        {
            j++;
        }

        const uint32_t next = j + 1 < frames.size() ? frames[j + 1] : frames[i];
        groups.push_back({frames[i], next - frames[i]});
        i = j + 1;
    }

    return groups;
}

void BeatGrid::Push(double frame, double unit, double per_quarter, double units, bool snap)
{
    stretches_.push_back({frame, quarter_, unit, per_quarter, units, snap});
    quarter_ += units / per_quarter;
}

double BeatGrid::LastQuarter() const
{
    return stretches_.empty() ? kTypicalQuarter : stretches_.back().unit * stretches_.back().per_quarter;
}

double BeatGrid::PerQuarterFor(double unit) const
{
    for (const auto& [beat, per_quarter] : beats_)
    {
        if (std::fabs(beat / unit - 1) < kSameTempo)
        {
            return per_quarter;
        }
    }

    return PerQuarter(unit, LastQuarter());
}

double BeatGrid::FitBeat(const std::vector<std::vector<Group>>& groups, const Part& part, double unit)
{
    // The events near a whole number of units from the part's start, which is on the beat, give the unit by least
    // squares, which a long part pins down far more closely than the lengths of its notes do.
    for (int pass = 0; pass < 2; pass++)
    {
        const double tolerance = LengthTolerance(unit);
        double sum = 0;
        double squares = 0;
        for (const std::vector<Group>& list : groups)
        {
            for (const Group& g : list)
            {
                const double at = double(g.frame) - part.from;
                const double k = std::round(at / unit);
                if (g.frame >= part.from && g.frame < part.to && k >= 1 && std::fabs(at - k * unit) <= tolerance)
                {
                    sum += k * at;
                    squares += k * k;
                }
            }
        }
        if (squares > 0)
        {
            unit = sum / squares;
        }
    }

    return unit;
}

double BeatGrid::PerQuarter(double unit, double quarter)
{
    return std::exp2(std::round(std::log2(quarter / unit)));
}

void BeatGrid::AddPart(const std::vector<std::vector<Group>>& groups, const Part& part, bool last)
{
    if (!part.steady)
    {
        AddChangingPart(groups, part, last);
        return;
    }

    // A steady beat. Where the notes of the channel that agrees most with the others leave its units, and settle on
    // them again later, as around a ritardando, the notes in between set the tempo one by one. Finer places, such as
    // triplets', are for moving notes onto the beat, since so many of them would hide such a change.
    const double from = part.from;
    const double to = part.to;
    const double unit = FitBeat(groups, part, part.unit);
    const double per_quarter = PerQuarterFor(unit);
    beats_.emplace_back(unit, per_quarter);
    std::vector<uint32_t> frames;
    for (const Group& g : ReferenceNotes(groups, part.from, part.to))
    {
        frames.push_back(g.frame);
    }
    const SteadyNotes notes(std::move(frames), unit, unit * per_quarter / 3);

    double origin = from;  // where the steady beat starts
    double on_beat = from; // the last of the notes on it
    for (size_t i = 0; i < notes.Count(); i++)
    {
        const double frame = notes.Frame(i);
        if (frame <= origin || notes.OnBeat(frame - origin))
        {
            on_beat = std::max(on_beat, frame);
            continue;
        }

        // A note off the beat matters only if most of the notes after it are off it too, and they settle on a beat.
        if (notes.Settled(i + 1, origin, kDelayCheck) * 3 >= 2 * std::min(kDelayCheck, notes.Count() - i - 1))
        {
            continue;
        }

        const size_t again = notes.Resettle(i);
        if (again == notes.Count())
        {
            break;
        }

        AddBeat(groups, origin, on_beat, notes.Frame(again), unit, per_quarter);
        origin = on_beat = notes.Frame(again);
        i = again;
    }

    // The beat ends on the part's end if that's on it. The song's last part keeps its beat after its end, which may not
    // be on the beat, such as where an echo's loop ends.
    const double units = std::max(1.0, std::round((to - origin) / unit));
    const bool on_beat_end = std::fabs(to - origin - units * unit) <= UnitTolerance(unit);
    if (on_beat_end || last)
    {
        Push(origin, on_beat_end ? (to - origin) / units : unit, per_quarter, units, true);
        return;
    }

    AddBeat(groups, origin, on_beat, to, unit, per_quarter);
}

void BeatGrid::AddChangingPart(const std::vector<std::vector<Group>>& groups, const Part& part, bool last)
{
    // A tempo that changes gradually follows the notes, for a whole number of their units if a part comes after, so
    // that its beat starts on the song's grid. Otherwise the part keeps the tempo before it, without a beat to move its
    // events onto.
    const double from = part.from;
    const double to = part.to;
    const double start = quarter_;
    if (FollowNotes(groups, part.from, part.to, kFewestEvents, true))
    {
        if (!last)
        {
            WholeUnits(start, stretches_.back().per_quarter);
        }
        return;
    }

    const double unit = LastQuarter();
    const double units = std::max(1.0, std::round((to - from) / unit));
    Push(from, last ? unit : (to - from) / units, 1, units, false);
}

void BeatGrid::AddBeat(const std::vector<std::vector<Group>>& groups, double from, double on_beat, double to,
                       double unit, double per_quarter)
{
    if (on_beat > from)
    {
        const double units = std::max(1.0, std::round((on_beat - from) / unit));
        Push(from, (on_beat - from) / units, per_quarter, units, true);
    }

    // Less than half a unit is part of the last stretch.
    if (!stretches_.empty() && std::round((to - on_beat) / unit) < 1)
    {
        Stretch& s = stretches_.back();
        s.unit = (to - s.frame) / s.units;
        return;
    }

    // The notes that set the tempo one by one last a whole number of units together, so that the beat after them stays
    // on the song's grid.
    const double start = quarter_;
    if (FollowNotes(groups, uint32_t(on_beat), uint32_t(to), 1, false))
    {
        WholeUnits(start, per_quarter);
        return;
    }

    const double units = std::max(1.0, std::round((to - on_beat) / unit));
    Push(on_beat, (to - on_beat) / units, per_quarter, units, true);
}

bool BeatGrid::FollowNotes(const std::vector<std::vector<Group>>& groups, uint32_t from, uint32_t to, size_t fewest,
                           bool gradual)
{
    const std::vector<Group> notes = ReferenceNotes(groups, from, to);
    if (notes.size() < fewest || notes.empty())
    {
        return false;
    }

    std::vector<double> lengths;
    size_t near = 0;
    for (size_t i = 0; i < notes.size(); i++)
    {
        lengths.push_back(notes[i].length);
        if (i > 0 && std::max(lengths[i], lengths[i - 1]) <= kGradualRatio * std::min(lengths[i], lengths[i - 1]))
        {
            near++;
        }
    }
    if (gradual && double(near) < kGradualShare * double(notes.size() - 1))
    {
        return false;
    }

    // Each note lasts a number of units of the note before it, in halves, and a note of a unit or more sets the unit
    // for the next, so that a tempo that changes gradually follows the notes, and a long note keeps the tempo before
    // it. The first unit is the median length of the first few notes.
    const size_t window = std::min(notes.size(), kFirstUnitNotes);
    double unit = Median(std::vector<double>(lengths.begin(), lengths.begin() + long(window)));
    const double per_quarter = PerQuarterFor(unit);
    beats_.emplace_back(unit, per_quarter);

    // Returns how many units a note of `length` frames lasts, to the nearest half, and half a unit at least.
    const auto units_of = [&](double length)
    {
        return std::max(0.5, std::round(2 * length / unit) / 2);
    };

    if (notes.front().frame > from)
    {
        const double lead = notes.front().frame - from;
        const double units = units_of(lead);
        Push(from, lead / units, per_quarter, units, true);
    }

    for (size_t i = 0; i < notes.size(); i++)
    {
        const double units = units_of(lengths[i]);
        Push(notes[i].frame, lengths[i] / units, per_quarter, units, true);
        if (units >= 1)
        {
            unit = lengths[i] / units;
        }
    }

    return true;
}

void BeatGrid::WholeUnits(double start, double per_quarter)
{
    // The last stretch takes up the difference. The length rounds up instead if rounding to the nearest unit would
    // leave that stretch no time.
    const double grid = std::max(1.0, per_quarter);
    Stretch& s = stretches_.back();
    double end = start + std::max(1.0, std::round((quarter_ - start) * grid)) / grid;
    if (end <= s.quarter)
    {
        end = start + std::ceil((quarter_ - start) * grid) / grid;
    }

    const double frames = s.units * s.unit;
    s.units = (end - s.quarter) * s.per_quarter;
    s.unit = frames / s.units;
    quarter_ = end;
}

std::vector<BeatGrid::Group> BeatGrid::ReferenceNotes(const std::vector<std::vector<Group>>& groups, uint32_t from,
                                                      uint32_t to)
{
    // Each channel's notes in the part, each lasting until the next or the part's end.
    std::vector<std::vector<Group>> channels;
    for (const std::vector<Group>& list : groups)
    {
        std::vector<Group>& these = channels.emplace_back();
        for (const Group& g : list)
        {
            if (g.frame >= from && g.frame < to)
            {
                const uint32_t length = g.length ? std::min(g.length, to - g.frame) : to - g.frame;
                these.push_back({g.frame, length});
            }
        }
    }

    // Returns true if a note of `list` starts within a frame of `frame`.
    const auto starts_near = [](const std::vector<Group>& list, uint32_t frame)
    {
        const auto earlier = [](const Group& g, uint32_t f)
        {
            return g.frame < f;
        };
        const auto it = std::lower_bound(list.begin(), list.end(), frame > 0 ? frame - 1 : 0, earlier);
        return it != list.end() && it->frame <= frame + 1;
    };

    // Choose the channel whose notes most often start within a frame of another channel's notes. A delayed channel,
    // such as an echo, won't match the others.
    std::vector<Group> notes;
    size_t best = 0;
    for (size_t c = 0; c < channels.size(); c++)
    {
        size_t agree = 0;
        for (const auto& note : channels[c])
        {
            for (size_t other = 0; other < channels.size(); other++)
            {
                agree += other != c && starts_near(channels[other], note.frame) ? 1 : 0;
            }
        }
        if (notes.empty() || agree > best || (agree == best && channels[c].size() > notes.size()))
        {
            notes = channels[c];
            best = agree;
        }
    }

    return notes;
}

const BeatGrid::Stretch& BeatGrid::StretchAt(double frame) const
{
    const auto later = [](double f, const Stretch& s)
    {
        return f < s.frame;
    };
    const auto it = std::upper_bound(stretches_.begin(), stretches_.end(), frame, later);
    return it == stretches_.begin() ? stretches_.front() : *(it - 1);
}

BeatGrid::Place BeatGrid::Nearest(double frame, double delay, double after) const
{
    const Stretch& s = StretchAt(frame - delay);
    const double units = (frame - delay - s.frame) / s.unit;
    Place place;
    place.error = (units - std::round(units)) * s.unit;
    place.unit = s.unit;
    place.free = !StretchAt(frame).snap;
    if (!s.snap || place.free)
    {
        return place;
    }

    // The longest of a quarter note, half of one, and so on down to a 32nd note, then of their triplets, that the frame
    // is near a whole number of.
    for (const double share : {1.0, 2.0 / 3})
    {
        for (double level = std::max(1.0, s.per_quarter); level >= Finest(s.unit, s.per_quarter, share); level /= 2)
        {
            const double step = level * share;
            const double tolerance = share == 1 ? UnitTolerance(step * s.unit) : ThirdTolerance(step * s.unit);
            const double whole = std::round(units / step) * step;
            const double on_beat = s.frame + whole * s.unit + delay;
            if (std::fabs(units - whole) * s.unit <= tolerance && on_beat > after)
            {
                place.found = true;
                place.frame = on_beat;
                return place;
            }
        }
    }

    return place;
}

void BeatGrid::PlaceChannel(int c, const std::vector<Group>& groups)
{
    std::vector<Anchor>& anchors = anchors_[size_t(c)];
    double delay = 0; // frames behind the beat, such as an echo's
    double after = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < groups.size(); i++)
    {
        const double frame = groups[i].frame;
        Place place = Nearest(frame, delay, after);

        // An event where the song has no beat stays where it is.
        if (place.free)
        {
            if (frame > after)
            {
                anchors.push_back({frame, 0});
                after = frame;
            }
            continue;
        }

        // An event that isn't on the beat starts a new delay if the events after it are on the beat with that delay,
        // and not with the old one.
        const auto fitting = [&](double with, double from)
        {
            size_t count = 0;
            double last = from;
            for (size_t j = i + 1; j <= i + kDelayCheck; j++)
            {
                const Place next = Nearest(groups[j].frame, with, last);
                if (!next.found)
                {
                    break;
                }

                last = next.frame;
                count++;
            }

            return count;
        };

        if (!place.found && i + kDelayCheck < groups.size())
        {
            double shifted = delay + place.error;
            shifted -= place.unit * std::round(shifted / place.unit);
            const Place moved = Nearest(frame, shifted, after);
            if (moved.found && fitting(shifted, moved.frame) == kDelayCheck && fitting(delay, after) < kDelayCheck)
            {
                delay = shifted;
                place = moved;
            }
        }

        if (!place.found)
        {
            continue;
        }

        anchors.push_back({frame, place.frame - frame});
        after = place.frame;
    }
}

} // namespace agbsf2
