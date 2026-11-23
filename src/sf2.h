// SPDX-License-Identifier: MIT

// The SoundFont writer, from agblib.

#pragma once

#include <string>

#include "agblib/sequence_export/agb/sf2.h"

#include "program.h"
#include "rom.h"

namespace agbsf2
{

namespace sf2 = seqlib::sf2;

// Writes the shared SoundFont `f` to `path`. The SoundFont names agbsf2 as its program. Its name is the game's title,
// then `detail`.
inline bool WriteSoundfont(agblib::sequence_export::agb::Sf2File& f, const Rom& rom, const std::string& path,
                           std::string& error, const std::string& detail = "")
{
    f.program = kProgramName;
    f.SetTitle(rom.Title(), detail);

    return f.Write(path, error);
}

} // namespace agbsf2
