// SPDX-License-Identifier: MIT

// The SoundFonts' writer, from ntdlib.

#pragma once

#include "ntdlib/sequence_export/agb/sf2.h"

#include "program.h"

namespace agbsf2
{

namespace sf2 = seqlib::sf2;
using ntdlib::sequence_export::agb::kDrumBank;
using ntdlib::sequence_export::agb::kInstant;
using ntdlib::sequence_export::agb::kLoopGuard;
using ntdlib::sequence_export::agb::kMinLoop;
using ntdlib::sequence_export::agb::kSilent;
using ntdlib::sequence_export::agb::Looped;
using ntdlib::sequence_export::agb::MakePreset;
using ntdlib::sequence_export::agb::PresetInstrument;
using ntdlib::sequence_export::agb::Timecents;
using ntdlib::sequence_export::agb::VolumeEnvelope;

// A SoundFont that names agbsf2 in its comment and in ISFT.
struct Sf2File : ntdlib::sequence_export::agb::Sf2File
{
    Sf2File()
    {
        program = kProgramName;
    }
};

} // namespace agbsf2
