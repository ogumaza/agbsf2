// SPDX-License-Identifier: MIT

// supergbamidi's SoundFonts, on seqlib's SoundFont model.

#pragma once

#include <cstdint>
#include <string>

#include "seqlib/sf2/soundfont.h"

namespace supergbamidi
{

namespace sf2 = seqlib::sf2;

// Returns true if `s` has a loop. A sample without one keeps the loop points 0 and 0 until Sf2File::Write().
inline bool Looped(const sf2::Sample& s)
{
    return s.loop_end > s.loop_start;
}

// Returns a preset with one zone. The zone names instrument `instrument`.
sf2::Preset MakePreset(const std::string& name, uint16_t bank, uint16_t program, int instrument);

// Returns the instrument that a preset from MakePreset() names.
int PresetInstrument(const sf2::Preset& preset);

// A SoundFont as supergbamidi writes it. Its INFO list holds the name, the program and then the comment. A sample
// without a loop gets the whole sample as its loop, and the presets are listed by bank and program.
struct Sf2File : sf2::SoundFont
{
    // Writes the file. Returns false and sets `error` if it's too big for the format or can't be written.
    bool Write(const std::string& path, std::string& error) const;

    std::string name = "Untitled";
    std::string comment;
};

} // namespace supergbamidi
