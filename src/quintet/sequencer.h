// SPDX-License-Identifier: MIT

// Quintet's model of the driver, from ntdlib.

#pragma once

#include "ntdlib/quintet/sequencer.h"
#include "quintet/driver.h"
#include "quintet/song.h"
#include "rom.h"

namespace agbsf2::quintet
{

using ntdlib::quintet::Channel;
using ntdlib::quintet::Event;
using ntdlib::quintet::Fifo;
using ntdlib::quintet::FifoFrames;
using ntdlib::quintet::Hardware;
using ntdlib::quintet::kAfterRest;
using ntdlib::quintet::kImageRegisters;
using ntdlib::quintet::kNr10;
using ntdlib::quintet::kNr11;
using ntdlib::quintet::kNr13;
using ntdlib::quintet::kNr21;
using ntdlib::quintet::kNr23;
using ntdlib::quintet::kNr30;
using ntdlib::quintet::kNr31;
using ntdlib::quintet::kNr33;
using ntdlib::quintet::kNr41;
using ntdlib::quintet::kNr43;
using ntdlib::quintet::kNr50;
using ntdlib::quintet::kPsgControl;
using ntdlib::quintet::kPsgEnvelope;
using ntdlib::quintet::kRestart;
using ntdlib::quintet::kSoundcntH;
using ntdlib::quintet::SampleCycles;
using ntdlib::quintet::Sequencer;

} // namespace agbsf2::quintet
