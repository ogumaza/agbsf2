// SPDX-License-Identifier: MIT

// Krawall's detection and modules, from ntdlib.

#pragma once

#include "ntdlib/krawall/driver.h"

#include "krawall/build.h"
#include "program.h"
#include "rom.h"

namespace agbsf2::krawall
{

using ntdlib::krawall::DetectDriver;
using ntdlib::krawall::DriverInfo;
using ntdlib::krawall::kModeJingle;
using ntdlib::krawall::kModeLoop;
using ntdlib::krawall::kModeSong;
using ntdlib::krawall::kModulePatterns;
using ntdlib::krawall::kNoteOff;
using ntdlib::krawall::kOrderEnd;
using ntdlib::krawall::kOrderSkip;
using ntdlib::krawall::kPatternRows;
using ntdlib::krawall::kSampleEnd;
using ntdlib::krawall::kSampleLoop;
using ntdlib::krawall::kSampleLoopLength;
using ntdlib::krawall::kSamplePoints;
using ntdlib::krawall::ModuleSong;
using ntdlib::krawall::PatternData;

// The overrides of detection. They give agbsf2 as the program that the messages name where a build or a file can't be
// read.
struct DriverOverrides : ntdlib::krawall::DriverOverrides
{
    DriverOverrides()
    {
        program = kProgramName;
    }
};

} // namespace agbsf2::krawall
