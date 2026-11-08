// SPDX-License-Identifier: MIT

// Brownie Brown's driver tables, from ntdlib.

#pragma once

#include "ntdlib/brownie/driver.h"
#include "rom.h"

namespace agbsf2::brownie
{

using ntdlib::brownie::ChannelKind;
using ntdlib::brownie::ChannelName;
using ntdlib::brownie::DetectDriver;
using ntdlib::brownie::DriverInfo;
using ntdlib::brownie::DriverOverrides;
using ntdlib::brownie::KindOf;
using ntdlib::brownie::LayoutOf;
using ntdlib::brownie::PsgTuning;
using ntdlib::brownie::ReadSong;
using ntdlib::brownie::Revision;
using ntdlib::brownie::RevisionLayout;
using ntdlib::brownie::SongAddress;
using ntdlib::brownie::SongChannel;
using ntdlib::brownie::Tuning;
using ntdlib::brownie::kChannelCount;
using ntdlib::brownie::kFifoCount;
using ntdlib::brownie::kFirstEffectChannel;
using ntdlib::brownie::kFirstEffectSampleChannel;
using ntdlib::brownie::kFirstSampleChannel;
using ntdlib::brownie::kMode0Voices;
using ntdlib::brownie::kNoiseChannel;
using ntdlib::brownie::kRecordCount;
using ntdlib::brownie::kVoiceCount;
using ntdlib::brownie::kWaveChannel;

} // namespace agbsf2::brownie
