// SPDX-License-Identifier: MIT

// Konami's driver tables, from ntdlib.

#pragma once

#include "ntdlib/konami/driver.h"

#include "program.h"

namespace agbsf2::konami
{

using ntdlib::konami::DetectDriver;
using ntdlib::konami::DriverInfo;
using ntdlib::konami::SampleInfo;
using ntdlib::konami::SamplePcm;

// The overrides of detection. They give agbsf2 as the program that the messages name where the driver's code or tables
// can't be read.
struct DriverOverrides : ntdlib::konami::DriverOverrides
{
    DriverOverrides()
    {
        program = kProgramName;
    }
};

} // namespace agbsf2::konami
