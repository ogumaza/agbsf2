// SPDX-License-Identifier: MIT

// Song conversion: the driver model's output -> MIDI file + SoundFont instruments.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "brownie/driver.h"
#include "brownie/soundfont.h"
#include "rom.h"

namespace agbsf2::brownie
{

// Conversion settings and output paths.
struct ConvertOptions
{
    int loops = 2;                // times a looping song's loop is played
    uint16_t track_mask = 0xFFFF; // channels to include (bit c = channel c)
    bool frame_timing = false;    // each event on the frame the driver plays it in, rather than on the beat
    std::string out_dir = ".";
    std::string base_name = "song";
};

// The result of converting a song.
struct SongSummary
{
    bool ok = false;                       // converted, or skipped for having no notes
    bool silent = false;                   // no notes on the chosen channels: nothing was written
    double seconds = 0;                    // the converted length, at the GBA's speed
    double loop_start = -1, loop_end = -1; // seconds; -1 if the song doesn't loop
    int tracks = 0;                        // chosen channels with notes
    double bpm = 0;                        // the song's first tempo, from its beat, at the GBA's speed
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// An instrument that notes play.
struct Instrument
{
    enum Kind : uint8_t
    {
        kSquare,
        kWave,
        kNoise,
        kSamples,
    };

    bool operator<(const Instrument& o) const
    {
        return std::tie(kind, first, frames, second, set) < std::tie(o.kind, o.first, o.frames, o.second, o.set);
    }

    Kind kind = kSquare;
    int first = 0;    // the duty or wave shape at a note's start, or a noise note's kit
    int frames = 0;   // the frames before it switches, or 0
    int second = 0;   // the duty or wave shape it switches to
    uint32_t set = 0; // a sample note's sample set, or in the Magical Vacation revision its sample's start
};

// The instruments that songs play, numbered in the order they're first played, which become a SoundFont's presets. One
// set can serve all of a game's songs, for a shared SoundFont.
class InstrumentSet
{
public:
    // Returns an instrument's program number, adding the instrument if it's new. Programs from 128 on go in the banks
    // after the first.
    int Program(const Instrument& inst);

    // Returns a wave shape's number, adding the shape if it's new.
    int Shape(const WaveShape& shape);

    const WaveShape& ShapeAt(int index) const
    {
        return shape_list_[size_t(index)];
    }

    // Returns a noise drum's kit and key, adding the drum if it's new: each kit has 92 drums, from key 36.
    std::pair<int, int> Drum(const NoiseSound& sound);

    // Adds a key of a sample set's instrument: the sample it plays, and the key and the cents that play the sample at
    // the rate the SoundFont gives it.
    void AddSampleKey(uint32_t set, int key, const GameSample& sample, int root, int cents = 0);

    // Adds the instruments and their presets to `sf`, with the PSG's square and wave zones tuned by `cents`. Returns
    // the warnings for instruments with nothing to play.
    std::vector<std::string> Build(SoundfontBuilder& sf, int cents) const;

private:
    // A key of a sample set's instrument.
    struct SampleKey
    {
        GameSample sample;
        int root = 60;
        int cents = 0;
    };

    std::map<Instrument, int> programs_;
    std::map<WaveShape, int> shapes_;
    std::vector<WaveShape> shape_list_;
    std::map<NoiseSound, int> drums_; // each drum's number, in order of first use
    std::map<uint32_t, std::map<int, SampleKey>> sample_keys_;
};

// Works out a song's length, loop and channels the way ConvertSong() does, without writing anything.
SongSummary InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt);

// Converts one song. If `shared` is supplied, the song's instruments are added to it for a shared SoundFont; otherwise
// a SoundFont is written next to the MIDI file.
SongSummary ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertOptions& opt,
                        InstrumentSet* shared);

// Returns the cents that the PSG's frequency table is out from equal temperament, from -50 to 49, and sets `base` to
// the MIDI key that the driver's key 0 stands for: the median over the keys of the PSG's lowest four octaves.
int PsgTuning(const Rom& rom, const DriverInfo& info, int& base);

} // namespace agbsf2::brownie
