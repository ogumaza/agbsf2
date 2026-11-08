// SPDX-License-Identifier: MIT

// MP2K's sequencer, from ntdlib.

#pragma once

#include "ntdlib/mp2k/sequencer.h"

namespace agbsf2::mp2k
{

using ntdlib::mp2k::Action;
using ntdlib::mp2k::CamelotKeyToFrequency;
using ntdlib::mp2k::Channel;
using ntdlib::mp2k::kChannelCount;
using ntdlib::mp2k::KeyToFrequency;
using ntdlib::mp2k::KeyToPsgFrequency;
using ntdlib::mp2k::kFirstPsgChannel;
using ntdlib::mp2k::kPhaseAttack;
using ntdlib::mp2k::kPhaseDecay;
using ntdlib::mp2k::kPhaseRelease;
using ntdlib::mp2k::kPhaseSustain;
using ntdlib::mp2k::Sequencer;

} // namespace agbsf2::mp2k
