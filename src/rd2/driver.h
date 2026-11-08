// SPDX-License-Identifier: MIT

// Nintendo R&D2's driver tables, from ntdlib.

#pragma once

#include "ntdlib/rd2/driver.h"
#include "rom.h"

namespace agbsf2::rd2
{

using ntdlib::rd2::DetectDriver;
using ntdlib::rd2::DriverInfo;
using ntdlib::rd2::DriverOverrides;
using ntdlib::rd2::Effect;
using ntdlib::rd2::kSampleVoices;
using ntdlib::rd2::kSettingsSize;
using ntdlib::rd2::MusicSettings;
using ntdlib::rd2::Revision;
using ntdlib::rd2::RevisionName;
using ntdlib::rd2::RevisionTraits;
using ntdlib::rd2::SongCount;
using ntdlib::rd2::SongEffect;
using ntdlib::rd2::TraitsOf;
using ntdlib::rd2::VersionSettings;

} // namespace agbsf2::rd2
