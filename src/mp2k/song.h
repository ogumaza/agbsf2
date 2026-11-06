// SPDX-License-Identifier: MIT

// MP2K's songs and voices, from ntdlib.

#pragma once

#include "ntdlib/mp2k/song.h"

namespace agbsf2::mp2k
{

using enum ntdlib::mp2k::Command;
using enum ntdlib::mp2k::Xcommand;
using ntdlib::mp2k::ClockLength;
using ntdlib::mp2k::Command;
using ntdlib::mp2k::DecodeEvent;
using ntdlib::mp2k::DescribeEvent;
using ntdlib::mp2k::DumpSong;
using ntdlib::mp2k::Event;
using ntdlib::mp2k::kVoiceCompressed;
using ntdlib::mp2k::kVoiceDrumKit;
using ntdlib::mp2k::kVoiceFixed;
using ntdlib::mp2k::kVoiceKeySplit;
using ntdlib::mp2k::ReadSong;
using ntdlib::mp2k::ReadSongHeader;
using ntdlib::mp2k::RunningStatus;
using ntdlib::mp2k::ScanTrack;
using ntdlib::mp2k::SongHeader;
using ntdlib::mp2k::Voice;
using ntdlib::mp2k::Wave;
using ntdlib::mp2k::Xcommand;

} // namespace agbsf2::mp2k
