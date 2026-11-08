// SPDX-License-Identifier: MIT

// Quintet's driver tables, from ntdlib.

#pragma once

#include "ntdlib/quintet/driver.h"
#include "rom.h"

namespace agbsf2::quintet
{

using ntdlib::quintet::DetectDriver;
using ntdlib::quintet::DriverInfo;
using ntdlib::quintet::DriverOverrides;
using ntdlib::quintet::EffectIndex;
using ntdlib::quintet::FileOfSong;
using ntdlib::quintet::kFrequencyEntries;
using ntdlib::quintet::kPcmPitchEntries;
using ntdlib::quintet::kRevisionTraits;
using ntdlib::quintet::PsgRate;
using ntdlib::quintet::ReadMusicFile;
using ntdlib::quintet::ReadMusicFiles;
using ntdlib::quintet::Revision;
using ntdlib::quintet::RevisionTraits;
using ntdlib::quintet::SongAddress;
using ntdlib::quintet::SongCount;
using ntdlib::quintet::TraitsOf;

} // namespace agbsf2::quintet
