// SPDX-License-Identifier: MIT

// The Game Boy's PSG voices, from ntdlib.

#pragma once

#include "ntdlib/sequence_export/agb/psg.h"

#include "sf2.h"

namespace agbsf2
{

using ntdlib::sequence_export::agb::KeyTuning;
using ntdlib::sequence_export::agb::kFifoLevel;
using ntdlib::sequence_export::agb::kMiddleC;
using ntdlib::sequence_export::agb::kNoiseSequenceRate;
using ntdlib::sequence_export::agb::kPsgLevelPerVolume;
using ntdlib::sequence_export::agb::NextLfsr;
using ntdlib::sequence_export::agb::NoiseSequence;
using ntdlib::sequence_export::agb::NoiseSound;
using ntdlib::sequence_export::agb::PsgFeed;
using ntdlib::sequence_export::agb::PsgModel;
using ntdlib::sequence_export::agb::PsgRegisters;
using ntdlib::sequence_export::agb::PsgSamples;
using ntdlib::sequence_export::agb::PsgScale;
using ntdlib::sequence_export::agb::PsgScaleOf;
using ntdlib::sequence_export::agb::PsgSemitones;
using ntdlib::sequence_export::agb::SettingCents;
using ntdlib::sequence_export::agb::SquareCycle;
using ntdlib::sequence_export::agb::SquareHz;
using ntdlib::sequence_export::agb::SquareName;
using ntdlib::sequence_export::agb::TunedZones;
using ntdlib::sequence_export::agb::TuningRuns;
using ntdlib::sequence_export::agb::UniformTuning;
using ntdlib::sequence_export::agb::WaveKeyOffset;
using ntdlib::sequence_export::agb::WaveShape;
using ntdlib::sequence_export::agb::WaveShapes;
using ntdlib::sequence_export::agb::WaveShare;

} // namespace agbsf2
