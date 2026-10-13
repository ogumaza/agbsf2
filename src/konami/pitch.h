// SPDX-License-Identifier: MIT

// Fundamental-frequency estimation for instrument samples.

#pragma once

#include "konami/driver.h"
#include "rom.h"

namespace agbsf2::konami
{

// Estimates the pitch of a DirectSound sample played at `rate` Hz using the YIN algorithm on its steady-state part (the
// loop, when it has one). Returns the fundamental in Hz, or 0 if the sample has no clear pitch.
double EstimateSamplePitch(const Rom& rom, const SampleInfo& sample, double rate);

// Returns the MIDI key nearest to `hz` (69 = A4 = 440 Hz).
int NearestKey(double hz);

} // namespace agbsf2::konami
