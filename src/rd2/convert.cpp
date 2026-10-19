// SPDX-License-Identifier: MIT

#include "rd2/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "psg.h"
#include "rd2/sequencer.h"
#include "sf2.h"
#include "song_banks.h"
#include "warnings.h"

namespace agbsf2::rd2
{
namespace
{

// The MIDI file's resolution: a tick for each of the driver's units, 150 to the driver's tick and 24 of those to the
// quarter note, so that a frame at the driver's tempo T is exactly T ticks long.
constexpr uint32_t kTicksPerQuarter = 24 * kTickUnits;

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The slowest tempo a MIDI file can hold at this resolution: at 3 or less, a quarter note lasts more than the 2^24
// microseconds a tempo event can give it.
constexpr int kMinMidiTempo = 4;

// A bend that no pitch has, for a note whose bend is unknown when two passes are compared.
constexpr double kNoBend = 1e9;

// Returns how far a voice's pitch in the frame is from its note's, in semitones. A square or wave voice's pitch is a
// frequency setting, whose period is 2048 less the setting. A noise voice has none.
double Semitones(const Voice& v)
{
    if (v.type == kSampleType)
    {
        return v.note_pitch && v.pitch ? 12.0 * std::log2(double(v.pitch) / double(v.note_pitch)) : 0.0;
    }
    if (v.type == kNoiseType || v.pitch >= 2048 || v.note_pitch >= 2048)
    {
        return 0.0;
    }

    return PsgSemitones(int(v.note_pitch), int(v.pitch));
}

// A frame's results for the conversion.
struct FrameData
{
    int tempo = kStartTempo;                // the units the frame lasts
    std::array<double, kPlayerTracks> bend; // semitones from its note of each track's newest voice, or NaN for none
};

// One of the model's events and the frame it happened in.
struct TimedEvent
{
    Event event;
    uint32_t frame = 0;
};

// The model's output for the stretch of a sequence that the plan covers, and its timing in the MIDI file.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<TimedEvent> events;
    std::vector<uint64_t> frame_ticks; // the MIDI tick each frame starts at, and the one after the last frame
    SongPlan plan;                     // in MIDI ticks
    bool frame_timing = false; // each event at the start of the frame the driver plays it in, rather than its time
    int volume2 = 0x80;        // the player's second volume, as the game sets it for the sequence
    std::vector<std::string> warnings;
};

// Returns the MIDI tick of an event at `units` that the driver plays in `frame`: its time, or with frame timing, the
// start of the frame.
uint64_t EventTick(const Simulation& sim, uint64_t units, uint32_t frame)
{
    return sim.frame_timing ? sim.frame_ticks[std::min<size_t>(frame, sim.frame_ticks.size() - 1)] : units;
}

// Returns the MIDI tick of a time in the sequence, `units`: the time, or with frame timing, the start of the first
// frame that reaches it.
uint64_t TimeTick(const Simulation& sim, uint64_t units)
{
    if (!sim.frame_timing)
    {
        return units;
    }

    const auto it = std::lower_bound(sim.frame_ticks.begin(), sim.frame_ticks.end(), units);
    return it == sim.frame_ticks.end() ? sim.frame_ticks.back() : *it;
}

// The plan's record of a track: its first loop, its count of loops, its end, and the time by which it has read its last
// command and its notes have ended. The track's end can release its notes before then. A track that F8 starts again
// doesn't hold the plan open.
struct TrackPlan
{
    bool started = false;
    bool restarted = false;
    bool ended = false;
    int loops = 0;
    uint64_t loop_start = 0;
    uint64_t loop_end = 0;
    uint64_t end = 0;
    uint64_t last_command = 0;
};

using TrackPlans = std::array<TrackPlan, kPlayerTracks>;

// Returns the frame's results, after the model's step: the tempo, and the bend of each track's newest voice.
FrameData RecordFrame(const Sequencer& seq)
{
    FrameData d;
    d.tempo = seq.FrameTempo();
    d.bend.fill(std::numeric_limits<double>::quiet_NaN());
    for (int n = 0; n < kPlayerTracks; n++)
    {
        const int slot = seq.GetPlayer().tracks[size_t(n)];
        if (slot < 0)
        {
            continue;
        }

        const int v = seq.Tracks()[size_t(slot)].voices;
        if (v >= 0 && seq.Voices()[size_t(v)].state == kVoicePlaying)
        {
            d.bend[size_t(n)] = Semitones(seq.Voices()[size_t(v)]);
        }
    }

    return d;
}

// Updates the tracks' plans from one of the model's events.
void UpdateTrackPlans(const Event& e, TrackPlans& tracks)
{
    if (e.track < 0 || e.track >= kPlayerTracks)
    {
        return;
    }

    TrackPlan& t = tracks[size_t(e.track)];
    t.started = true;
    if (e.phase == Event::kCommands && e.kind != Event::kTrackEnd)
    {
        t.last_command = std::max(t.last_command, e.units + (e.kind == Event::kNote ? e.length : 0));
    }

    if (e.kind == Event::kJump && e.target_units != kUnreadTarget && e.target_units < e.units)
    {
        if (++t.loops == 1)
        {
            t.loop_start = e.target_units;
            t.loop_end = e.units;
        }
    }
    else if (e.kind == Event::kTrackEnd)
    {
        t.ended = true;
        t.end = e.units;
    }
    else if (e.kind == Event::kTrackStart && e.value < uint32_t(kPlayerTracks))
    {
        tracks[e.value].started = true;
        tracks[e.value].restarted = true;
    }
}

// Returns PlanSong()'s plan for the sequence, in units, once every track that counts has looped or ended. Returns
// nothing before then. A track counts once it has started, unless F8 started it again. A track without a loop has read
// its last command and released its notes by its end at the latest. With `second`, the loop starts at the sequence's
// second pass through it.
std::optional<SongPlan> PlanLoop(const TrackPlans& tracks, int loops, bool second)
{
    std::vector<TrackLayout> layouts;
    for (const TrackPlan& t : tracks)
    {
        if (!t.started || t.restarted)
        {
            continue;
        }
        if (t.loops == 0 && !t.ended)
        {
            return std::nullopt;
        }

        TrackLayout layout;
        layout.loops = t.loops > 0;
        layout.loop_start = t.loop_start;
        layout.loop_end = t.loop_end;
        layout.end = t.end;
        layout.last_command = std::min(t.last_command, t.end);
        layouts.push_back(layout);
    }

    return PlanSong(layouts, loops, second);
}

// Returns the stretch of the sequence that the conversion covers, from PlanLoop()'s plan if it made one. A looping plan
// ends after its last pass through the loop, or at `stopped` if the model stopped first. Without a looping plan, the
// sequence ends at the latest of the tracks' ends. The tracks that F8 started again count too. If no track ended, the
// sequence ends at `stopped`.
SongPlan FinishPlan(const std::optional<SongPlan>& loop, const TrackPlans& tracks, uint64_t stopped)
{
    SongPlan plan;
    if (loop && loop->loops)
    {
        plan = *loop;
        plan.end = std::min(plan.end, stopped);
    }
    else
    {
        for (const TrackPlan& t : tracks)
        {
            if (t.ended)
            {
                plan.end = std::max(plan.end, t.end);
            }
        }
    }
    if (plan.end == 0)
    {
        plan.end = stopped;
    }

    return plan;
}

// Returns the conversion's warnings: the model's, then a cut-off after an hour, then a tempo too slow for the MIDI
// file.
std::vector<std::string> SimulationWarnings(const Sequencer& seq, const std::vector<FrameData>& frames, bool finished)
{
    std::vector<std::string> warnings = seq.Warnings();
    if (!finished && seq.Valid() && frames.size() == kMaxFrames)
    {
        warnings.push_back("the sequence was cut off after an hour");
    }
    for (const FrameData& d : frames)
    {
        if (d.tempo < kMinMidiTempo)
        {
            warnings.push_back("the tempo drops below " + std::to_string(kMinMidiTempo) +
                               ", which the MIDI file plays at " + std::to_string(kMinMidiTempo));
            break;
        }
    }

    return warnings;
}

// Runs the model until every track has looped or ended and the sequence has played its loop `loops` times. A track's
// units are the MIDI file's ticks: each frame lasts the tempo's units, and each command comes at its track's time, or
// with `frame_timing`, at the start of the frame the driver plays it in. With `second`, the loop starts at the
// sequence's second pass through it, and the sequence plays the first pass before it.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, bool frame_timing, bool second)
{
    Simulation sim;
    sim.frame_timing = frame_timing;
    Sequencer seq(rom, info, song);
    sim.frame_ticks.push_back(0);

    TrackPlans tracks;
    std::optional<SongPlan> loop;
    bool finished = false; // the sequence played what the plan requires, or ended
    for (uint32_t f = 0; f < kMaxFrames && seq.Valid(); f++)
    {
        seq.Step();
        if (f == 0)
        {
            sim.volume2 = seq.GetPlayer().volume2;
        }

        sim.frames.push_back(RecordFrame(seq));
        sim.frame_ticks.push_back(seq.Elapsed());
        for (int n = 0; n < kPlayerTracks; n++)
        {
            if (seq.GetPlayer().tracks[size_t(n)] >= 0)
            {
                tracks[size_t(n)].started = true;
            }
        }
        for (const Event& e : seq.Events())
        {
            sim.events.push_back({e, f});
            UpdateTrackPlans(e, tracks);
        }

        if (!loop)
        {
            loop = PlanLoop(tracks, loops, second);
        }
        if ((loop && (!loop->loops || sim.frame_ticks[f] >= loop->end)) || seq.Ended())
        {
            finished = true;
            break;
        }
    }

    sim.plan = FinishPlan(loop, tracks, sim.frame_ticks.back());
    sim.warnings = SimulationWarnings(seq, sim.frames, finished);

    return sim;
}

// Returns the MIDI file's tempo changes before the end, as (MIDI tick, tempo).
std::vector<std::pair<uint64_t, int>> TempoMap(const Simulation& sim)
{
    std::vector<std::pair<uint64_t, int>> tempos;
    for (size_t f = 0; f < sim.frames.size() && (f == 0 || sim.frame_ticks[f] < sim.plan.end); f++)
    {
        const int tempo = std::max(sim.frames[f].tempo, kMinMidiTempo);
        if (tempos.empty() || tempos.back().second != tempo)
        {
            tempos.emplace_back(sim.frame_ticks[f], tempo);
        }
    }
    if (tempos.empty())
    {
        tempos.emplace_back(0, kStartTempo);
    }

    return tempos;
}

// The sound a SoundFont zone plays for a key.
struct ZoneSound
{
    bool operator<(const ZoneSound& o) const
    {
        return std::tie(type, sound, envelope, release, root, coarse, pan, own_pan) <
               std::tie(o.type, o.sound, o.envelope, o.release, o.root, o.coarse, o.pan, o.own_pan);
    }

    bool operator==(const ZoneSound& o) const
    {
        return !(*this < o) && !(o < *this);
    }

    uint8_t type = kSampleType;
    uint32_t sound = 0;    // a sample's header, a wave's data, a square's duty or a noise's NR43
    uint32_t envelope = 0; // the envelope's points
    uint8_t release = 0;
    int root = 60;        // the key that plays the sound at its original pitch
    int coarse = 0;       // semitones added to it
    int pan = 0;          // the SoundFont's pan, from -500 to 500
    bool own_pan = false; // a drum's: the track's pan doesn't move it
};

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int key = 60;
    int velocity = 127;
    int program = 0;
    uint64_t on = 0; // MIDI ticks
    uint64_t off = 0;
    uint64_t length = 0;
    uint32_t frame = 0; // the frame the driver starts it in
    bool open = true;
};

// An instrument of the driver's banks, as a program of the MIDI file.
struct Program
{
    uint16_t bank = 0;
    uint16_t instrument = 0;
    std::map<int, ZoneSound> keys; // the sound of each key its notes play
};

// The conversion of a sequence's notes into the MIDI file's, and of its instruments into programs.
class NoteMaker
{
public:
    NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim, uint32_t track_mask)
        : rom_(rom), info_(info), sim_(sim), mask_(track_mask)
    {
    }

    // Works out the notes, the end of each, and the programs they play.
    std::vector<Note> Make()
    {
        std::vector<Note> notes;
        std::map<uint32_t, size_t> by_id;
        std::array<uint32_t, kVoices> voice_note = {};
        const uint64_t end = TimeTick(sim_, sim_.plan.end);

        auto close = [&](uint32_t id, uint64_t tick)
        {
            const auto found = by_id.find(id);
            if (found == by_id.end() || !notes[found->second].open)
            {
                return;
            }

            // A note lasts at least a tick, so that its note off can't come before its note on.
            Note& n = notes[found->second];
            n.open = false;
            n.off = std::max(std::min(tick, end), n.on + 1);
        };

        for (const TimedEvent& te : sim_.events)
        {
            const Event& e = te.event;
            if (e.kind == Event::kNote && e.voice >= 0)
            {
                // A voice that plays a new note in legato ends its last one.
                close(voice_note[size_t(e.voice)], EventTick(sim_, e.units, te.frame));
                voice_note[size_t(e.voice)] = 0;
                if (e.velocity == 0 || e.track < 0 || !(mask_ >> e.track & 1) || e.units >= sim_.plan.end)
                {
                    continue;
                }

                Note n;
                n.track = e.track;
                n.velocity = std::clamp(int(std::lround(127 * std::sqrt(e.velocity / 127.0))), 1, 127);
                n.on = EventTick(sim_, e.units, te.frame);
                n.length = e.length;
                n.frame = te.frame;
                n.program = ProgramOf(e, n.key);
                if (n.program < 0)
                {
                    continue;
                }

                by_id[e.id] = notes.size();
                notes.push_back(n);
                voice_note[size_t(e.voice)] = e.id;
            }
            else if (e.kind == Event::kRelease || e.kind == Event::kStop)
            {
                const auto found = by_id.find(e.id);
                if (found != by_id.end())
                {
                    close(e.id, OffTick(notes[found->second], e, te.frame));
                }
            }
        }

        for (size_t i = 0; i < notes.size(); i++)
        {
            if (notes[i].open)
            {
                notes[i].open = false;
                notes[i].off = std::max(end, notes[i].on + 1);
            }
        }

        return notes;
    }

    const std::vector<Program>& Programs() const
    {
        return programs_;
    }

    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Returns the tick at which a note's release or stop ends it in the MIDI file. A command's comes at its time. The
    // PSG voices' update comes at the start of its frame, and the mixer at the end, before the next frame: a release
    // there keeps to the note's length within the frame it starts, and a stop comes at that frame's start. With frame
    // timing, each comes at the start of the frame the driver plays it in.
    uint64_t OffTick(const Note& n, const Event& e, uint32_t frame) const
    {
        if (e.phase == Event::kCommands)
        {
            return EventTick(sim_, e.units, frame);
        }

        const size_t g = std::min<size_t>(e.phase == Event::kMixer ? frame + 1 : frame, sim_.frame_ticks.size() - 1);
        const uint64_t hi = sim_.frame_ticks[g];
        const uint64_t lo = g > 0 ? std::min(sim_.frame_ticks[g - 1] + 1, hi) : 0;
        return e.kind == Event::kRelease && !sim_.frame_timing ? std::clamp(n.on + n.length, lo, hi) : hi;
    }

    // Returns the program of a note's instrument, and sets the note's key: a sample voice's note, a square's or the
    // wave's pitch, or a noise voice's note. Returns -1 for a note that can't be converted.
    int ProgramOf(const Event& e, int& key)
    {
        const Lookup& lk = e.lookup;
        if (lk.missing)
        {
            WarnMissing(e);
            return -1;
        }

        const std::optional<ZoneSound> z = ZoneFor(e, key);
        if (!z)
        {
            return -1;
        }
        if (key < 0 || key > 127)
        {
            warnings_.push_back("a note's key is outside MIDI's range, so it's left out");
            return -1;
        }

        // A sample instrument without drums or a sample for each key gets a zone for every key.
        const bool fill = e.type == kSampleType && !lk.drum && !lk.key_sample;
        const size_t p = FindProgram(e.bank, e.instrument, fill);
        programs_[p].keys.emplace(key, *z);

        return int(p);
    }

    // Warns that a note's instrument has no region for the note. Each warning appears once. The driver plays such a
    // note from the bank's table of offsets. In Super Mario Advance 2, that table read as a region gives a level of 0,
    // and a sample from past the end of the sample set's table.
    void WarnMissing(const Event& e)
    {
        const uint32_t base = BankAddress(rom_, info_, e.bank);
        const bool none = rom_.U16(base + 2 * uint32_t(e.instrument)) == 0;
        char text[112];
        std::snprintf(text, sizeof text,
                      none ? "instrument %u isn't in bank %u, so its notes are left out"
                           : "instrument %u of bank %u has no region for some of its notes, which are left out",
                      unsigned(e.instrument), unsigned(e.bank));
        WarnOnce(warnings_, text);
    }

    // Returns the zone that plays a note, and sets the note's key. Returns nothing for a voice of a type that the
    // conversion doesn't handle.
    std::optional<ZoneSound> ZoneFor(const Event& e, int& key) const
    {
        const Lookup& lk = e.lookup;
        const bool fixed = lk.drum || lk.key_sample;
        if (e.type == kSampleType && !fixed)
        {
            key = e.key;
            return SampleZone(e.sample, lk);
        }

        const uint32_t region = lk.region;
        const uint8_t tuning = RegionTuning(rom_, region);
        const int index = PitchIndex(fixed ? kFixedNote : e.key, tuning);
        ZoneSound z;
        z.type = e.type;
        z.envelope = lk.envelope;
        z.release = RegionRelease(rom_, region);
        z.own_pan = lk.drum;
        if (lk.drum && lk.drum_pan != kCentre)
        {
            z.pan = std::clamp(int(std::lround((lk.drum_pan - kCentre) / 63.0 * 500)), -500, 500);
        }

        switch (e.type)
        {
        case kSampleType:
            // A drum or an instrument with a sample for each key plays the sample at the region's pitch.
            z.sound = e.sample;
            key = e.key;
            z.root = key;
            z.coarse = index - kUnityIndex;
            break;

        case kSquare1Type:
        case kSquare2Type:
            z.sound = SettledDuty(e) & 3u;
            key = index + 12;
            break;

        case kWaveType:
            z.sound = e.psg;
            z.envelope = 0; // the wave voice leaves its region's envelope out
            z.root = 48;
            key = index;
            break;

        case kNoiseType:
            {
                // A width that isn't 0 sets NR43's bit 3.
                uint8_t nr43 = NoiseTableEntry(rom_, info_, uint32_t(index));
                if (SettledDuty(e) != 0)
                {
                    nr43 |= 8;
                }
                z.sound = nr43;
                key = e.key;
                z.root = key;
                break;
            }

        default:
            return std::nullopt;
        }

        return z;
    }

    // Returns the duty or width that a square or noise voice settles on: the low byte of `e.psg`, or a table's last
    // entry. The driver steps through the table's other entries, a frame each, and holds the last.
    uint8_t SettledDuty(const Event& e) const
    {
        return HasPsgTable(rom_, e.lookup.region) ? LastPsgTableEntry(rom_, e.psg) : uint8_t(e.psg);
    }

    // Returns the zone of a sample region that plays `sample` at the region's tuning. The regions of drums, and of
    // instruments with a sample for each key, play at a fixed pitch instead.
    ZoneSound SampleZone(uint32_t sample, const Lookup& lk) const
    {
        const uint8_t tuning = RegionTuning(rom_, lk.region);
        ZoneSound z;
        z.sound = sample;
        z.envelope = lk.envelope;
        z.release = RegionRelease(rom_, lk.region);
        z.root = std::clamp(int(tuning), 0, 127);
        z.coarse = z.root - tuning;
        return z;
    }

    // Returns the index of the program for an instrument of a bank, after adding the program if it's new. The programs
    // come in the order the sequence first plays them. A new program gets a zone for every key if `fill` is set.
    size_t FindProgram(uint16_t bank, uint16_t instrument, bool fill)
    {
        size_t p = 0;
        while (p < programs_.size() && (programs_[p].bank != bank || programs_[p].instrument != instrument))
        {
            p++;
        }
        if (p == programs_.size())
        {
            programs_.push_back({bank, instrument, {}});
            if (fill)
            {
                FillKeys(programs_.back());
            }
        }

        return p;
    }

    // Gives a sample instrument without drums or a sample for each key a zone for every key, from its regions, so that
    // the SoundFont plays it on keys the sequence doesn't.
    void FillKeys(Program& program) const
    {
        for (int key = 0; key < 128; key++)
        {
            const Lookup lk = LookUpInstrument(rom_, info_, program.bank, program.instrument, uint8_t(key));
            if (!lk.found || !lk.region || lk.missing || lk.drum || lk.key_sample ||
                RegionType(rom_, lk.region) != kSampleType)
            {
                continue;
            }

            const uint32_t sample = SampleAddress(rom_, info_, program.bank, rom_.U16(lk.region + 2));
            program.keys.emplace(key, SampleZone(sample, lk));
        }
    }

    const Rom& rom_;
    const DriverInfo& info_;
    const Simulation& sim_;
    uint32_t mask_;
    std::vector<Program> programs_;
    std::vector<std::string> warnings_;
};

// Adds the SoundFont presets of a sequence's programs, each in the bank and program of `slots` that its program number
// picks. Returns the warnings for sounds that can't be read.
std::vector<std::string> AddPresets(const Rom& rom, SoundfontBuilder& sf, const std::vector<Program>& programs,
                                    const std::vector<BankProgram>& slots)
{
    std::vector<std::string> warnings;
    for (size_t p = 0; p < programs.size(); p++)
    {
        const Program& program = programs[p];

        // Keys in a row with the same sound share a zone.
        std::vector<sf2::Zone> zones;
        for (auto it = program.keys.begin(); it != program.keys.end();)
        {
            const int first = it->first;
            const ZoneSound z = it->second;
            int last = first;
            for (++it; it != program.keys.end() && it->first == last + 1 && it->second == z; ++it)
            {
                last = it->first;
            }

            // The PSG's samples hold its level against the samples' (see PsgShare()).
            int sample = -1;
            switch (z.type)
            {
            case kSampleType:
                sample = sf.GameSample(z.sound);
                break;
            case kSquare1Type:
            case kSquare2Type:
                sample = sf.SquareSample(int(z.sound));
                break;
            case kWaveType:
                sample = sf.WaveSample(z.sound);
                break;
            default:
                sample = sf.NoiseSample(uint8_t(z.sound));
                break;
            }
            if (sample < 0)
            {
                char text[96];
                std::snprintf(text, sizeof text, "the sample at 0x%08X can't be read, so its notes are left out",
                              unsigned(z.sound));
                warnings.push_back(text);
                continue;
            }

            sf2::Zone zone;
            zone.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, first, last));
            zone.generators.push_back(sf2::Generator::Value(sf2::kOverridingRootKey, z.root));
            if (z.coarse)
            {
                zone.generators.push_back(sf2::Generator::Value(sf2::kCoarseTune, z.coarse));
            }
            if (z.pan)
            {
                zone.generators.push_back(sf2::Generator::Value(sf2::kPan, z.pan));
            }

            // A drum keeps its pan whatever its track's pan is. This modulator replaces the default one from CC10.
            if (z.own_pan)
            {
                zone.modulators.push_back({uint16_t(sf2::kController | cc::kPan) | sf2::kBipolar, sf2::kPan, 0, 0, 0});
            }

            zone.generators.push_back(
                sf2::Generator::Value(sf2::kSampleModes, Looped(sf.File().samples[size_t(sample)]) ? 1 : 0));
            zone.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

            // A square's or noise's release is the PSG's envelope. A sample's or the wave's takes a share of its level
            // off each frame.
            const bool psg_release = z.type == kSquare1Type || z.type == kSquare2Type || z.type == kNoiseType;
            const int release = psg_release ? PsgRelease(z.release) : SampleRelease(z.release);
            const Sf2Envelope env = z.type == kWaveType ? WaveEnvelope(release) : EnvelopeFor(rom, z.envelope, release);
            AddEnvelope(zone, env);
            zones.push_back(zone);
        }

        char name[48];
        std::snprintf(name, sizeof name, "Bank %u instrument %u", unsigned(program.bank), unsigned(program.instrument));
        sf.AddPreset(name, slots[p].bank, slots[p].program, sf.AddInstrument(name, std::move(zones)));
    }

    return warnings;
}

// Returns a track's levels on each side (full scale 128) for a voice at full velocity and envelope, from its pan and
// volume and the player's volumes. The driver works out a sample voice's levels in the same way. The revision's
// volume_scale and volume_shift turn the voice's level into its volume. With the player's second volume at its
// default, 0x80, the volume reaches 190 at most in A Link to the Past's revision and 126 at most in the others. The
// Super Mario Advance 2 revision leaves out the player's second volume. That revision's mixer splits the volume
// between the sides in 128ths and divides each voice's points by 256. The other mixers split it in 256ths and divide
// the mix by 128. Both ways give the same levels.
std::pair<double, double> TrackLevels(Revision revision, int pan, int volume, int player_volume, int player_volume2)
{
    const RevisionTraits& traits = TraitsOf(revision);
    pan = std::clamp(pan, 0, 127);

    const double scale = double(traits.volume_scale) * 512.0 / double(1u << traits.volume_shift);
    const double second = traits.player_volume ? player_volume2 / 128.0 : 1.0;
    const double level = scale * 127 * (player_volume / 128.0) * second * (volume / 256.0) * (32767.0 / 32768.0);
    return {(127 - pan) * level / 256 * 127 / 128, pan * level / 256 * 127 / 128};
}

// A track's settings that the MIDI file's controllers follow, and the player's volumes.
struct Levels
{
    int pan = kCentre;
    int volume = 0x80;
    int player_volume = 0x80;
    int player_volume2 = 0x80;
};

// Returns a track's levels before its first change: the driver's, with the second volume that the game gives the
// player.
Levels StartLevels(const Simulation& sim)
{
    Levels start;
    start.player_volume2 = sim.volume2;

    return start;
}

// A change of a track's levels: its tick, the levels from there on, and whether it sets the track's pan.
struct LevelChange
{
    uint64_t tick = 0;
    Levels levels;
    bool pan = false;
};

// Returns each track's changes of levels. They change when the track's pan or volume changes, when the player's volume
// does, and when F8 starts the track with another track's settings. Changes at the same tick are one change.
std::array<std::vector<LevelChange>, kPlayerTracks> TrackChanges(const Simulation& sim)
{
    std::array<std::vector<LevelChange>, kPlayerTracks> changes;
    std::array<Levels, kPlayerTracks> levels;
    levels.fill(StartLevels(sim));
    for (const TimedEvent& te : sim.events)
    {
        const Event& e = te.event;
        if (e.track < 0 || e.track >= kPlayerTracks || e.units >= sim.plan.end)
        {
            continue;
        }

        // Each event changes the tracks it names; the player's volume changes them all.
        std::vector<int> changed;
        if (e.kind == Event::kPan || e.kind == Event::kTrackVolume)
        {
            int& setting = e.kind == Event::kPan ? levels[size_t(e.track)].pan : levels[size_t(e.track)].volume;
            setting = int(e.value);
            changed.push_back(e.track);
        }
        else if (e.kind == Event::kTrackStart && e.value < uint32_t(kPlayerTracks))
        {
            levels[e.value] = levels[size_t(e.track)];
            changed.push_back(int(e.value));
        }
        else if (e.kind == Event::kPlayerVolume)
        {
            for (int n = 0; n < kPlayerTracks; n++)
            {
                levels[size_t(n)].player_volume = int(e.value);
                changed.push_back(n);
            }
        }

        // The model can run another track's event after a track's later change in the same frame. The event then goes
        // at that change's tick. Otherwise the earlier tick would sort before the later change. The later change would
        // then keep the old levels.
        const uint64_t tick = EventTick(sim, e.units, te.frame);
        const bool pan = e.kind == Event::kPan || e.kind == Event::kTrackStart;
        for (int n : changed)
        {
            auto& list = changes[size_t(n)];
            const uint64_t at = list.empty() ? tick : std::max(tick, list.back().tick);
            if (list.empty() || list.back().tick != at)
            {
                list.push_back({at, levels[size_t(n)], false});
            }
            list.back().levels = levels[size_t(n)];
            list.back().pan = list.back().pan || pan;
        }
    }

    return changes;
}

// Writes a track's CC10 for pan and CC11 for loudness at the start and whenever they change. A player that jumps back
// to `loop`, the loop's start, retains the levels from the loop's end, as the game does until the loop sets them. So
// CC11 is written again at the first level-change record at or after `loop`, and CC10, which follows the pan alone, at
// the first record that sets the pan, even if their values haven't changed. `start` gives the levels before the
// track's first change.
void WriteLevels(MidiTrack& mt, int c, Revision revision, const Levels& start, const std::vector<LevelChange>& changes,
                 std::optional<uint64_t> loop)
{
    int cc10 = -1, cc11 = -1;
    bool again10 = loop.has_value(), again11 = loop.has_value();

    auto write = [&](const LevelChange& change)
    {
        const bool looped = loop && change.tick >= *loop;
        const bool write10 = looped && again10 && change.pan, write11 = looped && again11;
        again10 = again10 && !write10;
        again11 = again11 && !write11;

        const Levels& l = change.levels;
        const auto [left, right] = TrackLevels(revision, l.pan, l.volume, l.player_volume, l.player_volume2);
        int n10 = cc10 < 0 ? 64 : cc10, n11 = cc11 < 0 ? 0 : cc11;
        LevelsToControllers(left, right, n10, n11);
        if (n10 != cc10 || write10)
        {
            mt.Control(uint32_t(change.tick), c, cc::kPan, cc10 = n10);
        }
        if (n11 != cc11 || write11)
        {
            mt.Control(uint32_t(change.tick), c, cc::kExpression, cc11 = n11);
        }
    };

    if (changes.empty() || changes[0].tick != 0)
    {
        write({0, start, false});
    }
    for (const LevelChange& change : changes)
    {
        write(change);
    }
}

// Writes a track's pitch bends: each frame's, at the frame's start, and each note's first frame's at its note on, so
// that the note starts with it, wherever it changes, and the first from `loop`, the loop's start, on, since a player
// that jumps back there keeps the bend that the loop's end left.
void WriteBends(MidiTrack& mt, int c, const Simulation& sim, int n, const std::vector<const Note*>& notes, int range,
                std::optional<uint64_t> loop)
{
    std::vector<std::pair<uint64_t, double>> bends;
    for (size_t f = 0; f < sim.frames.size() && sim.frame_ticks[f] < sim.plan.end; f++)
    {
        if (!std::isnan(sim.frames[f].bend[size_t(n)]))
        {
            bends.push_back({sim.frame_ticks[f], sim.frames[f].bend[size_t(n)]});
        }
    }
    for (const Note* note : notes)
    {
        if (!std::isnan(sim.frames[note->frame].bend[size_t(n)]))
        {
            bends.push_back({note->on, sim.frames[note->frame].bend[size_t(n)]});
        }
    }
    std::stable_sort(bends.begin(), bends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    int bend = 8192;
    for (const auto& [tick, semitones] : bends)
    {
        if (loop && tick >= *loop)
        {
            loop.reset();
            bend = -1;
        }

        const int v = BendValue(semitones, range);
        if (v != bend)
        {
            mt.PitchBend(uint32_t(tick), c, bend = v);
        }
    }
}

// Writes a track's notes, each with its program, which starts as `program` and is written again at the first note at or
// after `loop`, the loop's start, since a looping player retains the program from the loop's end. A MIDI channel can't
// play a key twice at once, so a new note on a sounding key ends the earlier one, and two notes on the same key that
// start together become one MIDI note, the first, lasting as long as the longer of them.
void WriteNotes(MidiTrack& mt, int c, const std::vector<const Note*>& notes, const std::vector<BankProgram>& slots,
                int program, std::optional<uint64_t> loop)
{
    std::vector<uint64_t> offs(notes.size());
    std::vector<bool> merged(notes.size());
    std::array<int, 128> held;
    held.fill(-1);
    for (size_t i = 0; i < notes.size(); i++)
    {
        offs[i] = notes[i]->off;
        int& before = held[size_t(notes[i]->key)];
        if (before >= 0 && notes[size_t(before)]->on == notes[i]->on)
        {
            merged[i] = true;
            offs[size_t(before)] = std::max(offs[size_t(before)], offs[i]);
            continue;
        }
        if (before >= 0 && offs[size_t(before)] > notes[i]->on)
        {
            offs[size_t(before)] = notes[i]->on;
        }
        before = int(i);
    }

    for (size_t i = 0; i < notes.size(); i++)
    {
        const Note* note = notes[i];
        if (merged[i])
        {
            continue;
        }

        if (loop && note->on >= *loop)
        {
            loop.reset();
            program = -1;
        }

        if (note->program != program)
        {
            if (program < 0 || slots[size_t(note->program)].bank != slots[size_t(program)].bank)
            {
                mt.Bank(uint32_t(note->on), c, slots[size_t(note->program)].bank);
            }

            program = note->program;
            mt.Program(uint32_t(note->on), c, slots[size_t(program)].program);
        }
        mt.NoteOn(uint32_t(note->on), c, note->key, note->velocity);
        mt.NoteOff(uint32_t(std::max(offs[i], note->on + 1)), c, note->key);
    }
}

// Returns true if every track's notes of the sequence's second pass through its loop start as those of its first do: at
// the same ticks from the pass's start, with the same key, velocity and program, the levels that the MIDI file gives
// them, and the same bend and tempo. A track keeps its pan, volume and bend when it jumps back to its loop, so the
// first pass can play unlike the later ones. `sim` has to hold two passes of the loop, on the beat.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim, 0xFFFF);
    const std::array<std::vector<LevelChange>, kPlayerTracks> changes = TrackChanges(sim);
    const SongPlan& plan = sim.plan;
    PassComparison passes(plan.loop_start, plan.loop_end - plan.loop_start);
    for (const Note& n : maker.Make())
    {
        // The track's levels from its last change up to the note's start, as CC10 and CC11 give them.
        const std::vector<LevelChange>& list = changes[size_t(n.track)];
        const auto by_tick = [](uint64_t tick, const LevelChange& change)
        {
            return tick < change.tick;
        };
        const auto after = std::upper_bound(list.begin(), list.end(), n.on, by_tick);
        const Levels levels = after == list.begin() ? StartLevels(sim) : std::prev(after)->levels;
        const auto [left, right] =
            TrackLevels(info.revision, levels.pan, levels.volume, levels.player_volume, levels.player_volume2);
        int cc10 = 64, cc11 = 0;
        LevelsToControllers(left, right, cc10, cc11);

        // A note's bend is its track's newest voice's in the frame it starts, which a voice that the note didn't take
        // can leave unknown.
        const FrameData& d = sim.frames[n.frame];
        const double bend = std::isnan(d.bend[size_t(n.track)]) ? kNoBend : d.bend[size_t(n.track)];
        passes.Add(n.on, n.track, n.key,
                   {double(n.velocity), double(n.program), double(cc10), double(cc11), bend, double(d.tempo)});
    }

    return passes.Alike();
}

// Writes the MIDI file: a conductor track, then a track for each of the player's tracks that plays notes, with the
// levels of the driver's revision.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, Revision revision,
               const Simulation& sim, const std::vector<Note>& notes, const std::vector<BankProgram>& slots,
               std::string& error)
{
    constexpr uint16_t kDivision = kTicksPerQuarter;
    const SongPlan& plan = sim.plan;
    const uint32_t end = uint32_t(TimeTick(sim, plan.end));
    MidiFile midi(kDivision);

    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const auto& [tick, tempo] : TempoMap(sim))
    {
        conductor.Tempo(uint32_t(tick), QuarterMicros(kTicksPerQuarter, uint64_t(tempo)));
    }
    const uint64_t loop_start = TimeTick(sim, plan.loop_start);
    if (plan.loops)
    {
        conductor.Meta(uint32_t(loop_start), 0x06, "loopStart");
        conductor.Meta(uint32_t(TimeTick(sim, plan.loop_end)), 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    // The loop's start, from which each track writes its settings again, unless the loop starts with the sequence.
    const std::optional<uint64_t> loop = plan.loops && loop_start > 0 ? std::optional(loop_start) : std::nullopt;

    const auto changes = TrackChanges(sim);
    for (int n = 0; n < kPlayerTracks; n++)
    {
        std::vector<const Note*> list;
        for (const Note& note : notes)
        {
            if (note.track == n)
            {
                list.push_back(&note);
            }
        }
        if (list.empty())
        {
            continue;
        }

        const int c = MelodicChannel(n);
        MidiTrack& mt = midi.AddTrack();
        mt.Name("Track " + std::to_string(n));
        mt.SetEnd(end);

        // The pitch bend range covers the track's largest bend.
        double widest = 0;
        for (const FrameData& d : sim.frames)
        {
            if (!std::isnan(d.bend[size_t(n)]))
            {
                widest = std::max(widest, std::fabs(d.bend[size_t(n)]));
            }
        }
        const int range = BendRangeFor(widest, kSmallBend);

        const int program = list[0]->program;
        mt.Bank(0, c, slots[size_t(program)].bank);
        mt.Program(0, c, slots[size_t(program)].program);
        mt.Control(0, c, cc::kVolume, 127);
        mt.Control(0, c, cc::kReverb, 0);
        mt.Control(0, c, cc::kChorus, 0);
        if (range)
        {
            mt.BendRange(0, c, range);
            mt.PitchBend(0, c, 8192);
            WriteBends(mt, c, sim, n, list, range, loop);
        }

        WriteLevels(mt, c, revision, StartLevels(sim), changes[size_t(n)], loop);
        WriteNotes(mt, c, list, slots, program, loop);
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a sequence, writing its files if `write` is set.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write)
{
    SongReport report;
    if (song < 0 || song >= int(info.sequence_addresses.size()))
    {
        report.warnings.push_back("there's no such sequence");
        return report;
    }

    // The marked loop starts at the sequence's second pass if its first plays unlike it, as when the tracks come back
    // to their loops with the pan or volume that the loop's end left them. The passes are compared on the beat, as the
    // default MIDI file plays them, so frame timing moves the same loops.
    Simulation sim = Simulate(rom, info, song, settings.loops, settings.frame_timing, false);
    if (sim.plan.loops && !FirstPassRepeats(rom, info, Simulate(rom, info, song, 2, false, false)))
    {
        sim = Simulate(rom, info, song, settings.loops, settings.frame_timing, true);
    }
    report.warnings = sim.warnings;

    NoteMaker maker(rom, info, sim, settings.track_mask);
    const std::vector<Note> notes = maker.Make();
    report.warnings.insert(report.warnings.end(), maker.Warnings().begin(), maker.Warnings().end());

    std::array<bool, kPlayerTracks> played = {};
    for (const Note& n : notes)
    {
        played[size_t(n.track)] = true;
    }
    for (bool p : played)
    {
        report.tracks += p ? 1 : 0;
    }

    report.seconds = TickSeconds(sim.frame_ticks, sim.plan.end);
    if (sim.plan.loops)
    {
        report.loop_start = TickSeconds(sim.frame_ticks, sim.plan.loop_start);
        report.loop_end = TickSeconds(sim.frame_ticks, sim.plan.loop_end);
    }
    report.bpm = 60.0 / FrameSeconds(double(kTicksPerQuarter) / TempoMap(sim).front().second);

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The SoundFont's presets, in the sequence's file or the shared one, in the banks and programs that it gives the
    // sequence.
    // In a SoundFont for this sequence alone, the presets go in sequence 0's banks and programs.
    SoundfontBuilder own(rom, PsgShare(info.revision));
    SoundfontBuilder& sf = shared ? *shared : own;
    const int count = int(maker.Programs().size());
    const std::vector<BankProgram> slots = sf.Banks().Place(shared ? song : 0, count);
    if (int(slots.size()) < count)
    {
        report.warnings.push_back("needs " + std::to_string(count) +
                                  " presets, more than the SoundFont's banks have free" +
                                  (shared ? "; convert the sequence without --single-sf2" : ""));
        return report;
    }
    for (const std::string& w : AddPresets(rom, sf, maker.Programs(), slots))
    {
        report.warnings.push_back(w);
    }

    const std::string number = SongNumber(song, 0);
    const std::string title = rom.Title() + " #" + number;
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) sequence %d at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(info.sequence_addresses[size_t(song)]), kProgramName);
    const std::string stem = SongStem(settings, number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, info.revision, sim, notes, slots, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    if (!shared)
    {
        Sf2File& file = sf.File();
        file.SetTitle(title);
        report.sf2_path = stem + ".sf2";
        if (!file.Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return report;
        }
    }

    report.ok = true;

    return report;
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       SoundfontBuilder* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

} // namespace agbsf2::rd2
