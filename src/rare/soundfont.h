// SPDX-License-Identifier: MIT

// Builds SoundFont instruments from the driver's instruments and samples.

#pragma once

#include <cstdint>
#include <map>
#include <tuple>
#include <utility>

#include "rare/driver.h"
#include "rare/song.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rare
{

// The bank in which General MIDI players look for the drum channel's programs.
constexpr int kDrumBank = 128;

// A SoundFont volume envelope: times in timecents and the sustain level in centibels of attenuation.
struct Sf2Envelope
{
    int attack = -12000;
    int decay = -12000;
    int sustain = 0;
    int release = -12000;
};

// Returns the SoundFont envelope that comes closest to an instrument's envelope in the driver, with the settings from
// controllers 20-23 that its notes play with. The driver's decays and releases fall in a straight line in level, where
// a SoundFont's fall in a straight line in decibels.
Sf2Envelope EnvelopeFor(const DriverInfo& info, const Instrument& inst, const EnvelopeSettings& settings = {});

// A builder of SoundFont samples and instruments from the ROM, which reuses any already built. The converter adds the
// presets.
class SoundfontBuilder
{
public:
    // Keeps references to `rom` and `info`, which have to outlive the builder. The presets for envelope settings go in
    // banks from `first_free_bank` up.
    SoundfontBuilder(const Rom& rom, const DriverInfo& info, int first_free_bank = 1);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the index of the SoundFont instrument for the driver's instrument at `address`, played with the envelope
    // settings `settings`, or -1 if it has no sample that can be played.
    int InstrumentFor(uint32_t address, const EnvelopeSettings& settings = {});

    // Returns the bank for the presets of bank `bank` played with the envelope settings `settings`: `bank` itself
    // without any, and otherwise a bank of their own, the next free one the first time.
    int BankFor(int bank, const EnvelopeSettings& settings);

    // Adds a preset for `program` in `bank` that plays SoundFont instrument `instrument`, unless the bank has a preset
    // for the program already.
    void AddPreset(int bank, int program, int instrument);

    // Gives every instrument, those built later included, a modulator that mutes a note while the polyphonic key
    // pressure of its key is 64 or more. The MIDI file mutes the voices that the driver's mixer leaves out that way.
    void AddMutes();

private:
    // Returns the index of the SoundFont sample for a sample instrument's sample, or -1 if it isn't valid.
    int SampleFor(const Instrument& inst);

    // Returns a zone that plays sample instrument `voice` on keys `low` to `high`, with the envelope settings
    // `settings`. A drum kit's zones play at the sample's pitch whatever the key.
    sf2::Zone ZoneFor(const Instrument& voice, const EnvelopeSettings& settings, int sample, int low, int high,
                      bool fixed_pitch) const;

    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File file_;
    std::map<std::pair<uint32_t, EnvelopeSettings>, int> instruments_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, int> samples_;
    std::map<std::pair<int, EnvelopeSettings>, int> banks_;
    int next_bank_;
    bool mutes_ = false;
};

} // namespace agbsf2::rare
