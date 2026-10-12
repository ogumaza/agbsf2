// SPDX-License-Identifier: MIT

// Builds SoundFont samples and envelopes from the driver's sounds: the game's 8-bit samples, the Game Boy's square
// waves, wave patterns and noise, and the driver's envelopes of straight-line segments.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rd2/driver.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"

namespace supergbamidi::rd2
{

// Returns the level of a PSG voice at its full volume, as a share of a full-scale sample at the driver's default
// volumes. The driver mixes the PSG at 50%. Volume 15 is therefore 15/128 of the GBA's full scale. A sample voice plays
// a full-scale sample at about 47/128 on each side, or 31/128 in the Super Mario Advance 2 and 3 revisions.
constexpr double PsgShare(Revision revision)
{
    return revision == Revision::kLinkToThePast ? 15.0 / 47.0 : 15.0 / 31.0;
}

// A voice's envelope in a SoundFont's terms: times in timecents and levels in centibels below the full level.
struct Sf2Envelope
{
    int attack = -12000;
    int hold = -12000;
    int decay = -12000;
    int sustain = 0;
    int release = -12000;
    int attenuation = 0; // the envelope's peak, below the full level
};

// Returns the SoundFont envelope closest to the driver's envelope at `address`, with the release of a region's release
// value. `psg` selects a square or noise voice's release, which the PSG's envelope plays.
Sf2Envelope EnvelopeFor(const Rom& rom, uint32_t address, uint8_t release, bool psg);

// Returns the SoundFont envelope of a wave voice. The wave voice leaves its region's envelope out, and its level holds
// until the release. The release of a region's release value fades it as a sample voice's release does.
Sf2Envelope WaveEnvelope(uint8_t release);

// Adds an envelope's generators to a zone, before its last generator (the sample).
void AddEnvelope(sf2::Zone& zone, const Sf2Envelope& envelope);

// A SoundFont being built, with a cache of the samples made for it. The converter adds the instruments and presets.
class SoundfontBuilder
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive the builder. The square, wave and noise samples play at
    // `psg_share` of full scale, the PSG's level against a full-scale sample that PsgShare() gives. The level is in the
    // samples rather than the zones' attenuation. FluidSynth applies only 0.4 of a zone's attenuation. In a SoundFont
    // that `songs` sequences share, SongBanks gives each sequence's presets their bank and programs.
    SoundfontBuilder(const Rom& rom, double psg_share, int songs = 1);

    Sf2File& File()
    {
        return file_;
    }

    // Returns the banks and programs of the presets.
    SongBanks& Banks()
    {
        return banks_;
    }

    // Each returns the index of the SF2 sample for a sound, which is made on first use and shared afterwards.
    // GameSample() takes the address of a sample's header and returns -1 if it can't be read; the sample plays at its
    // own rate on key 60. A square sample plays middle C on key 60, a wave sample C3 on key 48, and a noise sample its
    // NR43 setting's noise on key 60.
    int GameSample(uint32_t address);
    int SquareSample(int duty);
    int WaveSample(uint32_t address);
    int NoiseSample(uint8_t nr43);

    // Adds an instrument with `zones`. Returns its index.
    int AddInstrument(const std::string& name, std::vector<sf2::Zone> zones);

    // Adds a preset that plays instrument `instrument`.
    void AddPreset(const std::string& name, int bank, int program, int instrument);

private:
    const Rom& rom_;
    double psg_share_;
    Sf2File file_;
    SongBanks banks_;
    std::map<uint32_t, int> game_samples_;
    std::map<int, int> squares_;
    std::map<uint32_t, int> waves_;
    std::map<uint8_t, int> noises_;
};

} // namespace supergbamidi::rd2
