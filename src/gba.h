// SPDX-License-Identifier: MIT

// The GBA's CPU clock and display frame rate, the times of frames, and the writes to its registers that some drivers'
// models record.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace agbsf2
{

// The CPU's cycles a second.
constexpr uint64_t kSecondCycles = 16777216;

// The CPU's cycles in a display frame.
constexpr uint64_t kFrameCycles = 280896;

// The display's frames a second, about 59.73.
constexpr double kFrameRate = double(kSecondCycles) / double(kFrameCycles);

// Returns the seconds that `frames` frames last.
constexpr double FrameSeconds(double frames)
{
    return frames * double(kFrameCycles) / double(kSecondCycles);
}

// Returns the microseconds of a quarter note of `quarter` MIDI ticks when a frame lasts `per_frame` ticks, rounded to
// the nearest.
constexpr uint32_t QuarterMicros(uint64_t quarter, uint64_t per_frame)
{
    const uint64_t numerator = quarter * kFrameCycles * 1000000;
    const uint64_t denominator = per_frame * kSecondCycles;
    return uint32_t((numerator + denominator / 2) / denominator);
}

// Returns the seconds from a song's start to MIDI tick `tick`. `starts` holds the tick at which each frame starts, from
// 0, and then the tick after the last frame. Within a frame, the time goes on in a straight line.
template <typename Tick>
double TickSeconds(const std::vector<Tick>& starts, Tick tick)
{
    const size_t f = size_t(std::upper_bound(starts.begin(), starts.end(), tick) - starts.begin()) - 1;
    const double into = f + 1 < starts.size() ? double(tick - starts[f]) / double(starts[f + 1] - starts[f]) : 0.0;
    return FrameSeconds(double(f) + into);
}

// A write to one of the GBA's hardware registers.
struct RegisterWrite
{
    uint32_t address = 0;
    uint8_t size = 0; // in bytes
    uint32_t value = 0;
};

// Prints `w` as a line of a trace in the form of the tools' driver_emu.py scripts: the frame, `tag` if there is one,
// then the address in 8 hex digits, the size in bytes and the value in 2 hex digits a byte.
inline void PrintWrite(std::FILE* out, long long frame, const RegisterWrite& w, const char* tag = nullptr)
{
    if (tag)
    {
        std::fprintf(out, "%lld %s ", frame, tag);
    }
    else
    {
        std::fprintf(out, "%lld ", frame);
    }
    std::fprintf(out, "%08x %d %0*x\n", unsigned(w.address), int(w.size), 2 * int(w.size), unsigned(w.value));
}

} // namespace agbsf2
