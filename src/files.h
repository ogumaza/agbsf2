// SPDX-License-Identifier: MIT

// File access with UTF-8 paths, and safe file names, from ntdlib.

#pragma once

#include "ntdlib/agb/files.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace agbsf2
{

using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::ReadFile;
using ntdlib::agb::Utf8;
using ntdlib::agb::WriteFile;
using ntdlib::agb::WriteTextFile;
using ntdlib::sequence_export::agb::SafeFileName;

} // namespace agbsf2
