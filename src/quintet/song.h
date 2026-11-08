// SPDX-License-Identifier: MIT

// Quintet's songs and commands, from ntdlib.

#pragma once

#include "ntdlib/quintet/song.h"

#include "quintet/driver.h"
#include "rom.h"

namespace agbsf2::quintet
{

using ntdlib::quintet::Command;
using ntdlib::quintet::DecodeCommand;
using ntdlib::quintet::DumpSong;
using ntdlib::quintet::kChannels;
using ntdlib::quintet::kNoise;
using ntdlib::quintet::kWave;
using ntdlib::quintet::ReadSongHeader;
using ntdlib::quintet::SongHeader;

} // namespace agbsf2::quintet
