// SPDX-License-Identifier: MIT

// A text listing of a module: its header, its orders and every row of its patterns.

#pragma once

#include <string>

#include "krawall/driver.h"
#include "rom.h"

namespace agbsf2::krawall
{

// Writes a listing of module `song` to `path`. Returns false and sets `error` if the module can't be read or the file
// can't be written.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::krawall
