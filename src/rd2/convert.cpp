// SPDX-License-Identifier: MIT

#include "rd2/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
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
#include "rd2/driver.h"
#include "rd2/sequencer.h"
#include "rd2/soundfont.h"
#include "rom.h"
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

// The most frames that a sequence without a loop runs on after its end while the PSG still sounds: about 2 s. The PSG's
// slowest envelope fades from 15 to 0 in 1.64 s, and a released wave voice fades out faster.
constexpr uint32_t kRunOutFrames = 120;

// The slowest tempo a MIDI file can hold at this resolution: at 3 or less, a quarter note lasts more than the 2^24
// microseconds a tempo event can give it.
constexpr int kMinMidiTempo = 4;

// A bend that no pitch has, for a note whose bend is unknown when two passes are compared.
constexpr double kNoBend = 1e9;

// The envelope level at which a sample voice plays at its full volume.
constexpr double kFullEnvelope = 0x7FFF;

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

// A frame's results for the conversion. `volume` holds each PSG channel's volume at the frame's start, from 0 to 15, or
// -1 when the channel is off. The wave channel's volume is 15 times the share of its full level that NR32 sets.
// `envelope` holds the share of its full level that each sample voice's envelope gives it in the frame.
struct FrameData
{
    int tempo = kStartTempo;                // the units the frame lasts
    std::array<double, kPlayerTracks> bend; // semitones from its note of each track's newest voice, or NaN for none
    std::array<double, kPsgVoices> volume;
    uint8_t nr51 = 0; // the PSG channels' sides after the frame
    std::array<double, kSampleVoices> envelope{};
};

// The wave voice's PSG channel. The squares come before it, and the noise after it.
constexpr int kWaveChannel = kWaveType - 1;

// The offsets from 0x04000060 of the PSG's registers: each channel's NRx1 and NRx2, and its NRx3 and NRx4, then NR10,
// NR30 and NR51.
constexpr size_t kEnvelopeRegisters[kPsgVoices] = {0x02, 0x08, 0x12, 0x18};
constexpr size_t kFrequencyRegisters[kPsgVoices] = {0x04, 0x0C, 0x14, 0x1C};
constexpr size_t kNr10 = 0x00;
constexpr size_t kNr30 = 0x10;
constexpr size_t kNr51 = 0x21;

// Returns PSG channel `c`'s registers after the sequencer's last frame, for the channel's model.
PsgRegisters ChannelRegisters(const Sequencer& seq, int c)
{
    const std::array<uint8_t, 0x40>& r = seq.Registers();
    const auto half = [&r](size_t at)
    {
        return uint16_t(r[at] | r[at + 1] << 8);
    };

    return {half(kNr10), half(kEnvelopeRegisters[c]), half(kFrequencyRegisters[c]), half(kNr30),
            (seq.Restarted() >> c & 1) != 0};
}

// The models of the PSG channels, from the sequence's first frame.
class PsgChannels
{
public:
    PsgChannels() : models_{PsgModel(0), PsgModel(1), PsgModel(2), PsgModel(3)}
    {
    }

    // Takes in the sequencer's last frame, and records each channel's volume at its start and the channels' sides. The
    // wave channel's model gives 15 while the channel is on. NR32 then sets its share of that.
    void Frame(const Sequencer& seq, FrameData& d)
    {
        for (int c = 0; c < kPsgVoices; c++)
        {
            const PsgRegisters r = ChannelRegisters(seq, c);
            const int volume = models_[size_t(c)].Frame(r);
            d.volume[size_t(c)] = c == kWaveChannel && volume >= 0 ? volume * WaveShare(r.envelope) : volume;
        }
        d.nr51 = seq.Registers()[kNr51];
    }

private:
    std::array<PsgModel, kPsgVoices> models_;
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
        // A track that F8 starts for the first time counts like the others.
        TrackPlan& other = tracks[e.value];
        other.restarted = other.restarted || other.started;
        other.started = true;
    }
}

// Returns PlanSong()'s plan for the sequence, in units, once every track that counts has looped or ended. Returns
// nothing before then. A track counts once it has started, unless F8 started it again. A track without a loop has read
// its last command and released its notes by its end at the latest. From LoopStart::kSecondPass, the loop starts at the
// sequence's second pass through it.
std::optional<SongPlan> PlanLoop(const TrackPlans& tracks, int loops, LoopStart start_pass)
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

    return PlanSong(layouts, loops, start_pass);
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

// Records the share of its full level that each sample voice's envelope gives it in frame `d` (see FrameData). The
// envelope stops at the release, and a released voice keeps the share it had there. A free voice gets none.
void RecordEnvelopes(const Sequencer& seq, FrameData& d)
{
    for (size_t v = 0; v < size_t(kSampleVoices); v++)
    {
        const Voice& x = seq.Voices()[v];
        if (x.state != kVoiceFree)
        {
            d.envelope[v] = std::clamp(x.level / kFullEnvelope, 0.0, 1.0);
        }
    }
}

// Runs the model's next frame, and records the frame's results, the PSG channels' volumes, the sample voices'
// envelopes and the tick after it.
void StepFrame(Sequencer& seq, PsgChannels& psg, Simulation& sim)
{
    seq.Step();
    sim.frames.push_back(RecordFrame(seq));
    psg.Frame(seq, sim.frames.back());
    RecordEnvelopes(seq, sim.frames.back());
    sim.frame_ticks.push_back(seq.Elapsed());
}

// Returns true if any of the PSG's channels sounds at the start of the frame, at a volume above 0.
bool PsgSounds(const FrameData& d)
{
    return std::any_of(d.volume.begin(), d.volume.end(), [](double volume) { return volume > 0; });
}

// Runs the model on after the end of a sequence without a loop. A square's or the noise's envelope can still fade then,
// and the driver goes on fading a released wave voice. The sequence then ends at the start of the first frame in which
// every channel is silent, or after kRunOutFrames.
void RunOut(Sequencer& seq, PsgChannels& psg, Simulation& sim)
{
    if (sim.frames.empty() || !PsgSounds(sim.frames.back()))
    {
        return;
    }

    for (uint32_t n = 0; n < kRunOutFrames && PsgSounds(sim.frames.back()); n++)
    {
        StepFrame(seq, psg, sim);
        const uint32_t f = uint32_t(sim.frames.size() - 1);
        for (const Event& e : seq.Events())
        {
            sim.events.push_back({e, f});
        }
    }

    const size_t silent = PsgSounds(sim.frames.back()) ? sim.frames.size() : sim.frames.size() - 1;
    sim.plan.end = std::max(sim.plan.end, sim.frame_ticks[silent]);
}

// Runs the model until every track has looped or ended and the sequence has played its loop `loops` times. A track's
// units are the MIDI file's ticks: each frame lasts the tempo's units, and each command comes at its track's time, or
// with `frame_timing`, at the start of the frame the driver plays it in. From LoopStart::kSecondPass, the loop starts
// at the sequence's second pass through it. The sequence then plays the first pass before it. A sequence without a loop
// runs on while the PSG sounds after the player stops (see RunOut()). The sequence plays as its version `version`.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int version, int loops, bool frame_timing,
                    LoopStart start_pass)
{
    Simulation sim;
    sim.frame_timing = frame_timing;
    Sequencer seq(rom, info, song, version);
    sim.frame_ticks.push_back(0);

    TrackPlans tracks;
    PsgChannels psg;
    std::optional<SongPlan> loop;
    bool finished = false; // the sequence played what the plan requires, or ended
    for (uint32_t f = 0; f < kMaxFrames && seq.Valid(); f++)
    {
        StepFrame(seq, psg, sim);
        if (f == 0)
        {
            sim.volume2 = seq.GetPlayer().volume2;
        }
        for (const Event& e : seq.Events())
        {
            sim.events.push_back({e, f});
            UpdateTrackPlans(e, tracks);
        }
        // The frame's events come first. A track that F8 starts in the frame then counts as started for the first time.
        for (int n = 0; n < kPlayerTracks; n++)
        {
            if (seq.GetPlayer().tracks[size_t(n)] >= 0)
            {
                tracks[size_t(n)].started = true;
            }
        }

        if (!loop)
        {
            loop = PlanLoop(tracks, loops, start_pass);
        }
        if ((loop && (!loop->loops || sim.frame_ticks[f] >= loop->end)) || seq.Ended())
        {
            finished = true;
            break;
        }
    }

    sim.plan = FinishPlan(loop, tracks, sim.frame_ticks.back());
    if (finished && !sim.plan.loops && seq.Ended())
    {
        RunOut(seq, psg, sim);
    }
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
        return std::tie(type, sound, envelope, release, root, coarse, pan, own_pan, legato) <
               std::tie(o.type, o.sound, o.envelope, o.release, o.root, o.coarse, o.pan, o.own_pan, o.legato);
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
    bool legato = false;  // a sample's zone in legato, with only the release (see PlaysLegato())
};

// Returns the pan of a zone that plays a drum at pan `pan` on a voice of type `type`. The driver holds a sample voice's
// pan to 0 to 127 and gives the voice levels of 127 - pan on the left and pan on the right. CC10 plays a track's notes
// at the centre (0x40) in the middle. The zone gives the ratio of the drum's levels to the centre's. A PSG voice plays
// on both sides at the centre. At any other pan, it plays on one side alone.
int DrumZonePan(int pan, uint8_t type)
{
    if (type != kSampleType)
    {
        return pan == kCentre ? 0 : (pan < kCentre ? -500 : 500);
    }

    pan = std::clamp(pan, 0, 127);
    return SidesPan((127 - pan) / double(127 - kCentre), pan / double(kCentre));
}

// A note as the MIDI file plays it.
struct Note
{
    int track = 0;
    int key = 60;
    int velocity = 127; // the driver's, up to 255
    int program = 0;
    uint64_t on = 0; // MIDI ticks
    uint64_t off = 0;
    uint64_t length = 0;
    uint32_t frame = 0; // the frame the driver starts it in
    bool open = true;
    int psg = -1;         // a PSG voice's channel, whose volume the note follows, or -1
    uint32_t psg_end = 0; // the frame after the last that the note's CC11 follows (see NoteMaker::EndSound())
    int legato = -1;      // the sample voice of a note in legato, whose envelope the note's CC11 follows, or -1
};

// Returns true if a voice of type `type` plays at its PSG channel's volume: a square's, the wave's or the noise's. The
// MIDI file's CC11 follows that volume. A square's or the noise's envelope steps it through 16 values. NR32 gives the
// wave channel 5 levels: 0, 25, 50, 75 and 100%.
bool FollowsVolume(uint8_t type)
{
    return type == kSquare1Type || type == kSquare2Type || type == kWaveType || type == kNoiseType;
}

// Returns the tick of frame `f` on a note's time line: as far after the note's start as the frame comes after the one
// the note starts in. With frame timing, that's the frame's start.
uint64_t NoteFrameTick(const Simulation& sim, const Note& n, uint32_t f)
{
    return n.on + (sim.frame_ticks[f] - sim.frame_ticks[n.frame]);
}

// Returns true if a note is a sample note of a track in legato. The track's voice keeps its envelope from note to note.
// The note plays on a zone whose envelope has only the release. Its CC11 follows the voice's envelope.
bool PlaysLegato(const Event& e)
{
    return e.legato && e.type == kSampleType;
}

// Returns a note's velocity in the MIDI file. A PSG voice's note plays at full velocity. Its CC11 follows the channel's
// volume. The volume holds the velocity. Another note plays its velocity divided by its track's `scale` (see
// VelocityScales()).
int MidiVelocity(const Note& n, double scale)
{
    return n.psg >= 0 ? 127 : SquareRootValue(n.velocity, 127 * scale, 1);
}

// An instrument of the driver's banks, as a program of the MIDI file.
struct Program
{
    uint16_t bank = 0;
    uint16_t instrument = 0;
    bool legato = false;           // the instrument's notes in legato (see PlaysLegato())
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

    // Works out the notes, the end of each, and the programs they play. A PSG voice's note holds its channel until
    // another note takes the channel. The note lasts as long as the channel sounds in that time (see EndSound()). A
    // sample note in legato lasts until its voice plays another note, or until the voice's release.
    std::vector<Note> Make()
    {
        std::vector<Note> notes;
        std::map<uint32_t, size_t> by_id;
        std::array<uint32_t, kVoices> voice_note = {};
        std::array<int, kPsgVoices> holder; // the index of the note that holds each PSG channel, or -1
        holder.fill(-1);
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
                // A voice that plays a new note in legato ends its last one. A note on a PSG voice takes the channel
                // from the note that held it, even if the conversion leaves the new note out.
                const uint64_t tick = EventTick(sim_, e.units, te.frame);
                close(voice_note[size_t(e.voice)], tick);
                voice_note[size_t(e.voice)] = 0;
                const int channel = e.voice >= kSampleVoices ? e.voice - kSampleVoices : -1;
                if (channel >= 0 && holder[size_t(channel)] >= 0)
                {
                    EndSound(notes[size_t(holder[size_t(channel)])], te.frame, tick, end);
                    holder[size_t(channel)] = -1;
                }
                if (e.velocity == 0 || e.track < 0 || !(mask_ >> e.track & 1) || e.units >= sim_.plan.end)
                {
                    continue;
                }

                Note n;
                n.track = e.track;
                n.velocity = e.velocity;
                n.on = tick;
                n.length = e.length;
                n.frame = te.frame;
                n.program = ProgramOf(e, n.key);
                if (n.program < 0)
                {
                    continue;
                }

                if (channel >= 0 && FollowsVolume(e.type))
                {
                    n.psg = channel;
                    holder[size_t(channel)] = int(notes.size());
                }
                else if (channel < 0 && PlaysLegato(e))
                {
                    n.legato = e.voice;
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
        for (int h : holder)
        {
            if (h >= 0)
            {
                EndSound(notes[size_t(h)], uint32_t(sim_.frames.size()), end, end);
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
    // Ends `n`, a PSG voice's note. The note holds its channel until another note takes it in frame `until`, at tick
    // `until_tick`. Where no note takes it, `until` is the frames' end. The channel sounds until the start of the frame
    // after the last one in which its volume is above 0. The note lasts that long, but ends by `until_tick` and `end`.
    // The release fades the volume out over frames. A square's or the noise's envelope can fade it out before the
    // release. A note that the channel never sounds lasts a tick. The note's CC11 follows the channel's volume up to
    // the frame in which the sound ends. That frame's volume of 0 holds CC11 at 0 until the note off.
    void EndSound(Note& n, uint32_t until, uint64_t until_tick, uint64_t end) const
    {
        uint32_t silent = n.frame; // the frame in which the sound ends
        for (uint32_t f = n.frame; f < until; f++)
        {
            if (sim_.frames[f].volume[size_t(n.psg)] > 0)
            {
                silent = f + 1;
            }
        }

        n.psg_end = std::max(std::min(until, silent + 1), n.frame + 1);
        n.off = std::max(std::min({NoteFrameTick(sim_, n, silent), until_tick, end}), n.on + 1);
    }

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

        // The SoundFont has no zone for a sample that can't be read. The note is left out.
        if (z->type == kSampleType && !SampleReadable(rom_, z->sound))
        {
            char text[96];
            std::snprintf(text, sizeof text, "the sample at 0x%08X can't be read, so its notes are left out",
                          unsigned(z->sound));
            WarnOnce(warnings_, text);
            return -1;
        }

        // A sample instrument without drums or a sample for each key gets a zone for every key. A note below the
        // driver's tables replaces the zone that fills its key (see BelowTables()).
        const bool fill = e.type == kSampleType && !lk.drum && !lk.key_sample;
        const size_t p = FindProgram(e.bank, e.instrument, PlaysLegato(e), fill);
        const auto [at, added] = programs_[p].keys.emplace(key, *z);
        if (!added && fill && BelowTables(uint8_t(key), RegionTuning(rom_, lk.region)))
        {
            at->second = *z;
        }

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
            ZoneSound z = SampleZone(e.sample, lk, PlaysLegato(e));
            if (BelowTables(e.key, RegionTuning(rom_, lk.region)))
            {
                z.root = key;
                z.coarse = -kUnityIndex;
            }
            return z;
        }

        const uint32_t region = lk.region;
        const uint8_t tuning = RegionTuning(rom_, region);
        const int index = PitchIndex(fixed ? kFixedNote : e.key, tuning);
        ZoneSound z;
        z.type = e.type;

        // The region's envelope and release set a PSG voice's volume. The notes' CC11 follows the volume, and the zone
        // plays at a constant level. A sample's zone in legato keeps only the release (see PlaysLegato()).
        z.legato = PlaysLegato(e);
        if (!FollowsVolume(e.type))
        {
            z.envelope = z.legato ? 0 : lk.envelope;
            z.release = RegionRelease(rom_, region);
        }
        z.own_pan = lk.drum;
        if (lk.drum)
        {
            z.pan = DrumZonePan(lk.drum_pan, e.type);
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

    // Returns true if a note's pitch index falls below the driver's tables. The driver then plays it at the pitch of
    // index 0 (see PitchIndex()). Its zone plays the note's key at that pitch. The zones that fill the keys that the
    // sequence doesn't play keep the region's tuning.
    static bool BelowTables(uint8_t note, uint8_t tuning)
    {
        return int(note) + kUnityIndex < int(tuning);
    }

    // Returns the zone of a sample region that plays `sample` at the region's tuning. The regions of drums, and of
    // instruments with a sample for each key, play at a fixed pitch instead. A zone in legato has only the release (see
    // PlaysLegato()).
    ZoneSound SampleZone(uint32_t sample, const Lookup& lk, bool legato) const
    {
        const uint8_t tuning = RegionTuning(rom_, lk.region);
        ZoneSound z;
        z.sound = sample;
        z.legato = legato;
        z.envelope = legato ? 0 : lk.envelope;
        z.release = RegionRelease(rom_, lk.region);
        z.root = std::clamp(int(tuning), 0, 127);
        z.coarse = z.root - tuning;

        return z;
    }

    // Returns the index of the program for an instrument of a bank, after adding the program if it's new. The notes
    // in legato get a separate program (see PlaysLegato()). The programs come in the order the sequence first plays
    // them. A new program gets a zone for every key if `fill` is set.
    size_t FindProgram(uint16_t bank, uint16_t instrument, bool legato, bool fill)
    {
        size_t p = 0;
        while (p < programs_.size() &&
               (programs_[p].bank != bank || programs_[p].instrument != instrument || programs_[p].legato != legato))
        {
            p++;
        }
        if (p == programs_.size())
        {
            programs_.push_back({bank, instrument, legato, {}});
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
            program.keys.emplace(key, SampleZone(sample, lk, program.legato));
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

            // A drum keeps its pan whatever its track's pan is. This modulator replaces the CC10 modulator of the
            // global zone.
            if (z.own_pan)
            {
                zone.modulators.push_back({uint16_t(sf2::kController | cc::kPan) | sf2::kBipolar, sf2::kPan, 0, 0, 0});
            }

            zone.generators.push_back(
                sf2::Generator::Value(sf2::kSampleModes, Looped(sf.File().samples[size_t(sample)]) ? 1 : 0));
            zone.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

            // A sample's release takes a share of its level off each frame. A PSG voice plays at a constant level, and
            // its notes' CC11 follows the channel's volume. A sample in legato plays at a constant level until the
            // release. Its notes' CC11 follows its voice's envelope.
            Sf2Envelope env;
            if (z.type == kSampleType && z.legato)
            {
                env.release = SampleRelease(z.release);
            }
            else if (z.type == kSampleType)
            {
                env = EnvelopeFor(rom, z.envelope, SampleRelease(z.release));
            }
            AddEnvelope(zone, env);
            zones.push_back(zone);
        }

        // A SoundFont name holds 20 characters. The name of the preset of an instrument's notes in legato has "legato"
        // in place of "instrument".
        char name[48];
        std::snprintf(name, sizeof name, "Bank %u %s %u", unsigned(program.bank),
                      program.legato ? "legato" : "instrument", unsigned(program.instrument));
        sf.AddPreset(name, slots[p].bank, slots[p].program, sf.AddInstrument(name, std::move(zones)));
    }

    return warnings;
}

// Returns a track's levels on each side (full scale 128) for a voice at full velocity and envelope, from its pan and
// volume and the player's volumes. The driver works out a sample voice's levels in the same way. The revision's
// volume_scale and volume_shift turn the voice's level into its volume. With the player's second volume at its default,
// 0x80, the volume reaches 190 at most in A Link to the Past's revision and 126 at most in the others. The Super Mario
// Advance 2 revision leaves out the player's second volume. That revision's mixer splits the volume between the sides
// in 128ths and divides each voice's points by 256. The other mixers split it in 256ths and divide the mix by 128. Both
// ways give the same levels.
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

// Returns, for each of the player's tracks, whether every note that it plays is a PSG note that takes the track's pan.
// Those are the notes of the squares, the wave and the noise, other than drums.
std::array<bool, kPlayerTracks> PsgTracks(const std::vector<Note>& notes, const std::vector<Program>& programs)
{
    std::array<bool, kPlayerTracks> psg;
    psg.fill(true);
    for (const Note& n : notes)
    {
        const ZoneSound& z = programs[size_t(n.program)].keys.at(n.key);
        if (z.type == kSampleType || z.own_pan)
        {
            psg[size_t(n.track)] = false;
        }
    }

    return psg;
}

// Sets CC10 and CC11 for a track's levels. A sample voice plays at the levels of TrackLevels(). A PSG voice plays on
// both sides at the centre pan, 0x40. At any other pan, it plays at twice the level on one side alone
// (Sequencer::WritePan() and PsgLevel()). `psg` is set when every note that the track plays is a PSG note other than a
// drum (see PsgTracks()). The controllers then follow the PSG's rule. TrackLevels() gives the two sides the same sum at
// every pan: twice the level of each side at the centre. CC11 plays the levels times `scale` (see VelocityScales()).
void TrackControllers(Revision revision, const Levels& l, bool psg, double scale, int& cc10, int& cc11)
{
    auto [left, right] = TrackLevels(revision, l.pan, l.volume, l.player_volume, l.player_volume2);
    left *= scale;
    right *= scale;
    if (!psg || l.pan == kCentre)
    {
        LevelsToControllers(left, right, cc10, cc11);
        return;
    }

    const double side = left + right;
    LevelsToControllers(l.pan < kCentre ? side : 0, l.pan < kCentre ? 0 : side, cc10, cc11);
}

// Returns CC11 for a note at full velocity on PSG channel `c` at volume `volume` (0-15, or -1 when the channel is off).
// The note plays on the sides that `nr51` enables for the channel. The driver's volume already holds the note's
// velocity and the volumes. Volume 15 plays at the level that PsgShare() gives it against a full-scale sample. A
// SoundFont's PSG samples hold that share. The note's CC10 comes from its track's pan (see TrackControllers()).
int PsgExpression(Revision revision, double volume, uint8_t nr51, int c)
{
    const double level = std::max(volume, 0.0) / PsgShare(revision);
    int cc10 = 64, cc11 = 0;
    LevelsToControllers((nr51 >> (c + 4) & 1) ? level : 0, (nr51 >> c & 1) ? level : 0, cc10, cc11);

    return cc11;
}

// Returns the CC11 of a PSG voice's note in frame `f`, from its channel's volume at the frame's start.
int NoteExpression(const Simulation& sim, Revision revision, const Note& n, uint32_t f)
{
    const FrameData& d = sim.frames[f];
    return PsgExpression(revision, d.volume[size_t(n.psg)], d.nr51, n.psg);
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

// Returns a track's levels from its start: `start` up to its first change, then each of `changes`. The records leave
// `start` out when the first change comes at tick 0.
std::vector<LevelChange> LevelRecords(const Levels& start, const std::vector<LevelChange>& changes)
{
    std::vector<LevelChange> records;
    if (changes.empty() || changes[0].tick != 0)
    {
        records.push_back({0, start, false});
    }
    records.insert(records.end(), changes.begin(), changes.end());

    return records;
}

// The amplitude of a track's levels on both sides, in 128ths, that CC11 plays at 127 (see LevelsToControllers()).
constexpr double kLoudestLevels = 127 * std::numbers::sqrt2;

// Returns, for each of the player's tracks, the factor that its CC11 raises its levels by. The driver plays a sample
// note's level in proportion to its velocity. The velocity goes up to 255. A MIDI note's velocity stops at 127. A
// track's sample notes therefore play at their velocity divided by the factor. The factor is the loudest sample note's
// velocity over 127, and at least 1. It stops where CC11 would pass 127 at the track's loudest levels.
std::array<double, kPlayerTracks> VelocityScales(Revision revision, const Levels& start, const std::vector<Note>& notes,
                                                 const std::array<std::vector<LevelChange>, kPlayerTracks>& changes)
{
    std::array<int, kPlayerTracks> loudest{};
    for (const Note& n : notes)
    {
        if (n.psg < 0)
        {
            loudest[size_t(n.track)] = std::max(loudest[size_t(n.track)], n.velocity);
        }
    }

    std::array<double, kPlayerTracks> scales;
    for (int t = 0; t < kPlayerTracks; t++)
    {
        double scale = loudest[size_t(t)] / 127.0;
        for (const LevelChange& r : LevelRecords(start, changes[size_t(t)]))
        {
            const Levels& l = r.levels;
            const auto [left, right] = TrackLevels(revision, l.pan, l.volume, l.player_volume, l.player_volume2);
            const double amplitude = std::hypot(left, right);
            if (amplitude > 0)
            {
                scale = std::min(scale, kLoudestLevels / amplitude);
            }
        }
        scales[size_t(t)] = std::max(scale, 1.0);
    }

    return scales;
}

// A note's stretch of its track's CC11, from its note on until `end`. A PSG voice's note sets CC11 to the value of each
// of its steps from the step's tick on. The steps follow the channel's volume (see PsgExpression()). A sample note in
// legato sets CC11 to the track's levels times each of its shares, from the share's tick on. The shares follow its
// voice's envelope (see PlaysLegato()). Any other note has neither. It plays at its track's levels until its note off.
struct ExpressionSpan
{
    uint64_t on = 0;
    uint64_t end = 0;
    std::vector<std::pair<uint64_t, int>> steps;     // (tick, CC11)
    std::vector<std::pair<uint64_t, double>> shares; // (tick, share of the track's levels)
};

// Returns the ticks at which a track's CC10 or CC11 can change: each of `records`, and each note on, step and end of
// `spans`. Each tick comes with whether CC11 returns to the track's levels there when no span holds the tick. It
// doesn't at a span's end.
std::map<uint64_t, bool> LevelTicks(const std::vector<LevelChange>& records, const std::vector<ExpressionSpan>& spans)
{
    std::map<uint64_t, bool> ticks;
    for (const LevelChange& change : records)
    {
        ticks[change.tick] = true;
    }
    for (const ExpressionSpan& span : spans)
    {
        ticks[span.on] = true;
        for (const auto& step : span.steps)
        {
            ticks[step.first] = true;
        }
        for (const auto& share : span.shares)
        {
            ticks[share.first] = true;
        }
        ticks.emplace(span.end, false);
    }

    return ticks;
}

// Writes a track's CC10 for pan and CC11 for loudness at the start and whenever they change. A player that jumps back
// to `loop`, the loop's start, retains the levels from the loop's end, as the game does until the loop sets them. So
// CC11 is written again at the first tick at or after `loop` that sets it, even if its value hasn't changed. CC10
// follows the pan alone. It's written again at the first level-change record that sets the pan. `start` gives the
// levels before the track's first change. With `psg`, the controllers follow the PSG's rule (see TrackControllers()).
// CC11 plays the track's levels times `scale` (see VelocityScales()).
//
// If any of the track's notes is a PSG voice's or in legato, `spans` holds each of the notes' stretches of CC11, in the
// order of their note ons. Otherwise it's empty, and CC11 follows the track's levels. At each tick, of the spans that
// hold the tick, the one with the latest note on sets CC11. Where no span holds a tick, CC11 follows the track's
// levels. It returns to them at the next level change, note on or step after a span's end.
void WriteLevels(MidiTrack& mt, int c, Revision revision, bool psg, double scale, const Levels& start,
                 const std::vector<LevelChange>& changes, const std::vector<ExpressionSpan>& spans,
                 std::optional<uint64_t> loop)
{
    const std::vector<LevelChange> records = LevelRecords(start, changes);

    int cc10 = -1, cc11 = -1;
    int level11 = 0; // CC11 for the track's levels
    Levels levels = start;
    bool again10 = loop.has_value(), again11 = loop.has_value();
    size_t next_record = 0, next_span = 0;
    std::vector<size_t> held;                     // the spans that hold the tick, in the order of their note ons
    std::vector<size_t> step_at(spans.size(), 0); // each span's last step so far
    for (const auto& [tick, returns] : LevelTicks(records, spans))
    {
        // A level-change record sets CC10, and the track's levels for CC11.
        const bool looped = loop && tick >= *loop;
        if (next_record < records.size() && records[next_record].tick == tick)
        {
            const LevelChange& change = records[next_record++];
            levels = change.levels;
            const bool write10 = looped && again10 && change.pan;
            again10 = again10 && !write10;
            int n10 = cc10 < 0 ? 64 : cc10;
            TrackControllers(revision, change.levels, psg, scale, n10, level11);
            if (n10 != cc10 || write10)
            {
                mt.Control(uint32_t(tick), c, cc::kPan, cc10 = n10);
            }
        }

        // The span with the latest note on sets CC11.
        while (next_span < spans.size() && spans[next_span].on <= tick)
        {
            held.push_back(next_span++);
        }
        std::erase_if(held, [&spans, tick](size_t s) { return spans[s].end <= tick; });
        int n11 = level11;
        if (!held.empty() && !spans[held.back()].steps.empty())
        {
            const auto& steps = spans[held.back()].steps;
            size_t& k = step_at[held.back()];
            while (k + 1 < steps.size() && steps[k + 1].first <= tick)
            {
                k++;
            }
            n11 = steps[k].second;
        }
        else if (!held.empty() && !spans[held.back()].shares.empty())
        {
            const auto& shares = spans[held.back()].shares;
            size_t& k = step_at[held.back()];
            while (k + 1 < shares.size() && shares[k + 1].first <= tick)
            {
                k++;
            }
            int unused10 = 64;
            TrackControllers(revision, levels, psg, scale * shares[k].second, unused10, n11);
        }
        else if (held.empty() && !returns)
        {
            continue;
        }

        const bool write11 = looped && again11;
        again11 = again11 && !write11;
        if (n11 != cc11 || write11)
        {
            mt.Control(uint32_t(tick), c, cc::kExpression, cc11 = n11);
        }
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
// start together become one MIDI note, the first, lasting as long as the longer of them. A sample note's velocity is
// divided by `scale` (see VelocityScales()).
void WriteNotes(MidiTrack& mt, int c, const std::vector<const Note*>& notes, const std::vector<BankProgram>& slots,
                int program, double scale, std::optional<uint64_t> loop)
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
        mt.NoteOn(uint32_t(note->on), c, note->key, MidiVelocity(*note, scale));
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
    const std::vector<Note> notes = maker.Make();
    const std::array<bool, kPlayerTracks> psg = PsgTracks(notes, maker.Programs());
    const std::array<std::vector<LevelChange>, kPlayerTracks> changes = TrackChanges(sim);
    const std::array<double, kPlayerTracks> scales = VelocityScales(info.revision, StartLevels(sim), notes, changes);
    const SongPlan& plan = sim.plan;
    PassComparison passes(plan.loop_start, plan.loop_end - plan.loop_start);
    for (const Note& n : notes)
    {
        // The track's levels from its last change up to the note's start, as CC10 and CC11 give them.
        const std::vector<LevelChange>& list = changes[size_t(n.track)];
        const auto by_tick = [](uint64_t tick, const LevelChange& change)
        {
            return tick < change.tick;
        };
        const auto after = std::upper_bound(list.begin(), list.end(), n.on, by_tick);
        const Levels levels = after == list.begin() ? StartLevels(sim) : std::prev(after)->levels;
        int cc10 = 64, cc11 = 0;
        TrackControllers(info.revision, levels, psg[size_t(n.track)], scales[size_t(n.track)], cc10, cc11);

        // A PSG voice's note starts at its channel's volume, and a sample note in legato at its voice's envelope.
        if (n.psg >= 0)
        {
            cc11 = NoteExpression(sim, info.revision, n, n.frame);
        }
        else if (n.legato >= 0)
        {
            const double share = sim.frames[n.frame].envelope[size_t(n.legato)];
            TrackControllers(info.revision, levels, psg[size_t(n.track)], scales[size_t(n.track)] * share, cc10, cc11);
        }

        // A note's bend is its track's newest voice's in the frame it starts, which a voice that the note didn't take
        // can leave unknown.
        const FrameData& d = sim.frames[n.frame];
        const double bend = std::isnan(d.bend[size_t(n.track)]) ? kNoBend : d.bend[size_t(n.track)];
        passes.Add(n.on, n.track, n.key,
                   {double(MidiVelocity(n, scales[size_t(n.track)])), double(n.program), double(cc10), double(cc11),
                    bend, double(d.tempo)});
    }

    return passes.Alike();
}

// Returns the stretches of CC11 of a track's notes (see ExpressionSpan) before `end`, in the order of their note ons. A
// PSG voice's note gets a step at each frame in which its channel's volume changes, from its first frame to the one
// after its sound ends (see NoteMaker::EndSound()). A sample note in legato gets a share at each frame in which its
// voice's envelope changes, up to its note off. Returns none if no note is a PSG voice's or in legato.
std::vector<ExpressionSpan> TrackSpans(const Simulation& sim, Revision revision, const std::vector<const Note*>& notes,
                                       uint64_t end)
{
    std::vector<ExpressionSpan> spans;
    bool steps = false;
    for (const Note* n : notes)
    {
        ExpressionSpan span;
        span.on = n->on;
        span.end = std::min(n->off, end);
        if (n->psg >= 0)
        {
            span.end = std::min(NoteFrameTick(sim, *n, n->psg_end), end);
            for (uint32_t f = n->frame; f < n->psg_end && NoteFrameTick(sim, *n, f) < end; f++)
            {
                const int value = NoteExpression(sim, revision, *n, f);
                if (span.steps.empty() || span.steps.back().second != value)
                {
                    span.steps.emplace_back(NoteFrameTick(sim, *n, f), value);
                }
            }
            steps = steps || !span.steps.empty();
        }
        else if (n->legato >= 0)
        {
            for (uint32_t f = n->frame; f < sim.frames.size() && NoteFrameTick(sim, *n, f) < span.end; f++)
            {
                const double share = sim.frames[f].envelope[size_t(n->legato)];
                if (span.shares.empty() || span.shares.back().second != share)
                {
                    span.shares.emplace_back(NoteFrameTick(sim, *n, f), share);
                }
            }
            steps = steps || !span.shares.empty();
        }
        spans.push_back(std::move(span));
    }
    if (!steps)
    {
        return {};
    }

    const auto by_on = [](const ExpressionSpan& a, const ExpressionSpan& b)
    {
        return a.on < b.on;
    };
    std::stable_sort(spans.begin(), spans.end(), by_on);

    return spans;
}

// Writes the MIDI file: a conductor track, then a track for each of the player's tracks that plays notes, with the
// levels of the driver's revision. `psg` gives the tracks whose notes are all PSG notes (see PsgTracks()).
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, Revision revision,
               const Simulation& sim, const std::vector<Note>& notes, const std::array<bool, kPlayerTracks>& psg,
               const std::vector<BankProgram>& slots, std::string& error)
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
    const std::array<double, kPlayerTracks> scales = VelocityScales(revision, StartLevels(sim), notes, changes);
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

        WriteLevels(mt, c, revision, psg[size_t(n)], scales[size_t(n)], StartLevels(sim), changes[size_t(n)],
                    TrackSpans(sim, revision, list, end), loop);
        WriteNotes(mt, c, list, slots, program, scales[size_t(n)], loop);
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a sequence or a sound effect. With `write`, it writes the files.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write)
{
    SongReport report;
    const Effect* effect = SongEffect(info, song);
    if (song < 0 || song >= SongCount(info))
    {
        report.warnings.push_back("there's no such sequence");
        return report;
    }
    if (effect && !effect->address)
    {
        report.warnings.push_back("the sound effect's entry gives no track");
        return report;
    }

    // The marked loop starts at the sequence's second pass if its first plays unlike it, as when the tracks come back
    // to their loops with the pan or volume that the loop's end left them. The passes are compared on the beat, as the
    // default MIDI file plays them, so frame timing moves the same loops.
    const int version = settings.version;
    Simulation sim = Simulate(rom, info, song, version, settings.loops, settings.frame_timing, LoopStart::kFirstPass);
    if (sim.plan.loops &&
        !FirstPassRepeats(rom, info, Simulate(rom, info, song, version, 2, false, LoopStart::kFirstPass)))
    {
        sim = Simulate(rom, info, song, version, settings.loops, settings.frame_timing, LoopStart::kSecondPass);
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
    // sequence. In a SoundFont for this sequence alone, the presets go in sequence 0's banks and programs.
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
        WarnOnce(report.warnings, w);
    }

    // A version after the first has its name in the titles and its suffix in the files' names.
    const std::string number = SongNumber(song, 0);
    const std::vector<SongVersion> versions = SongVersions(info, song);
    const SongVersion& picked = versions[size_t(std::clamp(version, 0, int(versions.size()) - 1))];
    const std::string named = picked.name.empty() ? "" : " " + picked.name;
    const std::string title = rom.Title() + " #" + number + named;
    char about[192];
    if (effect)
    {
        std::snprintf(about, sizeof about,
                      "%s (%s) sound effect at 0x%08X, track %d of the effects' sequence %d, "
                      "converted by %s",
                      rom.Title().c_str(), rom.GameCode().c_str(), unsigned(effect->address), effect->track,
                      effect->sequence, kProgramName);
    }
    else
    {
        std::snprintf(about, sizeof about, "%s (%s) sequence %d%s at 0x%08X, converted by %s", rom.Title().c_str(),
                      rom.GameCode().c_str(), song, named.c_str(), unsigned(info.sequence_addresses[size_t(song)]),
                      kProgramName);
    }
    const std::string stem = SongStem(settings, number) + picked.suffix;

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, info.revision, sim, notes, PsgTracks(notes, maker.Programs()), slots,
                   error))
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

std::vector<SongVersion> SongVersions(const DriverInfo& info, int song)
{
    std::vector<SongVersion> versions = {SongVersion()};
    if (song < 0 || size_t(song) >= info.music_settings.size())
    {
        return versions;
    }

    const std::vector<MusicSettings>& settings = info.music_settings[size_t(song)];
    for (size_t v = 1; v < settings.size(); v++)
    {
        SongVersion version;
        if (settings[v].music != settings[0].music)
        {
            const std::string music = std::to_string(settings[v].music);
            version = {"as music " + music, "_music" + music};
        }
        if (settings[v].riding_yoshi)
        {
            version.name += (version.name.empty() ? "" : " ") + std::string("riding Yoshi");
            version.suffix += "_yoshi";
        }
        versions.push_back(version);
    }

    return versions;
}

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
