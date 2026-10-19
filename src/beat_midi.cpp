// SPDX-License-Identifier: MIT

#include "beat_midi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "gba.h"

namespace agbsf2
{

uint32_t QuarterTick(double quarters)
{
    return uint32_t(std::lround(std::max(0.0, quarters) * kBeatQuarterTicks));
}

BeatTicks::BeatTicks(const BeatGrid& grid, bool frame_timing) : grid_(grid), frame_timing_(frame_timing)
{
}

uint32_t BeatTicks::Song(double frame) const
{
    return QuarterTick(frame_timing_ ? grid_.Quarters(frame) : grid_.Snap(frame));
}

uint32_t BeatTicks::Marker(double frame) const
{
    return QuarterTick(frame_timing_ ? grid_.Quarters(frame) : grid_.Marker(frame));
}

uint32_t BeatTicks::Channel(int c, double frame, uint32_t end) const
{
    return std::min(end, QuarterTick(frame_timing_ ? grid_.Quarters(frame) : grid_.Quarters(c, frame)));
}

void WriteBeatConductor(MidiTrack& conductor, const std::string& title, const std::string& about,
                        const BeatTicks& ticks, const FramePlan& plan, uint32_t end)
{
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const BeatGrid::Tempo& t : ticks.Grid().Tempos())
    {
        conductor.Tempo(QuarterTick(t.quarter), uint32_t(std::lround(FrameSeconds(t.frames) * 1e6)));
    }

    if (plan.loops)
    {
        conductor.Meta(ticks.Marker(plan.loop_start), 0x06, "loopStart");
        conductor.Meta(ticks.Marker(plan.loop_end), 0x06, "loopEnd");
        conductor.RepeatTempo(ticks.Marker(plan.loop_start));
    }
    conductor.SetEnd(end);
}

} // namespace agbsf2
