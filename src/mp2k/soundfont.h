// SPDX-License-Identifier: MIT

// Builds SoundFont instruments from the driver's voices and samples.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "mp2k/driver.h"
#include "mp2k/song.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::mp2k
{

// A SoundFont volume envelope: times in timecents and the sustain level in centibels of attenuation.
struct Sf2Envelope
{
    int attack = -12000;
    int decay = -12000;
    int sustain = 0;
    int release = -12000;
};

// Returns the SoundFont envelope that comes closest to a voice's envelope in the driver.
Sf2Envelope EnvelopeFor(const DriverInfo& info, const Voice& voice);

// The synth voices of Camelot's mixer, which plays a sample of no length as a wave it makes from the sample's data.
enum class Synth
{
    kNone,
    kPulse, // a pulse wave whose duty a triangle wave moves on each frame
    kSaw,   // a saw wave through a filter
    kTriangle
};

// Returns the synth that a voice plays, or Synth::kNone for a voice that plays its sample or a PSG channel.
Synth SynthOf(const Rom& rom, const DriverInfo& info, const Voice& voice);

// Returns `count` points of a pulse or saw synth voice whose sample data starts at `data`, from the start of a note
// that plays at `frequency` Hz, at the mixer's rate. Each point is the sample point that the mixer's output for it
// equals, times 256.
std::vector<int16_t> RenderSynth(const Rom& rom, const DriverInfo& info, Synth synth, uint32_t data, uint32_t frequency,
                                 uint32_t count);

// A preset's bank and program.
struct PresetSlot
{
    int bank = 0;
    int program = 0;
};

// Builds SoundFont samples and instruments from the ROM, reusing any already built. The converter adds the presets.
class SoundfontBuilder
{
public:
    // Keeps references to `rom` and `info`, which have to outlive the builder.
    SoundfontBuilder(const Rom& rom, const DriverInfo& info);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the index of the SoundFont instrument for the voice at `address`, or -1 if it has nothing to play. A
    // pulse or saw synth voice gets a zone for each key it plays, with a sample made for that key, so this adds the
    // zone for `key` if the instrument plays it with one of those.
    int InstrumentFor(uint32_t address, int key);

    // Adds a preset for `program` in `bank` that plays SoundFont instrument `instrument`, unless the bank has a preset
    // for the program already. Returns false if that preset plays another instrument.
    bool AddPreset(int bank, int program, int instrument);

    // Returns the preset of a shared SoundFont that plays SoundFont instrument `instrument` as program `program`,
    // adding one if there isn't one. Each instrument keeps its program, and the banks tell apart the instruments that
    // songs play as the same program: the first gets bank 0, the next bank 1, and so on up to 127. Once a program is
    // taken in every bank, its other instruments get the first free program of the lowest bank that has one. Returns a
    // bank of -1 if there's none.
    PresetSlot SharedPreset(int program, int instrument);

private:
    // Returns the index of the SoundFont sample for a voice's sound, or -1 if it has none or makes one for each key.
    int SampleFor(const Voice& voice);

    int DirectSample(uint32_t address);
    int SquareSample(int duty);
    int WaveSample(uint32_t address);
    int NoiseSample(int narrow);
    int TriangleSample(uint32_t wave);

    // Returns the index of the sample of a pulse or saw synth voice at `key`.
    int SynthSample(const Voice& voice, int key);

    // Adds a zone that plays `voice` on keys `low` to `high`, at the pitch of key `fixed_key` if it isn't -1, and with
    // the drum kit's pan `rhythm_pan`. Adds nothing for a pulse or saw synth voice, whose zones AddSynthKey() adds, or
    // for a voice that has nothing to play. Returns false for one that has nothing to play.
    bool AddZone(sf2::Instrument& instrument, const Voice& voice, int low, int high, int fixed_key, int rhythm_pan);

    // Adds a zone to instrument `instrument`, which plays the voice at `address`, for `key`, if the voice plays it with
    // a pulse or saw synth voice and the instrument has no zone for it.
    void AddSynthKey(int instrument, uint32_t address, int key);

    const Rom& rom_;
    const DriverInfo& info_;
    Sf2File file_;
    std::map<std::array<uint8_t, 12>, int> instruments_;    // by the voice's bytes
    std::map<std::pair<int, uint32_t>, int> samples_;       // by kind and source
    std::map<std::pair<uint32_t, int>, int> synth_samples_; // a pulse or saw synth voice's, by sample and key
    std::set<std::pair<int, int>> synth_keys_;              // the instruments' zones for a key of a synth voice
    std::map<std::pair<int, int>, PresetSlot> slots_;       // SharedPreset()'s presets, by program and instrument
};

} // namespace agbsf2::mp2k
