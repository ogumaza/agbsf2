// SPDX-License-Identifier: MIT

// Brownie Brown's model of the driver, from ntdlib.

#pragma once

#include "ntdlib/brownie/sequencer.h"

#include "brownie/driver.h"
#include "gba.h"
#include "rom.h"

namespace agbsf2::brownie
{

using ntdlib::brownie::Channel;
using ntdlib::brownie::Event;
using ntdlib::brownie::Fifo;
using ntdlib::brownie::FifoVoice;
using ntdlib::brownie::Globals;
using ntdlib::brownie::kFlagOn;
using ntdlib::brownie::kMaster;
using ntdlib::brownie::kQueueRead;
using ntdlib::brownie::kQueueWrite;
using ntdlib::brownie::kRateSteps;
using ntdlib::brownie::kVacationMaster;
using ntdlib::brownie::kVacationSoundcntLCopy;
using ntdlib::brownie::Sequencer;
using ntdlib::brownie::Voice;

} // namespace agbsf2::brownie
