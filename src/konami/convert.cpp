// SPDX-License-Identifier: MIT

#include "konami/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "beat_grid.h"
#include "beat_midi.h"
#include "files.h"
#include "gba.h"
#include "konami/driver.h"
#include "konami/seqformat.h"
#include "konami/sequencer.h"
#include "konami/soundfont.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "psg.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"

namespace agbsf2::konami
{
namespace
{

constexpr int kVelocity = 127;   // every note's velocity, as CC11 carries the loudness (see LevelsToControllers())
constexpr int kTailFrames = 120; // frames the last notes ring for when every track ends without stopping the song

// The number of entries in the driver's PSG frequency table: one for each 1/32 semitone of notes 0 to 83 (C2 to B8).
// Other data follows it.
constexpr int kPsgFreqEntries = 84 * 32;

// Returns the pitch that frequency register value `x` (0-2047) plays, to the nearest 1/32 semitone from PSG note 0. A
// square channel's note 0 is C2. The wave channel plays an octave lower. Its note 0 is an octave lower too.
int RegisterPitch(int x)
{
    return int(std::lround(384 * std::log2(SquareHz(x) / kC2)));
}

const char* const kPsgNames[kPsgTracks] = {"Square 1", "Square 2", "Wave", "Noise"};

std::string TrackName(int t)
{
    if (IsPsgTrack(t))
    {
        return kPsgNames[t];
    }

    return "Voice " + std::to_string(t - kPsgTracks);
}

// Shared settings for one frame: echo, plus PSG master volume and wave RAM selection in Dungeon Dice Monsters.
struct FrameState
{
    uint16_t bus0 = 0, bus1 = 0;
    uint8_t feedback = 0, delay = 0;
    uint8_t psg_volume = 0x77; // NR50
    int wave = 0;
    int wave_loads = 0; // wave loads since the song started, including reloads
};

// Every frame of a song's sequencer output, and the frames of the marked pass through its loop.
struct Simulation
{
    std::vector<std::array<TrackOutput, kTracks>> frames;
    std::vector<FrameState> state;
    int loop_start = -1, loop_end = -1; // frames of the marked pass through the loop
    int first_loop_end = -1;            // the frame the song first loops in, which starts its second pass
    bool delayed_loop_point = false;    // a track's loop point starts with a delay that only the later passes play
    std::vector<std::string> warnings;
};

// Records each frame's track outputs and echo settings until the song stops, all tracks end, or the loop has played
// `loops` times. From LoopStart::kSecondPass, the marked loop is the song's second pass through it. The song then plays
// the first pass before it. An invalid song header produces a warning and no frames.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass)
{
    Simulation sim;
    const std::unique_ptr<Sequencer> sequencer = Sequencer::Create(rom, info, song);
    Sequencer& seq = *sequencer;
    if (!seq.Valid())
    {
        sim.warnings.push_back("song header is invalid");
        return sim;
    }

    bool ended = false;
    for (uint32_t f = 0; f < kMaxFrames; f++)
    {
        const auto& out = seq.Step();
        if (seq.LoopedLastFrame())
        {
            if (sim.loop_end < 0)
            {
                sim.loop_end = int(f);
                sim.loop_start = seq.LoopStartFrame();
                sim.first_loop_end = int(f);
                sim.delayed_loop_point = seq.LoopPointDelayed();
            }
            else if (start_pass == LoopStart::kSecondPass && seq.LoopsDone() == 2)
            {
                sim.loop_start = sim.loop_end;
                sim.loop_end = int(f);
            }

            if (seq.LoopsDone() >= loops + (start_pass == LoopStart::kSecondPass ? 1 : 0))
            {
                ended = true; // this frame already belongs to the next pass
                break;
            }
        }

        sim.frames.push_back(out);
        FrameState e;
        e.bus0 = seq.Echo(0).voices;
        e.bus1 = seq.Echo(1).voices;
        e.feedback = uint8_t(std::clamp(seq.Echo(0).feedback, 0, 255));
        e.delay = uint8_t(std::clamp(seq.Echo(0).delay, 0, 255));
        e.psg_volume = seq.PsgVolume();
        e.wave = seq.LoadedWave();
        e.wave_loads = seq.WaveLoads();
        sim.state.push_back(e);

        if (seq.Stopped())
        {
            ended = true;
            break;
        }

        if (!seq.AnyTrackActive())
        {
            // Every track has ended without stopping the song: let the last notes ring.
            for (int i = 0; i < kTailFrames; i++)
            {
                sim.frames.emplace_back();
                sim.state.push_back(e);
            }

            ended = true;
            break;
        }
    }

    sim.warnings = seq.Warnings();
    if (!ended && sim.loop_end < 0)
    {
        sim.warnings.push_back("song didn't end or loop within the frame limit; output truncated");
    }
    else if (!ended)
    {
        sim.warnings.push_back(
            "song reached the frame limit after playing its loop " + std::to_string(seq.LoopsDone()) + " of " +
            std::to_string(loops + (start_pass == LoopStart::kSecondPass ? 1 : 0)) + " times; output truncated");
    }

    return sim;
}

// A track event for MIDI conversion.
struct Event
{
    // The kinds of event.
    enum Type
    {
        kNoteOn,
        kNoteOff,
        kLevels,
        kBend,
        kEcho
    };

    Type type;
    uint32_t frame;
    Kind kind = Kind::kSample;
    int param = 0; // duty, wave row (wave << 4 | volume), noise note or sample index
    int note = 0;  // PSG note number or DS semitone offset
    int dev = 0;   // pitch deviation from the note, 1/32 semitones
    double level_l = 0, level_r = 0;
    int echo = 0; // reverb send standing in for the driver's echo
};

// Mixer level of a DS voice / PSG channel on each side, in DirectSound units: a level of n plays a full-scale sample at
// amplitude n/128.
struct Levels
{
    double left = 0, right = 0;
};

// Writes the events of one track's notes, and holds the state of the note that's playing. The renderers of the output
// stages build on it.
class NoteWriter
{
public:
    // Ends any active note at frame `f`.
    void NoteOff(uint32_t f, std::vector<Event>& out)
    {
        if (on_)
        {
            out.push_back(Event{Event::kNoteOff, f});
        }

        on_ = false;
    }

protected:
    NoteWriter(const Rom& rom, const DriverInfo& info, int track) : rom_(rom), info_(info), t_(track)
    {
    }

    // Starts a note. Any note that's playing has to be ended first.
    void NoteOn(uint32_t f, Kind kind, int param, int note, int dev, Levels lv, std::vector<Event>& out)
    {
        Event e{Event::kNoteOn, f};
        e.kind = kind;
        e.param = param;
        e.note = note;
        e.dev = dev;
        e.level_l = lv.left;
        e.level_r = lv.right;
        out.push_back(e);
        on_ = true;
    }

    // Updates the active note's levels and, if `bend` is true, its pitch.
    void Update(uint32_t f, int dev, Levels lv, bool bend, std::vector<Event>& out) const
    {
        if (!on_)
        {
            return;
        }

        if (bend)
        {
            Event e{Event::kBend, f};
            e.dev = dev;
            out.push_back(e);
        }

        Event e{Event::kLevels, f};
        e.level_l = lv.left;
        e.level_r = lv.right;
        out.push_back(e);
    }

    // Changes the levels of the sample note that's playing to `lv` at frame `f`, if they differ.
    void ChangeLevels(uint32_t f, Levels lv, std::vector<Event>& out)
    {
        if (lv.left == levels_.left && lv.right == levels_.right)
        {
            return;
        }

        levels_ = lv;
        Event e{Event::kLevels, f};
        e.level_l = lv.left;
        e.level_r = lv.right;
        out.push_back(e);
    }

    // Warns, the first time only, that the track skips the notes of invalid sample `sample`.
    void SkipInvalidSample(int sample, std::vector<std::string>& warnings)
    {
        if (skipped_.insert(sample).second)
        {
            warnings.push_back("track " + std::to_string(t_) + ": notes of invalid sample " + std::to_string(sample) +
                               " skipped");
        }
    }

    const Rom& rom_;
    const DriverInfo& info_;
    int t_;
    bool on_ = false;
    int note_ = 0;          // the note number that bends of the note that's playing are measured from
    int key_ = 0;           // older revisions and Dungeon Dice Monsters: the sample of the note that's playing
    Levels levels_;         // older revisions and Dungeon Dice Monsters: the levels of the sample note that's playing
    std::set<int> skipped_; // invalid samples whose notes were skipped, with a warning the first time
};

// Turns one track's per-frame driver output into note, level, bend and echo events in the Ultimate Masters, WCT 2004,
// Rave Master and Eternal Duelist revisions, following what the revision's output stage does with it.
class TrackRenderer : public NoteWriter
{
public:
    TrackRenderer(const Rom& rom, const DriverInfo& info, int track) : NoteWriter(rom, info, track)
    {
        // SOUNDCNT_L starts as 0xBB77: squares and noise on both sides, wave off until used.
        left_ = right_ = track != 2;
    }

    // Adds the events of frame `f` to `out`, from the track's output record and the frame's other settings. In the WCT
    // 2004, Rave Master and Eternal Duelist revisions, PSG channels take their frames from PsgFrame() instead.
    void Frame(uint32_t f, const TrackOutput& o, const FrameState& state, std::vector<Event>& out,
               std::vector<std::string>& warnings)
    {
        const bool wct2004 = info_.revision == Revision::kWct2004;
        if (!IsPsgTrack(t_))
        {
            // CC91 stands in for bus 0's echo, the one the songs set up. A voice routed to buses 0 and 1 is mixed into
            // bus 1 alone. The WCT 2004 revision's mixer takes only voices 5 to 11 from bus 0.
            const int voice = t_ - kPsgTracks;
            const bool wet = ((state.bus0 >> voice) & 1) && !((state.bus1 >> voice) & 1) && (!wct2004 || voice >= 5);
            const int send = wet ? std::min<int>(127, state.feedback) : 0;
            if (send != echo_)
            {
                echo_ = send;
                Event e{Event::kEcho, f};
                e.echo = send;
                out.push_back(e);
            }

            if (OlderOutputStage(info_.revision))
            {
                SampleOlder(f, o, out, warnings);
                return;
            }
        }

        if (!o.Any())
        {
            return;
        }

        if (IsPsgTrack(t_))
        {
            Psg(f, o, out);
        }
        else
        {
            Sample(f, o, out, warnings);
        }
    }

    // Adds the events of frame `f` of a PSG channel in the WCT 2004, Rave Master and Eternal Duelist revisions to
    // `out`, from the record that the output stage hands the channel and the sides it plays on.
    void PsgFrame(uint32_t f, const TrackOutput& o, bool left, bool right, std::vector<Event>& out)
    {
        if (left != left_ || right != right_)
        {
            left_ = left;
            right_ = right;
            Update(f, 0, PsgLevels(level_), false, out);
        }

        if (o.Any())
        {
            PsgChannel(f, o, out);
        }
    }

private:
    // The frequency that the driver's square or wave output writes.
    struct PsgFrequency
    {
        int pitch = 0;       // 1/32 semitones from note 0
        int note = 0;        // the note that a trigger starts
        bool restart = true; // true if a trigger restarts a square channel
    };

    // Returns the levels of a PSG channel at volume v, on the sides it plays on.
    Levels PsgLevels(int v) const
    {
        const double level = kPsgLevelPerVolume * v;
        return {left_ ? level : 0, right_ ? level : 0};
    }

    // Returns the frequency written for square or wave output `o`. The frequency table approximates equal temperament
    // and sets the channel's restart bit. Out-of-range pitches read whatever data lies outside the table; note FF, used
    // as note -1, is one such case.
    PsgFrequency Frequency(const TrackOutput& o) const
    {
        PsgFrequency fr{o.pitch, o.note_pitch >> 5, true};
        if ((o.pitch < 0 || o.pitch >= kPsgFreqEntries) && info_.psg_freq_table)
        {
            const uint16_t v = rom_.U16(info_.psg_freq_table + 2 * uint32_t(o.pitch));
            fr.pitch = RegisterPitch(v & 0x7FF);
            fr.note = (fr.pitch + 16) >> 5;
            fr.restart = v & 0x8000;
        }

        return fr;
    }

    // Mirrors the Ultimate Masters revision's PSG output stage, which sets the sides a channel plays on as it writes
    // it.
    void Psg(uint32_t f, const TrackOutput& o, std::vector<Event>& out)
    {
        if (o.vol != 0)
        {
            left_ = right_ = true;
            if (o.pan_l < o.pan_r)
            {
                left_ = false;
            }
            if (o.pan_l > o.pan_r)
            {
                right_ = false;
            }
        }

        PsgChannel(f, o, out);
    }

    // Mirrors what the driver writes to a PSG channel for output `o`.
    void PsgChannel(uint32_t f, const TrackOutput& o, std::vector<Event>& out)
    {
        const PsgFrequency fr = Frequency(o);
        switch (t_)
        {
        case 0:
        case 1:
            // Squares. A trigger writes NRx2 = vol << 12 | duty byte, and a frequency that restarts the channel, unless
            // it came from past the end of the table without bit 15. Then the note that's playing, if there's one, goes
            // on at that frequency.
            if (o.trig)
            {
                level_ = o.vol & 15;
            }
            if (o.trig && (fr.restart || !level_))
            {
                NoteOff(f, out);
                if (level_)
                {
                    note_ = fr.note;
                    NoteOn(f, Kind::kSquare, o.b2 >> 6, note_, fr.pitch - 32 * note_, PsgLevels(level_), out);
                }
            }
            else
            {
                Update(f, fr.pitch - 32 * note_, PsgLevels(level_), true, out);
            }
            break;

        case 2:
            {
                // Wave channel. Volume 0 mutes it, and a trigger loads the wave pre-scaled to the volume. The driver
                // writes only the low 11 bits of the frequency, so it never restarts the channel. The older revisions
                // load the wave on every output, from the record's wave number, which a bend or a legato note leaves at
                // 0. MIDI can only play a new wave as a new note.
                const int row = (o.b2 << 4) + o.vol; // the driver's wave-table row
                const bool reload = OlderOutputStage(info_.revision) && on_ && row != row_;
                if (o.vol == 0)
                {
                    NoteOff(f, out);
                }
                else if (o.trig || reload)
                {
                    NoteOff(f, out);
                    level_ = kWaveRowLevel / kPsgLevelPerVolume; // the row itself holds the volume
                    if (row & 15)
                    {
                        note_ = fr.note;
                        row_ = row;
                        NoteOn(f, Kind::kWave, row, note_, fr.pitch - 32 * note_, PsgLevels(level_), out);
                    }
                }
                else
                {
                    Update(f, fr.pitch - 32 * note_, PsgLevels(level_), true, out);
                }
                break;
            }

        default:
            {
                // Noise channel. Every output rewrites NR42 and NR43 from the noise table's word, which restarts the
                // channel if its bit 15 is set, as it is throughout the Ultimate Masters revision's table. Otherwise a
                // note that's playing goes on with the new setting, which MIDI can only play as a new note.
                const uint16_t setting = info_.NoiseSetting(rom_, o.pitch);
                level_ = o.vol & 15;
                if ((setting & 0x8000) || !level_ || (on_ && (setting & 0xFF) != noise_))
                {
                    const bool sounds = (setting & 0x8000) || on_;
                    NoteOff(f, out);
                    if (level_ && sounds)
                    {
                        noise_ = setting & 0xFF;
                        NoteOn(f, Kind::kNoise, o.pitch, 0, 0, PsgLevels(level_), out);
                    }
                }
                else
                {
                    Update(f, 0, PsgLevels(level_), false, out);
                }
                break;
            }
        }
    }

    // Mirrors the driver's DirectSound voice update.
    void Sample(uint32_t f, const TrackOutput& o, std::vector<Event>& out, std::vector<std::string>& warnings)
    {
        const Levels lv{double(info_.VoiceLevel(rom_, o.vol, o.pan_l)), double(info_.VoiceLevel(rom_, o.vol, o.pan_r))};
        const int dev = o.pitch - o.note_pitch;
        if (o.flags & kOutStop)
        {
            NoteOff(f, out);
        }
        else
        {
            bool levels_written = false;
            if (o.flags & kOutNoteOn)
            {
                NoteOff(f, out);
                if (info_.Sample(rom_, o.key).valid)
                {
                    NoteOn(f, Kind::kSample, o.key, o.note_pitch >> 5, dev, lv, out);
                    levels_written = true;
                }
                else
                {
                    SkipInvalidSample(o.key, warnings);
                }
            }
            else if (o.flags & kOutPitch)
            {
                Update(f, dev, lv, true, out);
                levels_written = true;
            }
            if (!levels_written && (o.trig || (o.flags & kOutPan)))
            {
                Update(f, dev, lv, false, out);
            }
        }
    }

    // Mirrors the older revisions' DirectSound voice update. It sets a playing voice's levels from its track's volume
    // and pan, in WCT 2004 on every frame, and in Rave Master whenever the track retriggers the voice.
    void SampleOlder(uint32_t f, const TrackOutput& o, std::vector<Event>& out, std::vector<std::string>& warnings)
    {
        const Levels lv{double(info_.VoiceLevel(rom_, o.vol, o.pan >> 4)),
                        double(info_.VoiceLevel(rom_, o.vol, o.pan & 15))};
        if (o.flags & kOutStop)
        {
            NoteOff(f, out);
            return;
        }

        if (o.flags & kOutNoteOn)
        {
            NoteOff(f, out);
            if (info_.Sample(rom_, o.key).valid)
            {
                note_ = o.note_pitch >> 5;
                key_ = o.key;
                NoteOn(f, Kind::kSample, o.key, note_, o.pitch - 32 * note_, lv, out);
                levels_ = lv;
            }
            else
            {
                SkipInvalidSample(o.key, warnings);
            }

            return;
        }

        if (!on_)
        {
            return;
        }

        // Eternal Duelist takes any other flag to change the pitch. The driver works the voice's step out from the
        // track's pitch and sample, which differ from the note's when a bend or vibrato wiped out the start of the
        // track's next note. The voice then plays on with the old sample.
        const bool pitch = info_.revision == Revision::kEternalDuelist ? o.flags != 0 : (o.flags & kOutPitch);
        if (pitch)
        {
            Event e{Event::kBend, f};
            e.dev = o.pitch - 32 * note_ + StepDeviation(o.key, key_);
            out.push_back(e);
        }

        const bool every_frame = info_.revision == Revision::kWct2004;
        if (every_frame || o.trig)
        {
            ChangeLevels(f, lv, out);
        }
    }

    // Returns how much higher sample `key` plays than sample `playing` at the same pitch, in 1/32 semitones.
    int StepDeviation(int key, int playing) const
    {
        const SampleInfo s = info_.Sample(rom_, key);
        const SampleInfo p = info_.Sample(rom_, playing);
        if (key == playing || !s.valid || !p.valid || s.step == p.step || !s.step || !p.step)
        {
            return 0;
        }

        return int(std::lround(384.0 * std::log2(double(s.step) / p.step)));
    }

    bool left_ = true, right_ = true;
    int level_ = 0;
    int row_ = -1;   // the wave-table row of the wave note that's playing
    int noise_ = -1; // the NR43 value of the noise note that's playing
    int echo_ = 0;
};

// Turns one track's per-frame driver output into note, level and bend events in the Dungeon Dice Monsters revision,
// following what its output stage does with it.
class DungeonDiceRenderer : public NoteWriter
{
public:
    DungeonDiceRenderer(const Rom& rom, const DriverInfo& info, int track) : NoteWriter(rom, info, track)
    {
    }

    // Adds the events of frame `f` to `out`, from the track's output record and the frame's other settings.
    void Frame(uint32_t f, const TrackOutput& o, const FrameState& state, std::vector<Event>& out,
               std::vector<std::string>& warnings)
    {
        switch (t_)
        {
        case 0:
        case 1:
            Square(f, o, state, out);
            break;
        case 2:
            Wave(f, o, state, out);
            break;
        case 3:
            Noise(f, o, state, out);
            break;
        default:
            Sample(f, o, out, warnings);
            break;
        }
    }

private:
    // Returns the levels of a PSG channel at `level` on each side, scaled by the PSG's volume on that side, which the
    // Dungeon Dice Monsters revision sets with NR50.
    static Levels ScaledLevels(double level, const FrameState& state)
    {
        return {level * (((state.psg_volume >> 4) & 7) + 1) / 8, level * ((state.psg_volume & 7) + 1) / 8};
    }

    // Returns the Dungeon Dice Monsters revision's PSG frequency table entry `pitch`: the frequency register's value,
    // with bit 15 set to restart the channel, or a noise setting.
    uint16_t FrequencyEntry(int pitch) const
    {
        return TableEntry(rom_, info_.psg_freq_table, pitch).value_or(0);
    }

    // Plays the note that the Dungeon Dice Monsters revision's square, wave or noise channel sounds at the end of frame
    // `f`: `sound` (a duty, wave row or noise note) at frequency register `x`, from a note command's write or a restart
    // if `new_note`, at levels `lv`, or nothing if `lv` is 0 on both sides. A note goes on while the channel plays the
    // same sound, and a new one starts where the sound changes, since MIDI can only change it with a new note.
    void Play(uint32_t f, Kind kind, int sound, int x, bool new_note, Levels lv, std::vector<Event>& out)
    {
        const int pitch = kind == Kind::kNoise ? 0 : RegisterPitch(x);
        if (lv.left <= 0 && lv.right <= 0)
        {
            NoteOff(f, out);
        }
        else if (!on_ || new_note || sound != sound_)
        {
            NoteOff(f, out);
            note_ = (pitch + 16) >> 5;
            sound_ = sound;
            NoteOn(f, kind, sound, kind == Kind::kNoise ? 0 : note_, pitch - 32 * note_, lv, out);
        }
        else
        {
            Update(f, pitch - 32 * note_, lv, kind != Kind::kNoise, out);
        }
    }

    // Mirrors the Dungeon Dice Monsters revision's square channel output. Every write restarts the channel when the
    // frequency table entry has bit 15 set, as all the table's entries do, but only a note command's write starts a new
    // note. The others, such as vibrato's, change the note that's playing. The envelope that FB gives a square channel
    // is left out.
    void Square(uint32_t f, const TrackOutput& o, const FrameState& state, std::vector<Event>& out)
    {
        bool new_note = false;
        if (o.Any())
        {
            const uint16_t entry = FrequencyEntry(o.pitch);
            const bool restart = entry & 0x8000;
            level_ = o.vol & 15;
            register_ = entry & 0x7FF;
            duty_ = o.b2 >> 6;
            sounding_ = level_ && (sounding_ || restart);
            new_note = restart && o.trig && (o.flags & kOutDungeonDiceNote);
        }

        const double level = sounding_ ? double(kPsgLevelPerVolume * level_) : 0;
        Play(f, Kind::kSquare, duty_, register_, new_note, ScaledLevels(level, state), out);
    }

    // Mirrors the Dungeon Dice Monsters revision's wave channel output, which writes the channel's frequency without
    // restarting it, and its volume code from the wave volume table. A wave command loads the wave RAM at once, which
    // restarts the channel at frequency 0 until the next write.
    void Wave(uint32_t f, const TrackOutput& o, const FrameState& state, std::vector<Event>& out)
    {
        bool new_note = false;
        if (state.wave_loads != wave_loads_)
        {
            wave_loads_ = state.wave_loads;
            register_ = 0;
            new_note = true;
        }

        if (o.Any())
        {
            const uint16_t nr32 = TableEntry(rom_, info_.wave_volume_table, o.vol).value_or(o.vol ? 0x2000 : 0);
            wave_share_ = WaveShare(nr32);
            register_ = FrequencyEntry(o.pitch) & 0x7FF;
            new_note |= o.trig && (o.flags & kOutDungeonDiceNote);
        }

        const int row = (state.wave << 4) | 15;
        Play(f, Kind::kWave, row, register_, new_note, ScaledLevels(kWaveRowLevel * wave_share_, state), out);
    }

    // Mirrors the Dungeon Dice Monsters revision's noise channel output. A write restarts the channel when the noise
    // setting has bit 15 set, and a restart is a new note. The noise notes are entries of the frequency table from note
    // 72's on.
    void Noise(uint32_t f, const TrackOutput& o, const FrameState& state, std::vector<Event>& out)
    {
        bool new_note = false;
        if (o.Any())
        {
            noise_note_ = o.pitch - 16 * kDungeonDiceNoiseNote;
            const uint16_t setting = info_.NoiseSetting(rom_, noise_note_);
            const bool restart = setting & 0x8000;
            level_ = o.vol & 15;
            sounding_ = level_ && (sounding_ || restart);
            new_note = restart || (setting & 0xFF) != noise_;
            noise_ = setting & 0xFF;
        }

        const double level = sounding_ ? double(kPsgLevelPerVolume * level_) : 0;
        Play(f, Kind::kNoise, noise_note_, 0, new_note, ScaledLevels(level, state), out);
    }

    // Returns the pitch offset of `period` from the sample's native period `own`, in 1/32 semitones. A timer period of
    // 0 means 65536 cycles.
    static int PeriodPitch(uint32_t own, uint32_t period)
    {
        return int(std::lround(384.0 * std::log2(double(own) / (period ? period : 0x10000))));
    }

    // Mirrors the Dungeon Dice Monsters voice update. Every frame sets the voice's level from its track's volume. A
    // flagged record starts a note at the sample's rate or the rate from the sample period table; volume 0 stops the
    // voice. kOutDungeonDicePeriod instead treats the record's pitch as a timer period and changes the playing note's
    // rate. On hardware this also changes the other voice sharing the FIFO. agbsf2 doesn't model that change.
    void Sample(uint32_t f, const TrackOutput& o, std::vector<Event>& out, std::vector<std::string>& warnings)
    {
        const double level = info_.VoiceLevel(rom_, o.vol, 0);
        const Levels lv{level, level};
        if (o.flags & kOutDungeonDicePeriod)
        {
            if (on_)
            {
                Event e{Event::kBend, f};
                e.dev = PeriodPitch(info_.Sample(rom_, key_).period, uint16_t(o.pitch)) - 32 * note_;
                out.push_back(e);
            }
        }
        else if (o.flags)
        {
            NoteOff(f, out);
            const SampleInfo s = info_.Sample(rom_, o.b2);
            const uint32_t period = TableEntry(rom_, info_.sample_period_table, o.pitch).value_or(0);
            if (o.vol == 0)
            {
                return;
            }

            if (s.valid)
            {
                const int pitch = period ? PeriodPitch(s.period, period) : 0;
                note_ = (pitch + 16) >> 5;
                key_ = o.b2;
                levels_ = lv;
                NoteOn(f, Kind::kSample, o.b2, note_, pitch - 32 * note_, lv, out);
            }
            else
            {
                SkipInvalidSample(o.b2, warnings);
            }

            return;
        }

        if (on_)
        {
            ChangeLevels(f, lv, out);
        }
    }

    int level_ = 0;
    int noise_ = -1; // the NR43 value of the channel's last noise setting

    // PSG channel state and current sound after the Dungeon Dice Monsters driver's register writes.
    bool sounding_ = false;
    int register_ = 0;      // the frequency register
    int duty_ = 2;          // a square channel's duty
    double wave_share_ = 0; // the share of its output that the wave channel plays at
    int wave_loads_ = 0;    // FrameState::wave_loads, as the channel last saw it
    int noise_note_ = 0;    // the noise note, which gives the noise setting
    int sound_ = -1;        // the duty, wave row or noise note of the note that's playing
};

// Preset and key assigned to a note on a sample or noise track.
struct Placement
{
    int preset = 0; // index into the preset list
    int key = 60;
};

// A key range of a preset, and the sample it plays.
struct KeyZone
{
    int sample = -1; // SF2 sample
    int lo = 0, hi = 127;
    int root = 60;
    bool loop = false;
    bool melodic = false;
};

// Key layout of one DS track: the preset and key that play each (sample, semitone).
struct TrackKeymap
{
    std::vector<std::vector<KeyZone>> presets;
    std::map<std::pair<int, int>, Placement> notes;
};

// Finds the first free key at or above `want`, then searches below it. Returns -1 if all 128 keys are taken.
int FreeKey(const std::set<int>& taken, int want)
{
    for (int key = std::max(want, 0); key <= 127; key++)
    {
        if (!taken.count(key))
        {
            return key;
        }
    }

    for (int key = std::min(want, 128) - 1; key >= 0; key--)
    {
        if (!taken.count(key))
        {
            return key;
        }
    }

    return -1;
}

// Returns true if no zone covers any of the keys lo to hi.
bool RangeFree(const std::vector<KeyZone>& zones, int lo, int hi)
{
    for (const KeyZone& z : zones)
    {
        if (z.lo <= hi && lo <= z.hi)
        {
            return false;
        }
    }

    return true;
}

// Widens a preset's melodic zones over the free keys next to them, so the preset is playable: the keys between two
// zones are split halfway, and the outer zones grow by up to an octave.
void WidenMelodicZones(std::vector<KeyZone>& zones)
{
    std::vector<KeyZone*> mel;
    for (KeyZone& z : zones)
    {
        if (z.melodic)
        {
            mel.push_back(&z);
        }
    }
    std::sort(mel.begin(), mel.end(), [](const KeyZone* a, const KeyZone* b) { return a->lo < b->lo; });

    // Split the keys between two zones halfway, measured from where the lower zone ended before it grew.
    int prev_hi = 0;
    for (size_t i = 0; i < mel.size(); i++)
    {
        KeyZone& z = *mel[i];
        const int floor_key = i == 0 ? std::max(0, z.lo - 12) : (prev_hi + z.lo) / 2 + 1;
        const int ceil_key = i + 1 == mel.size() ? std::min(127, z.hi + 12) : (z.hi + mel[i + 1]->lo) / 2;
        prev_hi = z.hi;

        while (z.lo - 1 >= floor_key && RangeFree(zones, z.lo - 1, z.lo - 1))
        {
            z.lo--;
        }

        while (z.hi + 1 <= ceil_key && RangeFree(zones, z.hi + 1, z.hi + 1))
        {
            z.hi++;
        }
    }
}

// Lays out the samples a DS track plays on the keys of its presets. `usage` gives each sample the semitones it's played
// at.
TrackKeymap BuildKeymap(const std::map<int, std::set<int>>& usage, SoundfontBuilder& sf, const Rom& rom,
                        const DriverInfo& info)
{
    TrackKeymap km;

    // Samples played at several pitches are laid out at their true pitch; ones played at a single pitch (drum style) go
    // wherever there's room.
    std::vector<int> melodic, single;
    for (const auto& [s, semis] : usage)
    {
        (semis.size() > 1 ? melodic : single).push_back(s);
    }

    // The sort is stable so that samples starting on the same key stay in sample order on every platform. std::sort
    // leaves the order of ties to the standard library.
    auto lowest_note_first = [&](int a, int b)
    {
        return sf.DsRootKey(a) + *usage.at(a).begin() < sf.DsRootKey(b) + *usage.at(b).begin();
    };
    std::stable_sort(melodic.begin(), melodic.end(), lowest_note_first);

    // Puts sample s on keys lo to hi of the first preset that has them free, and returns the preset.
    auto place = [&](int s, int lo, int hi, int root, bool is_melodic)
    {
        for (size_t p = 0;; p++)
        {
            if (p == km.presets.size())
            {
                km.presets.emplace_back();
            }

            if (!RangeFree(km.presets[p], lo, hi))
            {
                continue;
            }

            KeyZone z;
            z.sample = sf.DsSample(s);
            z.lo = lo;
            z.hi = hi;
            z.root = root;
            z.loop = info.Sample(rom, s).Looped();
            z.melodic = is_melodic;
            km.presets[p].push_back(z);
            return int(p);
        }
    };

    for (int s : melodic)
    {
        const std::set<int>& semis = usage.at(s);
        int root = sf.DsRootKey(s);
        while (root + *semis.rbegin() > 127 && root >= 12)
        {
            root -= 12;
        }
        while (root + *semis.begin() < 0 && root <= 115)
        {
            root += 12;
        }

        const int lo = std::clamp(root + *semis.begin(), 0, 127);
        const int hi = std::clamp(root + *semis.rbegin(), 0, 127);
        const int p = place(s, lo, hi, root, true);
        for (int k : semis)
        {
            km.notes[{s, k}] = {p, std::clamp(root + k, 0, 127)};
        }
    }

    for (int s : single)
    {
        const int k = *usage.at(s).begin();

        // Prefer the sample's true pitch, then the first free key from C2 up. The zone's root key, k below the key, has
        // to be a MIDI key too, which leaves the keys from k to k + 127. That's none for k = -128, whose notes play on
        // key 0 with root key 127, a semitone high.
        const int lowest = std::clamp(k, 0, 127);
        const int highest = std::clamp(127 + k, 0, 127);
        std::vector<int> candidates;
        if (sf.DsPitched(s))
        {
            candidates.push_back(sf.DsRootKey(s) + k);
        }
        for (int key = 36; key < 128; key++)
        {
            candidates.push_back(key);
        }
        for (int key = 35; key >= 0; key--)
        {
            candidates.push_back(key);
        }

        Placement where{-1, 0};
        for (size_t p = 0; p <= km.presets.size() && where.preset < 0; p++)
        {
            if (p == km.presets.size())
            {
                km.presets.emplace_back();
            }

            for (int key : candidates)
            {
                if (key < lowest || key > highest || !RangeFree(km.presets[p], key, key))
                {
                    continue;
                }

                where = {int(p), key};
                break;
            }
        }

        KeyZone z;
        z.sample = sf.DsSample(s);
        z.lo = z.hi = where.key;
        z.root = std::min(where.key - k, 127);
        z.loop = info.Sample(rom, s).Looped();
        km.presets[size_t(where.preset)].push_back(z);
        km.notes[{s, k}] = where;
    }

    for (std::vector<KeyZone>& zones : km.presets)
    {
        WidenMelodicZones(zones);
    }

    return km;
}

// The older revisions' PSG output in one frame: the record that their output stage hands each PSG channel, and the
// sides each channel plays on.
struct Wct2004Psg
{
    std::array<TrackOutput, kPsgTracks> records;
    std::array<bool, kPsgTracks> left{}, right{};
};

// Returns the older revisions' PSG output in every frame of `sim`. A PSG track panned to anything but the centre or one
// side plays its right side on its own channel, and its left side on the next channel, which plays a copy of the
// track's output instead of its own track's. The pan's two levels scale the volume of each side. In the Eternal Duelist
// revision, a PSG track's pan is a set of NR51 bits, and NR51 is the OR of the four tracks' bits.
std::vector<Wct2004Psg> Wct2004PsgOutput(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    auto centre_or_one_side = [](uint8_t pan)
    {
        return pan == 0xFF || pan == 0x0F || pan == 0xF0;
    };

    std::vector<Wct2004Psg> frames(sim.frames.size());
    bool wave_on = false;
    for (size_t f = 0; f < sim.frames.size(); f++)
    {
        Wct2004Psg& p = frames[f];
        std::copy_n(sim.frames[f].begin(), kPsgTracks, p.records.begin());

        // NR51 has a bit for each channel on each side, right in the low nibble and left in the high one. The WCT 2004
        // and Rave Master revisions work it out from channel 4 down.
        uint8_t nr51 = 0;
        if (info.revision == Revision::kEternalDuelist)
        {
            for (const TrackOutput& r : p.records)
            {
                nr51 |= r.pan;
            }
        }
        else
        {
            for (int t = kPsgTracks - 1; t >= 0; t--)
            {
                TrackOutput& r = p.records[t];

                // When square 1 goes back to the centre or one side, and square 2's track plays nothing this frame,
                // square 2 takes square 1's record at volume 0: a retrigger silences it, and a pitch change moves it to
                // square 1's pitch.
                if (t == 0 && !p.records[1].Any() && centre_or_one_side(r.pan) && !centre_or_one_side(r.pan_start))
                {
                    p.records[1] = r;
                    p.records[1].vol = 0;
                }

                nr51 = uint8_t((nr51 << 1) | 0x11);
                if (r.pan == 0xF0)
                {
                    nr51 &= 0xFE;
                }
                else if (r.pan == 0x0F)
                {
                    nr51 &= 0xEF;
                }
                else if (r.pan != 0xFF)
                {
                    // A side's volume comes from the voices' volume table.
                    const int vol = r.vol;
                    if (t + 1 < kPsgTracks)
                    {
                        p.records[t + 1] = r;
                        p.records[t + 1].vol = uint8_t(info.TableLevel(rom, vol, r.pan >> 4));
                    }

                    r.vol = uint8_t(info.TableLevel(rom, vol, r.pan & 15));
                    nr51 = uint8_t((nr51 & 0xCC) | 0x21);
                }
            }

            // The wave channel is left out until it first plays a note at a volume above 0.
            wave_on = wave_on || (p.records[2].trig && p.records[2].vol);
            if (!wave_on)
            {
                nr51 &= 0xBB;
            }
        }

        for (int c = 0; c < kPsgTracks; c++)
        {
            p.right[size_t(c)] = (nr51 >> c) & 1;
            p.left[size_t(c)] = (nr51 >> (c + 4)) & 1;
        }
    }

    return frames;
}

// Turns every track's driver output into events, and ends the notes still playing where the song ends.
std::array<std::vector<Event>, kTracks> RenderTracks(const Rom& rom, const DriverInfo& info, const Simulation& sim,
                                                     std::vector<std::string>& warnings)
{
    std::array<std::vector<Event>, kTracks> events;
    const uint32_t end_frame = uint32_t(sim.frames.size());

    if (info.revision == Revision::kDungeonDiceMonsters)
    {
        for (int t = 0; t < kTracks; t++)
        {
            DungeonDiceRenderer r(rom, info, t);
            for (uint32_t f = 0; f < end_frame; f++)
            {
                r.Frame(f, sim.frames[f][t], sim.state[f], events[t], warnings);
            }
            r.NoteOff(end_frame, events[t]);
        }

        return events;
    }

    const bool older = OlderOutputStage(info.revision);
    std::vector<Wct2004Psg> psg;
    if (older)
    {
        psg = Wct2004PsgOutput(rom, info, sim);
    }

    for (int t = 0; t < kTracks; t++)
    {
        TrackRenderer r(rom, info, t);
        for (uint32_t f = 0; f < end_frame; f++)
        {
            if (older && IsPsgTrack(t))
            {
                const Wct2004Psg& p = psg[f];
                r.PsgFrame(f, p.records[size_t(t)], p.left[size_t(t)], p.right[size_t(t)], events[t]);
            }
            else
            {
                r.Frame(f, sim.frames[f][t], sim.state[f], events[t], warnings);
            }
        }
        r.NoteOff(end_frame, events[t]);
    }

    return events;
}

// Returns true if every track's notes of the song's second pass through its loop start as those of its first do: with
// the same gaps between them, the same sound, note and pitch, and the levels and echo send that the MIDI file gives
// them. The driver sends every track back to its loop point with the volume, pan and sample that the loop's end left,
// so the first pass can play unlike the later ones. Each track's notes are timed from its first note in the pass, since
// a track's loop point can come a frame after the loop's start, where every track starts the next pass together. `sim`
// has to hold two passes of the loop.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    std::vector<std::string> warnings;
    const std::array<std::vector<Event>, kTracks> events = RenderTracks(rom, info, sim, warnings);
    const uint32_t start = uint32_t(sim.loop_start), length = uint32_t(sim.loop_end - sim.loop_start);
    PassComparison passes(start, length);
    for (int t = 0; t < kTracks; t++)
    {
        // The frame of the track's first note in each pass.
        std::array<uint32_t, 2> first = {UINT32_MAX, UINT32_MAX};
        for (const Event& e : events[size_t(t)])
        {
            if (e.type == Event::kNoteOn && e.frame >= start && e.frame - start < 2 * length)
            {
                uint32_t& f = first[(e.frame - start) / length];
                f = std::min(f, e.frame);
            }
        }

        int echo = 0;
        for (const Event& e : events[size_t(t)])
        {
            if (e.type == Event::kEcho)
            {
                echo = e.echo;
            }
            else if (e.type == Event::kNoteOn && e.frame >= start && e.frame - start < 2 * length)
            {
                const uint32_t pass = (e.frame - start) / length;
                int cc10 = 64, cc11 = 0;
                LevelsToControllers(e.level_l, e.level_r, cc10, cc11);
                passes.Add(
                    start + pass * length + (e.frame - first[pass]), t, e.note,
                    {double(int(e.kind)), double(e.param), double(e.dev), double(cc10), double(cc11), double(echo)});
            }
        }
    }

    return passes.Alike();
}

// Sound sources, pitches and timing used to build the song's SoundFont and MIDI file.
struct Usage
{
    std::array<bool, kTracks> used{};                          // tracks that play notes
    std::array<int, kTracks> max_dev{};                        // largest pitch deviation, 1/32 semitones
    std::array<std::map<int, std::set<int>>, kTracks> samples; // DS sample -> the semitones it's played at
    std::set<int> duties, waves, noise_notes;
};

// Collects sound usage for the tracks selected by `track_mask`.
Usage CollectUsage(const std::array<std::vector<Event>, kTracks>& events, uint32_t track_mask)
{
    Usage u;
    for (int t = 0; t < kTracks; t++)
    {
        if (!((track_mask >> t) & 1))
        {
            continue;
        }

        for (const Event& e : events[t])
        {
            if (e.type == Event::kNoteOn)
            {
                u.used[size_t(t)] = true;

                switch (e.kind)
                {
                case Kind::kSquare:
                    u.duties.insert(e.param);
                    break;
                case Kind::kWave:
                    u.waves.insert(e.param);
                    break;
                case Kind::kNoise:
                    u.noise_notes.insert(e.param);
                    break;
                case Kind::kSample:
                    u.samples[size_t(t)][e.param].insert(e.note);
                    break;
                }
            }

            if ((e.type == Event::kNoteOn && e.kind != Kind::kNoise) || e.type == Event::kBend)
            {
                u.max_dev[size_t(t)] = std::max(u.max_dev[size_t(t)], std::abs(e.dev));
            }
        }
    }

    return u;
}

// The locations of a song's instruments in the SoundFont.
struct Programs
{
    std::map<int, BankProgram> duty;     // by duty cycle
    std::map<int, BankProgram> wave;     // by wave row, wave << 4 | volume
    std::vector<BankProgram> noise;      // the noise presets
    std::map<int, Placement> noise_keys; // noise note -> its preset and key
    std::array<TrackKeymap, kTracks> keymaps;
    std::array<std::vector<BankProgram>, kTracks> tracks; // the presets of each DS track
    bool fits = true;                                     // every preset found a free program
};

// Adds a preset for each instrument the song uses, in the banks and programs that the SoundFont gives song `song`, and
// with `prefix` in front of its name.
Programs AddPresets(const Usage& usage, SoundfontBuilder& sf, const Rom& rom, const DriverInfo& info, int song,
                    const std::string& prefix)
{
    Programs programs;
    Sf2File& file = sf.File();
    sf.Banks().Begin(song);

    auto add_preset = [&](const std::string& name, std::vector<sf2::Zone> zones)
    {
        file.instruments.push_back(MakeInstrument(prefix + name, std::move(zones)));

        const std::optional<BankProgram> slot = sf.Banks().Next();
        programs.fits = programs.fits && slot;
        const sf2::Preset p = MakePreset(prefix + name, uint16_t(slot ? slot->bank : 0),
                                         uint16_t(slot ? slot->program : 0), int(file.instruments.size()) - 1);
        file.presets.push_back(p);

        return BankProgram{p.bank, p.program};
    };

    // The square and wave zones follow the driver's frequency table.
    for (int d : usage.duties)
    {
        const int s = sf.SquareSample(d);
        programs.duty[d] = add_preset(SquareName(d), TunedZones(RootKeyZone(s, true, 0, 127, 60), sf.SquareTuning()));
    }

    for (int row : usage.waves)
    {
        const int w = row >> 4, vol = row & 15;
        const int s = sf.WaveSample(w, vol);
        const int root = std::clamp(sf.WaveKey(w, 36), 0, 127);
        programs.wave[row] =
            add_preset(WaveName(w, vol), TunedZones(RootKeyZone(s, true, 0, 127, root), sf.WaveTuning(w)));
    }

    // Each noise note gets a key of its own: the one 36 above its number, or failing that the nearest free key above
    // it, then below it. A song with more noise notes than there are keys goes on to a second preset.
    std::vector<std::vector<sf2::Zone>> noise_zones;
    std::vector<std::set<int>> noise_taken;
    for (int n : usage.noise_notes)
    {
        Placement where{-1, 0};
        for (size_t p = 0; where.preset < 0; p++)
        {
            if (p == noise_taken.size())
            {
                noise_zones.emplace_back();
                noise_taken.emplace_back();
            }

            const int key = FreeKey(noise_taken[p], 36 + n);
            if (key >= 0)
            {
                where = {int(p), key};
            }
        }

        noise_taken[size_t(where.preset)].insert(where.key);
        programs.noise_keys[n] = where;
        const int sample = sf.NoiseSample(info.NoiseSetting(rom, n));
        noise_zones[size_t(where.preset)].push_back(RootKeyZone(sample, true, where.key, where.key, where.key));
    }

    for (size_t p = 0; p < noise_zones.size(); p++)
    {
        const std::string name = noise_zones.size() > 1 ? "Noise (" + std::to_string(p + 1) + ")" : "Noise";
        programs.noise.push_back(add_preset(name, std::move(noise_zones[p])));
    }

    for (int t = kPsgTracks; t < kTracks; t++)
    {
        if (usage.samples[size_t(t)].empty())
        {
            continue;
        }

        programs.keymaps[size_t(t)] = BuildKeymap(usage.samples[size_t(t)], sf, rom, info);
        const auto& presets = programs.keymaps[size_t(t)].presets;
        for (size_t p = 0; p < presets.size(); p++)
        {
            std::vector<sf2::Zone> zones;
            for (const KeyZone& z : presets[p])
            {
                zones.push_back(RootKeyZone(z.sample, z.loop, z.lo, z.hi, z.root));
            }

            std::string name = TrackName(t);
            if (presets.size() > 1)
            {
                name += " (" + std::to_string(p + 1) + ")";
            }

            programs.tracks[size_t(t)].push_back(add_preset(name, std::move(zones)));
        }
    }

    return programs;
}

// Preset and MIDI key for a note.
struct NoteSound
{
    BankProgram program;
    int key = 0;
};

// Returns the preset and key that play track t's note-on event `e`.
NoteSound Resolve(const Programs& programs, const SoundfontBuilder& sf, int t, const Event& e)
{
    NoteSound s;
    switch (e.kind)
    {
    case Kind::kSquare:
        s = {programs.duty.at(e.param), sf.SquareBaseKey() + e.note};
        break;

    case Kind::kWave:
        s = {programs.wave.at(e.param), sf.WaveKey(e.param >> 4, e.note)};
        break;

    case Kind::kNoise:
        {
            const Placement& pl = programs.noise_keys.at(e.param);
            s = {programs.noise[size_t(pl.preset)], pl.key};
            break;
        }

    case Kind::kSample:
        {
            const Placement& pl = programs.keymaps[size_t(t)].notes.at({e.param, e.note});
            s = {programs.tracks[size_t(t)][size_t(pl.preset)], pl.key};
            break;
        }
    }

    s.key = std::clamp(s.key, 0, 127);

    return s;
}

// Returns a MIDI channel for each track that plays notes, and -1 for the others. Channel 10 (index 9), which players
// often reserve for drums, comes last.
std::array<int, kTracks> AssignChannels(const std::array<bool, kTracks>& used)
{
    constexpr int kOrder[kTracks] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 9};
    std::array<int, kTracks> channel;
    channel.fill(-1);
    int next = 0;
    for (int t = 0; t < kTracks; t++)
    {
        if (used[size_t(t)])
        {
            channel[size_t(t)] = kOrder[next++];
        }
    }

    return channel;
}

// Writes the conductor track: the title and the tempo map, the game and song it's from, its loop, and the driver's echo
// settings, which MIDI can't express. `ticks` gives the grid's tempos and the ticks of frames. The track's events at
// tick 0 come in another order than WriteBeatConductor()'s. That function would change the files.
void WriteConductor(MidiTrack& conductor, const std::string& title, const BeatTicks& ticks, const Rom& rom,
                    const DriverInfo& info, int song, const Simulation& sim)
{
    conductor.Name(title);
    for (const BeatGrid::Tempo& t : ticks.Grid().Tempos())
    {
        conductor.Tempo(QuarterTick(t.quarter), uint32_t(std::lround(FrameSeconds(t.frames) * 1e6)));
    }
    conductor.TimeSignature(0, 4, 2);

    SongHeader h;
    ReadSongHeader(rom, info.song_table, song, info.revision, h);
    char text[160];
    std::snprintf(text, sizeof text, "%s (%s) song %d, sequence data at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, h.base, kProgramName);
    conductor.Meta(0, 0x01, text);

    // The beat's tempo at the loop's start is given again after the marker. The loop's end can have another, which a
    // player that keeps its tempo when it jumps back would otherwise play the loop's start at.
    if (sim.loop_end >= 0)
    {
        conductor.Meta(ticks.Marker(sim.loop_start), 0x06, "loopStart");
        conductor.Meta(ticks.Marker(sim.loop_end), 0x06, "loopEnd");
        conductor.RepeatTempo(ticks.Marker(sim.loop_start));
    }

    int last_fb = -1, last_delay = -1;
    for (uint32_t f = 0; f < sim.state.size(); f++)
    {
        const FrameState& e = sim.state[f];
        if (e.feedback == last_fb && e.delay == last_delay)
        {
            continue;
        }

        if (e.feedback || last_fb > 0)
        {
            std::snprintf(text, sizeof text, "echo: delay %.0f ms, feedback %d/256 (sent as CC91 reverb)",
                          e.delay * 32 * 1000.0 / info.mix_rate, e.feedback);
            conductor.Meta(ticks.Song(f), 0x01, text);
        }

        last_fb = e.feedback;
        last_delay = e.delay;
    }

    conductor.SetEnd(ticks.Song(double(sim.frames.size())));
}

// Writes one track's events on MIDI channel `ch`, sending program changes and controllers where they change, and where
// each first comes from `loop`, the loop's start, on. `sound_of` gives the preset and key of a note-on event, `range`
// is the pitch bend range in semitones, and `tick_of` gives a frame's tick on the track.
void WriteTrack(MidiTrack& mt, int ch, int range, const std::vector<Event>& events, std::optional<uint32_t> loop,
                const std::function<NoteSound(const Event&)>& sound_of, const std::function<uint32_t(double)>& tick_of)
{
    // The first note's preset is selected before anything plays, and the controllers start reset.
    BankProgram program;
    for (const Event& e : events)
    {
        if (e.type == Event::kNoteOn)
        {
            program = sound_of(e).program;
            break;
        }
    }
    mt.Bank(0, ch, program.bank);
    mt.Program(0, ch, program.program);
    mt.Control(0, ch, cc::kVolume, 127);
    mt.Control(0, ch, cc::kPan, 64);
    mt.Control(0, ch, cc::kExpression, 0);
    mt.Control(0, ch, cc::kReverb, 0);
    mt.Control(0, ch, cc::kChorus, 0);

    mt.BendRange(0, ch, range);
    mt.PitchBend(0, ch, 8192);

    // The channel's settings, the echo that the track's events last gave, and the key that's playing (-1 if none).
    int cc10 = 64, cc11 = 0, cc91 = 0, bend = 8192, echo = 0, key = -1;

    auto set_levels = [&](uint32_t tick, double l, double r)
    {
        int n10 = cc10, n11 = cc11;
        LevelsToControllers(l, r, n10, n11);
        if (n10 != cc10)
        {
            mt.Control(tick, ch, cc::kPan, cc10 = n10);
        }
        if (n11 != cc11)
        {
            mt.Control(tick, ch, cc::kExpression, cc11 = n11);
        }
    };

    auto set_bend = [&](uint32_t tick, int dev)
    {
        const int v = FixedBendValue(dev, 32, range);
        if (v != bend)
        {
            mt.PitchBend(tick, ch, bend = v);
        }
    };

    for (const Event& e : events)
    {
        const uint32_t tick = tick_of(e.frame);

        // A player that jumps back to the loop's start keeps the settings that the loop's end left, so they're all
        // written again from there.
        if (loop && tick >= *loop)
        {
            loop.reset();
            program = {-1, -1};
            cc10 = cc11 = cc91 = bend = -1;
        }

        switch (e.type)
        {
        case Event::kNoteOff:
            if (key >= 0)
            {
                mt.NoteOff(tick, ch, key);
            }
            key = -1;
            break;

        case Event::kNoteOn:
            {
                if (key >= 0)
                {
                    mt.NoteOff(tick, ch, key);
                }

                const NoteSound sound = sound_of(e);
                if (sound.program.bank != program.bank)
                {
                    mt.Bank(tick, ch, sound.program.bank);
                    program = {sound.program.bank, -1};
                }
                if (sound.program.program != program.program)
                {
                    mt.Program(tick, ch, sound.program.program);
                    program.program = sound.program.program;
                }

                set_levels(tick, e.level_l, e.level_r);
                set_bend(tick, e.dev);
                if (echo != cc91)
                {
                    mt.Control(tick, ch, cc::kReverb, cc91 = echo);
                }
                mt.NoteOn(tick, ch, sound.key, kVelocity);
                key = sound.key;
                break;
            }

        case Event::kLevels:
            set_levels(tick, e.level_l, e.level_r);
            break;

        case Event::kBend:
            set_bend(tick, e.dev);
            break;

        case Event::kEcho:
            echo = e.echo;
            if (echo != cc91)
            {
                mt.Control(tick, ch, cc::kReverb, cc91 = echo);
            }
            break;
        }
    }
}

// Simulates the song for its conversion. The marked loop starts at the song's second pass if the first pass plays
// unlike the second. That happens, for example, when the tracks come back to their loop points with the volume or pan
// that the loop's end left them. A delay at the start of a loop point plays only in the later passes. It moves its
// track's notes there. The comparison of passes times each track's notes from its first note and misses the move.
Simulation MarkedSimulation(const Rom& rom, const DriverInfo& info, int song, int loops)
{
    Simulation sim = Simulate(rom, info, song, loops, LoopStart::kFirstPass);
    if (sim.loop_end >= 0 &&
        (sim.delayed_loop_point || !FirstPassRepeats(rom, info, Simulate(rom, info, song, 2, LoopStart::kFirstPass))))
    {
        sim = Simulate(rom, info, song, loops, LoopStart::kSecondPass);
    }

    return sim;
}

// Copies the presets that track `t` plays in `events` into the drum bank. Some players force channel 10 to that bank.
void CopyDrumPresets(int t, const std::vector<Event>& events, const Programs& programs, SoundfontBuilder& sf)
{
    std::set<std::pair<int, int>> used; // (bank, program) of each of the track's presets
    for (const Event& e : events)
    {
        if (e.type == Event::kNoteOn)
        {
            const BankProgram p = Resolve(programs, sf, t, e).program;
            used.insert({p.bank, p.program});
        }
    }

    std::vector<sf2::Preset>& presets = sf.File().presets;
    const size_t count = presets.size();
    for (size_t i = 0; i < count; i++)
    {
        if (used.count({presets[i].bank, presets[i].program}))
        {
            sf2::Preset copy = presets[i];
            copy.bank = kDrumBank;
            presets.push_back(copy);
        }
    }
}

// Returns the song's beat. The beat comes from the song's start and from the frames on which every track's notes start,
// whichever tracks the conversion includes. Conversions of different tracks then line up. Each pass through the loop
// starts again on the beat. The song ends at frame `end_frame`.
BeatGrid SongGrid(const std::array<std::vector<Event>, kTracks>& events, const Simulation& sim, uint32_t end_frame)
{
    std::vector<std::vector<uint32_t>> starts(kTracks);
    for (int t = 0; t < kTracks; t++)
    {
        for (const Event& e : events[size_t(t)])
        {
            if (e.type == Event::kNoteOn)
            {
                starts[size_t(t)].push_back(e.frame);
            }
        }
        if (!starts[size_t(t)].empty())
        {
            starts[size_t(t)].insert(starts[size_t(t)].begin(), 0);
        }
    }

    std::vector<uint32_t> hints;
    const int length = sim.loop_end - sim.loop_start;
    for (int f = sim.first_loop_end; length > 0 && f < int(end_frame); f += length)
    {
        hints.push_back(uint32_t(f));
    }

    return BeatGrid(starts, hints);
}

// Writes the song's MIDI file to the folder and under the base name in `settings`. Writes the SoundFont `sf` there too,
// titled `title`, unless `sf` is null. Adds each file's path to `report`. Sets report.ok if every file is written.
// Otherwise adds the error to report.warnings.
void WriteSongFiles(const ConvertSettings& settings, const std::string& number, const std::string& title,
                    const MidiFile& midi, SoundfontBuilder* sf, SongReport& report)
{
    const std::string stem = SongStem(settings, number);
    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteFile(report.midi_path, midi.Encode(), error))
    {
        report.warnings.push_back(error);
        return;
    }

    if (sf)
    {
        Sf2File& file = sf->File();
        file.SetTitle(title);
        report.sf2_path = stem + ".sf2";
        if (!file.Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return;
        }
    }

    report.ok = true;
}

// Converts a song, writing its files if `write` is set.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write)
{
    SongReport report;
    const Simulation sim = MarkedSimulation(rom, info, song, settings.loops);
    report.warnings = sim.warnings;
    if (sim.frames.empty())
    {
        return report;
    }

    const uint32_t end_frame = uint32_t(sim.frames.size());
    report.seconds = FrameSeconds(end_frame);
    if (sim.loop_end >= 0)
    {
        report.loop_start = FrameSeconds(sim.loop_start);
        report.loop_end = FrameSeconds(sim.loop_end);
    }

    const std::array<std::vector<Event>, kTracks> events = RenderTracks(rom, info, sim, report.warnings);
    const Usage usage = CollectUsage(events, settings.track_mask);
    report.tracks = int(std::count(usage.used.begin(), usage.used.end(), true));
    report.silent = report.tracks == 0; // placeholder entries, such as a "no music" song, play no notes at all
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // With a SoundFont shared by all songs, SongBanks gives each song's presets their bank and programs.
    std::unique_ptr<SoundfontBuilder> own;
    SoundfontBuilder& sf = shared ? *shared : *(own = std::make_unique<SoundfontBuilder>(rom, info));
    const SoundfontBuilder::Checkpoint before = sf.Save();
    const std::string number = SongNumber(song, 0);
    const std::string prefix = shared ? "S" + number + " " : "";
    const Programs programs = AddPresets(usage, sf, rom, info, shared ? song : 0, prefix);
    if (!programs.fits)
    {
        const size_t preset_count = sf.File().presets.size() - before.presets;
        sf.Restore(before);
        report.warnings.push_back("needs " + std::to_string(preset_count) +
                                  " presets, more than the SoundFont's banks have free" +
                                  (shared ? "; convert the song without --single-sf2" : ""));
        return report;
    }

    // Some players force channel 10 to the drum bank, so the presets of the track on it get a copy there, to be found
    // either way. Copies of every preset would clash when there are more than 128.
    const std::array<int, kTracks> channel = AssignChannels(usage.used);
    const auto drum_channel = std::find(channel.begin(), channel.end(), kDrumChannel);
    if (drum_channel != channel.end())
    {
        if (shared)
        {
            report.warnings.push_back("a 16th channel is needed; channel 10 may play from the drum bank");
        }
        else
        {
            const int t = int(drum_channel - channel.begin());
            CopyDrumPresets(t, events[size_t(t)], programs, sf);
        }
    }

    const BeatGrid grid = SongGrid(events, sim, end_frame);
    report.bpm = 60 / FrameSeconds(grid.Tempos().front().frames);

    // Each event goes on the beat, or on the driver's frame, up to the song's end.
    const BeatTicks ticks(grid, settings.frame_timing);
    const uint32_t end_tick = ticks.Song(end_frame);

    // The loop's start, from which each track writes its settings again, unless the loop starts with the song.
    const uint32_t loop_start = sim.loop_end >= 0 ? ticks.Marker(sim.loop_start) : 0;
    const std::optional<uint32_t> loop = loop_start > 0 ? std::optional(loop_start) : std::nullopt;

    MidiFile midi(kBeatQuarterTicks);
    const std::string title = rom.Title() + " #" + number;
    WriteConductor(midi.AddTrack(), title, ticks, rom, info, song, sim);

    for (int t = 0; t < kTracks; t++)
    {
        if (!usage.used[size_t(t)])
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name(TrackName(t));

        // The range fits the track's largest bend. In the Eternal Duelist revision, a PSG note at the volume that's
        // playing only changes the channel's frequency, and a run of them can take a bend past 24 semitones.
        const int range = std::clamp((usage.max_dev[size_t(t)] + 31) / 32, 2, 127);
        auto sound_of = [&](const Event& e)
        {
            return Resolve(programs, sf, t, e);
        };
        const auto tick_of = [&](double frame)
        {
            return ticks.Channel(t, frame, end_tick);
        };
        WriteTrack(mt, channel[size_t(t)], range, events[size_t(t)], loop, sound_of, tick_of);
        mt.SetEnd(end_tick);
    }

    WriteSongFiles(settings, number, title, midi, shared ? nullptr : &sf, report);

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

bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error)
{
    SongHeader h;
    if (!ReadSongHeader(rom, info.song_table, song, info.revision, h))
    {
        error = "invalid song";
        return false;
    }

    std::string text;
    char line[256];
    std::snprintf(line, sizeof line, "; %s (%s) song %d\n; header 0x%08X, sequence base 0x%08X\n", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, info.song_table + uint32_t(song) * SongEntrySize(info.revision),
                  h.base);
    text += line;
    text += "; columns: address, frame, bytes, command, then the delay that follows it\n";

    for (int t = 0; t < h.tracks; t++)
    {
        std::snprintf(line, sizeof line, "\n; ---- track %d (%s), offset 0x%04X -> 0x%08X ----\n", t,
                      TrackName(t).c_str(), h.offsets[t], h.base + h.offsets[t]);
        text += line;

        // Each command's line is finished by the delay that follows it. The commands that a call plays come after the
        // call's line. The frames after the call count their delays.
        std::string pending;
        auto list_command = [&](const Command& c, uint32_t frame)
        {
            if (c.op == Op::kDelay)
            {
                if (pending.empty())
                {
                    std::snprintf(line, sizeof line, "0x%08X  %6u  %-14s  %-44s wait %u\n", c.addr, frame, "",
                                  "(start)", c.delay);
                    text += line;
                }
                else
                {
                    text += pending + " wait " + std::to_string(c.delay) + "\n";
                    pending.clear();
                }

                return;
            }

            const std::string bytes = HexBytes(rom, c.addr, c.length);
            std::snprintf(line, sizeof line, "0x%08X  %6u  %-14s  %-44s", c.addr, frame, bytes.c_str(),
                          Describe(c, t, info.revision).c_str());
            pending = line;
            if (c.op == Op::kEndTrack || c.op == Op::kEndTrackHard || c.op == Op::kJump || c.op == Op::kUnknown)
            {
                text += pending + "\n";
                pending.clear();
            }
        };
        WalkTrack(rom, h, t, info.revision, list_command, kMaxWalkCommands, true);

        if (!pending.empty())
        {
            text += pending + "\n";
        }
    }

    return WriteTextFile(path, text, error);
}

} // namespace agbsf2::konami
