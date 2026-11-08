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
using ntdlib::krawall::kEntryChannel;
using ntdlib::krawall::kEntryEffect;
using ntdlib::krawall::kEntryNote;
using ntdlib::krawall::kEntryVolume;
using ntdlib::krawall::kEnvelopeFlags;
using ntdlib::krawall::kEnvelopeLast;
using ntdlib::krawall::kEnvelopeLoops;
using ntdlib::krawall::kEnvelopeLoopStart;
using ntdlib::krawall::kEnvelopeOn;
using ntdlib::krawall::kEnvelopeSustain;
using ntdlib::krawall::kEnvelopeSustains;
using ntdlib::krawall::kInstrumentFade;
using ntdlib::krawall::kInstrumentInterpolate;
using ntdlib::krawall::kInstrumentPanEnvelope;
using ntdlib::krawall::kInstrumentSize;
using ntdlib::krawall::kInstrumentVibratoDepth;
using ntdlib::krawall::kInstrumentVibratoRate;
using ntdlib::krawall::kInstrumentVolumeEnvelope;
using ntdlib::krawall::kModeJingle;
using ntdlib::krawall::kModeLoop;
using ntdlib::krawall::kModeSong;
using ntdlib::krawall::kModifiedPatterns;
using ntdlib::krawall::kModuleChannelPans;
using ntdlib::krawall::kModuleChannels;
using ntdlib::krawall::kModuleFlags;
using ntdlib::krawall::kModuleGlobalVolume;
using ntdlib::krawall::kModuleInstruments;
using ntdlib::krawall::kModuleOrderCount;
using ntdlib::krawall::kModuleOrders;
using ntdlib::krawall::kModulePatterns;
using ntdlib::krawall::kModuleRestart;
using ntdlib::krawall::kModuleSongStarts;
using ntdlib::krawall::kModuleSpeed;
using ntdlib::krawall::kModuleTempo;
using ntdlib::krawall::kNoteOff;
using ntdlib::krawall::kOrderEnd;
using ntdlib::krawall::kOrderSkip;
using ntdlib::krawall::kPatternIndexEntries;
using ntdlib::krawall::kPatternRows;
using ntdlib::krawall::kSampleEnd;
using ntdlib::krawall::kSampleFineTune;
using ntdlib::krawall::kSampleInterpolate;
using ntdlib::krawall::kSampleLoop;
using ntdlib::krawall::kSampleLoopLength;
using ntdlib::krawall::kSamplePan;
using ntdlib::krawall::kSamplePoints;
using ntdlib::krawall::kSampleRelativeNote;
using ntdlib::krawall::kSampleVolume;
using ntdlib::krawall::ModuleInfo;
using ntdlib::krawall::ModulePatterns;
using ntdlib::krawall::ModuleSong;
using ntdlib::krawall::PatternData;
using ntdlib::krawall::PatternRows;
using ntdlib::krawall::ReadEntry;
using ntdlib::krawall::ReadModule;
using ntdlib::krawall::RowEntry;
using ntdlib::krawall::SampleEnd;
using ntdlib::krawall::SongsInModule;

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
