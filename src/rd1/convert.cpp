// SPDX-License-Identifier: MIT
// Song conversion for Nintendo R&D1's MIDI-file sound driver. The model plays a song frame by frame. Its notes keep the
// ticks of the song's MIDI file, and a tempo map gives those ticks the time that the driver gives them. The channels'
// levels, pans and bends go in where the model changes them.

#include "rd1/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "psg.h"
#include "rd1/driver.h"
#include "rd1/sequencer.h"
#include "rd1/song.h"
#include "rd1/soundfont.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rd1
{
namespace
{

// The channels that play notes: the driver gives a track's notes the low 4 bits of the track's number.
constexpr int kNoteChannels = 16;

// The most frames that a conversion plays: an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The most frames that a PSG note's release is followed for: 10 s.
constexpr uint32_t kMaxRelease = 600;

// The MIDI file's ticks to a quarter note are at least this many. Each tick of the song's file becomes a whole number
// of them, and a channel's changes within a frame get ticks of their own.
constexpr int kMinQuarterTicks = 480;

// A note that the model played. Ticks are the song's, counted through every pass of its loop.
struct Note
{
    int channel = 0;
    int key = 0;
    int velocity = 0;
    int program = 0;
    uint64_t on = 0;
    uint64_t off = 0;
    uint32_t on_frame = 0;
    uint32_t off_frame = 0;
    bool ended = false;
    int level = 0; // the channel's level and pan where the note starts
    int pan = 0;
    InstrumentRecord part;     // the record that plays the note's key
    double velocity_level = 0; // the velocity that the MIDI note gives, 0-127 in a straight line of level
    bool late_end = false;     // whether a PSG note's release starts a frame after its end (PsgVelocity())
    double sweep_frames = 0;   // how long a note on square 1 with a sweep sounds, in frames; 0 for other notes
    std::vector<std::pair<double, double>> sweep; // its sweep's steps: frames after its start, semitones from its start
};

// What a channel plays with from a place on: its level on each side, 0 to 1, and its pitch in semitones. The level
// and CC10 give a PSG voice's sides (PsgSides()).
struct Setting
{
    uint64_t tick = 0;
    uint32_t frame = 0;
    double left = 0;
    double right = 0;
    double semitones = 0;
    double level = 0; // the channel's level, 0 to 1
    int pan = 64;     // the channel's CC10
};

// A change of tempo: the driver's step, in 256ths of a tick a frame.
struct TempoChange
{
    uint64_t tick = 0;
    uint32_t frame = 0;
    uint32_t step = 0;
};

// What the model played, up to the conversion's end.
struct Simulation
{
    std::vector<Note> notes;
    std::array<std::vector<Setting>, kNoteChannels> settings;
    std::vector<TempoChange> tempos;
    std::vector<uint64_t> positions; // each frame's place at its end, in 256ths of a tick
    std::vector<std::string> warnings;
    int division = 24;
    bool loops = false;
    uint64_t loop_start = 0;
    uint64_t loop_end = 0;
    uint64_t end = 0;
    uint32_t loop_start_frame = 0;
    uint32_t loop_end_frame = 0;
    uint32_t end_frame = 0;
    int echo = 0;
};

// Returns the level of a side: the driver's pan law, which keeps the nearer side at the full 127.
void PanSides(int pan, double& left, double& right)
{
    left = pan <= 63 ? 127 : 254 - 2 * pan;
    right = pan <= 63 ? 2 * pan : 127;
}

// Returns the pitch of a channel in semitones from its key: the bend and the LFO. The driver's pitch is a frequency.
// Its bend follows a curve between the key's pitch and the pitch `range` semitones away, and its LFO adds a share of a
// semitone's width in frequency.
double ChannelSemitones(const ChannelRecord& c)
{
    const int range = c.BendRange();
    const double width = std::exp2(range / 12.0);
    const int bend = c.Bend();
    double ratio = 1;
    if (bend >= 0x2000)
    {
        ratio = 1 + (width - 1) * (std::exp2((bend - 0x2000) / 8192.0) - 1);
    }
    else
    {
        ratio = 1 / width + (1 - 1 / width) * (std::exp2(bend / 8192.0) - 1);
    }
    if (c.LfoTarget() == 0)
    {
        ratio += (std::exp2(c.ModDepth() / 12.0) - 1) * c.Swing() / 32.0;
    }
    return ratio > 0 ? 12 * std::log2(ratio) : 0;
}

// Plays a song for the conversion. A song with a loop plays `loops` passes of it, from its second pass with
// LoopStart::kSecondPass. Other songs play until every track ends. The driver stops every channel's notes at the loop's
// end and a channel's notes at its track's end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, const SongRecord& record, int loops, LoopStart start_pass)
{
    Simulation sim;
    Sequencer seq(rom, info, record);
    if (!seq.Valid())
    {
        sim.warnings.push_back(seq.Error());
        return sim;
    }
    sim.division = seq.Division();
    sim.echo = std::clamp(info.echo[0], 0, 127);

    const bool second = start_pass == LoopStart::kSecondPass;
    const size_t passes = size_t(std::max(loops, 1)) + (second ? 1 : 0);
    std::vector<uint64_t> loop_ends;
    std::vector<uint32_t> loop_end_frames;
    uint64_t first_loop_start = 0;
    uint32_t first_loop_start_frame = 0;
    bool loop_seen = false;
    uint64_t last_tick = 0;
    std::array<int, kNoteChannels> programs = {};
    std::array<std::vector<size_t>, kNoteChannels> sounding;
    const auto end_notes = [&](int channel, uint64_t tick, uint32_t frame)
    {
        for (size_t n : sounding[size_t(channel)])
        {
            sim.notes[n].ended = true;
            sim.notes[n].off = tick;
            sim.notes[n].off_frame = frame;
        }
        sounding[size_t(channel)].clear();
    };

    uint32_t frame = 0;
    bool done = false;
    std::array<uint64_t, kNoteChannels> changed_at = {};
    std::array<bool, kNoteChannels> changed = {};
    for (; !done && frame < kMaxFrames; frame++)
    {
        const std::vector<SeqEvent>& events = seq.Step();
        sim.positions.push_back(seq.Position());
        changed.fill(false);
        size_t first_new = sim.notes.size();
        uint64_t stop_tick = UINT64_MAX;
        for (const SeqEvent& e : events)
        {
            if (e.tick >= stop_tick)
            {
                continue;
            }
            const int channel = e.channel & (kNoteChannels - 1);
            switch (e.kind)
            {
            case SeqEvent::Kind::kNoteOn:
                {
                    Note n;
                    n.channel = channel;
                    n.key = e.key;
                    n.velocity = e.velocity;
                    n.program = programs[size_t(channel)];
                    n.on = e.tick;
                    n.on_frame = frame;
                    sounding[size_t(channel)].push_back(sim.notes.size());
                    sim.notes.push_back(n);
                    break;
                }
            case SeqEvent::Kind::kNoteOff:
                {
                    std::vector<size_t>& list = sounding[size_t(channel)];
                    for (size_t k = 0; k < list.size();)
                    {
                        if (sim.notes[list[k]].key == e.key)
                        {
                            sim.notes[list[k]].ended = true;
                            sim.notes[list[k]].off = e.tick;
                            sim.notes[list[k]].off_frame = frame;
                            list.erase(list.begin() + std::ptrdiff_t(k));
                        }
                        else
                        {
                            k++;
                        }
                    }
                    break;
                }
            case SeqEvent::Kind::kProgram:
                programs[size_t(channel)] = e.value;
                break;
            case SeqEvent::Kind::kTempo:
                sim.tempos.push_back({e.tick, frame, uint32_t(e.value)});
                break;
            case SeqEvent::Kind::kSetting:
                if (!changed[size_t(channel)])
                {
                    changed[size_t(channel)] = true;
                    changed_at[size_t(channel)] = e.tick;
                }
                break;
            case SeqEvent::Kind::kLoopStart:
                if (!loop_seen)
                {
                    loop_seen = true;
                    first_loop_start = e.tick;
                    first_loop_start_frame = frame;
                }
                break;
            case SeqEvent::Kind::kLoopEnd:
                loop_ends.push_back(e.tick);
                loop_end_frames.push_back(frame);
                for (int c = 0; c < kNoteChannels; c++)
                {
                    end_notes(c, e.tick, frame);
                }
                if (loop_ends.size() >= passes)
                {
                    stop_tick = e.tick;
                    done = true;
                }
                break;
            case SeqEvent::Kind::kTrackEnd:
                end_notes(channel, e.tick, frame);
                last_tick = std::max(last_tick, e.tick);
                break;
            }
        }

        // What each channel plays with after the frame, where it differs from before.
        for (int c = 0; c < std::min(kNoteChannels, seq.ChannelCount()); c++)
        {
            const ChannelRecord& r = seq.Channel(c);
            for (size_t n = first_new; n < sim.notes.size(); n++)
            {
                if (sim.notes[n].channel == c)
                {
                    sim.notes[n].level = seq.ChannelLevel(c);
                    sim.notes[n].pan = r.Pan();
                }
            }
            Setting s;
            s.frame = frame;
            s.tick = changed[size_t(c)] ? changed_at[size_t(c)] : seq.Position() / 256;
            int pan = r.Pan() + (seq.BusPan() >> 1);
            if (r.LfoTarget() == 2)
            {
                pan += r.Swing() >> 1;
            }
            PanSides(std::clamp(pan, 0, 127), s.left, s.right);
            s.left = seq.ChannelLevel(c) * s.left / (127.0 * 127.0);
            s.right = seq.ChannelLevel(c) * s.right / (127.0 * 127.0);
            s.semitones = ChannelSemitones(r);
            s.level = seq.ChannelLevel(c) / 127.0;
            s.pan = r.Pan();
            std::vector<Setting>& list = sim.settings[size_t(c)];
            if (list.empty() || list.back().left != s.left || list.back().right != s.right ||
                list.back().semitones != s.semitones || list.back().level != s.level || list.back().pan != s.pan)
            {
                if (done && s.tick >= stop_tick)
                {
                    continue;
                }
                list.push_back(s);
            }
        }

        done = done || !seq.Playing();
    }

    sim.warnings.insert(sim.warnings.end(), seq.Warnings().begin(), seq.Warnings().end());
    if (loop_ends.size() >= passes)
    {
        sim.loops = true;
        sim.loop_start = second ? loop_ends[0] : first_loop_start;
        sim.loop_start_frame = second ? loop_end_frames[0] : first_loop_start_frame;
        sim.loop_end = loop_ends[second ? 1 : 0];
        sim.loop_end_frame = loop_end_frames[second ? 1 : 0];
        sim.end = loop_ends[passes - 1];
        sim.end_frame = loop_end_frames[passes - 1];
    }
    else
    {
        for (const Note& n : sim.notes)
        {
            last_tick = std::max(last_tick, n.on + 1);
        }
        sim.end = std::max<uint64_t>(last_tick, 1);
        sim.end_frame = std::max<uint32_t>(frame, 1);
        if (seq.Playing())
        {
            sim.warnings.push_back("it was cut off after an hour");
        }
    }

    // Notes end with the conversion. A note that starts on the end's tick belongs to the next pass.
    std::vector<Note> kept;
    for (Note n : sim.notes)
    {
        if (n.on >= sim.end)
        {
            continue;
        }
        if (!n.ended || n.off > sim.end)
        {
            n.ended = true;
            n.off = sim.end;
            n.off_frame = sim.end_frame;
        }
        kept.push_back(n);
    }
    sim.notes = std::move(kept);

    return sim;
}

// Returns true if the notes of a looping song's second pass start as those of its first: the same keys at the same
// ticks from the pass's start, with the same velocity and program, and within 2 of the same level and pan. The driver
// keeps the channels' settings when it goes back to the loop's start, so that a pass can leave them to the next.
bool FirstPassRepeats(const Simulation& sim)
{
    if (!sim.loops)
    {
        return true;
    }

    PassComparison passes(sim.loop_start, sim.loop_end - sim.loop_start);
    for (const Note& n : sim.notes)
    {
        passes.Add(n.on, n.channel, n.key, {double(n.velocity), double(n.program), double(n.level), double(n.pan)});
    }

    return passes.Alike(2);
}

// Plays a song for the conversion, with `loops` passes of its loop. A song whose first pass plays unlike the next marks
// its second pass.
Simulation SimulateConversion(const Rom& rom, const DriverInfo& info, const SongRecord& record, int loops)
{
    Simulation sim = Simulate(rom, info, record, loops, LoopStart::kFirstPass);
    if (sim.loops && !FirstPassRepeats(Simulate(rom, info, record, 2, LoopStart::kFirstPass)))
    {
        sim = Simulate(rom, info, record, loops, LoopStart::kSecondPass);
    }

    return sim;
}

// The MIDI ticks of a conversion: a whole number of them for each of the song's ticks, or with frame timing, the place
// of the frame that the driver plays an event in.
class Ticks
{
public:
    Ticks(const Simulation& sim, bool frame_timing)
        : sim_(sim),
          frame_timing_(frame_timing),
          scale_(std::max(1, (kMinQuarterTicks + sim.division - 1) / sim.division))
    {
    }

    int Division() const
    {
        return sim_.division * scale_;
    }

    uint32_t Of(uint64_t tick, uint32_t frame) const
    {
        if (frame_timing_ && frame < sim_.positions.size())
        {
            return uint32_t(sim_.positions[frame] * uint64_t(scale_) / 256);
        }
        return uint32_t(tick * uint64_t(scale_));
    }

    // Returns the tick `frames` frames after an event at `tick` in frame `frame`. The frames move at the song's pace.
    uint32_t After(uint64_t tick, uint32_t frame, double frames) const
    {
        if (sim_.positions.empty())
        {
            return Of(tick, frame);
        }
        const auto position = [&](double f)
        {
            const size_t last = sim_.positions.size() - 1;
            const size_t i = std::min(size_t(f), last);
            const double from = double(sim_.positions[i]);
            return from + (f - double(i)) * (double(sim_.positions[std::min(i + 1, last)]) - from);
        };
        const double moved = (position(frame + frames) - position(frame)) * scale_ / 256;
        return Of(tick, frame) + uint32_t(std::lround(std::max(0.0, moved)));
    }

    // Returns the tempo for a step of `step` 256ths of a tick a frame, in microseconds a quarter note.
    uint32_t Tempo(uint32_t step) const
    {
        if (!step)
        {
            return 0xFFFFFF;
        }
        const double frames = sim_.division * 256.0 / step;
        return uint32_t(std::clamp(std::lround(FrameSeconds(frames) * 1e6), 1L, 0xFFFFFFL));
    }

private:
    const Simulation& sim_;
    bool frame_timing_;
    int scale_;
};

// Returns the PSG's volume (0-15) at a voice's level `v`, as the driver works it out (0x080F28F8).
int PsgVolume(int v)
{
    return std::min(15, (v >> 3) + (v >> 4));
}

// Returns the wave channel's level as a PSG volume at the driver's volume `volume`. NR32 comes from the table at
// 0x083EBFC8: 25%, 50%, 75% or 100% for each 4 steps of volume.
double WaveVolume(int volume)
{
    return 15 * (1 + (volume >> 2)) / 4.0;
}

// Returns the envelope's level (0-127) in each frame of a voice from its note-on, as the driver steps it (0x080F1B74):
// `held` frames before the note's end, then the release, and at most `limit` frames. The driver writes the PSG's
// volume from the level before the frame's step, and turns the voice off in the frame whose step ends it (0x080F2B96).
// A level can end the voice only in the fall or the release.
std::vector<int> EnvelopeLevels(const InstrumentRecord& r, uint32_t held, uint32_t limit)
{
    std::vector<int> levels;
    int64_t level = r.start & 0xFFFFFF;
    int phase = 0;
    for (uint32_t f = 0; f < limit; f++)
    {
        if (f == held && phase < 3)
        {
            phase = 3;
        }
        const int current = int(std::max<int64_t>(level, 0) >> 16);
        bool ends = false;
        switch (phase)
        {
        case 0:
            level += int32_t(r.attack);
            if (level > 0x7EFFFF)
            {
                level = 0x7F0000;
                phase = 1;
            }
            break;
        case 1:
            level -= int32_t(r.decay);
            if (level <= int64_t(r.sustain))
            {
                level = r.sustain;
                phase = 2;
            }
            break;
        case 2:
            level = std::min<int64_t>(level - int32_t(r.fall), 0x7F0000);
            ends = level <= 0;
            break;
        default:
            level -= int32_t(r.release);
            ends = level <= 0;
            break;
        }
        if (ends)
        {
            break;
        }
        levels.push_back(current);
    }

    return levels;
}

// Returns the level (0 to 1) of a zone with envelope `env` in each of `frames` frames from its note-on, with the
// note's end after `held` frames, as a SoundFont player plays it. The attack rises in a straight line. The decay and
// the release fall 96 dB in their times, in a straight line in decibels, and the sustain keeps its attenuation. Each
// frame's level is the root of its mean square over four points.
std::vector<double> ZoneLevels(const VolumeEnvelope& env, uint32_t held, uint32_t frames)
{
    const auto seconds = [](int timecents)
    {
        return std::exp2(timecents / 1200.0);
    };
    const double attack = seconds(env.attack);
    const double hold = seconds(env.hold);
    const double decay = seconds(env.decay);
    const double release = seconds(env.release);
    const double sustain = env.sustain / 960.0; // the sustain's attenuation, as a share of 96 dB
    const double frame = FrameSeconds(1);
    const double off = held * frame;

    // The level of the attack and the attenuation as a share of 96 dB, at a time before the note's end.
    const auto before = [&](double t, double& level)
    {
        level = std::min(1.0, t / attack);
        t -= attack + hold;
        return t <= 0 ? 0.0 : std::min(sustain, t / decay);
    };
    double off_level = 0;
    const double off_share = before(off, off_level);
    std::vector<double> levels;
    for (uint32_t f = 0; f < frames; f++)
    {
        double sum = 0;
        for (int k = 0; k < 4; k++)
        {
            const double t = (f + (k + 0.5) / 4) * frame;
            double level = off_level;
            const double share = t < off ? before(t, level) : off_share + (t - off) / release;
            const double a = share >= 1 ? 0 : level * std::pow(10.0, -share * 96 / 20);
            sum += a * a;
        }
        levels.push_back(std::sqrt(sum / 4));
    }
    return levels;
}

// Returns the velocity, in a straight line of level, that gives PSG note `n` the loudness of the driver's volumes. The
// driver's volume follows the voice's level v = channel level x velocity x envelope >> 14 up to its limit of 15, in
// whole steps, and the wave plays one of four levels. The note's zone plays the line 3v/16 at kPsgFullVolume, with
// its envelope as a player plays it (ZoneLevels()). The velocity scales the zone to the same energy over the note, from
// the channel's level at the note's start. In the frame of the note's end, the driver writes the level from before the
// release's first step. A release that the step doesn't end therefore starts to be heard a frame later, and the MIDI
// note ends there (`n.late_end`). The energy then includes the release. A release that ends the voice at once leaves
// the zone's short fade out of the sum.
double PsgVelocity(Note& n)
{
    const uint32_t held = n.off_frame > n.on_frame ? n.off_frame - n.on_frame : 0;
    const std::vector<int> levels = EnvelopeLevels(n.part, held, held + kMaxRelease);
    double game = 0;
    for (int e : levels)
    {
        const int v = (n.level * n.velocity * e) >> 14;
        const double g = n.part.psg_channel == 2 ? WaveVolume(PsgVolume(v)) : PsgVolume(v);
        game += g * g;
    }
    n.late_end = levels.size() > held;
    const uint32_t sounds = held + (n.late_end ? 1 : 0);
    const VolumeEnvelope env = ZoneEnvelope(n.part);
    const uint32_t release = uint32_t(std::ceil(std::exp2(env.release / 1200.0) / FrameSeconds(1))) + 1;
    const double full = kPsgFullVolume * n.level * n.velocity / (127.0 * 127.0);
    double zone = 0;
    for (double z : ZoneLevels(env, sounds, n.late_end ? sounds + std::min(release, kMaxRelease) : held))
    {
        zone += full * z * full * z;
    }

    return zone > 0 ? std::min(127.0, n.velocity * std::sqrt(game / zone)) : n.velocity;
}

// Returns the driver's level (0-127) at which sample note `n` starts its release (EnvelopeLevels()), or -1 for a PSG
// note and for a note whose voice ends before its release. A short note releases from above the sustain level, and the
// driver's release then takes longer.
int ReleaseLevel(const Note& n)
{
    if (n.part.kind != InstrumentKind::kSample && n.part.kind != InstrumentKind::kFixed)
    {
        return -1;
    }
    const uint32_t held = n.off_frame > n.on_frame ? n.off_frame - n.on_frame : 0;
    const std::vector<int> levels = EnvelopeLevels(n.part, held, held + 1);
    return levels.size() > held ? levels[held] : -1;
}

// Moves the end of a song without a loop back to where its last sound dies away, when the end lies more than
// kMaxSilentEndSeconds later. A note's voice sounds while the driver's envelope keeps it on (EnvelopeLevels()), and
// its zone sounds until its release ends after the MIDI note's end. A note without an instrument plays nothing. The
// song then ends with the frame after the later of the two. A voice whose release doesn't end within kMaxRelease
// frames keeps the end.
void EndWithLastSound(const Rom& rom, const DriverInfo& info, const SongRecord& record, Simulation& sim)
{
    if (sim.loops)
    {
        return;
    }
    // The sample zones release as the SoundFont's do (InstrumentSet::ReleaseFrom()).
    InstrumentSet releases(rom, info);
    for (const Note& n : sim.notes)
    {
        Note kept = n;
        kept.part =
            PlayedInstrument(rom, ReadInstrument(rom, ProgramInstrument(rom, info, record.bank, n.program)), n.key);
        releases.Add(record.bank, n.program, n.key, ReleaseLevel(kept));
    }

    uint32_t quiet = 0; // the frame after the last that sounds
    for (const Note& n : sim.notes)
    {
        const InstrumentRecord program = ReadInstrument(rom, ProgramInstrument(rom, info, record.bank, n.program));
        const InstrumentRecord part = PlayedInstrument(rom, program, n.key);
        if (program.kind == InstrumentKind::kNone || part.kind == InstrumentKind::kNone)
        {
            continue;
        }
        const uint32_t held = n.off_frame > n.on_frame ? n.off_frame - n.on_frame : 0;
        const size_t frames = EnvelopeLevels(part, held, held + kMaxRelease).size();
        if (frames >= held + kMaxRelease)
        {
            return;
        }
        // A PSG note whose release the driver plays ends a frame late (PsgVelocity()).
        const uint32_t late = part.kind == InstrumentKind::kPsg && frames > held ? 1 : 0;
        const double from =
            part.kind == InstrumentKind::kPsg ? -1 : releases.ReleaseFrom(record.bank, n.program, n.key);
        const double release = std::exp2(ZoneEnvelope(part, from).release / 1200.0) / FrameSeconds(1);
        const uint32_t zone = n.off_frame + late + uint32_t(std::ceil(std::min(release, double(kMaxRelease))));
        quiet = std::max({quiet, n.on_frame + uint32_t(frames), zone});
    }

    if (quiet > 0 && quiet < sim.end_frame && quiet < sim.positions.size() &&
        FrameSeconds(sim.end_frame - quiet) > kMaxSilentEndSeconds)
    {
        sim.end_frame = quiet;
        sim.end = std::max<uint64_t>((sim.positions[quiet] + 255) / 256, 1);
    }
}

// Returns channel `c`'s pitch in semitones in frame `frame`.
double SemitonesAt(const Simulation& sim, int c, uint32_t frame)
{
    double semitones = 0;
    for (const Setting& s : sim.settings[size_t(c)])
    {
        if (s.frame > frame)
        {
            break;
        }
        semitones = s.semitones;
    }
    return semitones;
}

// Works out the notes that play square 1 with a sweep (Sweeps()). A new note writes NR10 and restarts square 1 at the
// voice's volume and frequency (0x080F2A52). After that the driver writes nothing to the channel while the voice
// sounds (0x080F2AB0). The volume stays, and the hardware's sweep moves the frequency. The note sounds until the voice
// ends, a later note restarts square 1, or a rising sweep passes the top and turns the channel off. Its velocity
// follows the volume of the restart, and its zone plays at a flat level. One model of square 1 plays every such note
// of the song, in the song's frames.
void SweepNotes(const Rom& rom, const DriverInfo& info, const Simulation& sim, std::vector<Note>& notes)
{
    PsgModel model(0);
    uint32_t fed = 0;
    for (size_t i = 0; i < notes.size(); i++)
    {
        Note& n = notes[i];
        if (!Sweeps(n.part))
        {
            continue;
        }
        for (; fed < n.on_frame; fed++)
        {
            model.Frame(PsgRegisters());
        }

        const uint32_t held = n.off_frame > n.on_frame ? n.off_frame - n.on_frame : 0;
        const std::vector<int> levels = EnvelopeLevels(n.part, held, held + kMaxRelease);
        uint32_t frames = uint32_t(levels.size());
        for (size_t j = i + 1; j < notes.size(); j++)
        {
            const Note& next = notes[j];
            if (next.part.kind == InstrumentKind::kPsg && next.part.psg_channel == 0 && next.on_frame > n.on_frame)
            {
                frames = std::min(frames, next.on_frame - n.on_frame);
                break;
            }
        }
        const int volume = levels.empty() ? 0 : PsgVolume((n.level * n.velocity * levels[0]) >> 14);
        const double hz = KeyPitch(rom, info, n.key) * std::exp2(SemitonesAt(sim, n.channel, n.on_frame) / 12);
        const int setting = PsgSetting(int(std::lround(hz)));
        PsgRegisters r;
        r.nr10 = uint16_t(n.part.psg_sweep);
        r.envelope = uint16_t(volume << 12);
        r.frequency = uint16_t(setting | 0x8000);
        r.restarted = true;
        n.sweep.clear();
        n.sweep_frames = frames;
        for (uint32_t k = 0; k < frames; k++, fed++)
        {
            if (model.Frame(r) < 0)
            {
                n.sweep_frames = k;
                fed++;
                break;
            }
            for (const auto& [at, frequency] : model.Sweeps())
            {
                n.sweep.emplace_back(k + at, PsgSemitones(setting, frequency));
            }
            r.frequency = uint16_t(setting);
            r.restarted = false;
        }

        const double line = kPsgFullVolume * n.level * n.velocity / (127.0 * 127.0);
        n.velocity_level = line > 0 && n.sweep_frames > 0 ? n.velocity * std::min(1.0, volume / line) : 0;
    }
}

// Returns the notes of the channels whose bits `track_mask` sets, and adds their instruments to `instruments`. Leaves
// out a note whose program or key has no instrument. The driver plays nothing for those. A PSG note's velocity
// follows the driver's volume (PsgVelocity()).
std::vector<Note> ChooseNotes(const Rom& rom, const DriverInfo& info, const SongRecord& record, const Simulation& sim,
                              uint32_t track_mask, InstrumentSet& instruments)
{
    std::vector<Note> notes;
    for (const Note& n : sim.notes)
    {
        if (!(track_mask & (1u << n.channel)))
        {
            continue;
        }
        const InstrumentRecord program = ReadInstrument(rom, ProgramInstrument(rom, info, record.bank, n.program));
        Note kept = n;
        kept.part = PlayedInstrument(rom, program, n.key);
        if (program.kind == InstrumentKind::kNone || kept.part.kind == InstrumentKind::kNone)
        {
            continue;
        }
        kept.velocity_level = kept.part.kind == InstrumentKind::kPsg ? PsgVelocity(kept) : n.velocity;
        notes.push_back(kept);
        instruments.Add(record.bank, n.program, n.key, ReleaseLevel(kept));
    }
    SweepNotes(rom, info, sim, notes);

    return notes;
}

// Returns the MIDI channel of each of the driver's channels that play notes in the song, whichever the conversion
// keeps: the melodic channels in order. A channel past the fifteenth shares the last of them.
std::array<int, kNoteChannels> MidiChannels(const Simulation& sim)
{
    std::array<bool, kNoteChannels> used = {};
    for (const Note& n : sim.notes)
    {
        used[size_t(n.channel)] = true;
    }
    std::array<int, kNoteChannels> channels = {};
    int next = 0;
    for (int c = 0; c < kNoteChannels; c++)
    {
        channels[size_t(c)] = MelodicChannel(std::min(next, 14));
        next += used[size_t(c)] ? 1 : 0;
    }

    return channels;
}

// Returns setting `s` with the sides of a PSG voice. Each frame, the driver enables a PSG channel's left side for a
// CC10 of 64 or less and its right side for 64 or more (0x080F2BE4). The PSG plays the channel's level on the sides
// that it enables.
Setting PsgSides(Setting s)
{
    s.left = s.pan <= 64 ? s.level : 0;
    s.right = s.pan >= 64 ? s.level : 0;
    return s;
}

// Returns the settings that channel `c` plays with. A sample voice plays by the channel's pan law, and a PSG voice on
// the sides that CC10 enables. Each setting follows the kind of the channel's latest note to start, or of its first
// note before that. Where the kind changes at a note's start, a setting goes there.
std::vector<Setting> ChannelSettings(int c, const std::vector<Note>& notes, const Simulation& sim)
{
    struct Switch
    {
        uint64_t tick;
        uint32_t frame;
        bool psg;
    };
    std::vector<Switch> switches;
    for (const Note& n : notes)
    {
        const bool psg = n.part.kind == InstrumentKind::kPsg;
        if (n.channel == c && (switches.empty() || switches.back().psg != psg))
        {
            switches.push_back({n.on, n.on_frame, psg});
        }
    }
    bool psg = !switches.empty() && switches.front().psg;
    std::vector<Setting> out;
    const Setting* current = nullptr;
    size_t k = 0;
    const auto apply = [&](const Setting& s)
    {
        out.push_back(psg ? PsgSides(s) : s);
    };
    for (const Setting& s : sim.settings[size_t(c)])
    {
        for (; k < switches.size() && switches[k].frame <= s.frame; k++)
        {
            psg = switches[k].psg;
            if (current && switches[k].frame < s.frame)
            {
                Setting moved = *current;
                moved.tick = switches[k].tick;
                moved.frame = switches[k].frame;
                apply(moved);
            }
        }
        apply(s);
        current = &s;
    }
    for (; current && k < switches.size(); k++)
    {
        psg = switches[k].psg;
        Setting moved = *current;
        moved.tick = switches[k].tick;
        moved.frame = switches[k].frame;
        apply(moved);
    }

    return out;
}

// Writes channel `c`'s track. It starts with the channel's bank and first program, CC7 at 127, the reverb's send of its
// first note and no chorus. The mixer echoes every sample voice: each pass starts from its output of 1568 samples
// before (0x080F0224). The PSG plays outside the mixer. A sample note sends the echo's first setting to the reverb, and
// a PSG note sends 0. CC91 changes at a note where the send differs. Notes hold their velocities and the channel's
// level goes to CC10 and CC11 (ChannelSettings()). The channel's first settings come at tick 0, and all of them come
// again at the loop's start. A player that goes back there then has the settings that the model has.
void WriteChannel(MidiTrack& mt, int c, int midi_channel, const std::vector<Note>& notes, const Simulation& sim,
                  const SongRecord& record, const Ticks& ticks, uint32_t loop_start, uint32_t end)
{
    char name[32];
    std::snprintf(name, sizeof name, "Channel %d", c);
    mt.Name(name);
    mt.SetEnd(end);
    const Note* first = nullptr;
    for (const Note& n : notes)
    {
        if (n.channel == c)
        {
            first = &n;
            break;
        }
    }
    const int program = first ? first->program : -1;
    const auto send = [&](const Note& n)
    {
        return n.part.kind == InstrumentKind::kPsg ? 0 : sim.echo;
    };
    mt.Bank(0, midi_channel, record.bank);
    mt.Program(0, midi_channel, program);
    mt.Control(0, midi_channel, cc::kVolume, 127);
    mt.Control(0, midi_channel, cc::kReverb, first ? send(*first) : sim.echo);
    mt.Control(0, midi_channel, cc::kChorus, 0);

    const std::vector<Setting> settings = ChannelSettings(c, notes, sim);

    // The sweeps of the channel's notes on square 1. The channel's bends don't reach such a note after its start.
    struct Sweep
    {
        uint32_t on = 0;
        uint32_t end = 0;
        double from = 0; // the channel's pitch at the note's start
        const Note* note = nullptr;
    };
    std::vector<Sweep> sweeps;
    double widest = 0;
    for (const Note& n : notes)
    {
        if (n.channel == c && Sweeps(n.part))
        {
            const double from = SemitonesAt(sim, c, n.on_frame);
            sweeps.push_back({ticks.Of(n.on, n.on_frame), ticks.After(n.on, n.on_frame, n.sweep_frames), from, &n});
            for (const auto& step : n.sweep)
            {
                widest = std::max(widest, std::abs(from + step.second));
            }
        }
    }
    const auto swept = [&](uint32_t at)
    {
        return std::any_of(sweeps.begin(), sweeps.end(), [&](const Sweep& w) { return at > w.on && at < w.end; });
    };
    for (const Setting& s : settings)
    {
        widest = std::max(widest, std::abs(s.semitones));
    }
    const int range = BendRangeFor(widest, kSmallBend);
    if (range)
    {
        mt.BendRange(0, midi_channel, range);
    }

    int pan = -1;
    int expression = -1;
    int bend = -1;
    const auto write = [&](uint32_t at, const Setting& s)
    {
        int cc10 = pan;
        int cc11 = expression;
        LevelsToControllers(s.left * 128, s.right * 128, cc10, cc11);
        if (cc10 != pan)
        {
            mt.Control(at, midi_channel, cc::kPan, cc10);
            pan = cc10;
        }
        if (cc11 != expression)
        {
            mt.Control(at, midi_channel, cc::kExpression, cc11);
            expression = cc11;
        }
        if (range && swept(at))
        {
            bend = -1;
        }
        else if (range)
        {
            const int value = BendValue(s.semitones, range);
            if (value != bend)
            {
                mt.PitchBend(at, midi_channel, value);
                bend = value;
            }
        }
    };

    // The settings, up to the end. The first ones go at tick 0.
    const Setting* last = nullptr;
    bool restated = loop_start == 0;
    for (const Setting& s : settings)
    {
        uint32_t at = ticks.Of(s.tick, s.frame);
        if (at >= end)
        {
            break;
        }
        if (!last)
        {
            at = 0;
        }
        if (!restated && at >= loop_start && last)
        {
            pan = expression = bend = -1;
            write(loop_start, *last);
            restated = true;
        }
        write(at, s);
        last = &s;
    }
    if (!restated && last)
    {
        pan = expression = bend = -1;
        write(loop_start, *last);
    }

    // Each sweep's steps, then the channel's pitch again where the note ends.
    for (const Sweep& w : sweeps)
    {
        if (!range || w.on >= end)
        {
            continue;
        }
        for (const auto& [frames, semitones] : w.note->sweep)
        {
            const uint32_t at = ticks.After(w.note->on, w.note->on_frame, frames);
            if (at > w.on && at < w.end && at < end)
            {
                mt.PitchBend(at, midi_channel, BendValue(w.from + semitones, range));
            }
        }
        if (w.end < end && !swept(w.end))
        {
            double semitones = 0;
            for (const Setting& s : settings)
            {
                if (&s != &settings.front() && ticks.Of(s.tick, s.frame) > w.end)
                {
                    break;
                }
                semitones = s.semitones;
            }
            mt.PitchBend(w.end, midi_channel, BendValue(semitones, range));
        }
    }

    // The notes. The driver's program applies to the notes that start after it. Every note gives its program and its
    // reverb's send where they differ. A note at or after the loop's start gives them once more.
    int current = program;
    int reverb = first ? send(*first) : sim.echo;
    bool program_restated = loop_start == 0;
    for (const Note& n : notes)
    {
        if (n.channel != c)
        {
            continue;
        }
        const uint32_t on = ticks.Of(n.on, n.on_frame);
        uint32_t off = std::max(
            on + 1, Sweeps(n.part) ? ticks.After(n.on, n.on_frame, n.sweep_frames) : ticks.Of(n.off, n.off_frame));
        if (n.late_end && !Sweeps(n.part))
        {
            // The note ends a frame later. A later note that restarts its PSG channel, or plays its key on the
            // channel, can end it sooner, but not before its end in the song.
            uint32_t late = ticks.After(n.off, n.off_frame, 1);
            for (const Note* next = &n + 1; next < notes.data() + notes.size(); next++)
            {
                const bool restarts =
                    next->part.kind == InstrumentKind::kPsg && next->part.psg_channel == n.part.psg_channel;
                if (restarts || (next->channel == c && next->key == n.key))
                {
                    late = std::min(late, ticks.Of(next->on, next->on_frame));
                    break;
                }
            }
            off = std::max(off, late);
        }
        if (!program_restated && on >= loop_start)
        {
            current = reverb = -1;
            program_restated = true;
        }
        if (current != n.program)
        {
            mt.Program(on, midi_channel, n.program);
            current = n.program;
        }
        if (reverb != send(n))
        {
            reverb = send(n);
            mt.Control(on, midi_channel, cc::kReverb, reverb);
        }
        mt.NoteOn(on, midi_channel, n.key, SquareRootValue(n.velocity_level, 127, 1));
        mt.NoteOff(std::min(off, end), midi_channel, n.key);
    }
}

// Writes the MIDI file: a conductor track with the tempo map and the loop's markers, then a track for each channel that
// plays notes.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
               const SongRecord& record, const std::vector<Note>& notes, bool frame_timing, std::string& error)
{
    const Ticks ticks(sim, frame_timing);
    const uint32_t end = std::max<uint32_t>(ticks.Of(sim.end, sim.end_frame), 1);
    MidiFile midi(uint16_t(ticks.Division()));
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    bool first = true;
    for (const TempoChange& t : sim.tempos)
    {
        const uint32_t at = first ? 0 : ticks.Of(t.tick, t.frame);
        if (at < end)
        {
            conductor.Tempo(at, ticks.Tempo(t.step));
        }
        first = false;
    }
    const uint32_t loop_start = sim.loops ? ticks.Of(sim.loop_start, sim.loop_start_frame) : 0;
    if (sim.loops)
    {
        conductor.Meta(loop_start, 0x06, "loopStart");
        conductor.Meta(ticks.Of(sim.loop_end, sim.loop_end_frame), 0x06, "loopEnd");
        conductor.RepeatTempo(loop_start);
    }
    conductor.SetEnd(end);

    const std::array<int, kNoteChannels> channels = MidiChannels(sim);
    for (int c = 0; c < kNoteChannels; c++)
    {
        const bool plays = std::any_of(notes.begin(), notes.end(), [&](const Note& n) { return n.channel == c; });
        if (plays)
        {
            WriteChannel(midi.AddTrack(), c, channels[size_t(c)], notes, sim, record, ticks, loop_start, end);
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, or with `write` false, works out what the conversion would be.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write)
{
    SongReport report;
    SongRecord record;
    if (!ReadSongRecord(rom, info, song, record))
    {
        report.warnings.push_back("its record can't be read");
        return report;
    }
    report.address = record.midi;

    Simulation sim = SimulateConversion(rom, info, record, settings.loops);
    EndWithLastSound(rom, info, record, sim);
    report.warnings = sim.warnings;
    InstrumentSet own(rom, info);
    const std::vector<Note> notes = ChooseNotes(rom, info, record, sim, settings.track_mask, shared ? *shared : own);

    std::array<bool, kNoteChannels> used = {};
    for (const Note& n : notes)
    {
        used[size_t(n.channel)] = true;
    }
    report.tracks = int(std::count(used.begin(), used.end(), true));
    report.seconds = FrameSeconds(sim.end_frame);
    if (sim.loops)
    {
        report.loop_start = FrameSeconds(sim.loop_start_frame);
        report.loop_end = FrameSeconds(sim.loop_end_frame);
    }
    if (!sim.tempos.empty() && sim.tempos.front().step)
    {
        report.bpm = 60 / FrameSeconds(sim.division * 256.0 / sim.tempos.front().step);
    }

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    const std::string number = SongNumber(song, info.song_count);
    const std::string title = rom.Title() + " #" + number;
    char about[200];
    std::snprintf(about, sizeof about, "%s (%s) song %d, %s, MIDI file at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, record.name.c_str(), unsigned(record.midi), kProgramName);
    const std::string stem = SongStem(settings, number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, sim, record, notes, settings.frame_timing, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    if (!shared)
    {
        Sf2File file = own.Build();
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

// Appends a line for each event of the track from `at` to `end` to `text`.
void DumpTrack(const Rom& rom, uint32_t at, uint32_t end, std::string& text)
{
    char line[64];
    uint64_t tick = 0;
    int status = 0;
    const auto bytes = [&](uint32_t from, uint32_t length)
    {
        for (uint32_t k = 0; k < length && k < 64; k++)
        {
            std::snprintf(line, sizeof line, " %02X", unsigned(rom.U8(from + k)));
            text += line;
        }
    };
    while (at < end)
    {
        tick += ReadVariable(rom, at);
        if (at >= end)
        {
            break;
        }
        if (rom.U8(at) & 0x80)
        {
            status = rom.U8(at++);
        }
        std::snprintf(line, sizeof line, "  %8llu %08X %02X", static_cast<unsigned long long>(tick), unsigned(at),
                      unsigned(status));
        text += line;
        if (status == 0xFF)
        {
            const int type = rom.U8(at++);
            const uint32_t length = ReadVariable(rom, at);
            std::snprintf(line, sizeof line, " meta %02X:", unsigned(type));
            text += line;
            bytes(at, length);
            at += length;
        }
        else if (status >= 0xF0)
        {
            const uint32_t length = ReadVariable(rom, at);
            text += " sysex:";
            bytes(at, length);
            at += length;
        }
        else
        {
            const uint32_t size = (status & 0xE0) == 0xC0 ? 1 : 2;
            bytes(at, size);
            at += size;
        }
        text += "\n";
    }
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    SongRecord record;
    SongFile file;
    if (!ReadSongRecord(rom, info, song, record) || !ReadSongFile(rom, record.midi, file, error))
    {
        if (error.empty())
        {
            error = "song " + std::to_string(song) + " has no record";
        }
        return false;
    }
    char line[200];
    std::snprintf(line, sizeof line,
                  "song %d: %s, record at %08X, MIDI file at %08X, player %d, bank %d, volume %d, "
                  "priority %d\n",
                  song, record.name.c_str(), unsigned(record.address), unsigned(record.midi), record.player,
                  record.bank, record.volume, record.priority);
    std::string text = line;
    std::snprintf(line, sizeof line, "format %d, %zu tracks, %d ticks a quarter note\n", file.format,
                  file.tracks.size(), file.division);
    text += line;
    for (size_t t = 0; t < file.tracks.size(); t++)
    {
        text += "track " + std::to_string(t) + "\n";
        DumpTrack(rom, file.tracks[t].start, file.tracks[t].end, text);
    }

    return WriteTextFile(path, text, error);
}

} // namespace agbsf2::rd1
