// SPDX-License-Identifier: MIT

// Nintendo R&D2's model of the driver, from ntdlib.

#pragma once

#include "ntdlib/rd2/sequencer.h"
#include "gba.h"
#include "rd2/driver.h"
#include "rom.h"

namespace agbsf2::rd2
{

using ntdlib::rd2::BankAddress;
using ntdlib::rd2::Event;
using ntdlib::rd2::HasPsgTable;
using ntdlib::rd2::kCentre;
using ntdlib::rd2::kEffectEcho;
using ntdlib::rd2::kEffectPriority;
using ntdlib::rd2::kFixedNote;
using ntdlib::rd2::kFrameSamples;
using ntdlib::rd2::kLastIndex;
using ntdlib::rd2::kMaxDepth;
using ntdlib::rd2::kMixRate;
using ntdlib::rd2::kNoiseType;
using ntdlib::rd2::kNoTempoAdjustFlag;
using ntdlib::rd2::kPlayerTracks;
using ntdlib::rd2::kPsgVoices;
using ntdlib::rd2::kSampleType;
using ntdlib::rd2::kSquare1Type;
using ntdlib::rd2::kSquare2Type;
using ntdlib::rd2::kStartTempo;
using ntdlib::rd2::kTickUnits;
using ntdlib::rd2::kTrackSlots;
using ntdlib::rd2::kUnityIndex;
using ntdlib::rd2::kUnreadTarget;
using ntdlib::rd2::kVoiceFree;
using ntdlib::rd2::kVoicePlaying;
using ntdlib::rd2::kVoiceReleased;
using ntdlib::rd2::kVoices;
using ntdlib::rd2::kWaveType;
using ntdlib::rd2::LastPsgTableEntry;
using ntdlib::rd2::Lookup;
using ntdlib::rd2::LookUpInstrument;
using ntdlib::rd2::NoiseTableEntry;
using ntdlib::rd2::PitchIndex;
using ntdlib::rd2::Player;
using ntdlib::rd2::PsgTableEntry;
using ntdlib::rd2::RegionFlags;
using ntdlib::rd2::RegionRelease;
using ntdlib::rd2::RegionSweep;
using ntdlib::rd2::RegionTuning;
using ntdlib::rd2::RegionType;
using ntdlib::rd2::SampleAddress;
using ntdlib::rd2::SampleTable;
using ntdlib::rd2::Sequencer;
using ntdlib::rd2::Track;
using ntdlib::rd2::Voice;

} // namespace agbsf2::rd2
