// SPDX-License-Identifier: MIT

// Builds SoundFont samples from the driver's sound sources: DirectSound PCM samples and synthesized Game Boy PSG
// waveforms.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "konami/driver.h"
#include "psg.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"

namespace agbsf2::konami
{

// The fixed level that wave notes play at. Wave samples are the driver's pre-scaled wave RAM images (see WaveSample),
// stored at half scale so any row fits without clipping.
constexpr int kWaveRowLevel = 2 * kPsgLevelPerVolume * 15;

// The sound source that plays a note.
enum class Kind
{
    kSquare,
    kWave,
    kNoise,
    kSample
};

// Returns the name of the sample of wave `wave` at volume `volume` (0-15). Its preset has the same name.
std::string WaveName(int wave, int volume);

// Builds and caches SoundFont samples from a ROM. The converter adds presets and instruments to the same file.
class SoundfontBuilder
{
public:
    // Sample, instrument and preset counts and the banks given out, for Restore().
    struct Checkpoint
    {
        size_t samples = 0, instruments = 0, presets = 0;
        SongBanks banks = SongBanks(1);
    };

    // Keeps references to `rom` and `info`. Both have to outlive the builder. In a SoundFont that `songs` songs share,
    // SongBanks gives each song's presets their bank and programs.
    SoundfontBuilder(const Rom& rom, const DriverInfo& info, int songs = 1);

    // Returns the SoundFont that the builder adds samples to.
    Sf2File& File()
    {
        return file_;
    }

    // Saves the current sample, instrument and preset counts and the banks given out.
    Checkpoint Save() const
    {
        return {file_.samples.size(), file_.instruments.size(), file_.presets.size(), banks_};
    }

    // Returns the banks and programs of the presets.
    SongBanks& Banks()
    {
        return banks_;
    }

    // Removes everything added since checkpoint `c`, including samples.
    void Restore(const Checkpoint& c);

    // Each returns the index of the SF2 sample for a sound source, which is created on first use and shared afterwards.
    // DsSample() returns -1 for a sample that isn't valid.
    int DsSample(int index);
    int SquareSample(int duty);
    int WaveSample(int wave, int volume);
    int NoiseSample(uint16_t setting);

    // DsRootKey() returns the musical root key of a DirectSound sample (estimated from its waveform), and DsPitched()
    // returns true if a clear pitch was found.
    int DsRootKey(int index);
    bool DsPitched(int index);

    // Returns the MIDI key of PSG square note 0 (from the driver's frequency table).
    int SquareBaseKey() const
    {
        return square_base_;
    }

    // Returns the MIDI key for a note on the wave channel using `wave`.
    int WaveKey(int wave, int note) const;

private:
    // The root key of a DirectSound sample, and whether a clear pitch was found for it.
    struct Pitch
    {
        int key = 60;
        bool pitched = false;
    };

    // Returns the estimated pitch of DirectSound sample `index`, caching it on first use.
    const Pitch& PitchOf(int index);

    // Returns the number of waveform cycles in a wave pattern's 32 steps (1, 2, 4, ...).
    int WaveCycles(int wave) const;

    // Returns the address of the wave table's row for `wave` pre-scaled to `volume`: 16 bytes, two steps each. A wave
    // from 80 up is wave & 7F of the table that sound effects use, or 0 if that table wasn't found. The Dungeon Dice
    // Monsters revision has one row for each wave, at full scale.
    uint32_t WaveRowAddress(int wave, int volume) const;

    // Fills `steps` with the 32 steps (0-15) of a wave: its loudest row in the driver's wave table, scaled to full
    // volume.
    void WavePattern(int wave, int (&steps)[32]) const;

    // Fills `steps` with the 32 steps of the driver's wave table row for `wave` at `volume`.
    void WaveRow(int wave, int volume, int (&steps)[32]) const;

    const Rom& rom_;
    const DriverInfo& info_;
    const double square_note0_hz_; // frequency of PSG square note 0
    const int square_base_;
    Sf2File file_;
    SongBanks banks_;
    std::map<std::pair<Kind, int>, int> cache_; // (kind, parameter) -> SF2 sample
    std::map<int, Pitch> pitch_;
};

} // namespace agbsf2::konami
