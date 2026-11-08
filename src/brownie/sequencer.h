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
using ntdlib::brownie::SampleKey;
using ntdlib::brownie::SampleOctave;
using ntdlib::brownie::Sequencer;
using ntdlib::brownie::Voice;
using ntdlib::brownie::kFadeInFrames;
using ntdlib::brownie::kFadeInPeriod;
using ntdlib::brownie::kFadeInTimer;
using ntdlib::brownie::kFadeOutFrames;
using ntdlib::brownie::kFadeOutPeriod;
using ntdlib::brownie::kFadeOutTimer;
using ntdlib::brownie::kFadeStep;
using ntdlib::brownie::kFlagEffect;
using ntdlib::brownie::kFlagOn;
using ntdlib::brownie::kFlagPaused;
using ntdlib::brownie::kIoControlRegisters;
using ntdlib::brownie::kIoFrequencyRegisters;
using ntdlib::brownie::kIoNr10;
using ntdlib::brownie::kIoNr30;
using ntdlib::brownie::kIoNr52;
using ntdlib::brownie::kIoSoundEnd;
using ntdlib::brownie::kIoSoundcntH;
using ntdlib::brownie::kIoSoundcntL;
using ntdlib::brownie::kIoWaveRam;
using ntdlib::brownie::kMaster;
using ntdlib::brownie::kMixPoints;
using ntdlib::brownie::kMixerMode;
using ntdlib::brownie::kOctaveKeys;
using ntdlib::brownie::kQueue;
using ntdlib::brownie::kQueueRead;
using ntdlib::brownie::kQueueWrite;
using ntdlib::brownie::kRateSteps;
using ntdlib::brownie::kSampleOctaves;
using ntdlib::brownie::kSoundcntLCopy;
using ntdlib::brownie::kVacationGlobals;
using ntdlib::brownie::kVacationMaster;
using ntdlib::brownie::kVacationSoundcntLCopy;

} // namespace agbsf2::brownie
