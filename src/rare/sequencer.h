// SPDX-License-Identifier: MIT

// The model of Rare's per-frame routine, from ntdlib.

#pragma once

#include "ntdlib/rare/sequencer.h"
#include "rom.h"

namespace agbsf2::rare
{

using ntdlib::rare::Action;
using ntdlib::rare::AttackSteps;
using ntdlib::rare::ChannelState;
using ntdlib::rare::kAudibleLevel;
using ntdlib::rare::kCent;
using ntdlib::rare::kChannels;
using ntdlib::rare::kFrameTime;
using ntdlib::rare::kFullLevel;
using ntdlib::rare::kFullSustain;
using ntdlib::rare::kPhaseAttack;
using ntdlib::rare::kPhaseAttackStart;
using ntdlib::rare::kPhaseDecay;
using ntdlib::rare::kPhaseDecayStart;
using ntdlib::rare::kPhaseRelease;
using ntdlib::rare::kPhaseReleaseStart;
using ntdlib::rare::kPhaseSustain;
using ntdlib::rare::kSemitone;
using ntdlib::rare::kSlotFree;
using ntdlib::rare::kSlotOn;
using ntdlib::rare::kSlotReleased;
using ntdlib::rare::MixResult;
using ntdlib::rare::Sequencer;
using ntdlib::rare::Slot;
using ntdlib::rare::SustainLevel;

} // namespace agbsf2::rare
