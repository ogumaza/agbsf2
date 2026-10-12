// SPDX-License-Identifier: MIT

// supergbamidi's names for seqlib's MIDI file builder.

#pragma once

#include "seqlib/midi/builder.h"

namespace supergbamidi
{

namespace cc = seqlib::midi::cc;

using seqlib::midi::LevelsToControllers;
using MidiFile = seqlib::midi::FileBuilder;
using MidiTrack = seqlib::midi::TrackBuilder;

} // namespace supergbamidi
