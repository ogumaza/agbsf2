// SPDX-License-Identifier: MIT

// Rare's tune format: headers, track commands and instruments, from ntdlib.

#pragma once

#include "ntdlib/rare/song.h"

#include "rom.h"

namespace agbsf2::rare
{

using ntdlib::rare::Command;
using ntdlib::rare::DecodeEvent;
using ntdlib::rare::DescribeEvent;
using ntdlib::rare::EnvelopeSettings;
using ntdlib::rare::Event;
using ntdlib::rare::Format;
using ntdlib::rare::Instrument;
using ntdlib::rare::kCmdBend;
using ntdlib::rare::kCmdControl;
using ntdlib::rare::kCmdDelay1;
using ntdlib::rare::kCmdDelay2;
using ntdlib::rare::kCmdDelay3;
using ntdlib::rare::kCmdEnd;
using ntdlib::rare::kCmdNoteOff;
using ntdlib::rare::kCmdNoteOn;
using ntdlib::rare::kCmdNoteOnB;
using ntdlib::rare::kCmdProgram;
using ntdlib::rare::kCmdTempo;
using ntdlib::rare::kCtrlAttack;
using ntdlib::rare::kCtrlDecay;
using ntdlib::rare::kCtrlLoopEnd;
using ntdlib::rare::kCtrlLoopStart;
using ntdlib::rare::kCtrlModulation;
using ntdlib::rare::kCtrlRelease;
using ntdlib::rare::kCtrlSustain;
using ntdlib::rare::kCtrlVolume;
using ntdlib::rare::kInstDrumKit;
using ntdlib::rare::kInstKeySplit;
using ntdlib::rare::kInstSample;
using ntdlib::rare::kLoopForward;
using ntdlib::rare::kLoopForwardB;
using ntdlib::rare::kLoopOnce;
using ntdlib::rare::ReadInstrument;
using ntdlib::rare::ReadTuneHeader;
using ntdlib::rare::ScanTrack;
using ntdlib::rare::TuneHeader;

} // namespace agbsf2::rare
