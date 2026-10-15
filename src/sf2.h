// SPDX-License-Identifier: MIT

// agbsf2's SoundFonts, on seqlib's SoundFont model.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/sf2/soundfont.h"

namespace agbsf2
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

// Returns an instrument named `name` that holds a global zone and then `zones`. Every instrument that agbsf2 writes
// comes from here. The global zone holds seqlib's kNoVelocityFilterFullBend. Those modulators turn the default
// velocity-to-filter modulators off and give the pitch wheel its full range. SoundFont 2.04's default modulator gives
// the pitch wheel 127/128 of the range that RPN 0 sets. FluidSynth follows it. Without the set, a bend of 34 semitones
// plays 27 cents flat. A zone's modulator takes the place of the default modulator with the same sources, destination
// and transform. A local zone's modulator takes the place of the same one in the global zone.
sf2::Instrument MakeInstrument(const std::string& name, std::vector<sf2::Zone> zones = {});

// A SoundFont as agbsf2 writes it. Its INFO list holds the name, the program and then the comment. A sample without a
// loop gets the whole sample as its loop, and the presets are listed by bank and program.
struct Sf2File : sf2::SoundFont
{
    // Writes the file. Returns false and sets `error` if it's too big for the format or can't be written.
    bool Write(const std::string& path, std::string& error) const;

    std::string name = "Untitled";
    std::string comment;
};

} // namespace agbsf2
