// SPDX-License-Identifier: MIT

// The model of Rare's per-frame routine, from ntdlib.

#pragma once

#include "ntdlib/rare/sequencer.h"

#include "rom.h"

namespace agbsf2::rare
{

using ntdlib::rare::Action;
using ntdlib::rare::kChannels;
using ntdlib::rare::kFullLevel;
using ntdlib::rare::kPhaseAttack;
using ntdlib::rare::kPhaseDecay;
using ntdlib::rare::kPhaseRelease;
using ntdlib::rare::kPhaseSustain;
using ntdlib::rare::kSlotFree;
using ntdlib::rare::kSlotOn;
using ntdlib::rare::kSlotReleased;
using ntdlib::rare::MixResult;
using ntdlib::rare::Sequencer;
using ntdlib::rare::Slot;

} // namespace agbsf2::rare
