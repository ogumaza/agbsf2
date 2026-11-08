// SPDX-License-Identifier: MIT

// Nintendo R&D2's model of the driver, from ntdlib.

#pragma once

#include "ntdlib/rd2/sequencer.h"

#include "gba.h"
#include "rd2/driver.h"
#include "rom.h"

namespace agbsf2::rd2
{

using ntdlib::rd2::Event;
using ntdlib::rd2::kEffectEcho;
using ntdlib::rd2::kEffectPriority;
using ntdlib::rd2::kPsgVoices;
using ntdlib::rd2::kVoiceFree;
using ntdlib::rd2::kVoices;
using ntdlib::rd2::Player;
using ntdlib::rd2::Sequencer;
using ntdlib::rd2::Track;
using ntdlib::rd2::Voice;

} // namespace agbsf2::rd2
