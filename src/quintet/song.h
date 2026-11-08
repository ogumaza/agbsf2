// SPDX-License-Identifier: MIT

// Quintet's songs and commands, from ntdlib.

#pragma once

#include "ntdlib/quintet/song.h"
#include "quintet/driver.h"
#include "rom.h"

namespace agbsf2::quintet
{

using ntdlib::quintet::Command;
using ntdlib::quintet::CommandOfS;
using ntdlib::quintet::DecodeCommand;
using ntdlib::quintet::DumpSong;
using ntdlib::quintet::kChannels;
using ntdlib::quintet::kDotS;
using ntdlib::quintet::kFirstCommandS;
using ntdlib::quintet::kNoise;
using ntdlib::quintet::kPcmA;
using ntdlib::quintet::kPcmB;
using ntdlib::quintet::kPlayAgainS;
using ntdlib::quintet::kRestS;
using ntdlib::quintet::kSkipByteS;
using ntdlib::quintet::kSquare1;
using ntdlib::quintet::kSquare2;
using ntdlib::quintet::kTieS;
using ntdlib::quintet::kWave;
using ntdlib::quintet::kWholeNote;
using ntdlib::quintet::ReadSongHeader;
using ntdlib::quintet::SongHeader;

} // namespace agbsf2::quintet
