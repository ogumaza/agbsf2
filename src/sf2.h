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

// The shortest time a SoundFont envelope can give a phase, in timecents: 1 ms.
constexpr int kInstant = -12000;

// The greatest attenuation, in centibels.
constexpr int kSilent = 1440;

// The release that FluidSynth gives a note when a note of the same exclusive class starts on its MIDI channel, in
// timecents: 0.315 s for a fall of 96 dB. FluidSynth gives it in place of the zone's release, even a quicker one.
constexpr int kExclusiveRelease = -2000;

// The bank in which General MIDI players look for the drum channel's programs.
constexpr int kDrumBank = 128;

// A modulator that makes velocity scale the level in a straight line. The default modulator's curve squares it. This
// one takes its place.
constexpr sf2::Modulator kLinearVelocity = {sf2::kNoteOnVelocity | sf2::kNegative | sf2::kConcave,
                                            sf2::kInitialAttenuation, 480, 0, 0};

// A modulator that makes controller 7 scale the level in a straight line. The default modulator's curve squares it.
// This one takes its place.
constexpr sf2::Modulator kLinearVolume = {uint16_t(sf2::kController | 7) | sf2::kNegative | sf2::kConcave,
                                          sf2::kInitialAttenuation, 480, 0, 0};

// Returns a time in timecents, the unit of SoundFont envelope times, from kInstant to 8000.
int Timecents(double seconds);

// Returns the attenuation of `level`, a fraction of the full level, in centibels from 0 to kSilent. A level of 0 or
// below gives kSilent.
int Centibels(double level);

// Returns the factor that lengthens a SoundFont fade to match a fade to silence over `frames` frames that falls in a
// straight line in level. Such a fade stays loud for most of its length. A SoundFont's level falls in a straight line
// in decibels and becomes quiet sooner. The longer SoundFont fade keeps their loudness close while the fade is audible.
// Short fades fall in larger steps and need less stretching.
double FadeStretch(double frames);

// Returns the length in seconds of the SoundFont fade for a fade to silence over `frames` frames. That fade falls in a
// straight line in level. FadeStretch() gives the factor that lengthens the SoundFont fade to match it.
double FadeSeconds(double frames);

// A SoundFont volume envelope: times in timecents and the sustain level in centibels of attenuation.
struct VolumeEnvelope
{
    int attack = kInstant;
    int decay = kInstant;
    int sustain = 0;
    int release = kInstant;
};

// Adds the generators of `envelope` to `zone`: the attack, decay and release, then the sustain. A phase at its default
// is left out.
void AddVolumeEnvelope(sf2::Zone& zone, const VolumeEnvelope& envelope);

// Adds the generators that tune `zone` by `cents` to it. The whole semitones go in the coarse tune, up to 120 either
// way, and the rest in the fine tune. The fine tune reaches only 99 cents. A part that is 0 is left out.
void AddTuning(sf2::Zone& zone, int cents);

// Returns a zone that plays `sample` on keys `lo` to `hi`, looping it if `loop` is set. It holds the key range, a fine
// tune of `cents` unless that's 0, the sample mode and the sample.
sf2::Zone SampleZone(int sample, bool loop, int lo = 0, int hi = 127, int cents = 0);

// Returns a zone that plays `sample` on keys `lo` to `hi`, with key `root` at the sample's pitch. The zone loops the
// sample if `loop` is set. It holds the key range, the root key, the sample mode and the sample.
sf2::Zone RootKeyZone(int sample, bool loop, int lo, int hi, int root);

// Returns the zones of an instrument that plays sample `first` for `frames` frames of each note, and then sample
// `second`. Both loop on every key with a fine tune of `cents`. With no frames, the instrument plays `first` alone. The
// time of the switch isn't held to the range of Timecents().
std::vector<sf2::Zone> SwitchZones(int first, int frames, int second, int cents = 0);

// An instrument's name and zones, before a SoundFont builder adds the instrument and its preset.
struct NamedZones
{
    std::string name;
    std::vector<sf2::Zone> zones;
};

// The shortest loop a sample gets, in points. A shorter one is repeated until it's at least this long, since some
// players can't play a very short loop.
constexpr uint32_t kMinLoop = 32;

// The points after a loop's end that repeat its start, for players that read past the end when they interpolate.
constexpr uint32_t kLoopGuard = 8;

// Returns true if `s` has a loop. A sample without one keeps the loop points 0 and 0 until Sf2File::Write().
inline bool Looped(const sf2::Sample& s)
{
    return s.loop_end > s.loop_start;
}

// Stores one waveform cycle as three copies looping over the middle one, so players get lead-in and lead-out points
// around the loop.
void SetCycle(sf2::Sample& out, const std::vector<int16_t>& cycle);

// Repeats the loop of `s` after the end of its data until the loop lasts kMinLoop points or more. The loop has to end
// at the end of the data and can't be empty.
void LengthenLoop(sf2::Sample& s);

// Adds kLoopGuard points after the data of `s` that repeat the start of its loop. The loop has to end at the end of the
// data and can't be empty.
void AddLoopGuard(sf2::Sample& s);

// Returns a name made of `kind` and `address` in 8 hex digits, such as "Sample 08123456".
std::string AddressName(const char* kind, uint32_t address);

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
    // Names the SoundFont `title` and gives it the comment "Instruments for <title>, extracted by agbsf2", with
    // `detail` after the title.
    void SetTitle(const std::string& title, const std::string& detail = "");

    // Writes the file. Returns false and sets `error` if it's too big for the format or can't be written.
    bool Write(const std::string& path, std::string& error) const;

    std::string name = "Untitled";
    std::string comment;
};

} // namespace agbsf2
