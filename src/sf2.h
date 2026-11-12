// SPDX-License-Identifier: MIT

// The SoundFonts' writer, from ntdlib.

#pragma once

#include <string>

#include "ntdlib/sequence_export/agb/sf2.h"

#include "program.h"
#include "rom.h"

namespace agbsf2
{

namespace sf2 = seqlib::sf2;

// Writes the shared SoundFont `f` to `path`, with agbsf2 as its program and the game's title, then `detail`, as its
// name.
inline bool WriteSoundfont(ntdlib::sequence_export::agb::Sf2File& f, const Rom& rom, const std::string& path,
                           std::string& error, const std::string& detail = "")
{
    f.program = kProgramName;
    f.SetTitle(rom.Title(), detail);

    return f.Write(path, error);
}

} // namespace agbsf2
