// SPDX-License-Identifier: MIT

// Ubisoft Milan's driver tables, from ntdlib.

#pragma once

#include "ntdlib/ubimilan/driver.h"

namespace agbsf2::ubimilan
{

using ntdlib::ubimilan::DetectDriver;
using ntdlib::ubimilan::DriverInfo;
using ntdlib::ubimilan::DriverOverrides;
using ntdlib::ubimilan::GameSample;
using ntdlib::ubimilan::kFirstSampleKey;
using ntdlib::ubimilan::kFullVolume;
using ntdlib::ubimilan::KitPlays;
using ntdlib::ubimilan::kKit;
using ntdlib::ubimilan::kKitChannel;
using ntdlib::ubimilan::kLastNoiseKey;
using ntdlib::ubimilan::kLoopKeyOffset;
using ntdlib::ubimilan::kMusic;
using ntdlib::ubimilan::kNoResource;
using ntdlib::ubimilan::kPsgChannels;
using ntdlib::ubimilan::kRunOutput;
using ntdlib::ubimilan::kRunPoints;
using ntdlib::ubimilan::kSample;
using ntdlib::ubimilan::kSequence;
using ntdlib::ubimilan::kSequenceHeader;
using ntdlib::ubimilan::kVoiceCount;
using ntdlib::ubimilan::kWaveChannel;
using ntdlib::ubimilan::MusicPiece;
using ntdlib::ubimilan::OutputCycles;
using ntdlib::ubimilan::OutputRate;
using ntdlib::ubimilan::PlaysKit;
using ntdlib::ubimilan::PlaysNoise;
using ntdlib::ubimilan::PointRate;
using ntdlib::ubimilan::ReadMusic;
using ntdlib::ubimilan::ReadSample;
using ntdlib::ubimilan::ResourceAddress;
using ntdlib::ubimilan::ResourceType;

} // namespace agbsf2::ubimilan
