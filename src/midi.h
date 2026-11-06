// SPDX-License-Identifier: MIT

// The MIDI files' writer, from ntdlib.

#pragma once

#include "ntdlib/sequence_export/agb/midi.h"

#include "song_banks.h"

namespace agbsf2
{

namespace cc = seqlib::midi::cc;
using ntdlib::sequence_export::agb::BendRangeFor;
using ntdlib::sequence_export::agb::BendValue;
using ntdlib::sequence_export::agb::ChannelWriter;
using ntdlib::sequence_export::agb::FixedBendValue;
using ntdlib::sequence_export::agb::kDrumChannel;
using ntdlib::sequence_export::agb::kSmallBend;
using ntdlib::sequence_export::agb::LevelsToControllers;
using ntdlib::sequence_export::agb::MelodicChannel;
using ntdlib::sequence_export::agb::MidiFile;
using ntdlib::sequence_export::agb::MidiTrack;
using ntdlib::sequence_export::agb::NumberedPreset;
using ntdlib::sequence_export::agb::SquareRootValue;

} // namespace agbsf2
