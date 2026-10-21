// SPDX-License-Identifier: MIT

#include "quintet/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#include "quintet/driver.h"
#include "quintet/sequencer.h"
#include "quintet/song.h"
#include "quintet/soundfont.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"

namespace agbsf2::quintet
{
namespace
{

// The MIDI file's resolution: 37 ticks for each of the driver's, so that a frame at the driver's tempo T is exactly T
// ticks long.
constexpr uint32_t kMidiTicksPerTick = 37;
constexpr uint32_t kTicksPerQuarter = 96 * kMidiTicksPerTick;

// The longest conversion, in frames: about an hour.
constexpr uint32_t kMaxFrames = 60 * 60 * 60;

// The MIDI file's tempo when the model plays no frames.
constexpr int kDefaultTempo = 90;

// The slowest tempo a MIDI file can hold at this resolution: at 3 or less, a quarter note lasts more than the 2^24
// microseconds a tempo event can give it.
constexpr int kMinMidiTempo = 4;

// Each channel's MIDI track name.
constexpr const char* kChannelNames[kChannels] = {"Square 1", "Square 2", "Wave", "Noise", "PCM A", "PCM B"};

// The key of the first drum in the noise channel's kit.
constexpr int kFirstDrumKey = 36;

// The MIDI key of a square channel's pitch table index 0 (C2). The wave channel plays a pattern of 32 steps an octave
// lower, and one of 64 steps two octaves lower.
constexpr int kSquareBaseKey = 36;

// A PCM note's pitch table index that plays a sample at its own rate, and the key that does in the SoundFont.
constexpr int kPcmUnityIndex = 24;
constexpr int kPcmRootKey = 60;

// The bits of SOUNDCNT_H for each FIFO: its volume (100% when set), and its right and left outputs.
constexpr uint16_t kFifoVolume[2] = {0x0004, 0x0008};
constexpr uint16_t kFifoRight[2] = {0x0100, 0x1000};
constexpr uint16_t kFifoLeft[2] = {0x0200, 0x2000};

// The state the model leaves after a frame, for the conversion.
struct FrameData
{
    Hardware hardware;
    std::array<Fifo, 2> fifos;
    std::array<uint16_t, kChannels> tempo = {};
};

// How the MIDI file times the events.
enum class Timing
{
    kBeat,   // on the song's ticks within the frames that the driver plays them in
    kFrames, // at the start of the frames that the driver plays them in
};

// The stretch of a song that the conversion covers, in MIDI ticks.
struct Plan
{
    bool loops = false;
    uint32_t loop_start = 0; // the latest of the looping channels' loop points and the others' ends, or a loop later
    uint32_t loop_end = 0;   // the loop start plus the length LoopLength() gives for the channels' loops
    uint32_t next_end = 0;   // as far again: the end of the song's next pass through its loop
    uint32_t end = 0;
};

// A channel's loop point, the ends of its passes through its loop, and where it ends or stops at its loop point. Each
// is an index into the events. Each is -1 or empty until the channel gets there.
struct ChannelLoop
{
    // Returns true if the channel has been through its loop.
    bool Loops() const
    {
        return !passes.empty() && loop_point >= 0;
    }

    int loop_point = -1;
    std::vector<int> passes;
    int end = -1;
};

// The model's output for the stretch of a song that the plan covers, and its timing in the MIDI file.
struct Simulation
{
    std::vector<FrameData> frames;
    std::vector<Event> events;
    std::vector<uint32_t> frame_ticks; // the MIDI tick each frame starts at, and the one after the last frame
    std::array<ChannelLoop, kChannels> channel_loops;
    int loop_channel = -1;             // the channel with the longest loop, or -1 if no channel loops
    size_t times = 1;                  // the loop channel's passes through its loop in each pass of the song's loop
    std::vector<uint32_t> event_ticks; // each event's MIDI tick
    Plan plan;
    std::vector<std::string> warnings;
};

// Returns the MIDI file's tempo in a frame: the first channel's tempo T, in which the frame lasts T ticks.
int MidiTempo(const FrameData& d)
{
    return std::max<int>(d.tempo[0], kMinMidiTempo);
}

// Returns each event's MIDI tick at the start of the frame that the driver plays it in.
std::vector<uint32_t> FrameStartTicks(const Simulation& sim)
{
    std::vector<uint32_t> ticks(sim.events.size());
    for (size_t i = 0; i < sim.events.size(); i++)
    {
        ticks[i] = sim.frame_ticks[sim.events[i].frame];
    }

    return ticks;
}

// Returns each event's MIDI tick on the song's beat. The driver times a channel's notes by counting its ticks from the
// last time it reset the count: at the song's start, at a loop and at a tempo change. It plays each note at the start
// of the frame that its count reaches. So the first note after a reset starts its frame. The notes after it keep to
// their ticks within the frames that the driver plays them in. A tick lasts 37 MIDI ticks at the first channel's tempo.
std::vector<uint32_t> BeatTicks(const Simulation& sim)
{
    std::vector<uint32_t> ticks(sim.events.size());
    std::array<bool, kChannels> reset;
    reset.fill(true);
    std::array<uint32_t, kChannels> base = {};
    std::array<uint64_t, kChannels> base_tick = {};
    std::array<double, kChannels> scale = {};
    for (size_t i = 0; i < sim.events.size(); i++)
    {
        const Event& e = sim.events[i];
        const size_t c = e.channel;
        const uint32_t start = sim.frame_ticks[e.frame];
        const uint32_t next = sim.frame_ticks[e.frame + 1];

        // A loop resets the channel's count, and so does a tempo: on the first channel, every channel's. A loop point
        // read before the first note after a reset belongs to that note's frame.
        if (e.kind == Event::kTempo || e.kind == Event::kLoop)
        {
            if (e.kind == Event::kTempo && c == 0)
            {
                reset.fill(true);
            }

            reset[c] = true;
            ticks[i] = start;
            continue;
        }
        if (reset[c] && e.kind == Event::kLoopPoint)
        {
            ticks[i] = start;
            continue;
        }

        if (reset[c])
        {
            reset[c] = false;
            base[c] = start;
            base_tick[c] = e.tick;
            const FrameData& d = sim.frames[e.frame];
            scale[c] = double(kMidiTicksPerTick) * MidiTempo(d) / std::max<uint16_t>(d.tempo[c], 1);
        }

        const double tick = base[c] + double(e.tick - base_tick[c]) * scale[c];
        ticks[i] = uint32_t(std::clamp<double>(std::round(tick), start, next - 1));
    }

    return ticks;
}

// Returns the events that the song's loop waits for: each looping channel's loop point, and the end of each other
// channel that has ended or stopped at its loop point.
std::vector<int> LoopStartEvents(const std::array<ChannelLoop, kChannels>& channel_loops)
{
    std::vector<int> events;
    for (const ChannelLoop& loop : channel_loops)
    {
        if (loop.Loops())
        {
            events.push_back(loop.loop_point);
        }
        else if (loop.end >= 0)
        {
            events.push_back(loop.end);
        }
    }

    return events;
}

// Works out the song's loop from the events so far. Returns true once every channel has looped, ended or stopped at its
// loop point. Each call sets the loop channel: the channel with the longest loop, or -1. When a channel loops, the call
// that returns true also sets `times` and `run_on`. Channels may have different loop points and lengths. The song's
// loop starts once all looping channels have entered their loops and the others have ended or stopped. `times` is the
// number of passes of the longest loop that LoopLength() gives for the channels' loops.
bool TryPlan(Simulation& sim, uint32_t& run_on)
{
    bool planned = true;
    sim.loop_channel = -1;
    uint64_t longest = 0;
    std::vector<uint64_t> lengths;
    for (size_t c = 0; c < kChannels; c++)
    {
        const ChannelLoop& loop = sim.channel_loops[c];
        planned = planned && (!loop.passes.empty() || loop.end >= 0);
        const uint64_t length =
            loop.Loops() ? sim.events[size_t(loop.passes[0])].tick - sim.events[size_t(loop.loop_point)].tick : 0;
        if (length > 0)
        {
            lengths.push_back(length);
        }
        if (length > longest)
        {
            longest = length;
            sim.loop_channel = int(c);
        }
    }

    // The song runs on as much longer as the loop starts after the loop channel's loop point. It runs on for at least
    // as many MIDI ticks as the frames from that loop point to the latest took. The tempo while it runs on doesn't
    // change that. If they're in the same frame, it runs on to the end of that frame.
    if (planned && sim.loop_channel >= 0)
    {
        sim.times = size_t(LoopLength(lengths) / longest);
        const int loop_point = sim.channel_loops[size_t(sim.loop_channel)].loop_point;
        const uint32_t first = sim.events[size_t(loop_point)].frame;
        uint32_t latest = first;
        for (int e : LoopStartEvents(sim.channel_loops))
        {
            latest = std::max(latest, sim.events[size_t(e)].frame);
        }
        run_on = latest > first ? sim.frame_ticks[latest + 1] - sim.frame_ticks[first] : 0;
    }

    return planned;
}

// Runs the model until every channel has looped, ended or stopped at its loop point and the song has played its loop
// `loops` times.
Simulation RunModel(const Rom& rom, const DriverInfo& info, int song, int loops)
{
    Simulation sim;
    Sequencer seq(rom, info, song);
    sim.frame_ticks.push_back(0);
    bool planned = false;
    bool finished = false; // the song played what the plan requires
    uint32_t run_on = 0;   // the MIDI ticks the model runs on for after the loop channel's last pass
    for (uint32_t f = 0; f < kMaxFrames && !seq.Ended(); f++)
    {
        seq.Step();
        FrameData d;
        d.hardware = seq.GetHardware();
        d.fifos = {seq.GetFifo(0), seq.GetFifo(1)};
        for (int c = 0; c < kChannels; c++)
        {
            d.tempo[size_t(c)] = seq.GetChannel(c).tempo;
        }
        sim.frames.push_back(d);

        // The MIDI file's tempo map follows the first channel, which keeps every frame the same length.
        sim.frame_ticks.push_back(sim.frame_ticks.back() + uint32_t(MidiTempo(d)));

        for (const Event& e : seq.Events())
        {
            ChannelLoop& loop = sim.channel_loops[e.channel];
            const int index = int(sim.events.size());
            sim.events.push_back(e);
            if (e.kind == Event::kLoopPoint && loop.passes.empty())
            {
                loop.loop_point = index;
            }
            else if (e.kind == Event::kLoop)
            {
                loop.passes.push_back(index);
            }
            else if (e.kind == Event::kEnd || e.kind == Event::kHold)
            {
                loop.end = index;
            }
        }

        // Once every channel has looped, ended or stopped at its loop point, the loop is known. The song then continues
        // for `loops` passes through it.
        if (!planned)
        {
            planned = TryPlan(sim, run_on);
        }
        if (planned && sim.loop_channel < 0)
        {
            finished = true;
            break;
        }
        const size_t needed = size_t(loops) * sim.times; // the loop channel's passes in the song
        if (planned && sim.channel_loops[size_t(sim.loop_channel)].passes.size() >= needed)
        {
            const int last = sim.channel_loops[size_t(sim.loop_channel)].passes[needed - 1];
            if (sim.frame_ticks[f] >= sim.frame_ticks[sim.events[size_t(last)].frame] + run_on)
            {
                finished = true;
                break;
            }
        }
    }

    sim.warnings = seq.Warnings();
    if (!finished && !seq.Ended() && sim.frames.size() == kMaxFrames)
    {
        sim.warnings.push_back("the song was cut off after an hour");
    }

    return sim;
}

// Times the events of `sim` for `timing`. Then plans the stretch of the song that the MIDI file covers: `loops` passes
// through the loop from `start_pass`. The loop starts where every looping channel has started its loop and every other
// channel has ended or stopped. The delay is how far that comes after the loop channel's loop point. The loop ends that
// delay after the end of the loop channel's `times`th pass. From the second pass, the loop starts and ends a loop
// later: that delay after the ends of its `times`th and twice `times`th passes. The song's next pass through the loop
// ends a loop after the loop's end. The song ends that delay after its last pass. If the song doesn't loop, it ends
// where the last channel ends or stops. A song that the model cut off ends where it stopped.
void PlanLoop(Simulation& sim, Timing timing, int loops, LoopStart start_pass)
{
    sim.event_ticks = timing == Timing::kFrames ? FrameStartTicks(sim) : BeatTicks(sim);
    sim.plan = Plan();
    Plan& plan = sim.plan;
    if (sim.loop_channel >= 0)
    {
        const ChannelLoop& loop = sim.channel_loops[size_t(sim.loop_channel)];
        const std::vector<int>& p = loop.passes;
        uint32_t start = sim.event_ticks[size_t(loop.loop_point)];
        for (int e : LoopStartEvents(sim.channel_loops))
        {
            start = std::max(start, sim.event_ticks[size_t(e)]);
        }

        const uint32_t first_pass = sim.event_ticks[size_t(p[0])] - sim.event_ticks[size_t(loop.loop_point)];
        const uint32_t delay = start - sim.event_ticks[size_t(loop.loop_point)];

        // Where the loop channel's `n`th pass ends, moved as far on as the loop starts after its loop point: the loop
        // start for 0, and if the model stopped before that pass ended, where it would at the first pass's length.
        const auto pass_end = [&](size_t n)
        {
            return n == 0          ? start
                   : p.size() >= n ? sim.event_ticks[size_t(p[n - 1])] + delay
                                   : start + uint32_t(n) * first_pass;
        };

        // The loop channel's passes before the marked loop, and up to the song's end.
        const size_t skip = start_pass == LoopStart::kSecondPass ? sim.times : 0;
        const size_t last = size_t(loops) * sim.times + skip;
        plan.loops = true;
        plan.loop_start = pass_end(skip);
        plan.loop_end = pass_end(skip + sim.times);
        plan.next_end = pass_end(skip + 2 * sim.times);
        plan.end = p.size() >= last ? std::min(pass_end(last), sim.frame_ticks.back()) : sim.frame_ticks.back();
    }
    else
    {
        for (const ChannelLoop& channel : sim.channel_loops)
        {
            if (channel.end >= 0)
            {
                plan.end = std::max(plan.end, sim.event_ticks[size_t(channel.end)]);
            }
        }
    }

    if (plan.end == 0)
    {
        plan.end = sim.frame_ticks.back();
    }
}

// Runs the model until every channel has looped, ended or stopped at its loop point and the song has played its loop
// `loops` times. Then works out the MIDI file's timing. From LoopStart::kSecondPass, the song plays the first pass
// through the loop before the marked loop.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, Timing timing, LoopStart start_pass)
{
    Simulation sim = RunModel(rom, info, song, loops + (start_pass == LoopStart::kSecondPass ? 1 : 0));
    PlanLoop(sim, timing, loops, start_pass);

    return sim;
}

// A change that the frequency sweep makes to a square's pitch within a frame.
struct SweepStep
{
    double at = 0;     // its position in the frame, from 0 to 1
    uint32_t tick = 0; // its MIDI tick
    double bend = 0;   // semitones from the note's key
};

// A channel's sound in one frame.
struct Sound
{
    bool sounding = false;
    double left = 0, right = 0; // levels, with a full-scale FIFO sample at 128
    double bend = 0;            // semitones from the note's key
    int shape = -1;             // the duty or the wave shape that the frame plays
    uint32_t tick = 0;          // the MIDI tick its changes go at
    std::vector<SweepStep> sweeps;
};

// The SoundFont instrument a note plays.
struct Instrument
{
    enum Kind : uint8_t
    {
        kSquare,
        kWave,
        kNoiseKit,
        kPcm,
    };

    bool operator<(const Instrument& o) const
    {
        return std::tie(kind, first, frames, second, pcm) < std::tie(o.kind, o.first, o.frames, o.second, o.pcm);
    }

    Kind kind = kSquare;
    int first = 0;  // the duty or the wave shape at a note's start
    int frames = 0; // the frames before it switches, or 0
    int second = 0; // the duty or the wave shape it switches to
    PcmSound pcm;
};

// A note as the MIDI file plays it.
struct Note
{
    int key = 60;
    int program = 0;
    uint32_t on = 0; // MIDI ticks
    uint32_t off = 0;
    std::vector<Sound> sounds; // one for each of its frames
    NoiseSound noise;          // a noise note's drum
    PcmSound pcm;              // a PCM note's sample
};

// Returns the registers of PSG channel `c` after a frame, for its model.
PsgRegisters PsgRegistersOf(const Hardware& hw, int c)
{
    return {hw.registers[kNr10], hw.registers[size_t(kPsgEnvelope[c])], hw.registers[size_t(kPsgControl[c])],
            hw.registers[kNr30], (hw.restarted & (1 << c)) != 0};
}

// Returns the wave shape that the wave channel plays after a frame: the bank NR30 selects, or both banks starting with
// it in the 64-step mode.
WaveShape WaveShapeOf(const Hardware& hw)
{
    const uint16_t nr30 = hw.registers[kNr30];
    const size_t bank = (nr30 >> 6) & 1;
    WaveShape shape;
    for (size_t b = 0; b < ((nr30 & 0x20) ? 2u : 1u); b++)
    {
        for (uint8_t v : hw.wave_ram[(bank + b) & 1])
        {
            shape.steps.push_back(uint8_t(v >> 4));
            shape.steps.push_back(uint8_t(v & 15));
        }
    }

    return shape;
}

// Converts a song's channels into the MIDI file's notes.
class NoteMaker
{
public:
    NoteMaker(const Rom& rom, const DriverInfo& info, const Simulation& sim)
        : rom_(rom), info_(info), sim_(sim), end_(sim.plan.end)
    {
    }

    // Returns channel `c`'s notes, and adds the instruments they play.
    std::vector<Note> Make(int c)
    {
        // Each note or "play again" starts a stretch of the channel, which lasts until its next note, rest or end.
        std::vector<size_t> starts;
        for (size_t i = 0; i < sim_.events.size(); i++)
        {
            const Event& e = sim_.events[i];
            if (e.channel == c && (e.kind == Event::kNote || e.kind == Event::kPlayAgain || e.kind == Event::kRest ||
                                   e.kind == Event::kEnd))
            {
                starts.push_back(i);
            }
        }

        // A PSG channel's sound comes from a model of the channel.
        std::vector<Note> notes;
        std::optional<PsgFeed> psg;
        if (c < kPcmA)
        {
            psg.emplace(c, [this, c](uint32_t f) { return PsgRegistersOf(sim_.frames[f].hardware, c); });
        }

        // A "play again" keeps the key of the note before. On a PCM channel, a "play again" plays the sample at the
        // sample's rate on the root key.
        int key = 60;
        int base = 0; // the frequency setting or PCM pitch that the key stands for
        for (size_t i = 0; i < starts.size(); i++)
        {
            const Event& e = sim_.events[starts[i]];
            Stretch stretch;
            stretch.first = e.frame;
            stretch.last = i + 1 < starts.size() ? sim_.events[starts[i + 1]].frame : uint32_t(sim_.frames.size());
            stretch.on = sim_.event_ticks[starts[i]];
            stretch.stop = std::min(i + 1 < starts.size() ? sim_.event_ticks[starts[i + 1]] : end_, end_);
            if (e.kind == Event::kRest || e.kind == Event::kEnd || stretch.on >= stretch.stop)
            {
                continue;
            }

            if (e.kind == Event::kNote)
            {
                key = KeyOf(c, e.index, base);
            }
            else if (c >= kPcmA)
            {
                key = kPcmRootKey;
                base = 0;
            }

            stretch.key = key;
            stretch.base = base;
            AddStretch(c, stretch, psg ? &*psg : nullptr, notes);
        }

        return notes;
    }

    // Returns the instruments the notes play, with their programs.
    const std::map<Instrument, int>& Programs() const
    {
        return programs_;
    }

    // Returns the noise kit's drums, with their keys.
    const std::map<NoiseSound, int>& Drums() const
    {
        return drums_;
    }

    // Returns the wave shapes that the notes play, by number.
    const WaveShapes& Shapes() const
    {
        return shapes_;
    }

private:
    // A stretch of a channel from a note or "play again" to the next note, rest or end.
    struct Stretch
    {
        uint32_t first = 0; // the frame it starts in
        uint32_t last = 0;  // the frame the next stretch starts in
        uint32_t on = 0;    // its start and end in MIDI ticks
        uint32_t stop = 0;
        int key = 60;
        int base = 0; // the frequency setting or PCM pitch that the key stands for
    };

    // Adds the notes of a stretch of channel `c`: one for each run of its frames in which the channel sounds. Each
    // frame's changes go as far into it as the stretch's start is into its first frame. They stay before the next
    // stretch. `psg` is the model of a PSG channel. It's null for a PCM channel.
    void AddStretch(int c, const Stretch& stretch, PsgFeed* psg, std::vector<Note>& notes)
    {
        Note note;
        bool open = false;
        for (uint32_t f = stretch.first; f < stretch.last && f < sim_.frames.size(); f++)
        {
            const FrameData& d = sim_.frames[f];
            const uint32_t into = stretch.on - sim_.frame_ticks[stretch.first];
            const uint32_t tick = std::min({sim_.frame_ticks[f] + into, sim_.frame_ticks[f + 1] - 1, stretch.stop - 1});
            Sound s = psg ? PsgSound(c, psg->Volume(f), psg->Model(), d, stretch.base) : FifoSound(c, d, stretch.base);
            s.tick = tick;

            // The sweep's steps go as far into the frame's ticks as they come into the frame. They stay before the next
            // frame's changes.
            const uint32_t length = sim_.frame_ticks[f + 1] - sim_.frame_ticks[f];
            const uint32_t last_tick = std::min(sim_.frame_ticks[f + 1] + into, stretch.stop) - 1;
            for (SweepStep& step : s.sweeps)
            {
                step.tick = std::min(tick + uint32_t(std::lround(step.at * length)), last_tick);
            }

            if (!s.sounding)
            {
                if (open)
                {
                    note.off = tick;
                    Finish(c, note, notes);
                    open = false;
                }
                continue;
            }

            if (!open)
            {
                note = Note();
                note.key = stretch.key;
                note.on = tick;
                if (c >= kPcmA)
                {
                    note.pcm = PcmSoundOf(d.fifos[size_t(c - kPcmA)]);
                }
                open = note.on < stretch.stop;
            }
            if (open)
            {
                note.sounds.push_back(s);
                if (c == kNoise)
                {
                    note.noise.AddFrame(d.hardware.registers[kNr41], d.hardware.registers[kNr43],
                                        (d.hardware.restarted & (1 << kNoise)) != 0);
                }
            }
        }

        if (open)
        {
            note.off = stretch.stop;
            Finish(c, note, notes);
        }
    }

    // Returns a note's MIDI key, and sets `base` to the pitch it stands for.
    int KeyOf(int c, int index, int& base) const
    {
        if (c >= kPcmA)
        {
            base = int16_t(rom_.U16(info_.pcm_pitch_table + uint32_t(index) * 2));
            return kPcmRootKey + index - kPcmUnityIndex;
        }

        base = rom_.U16(info_.frequency_table + uint32_t(index) * 2) & 0x7FF;

        return kSquareBaseKey + index;
    }

    // Returns a PSG channel's sound in a frame. `volume` is the volume that the channel's model gives for the frame's
    // start, or -1 if the channel is off. `psg` holds the frequency and the sweep's changes for the frame.
    Sound PsgSound(int c, int volume, const PsgModel& psg, const FrameData& d, int base)
    {
        Sound s;
        const Hardware& hw = d.hardware;
        if (volume < 0)
        {
            return s;
        }

        const PsgScale scale = PsgScaleOf(hw.registers[kNr50], hw.registers[kSoundcntH]);
        const int nr51 = hw.registers[kNr50] >> 8;
        double level = 0;
        switch (c)
        {
        case kWave:
            // NR32 scales the wave.
            level = 2 * kPsgLevelPerVolume * 15 * WaveShare(hw.registers[kNr31]);
            s.shape = shapes_.Number(WaveShapeOf(hw));
            break;

        case kNoise:
            // The drum's sample has the noise channel's volume in it.
            level = kPsgLevelPerVolume * 15;
            break;

        default:
            level = kPsgLevelPerVolume * volume;
            s.shape = (hw.registers[size_t(kPsgEnvelope[c])] >> 6) & 3;
            break;
        }

        s.sounding = true;
        s.left = (nr51 & (0x10 << c)) ? level * scale.left : 0;
        s.right = (nr51 & (0x01 << c)) ? level * scale.right : 0;

        if (c != kNoise && base)
        {
            s.bend = PsgSemitones(base, psg.Frequency());
            for (const auto& [at, frequency] : psg.Sweeps())
            {
                s.sweeps.push_back({at, 0, PsgSemitones(base, frequency)});
            }
        }

        return s;
    }

    // Returns a PCM channel's sound in a frame.
    Sound FifoSound(int c, const FrameData& d, int base) const
    {
        Sound s;
        const int f = c - kPcmA;
        const Fifo& fifo = d.fifos[size_t(f)];
        if (fifo.position == ~0u || !fifo.base_rate || !fifo.rate)
        {
            return s;
        }

        const uint16_t h = d.hardware.registers[kSoundcntH];
        const double level = (h & kFifoVolume[f]) ? kFifoLevel : kFifoLevel / 2;
        s.sounding = true;
        s.left = (h & kFifoLeft[f]) ? level : 0;
        s.right = (h & kFifoRight[f]) ? level : 0;

        // The key plays at the rate that the note's pitch stands for, which may differ from the rate it plays at by a
        // detune or an effect.
        const double nominal = fifo.base_rate + double(fifo.base_rate) * base / 1000.0;
        s.bend = 12 * std::log2(double(fifo.rate) / nominal);

        return s;
    }

    // Returns what a FIFO plays from its last start, at the rate it plays at then: the frames until the driver stops it
    // or loops it, and the frames of each pass through the loop, as the bytes the FIFO plays in them. A FIFO that
    // started from its loop plays passes through the loop from the start.
    static PcmSound PcmSoundOf(const Fifo& fifo)
    {
        PcmSound p;
        p.start = fifo.sample;
        p.from = fifo.next_start - fifo.sample;
        p.looped = fifo.loop != 0;
        p.loop = p.looped ? fifo.loop - fifo.sample : 0;
        p.rate = fifo.base_rate;

        const double bytes = double(kFrameCycles) / std::max<uint32_t>(SampleCycles(fifo.rate), 1);
        const bool in_loop = p.looped && fifo.next_start == fifo.loop;
        const uint32_t first = in_loop ? 0 : FifoFrames(p.from * 60, fifo.rate, fifo.end);
        const uint32_t pass = FifoFrames(fifo.loop_position + fifo.rate, fifo.rate, fifo.end) + 1;
        p.first = uint32_t(std::lround(first * bytes));
        p.loop_length = p.looped ? uint32_t(std::lround(pass * bytes)) : 0;
        p.from = in_loop ? p.loop : p.from;

        return p;
    }

    // Gives a finished note its instrument and keeps it.
    void Finish(int c, Note& note, std::vector<Note>& notes)
    {
        if (note.sounds.empty() || note.on >= note.off)
        {
            return;
        }

        Instrument inst;
        if (c == kNoise)
        {
            note.noise.Trim();
            inst.kind = Instrument::kNoiseKit;
            note.key = drums_.try_emplace(note.noise, kFirstDrumKey + int(drums_.size())).first->second;
        }
        else if (c >= kPcmA)
        {
            inst.kind = Instrument::kPcm;
            inst.pcm = note.pcm;
        }
        else
        {
            // The shape at the start, and the first one it switches to.
            inst.kind = c == kWave ? Instrument::kWave : Instrument::kSquare;
            inst.first = note.sounds[0].shape;
            inst.second = inst.first;
            for (size_t i = 1; i < note.sounds.size(); i++)
            {
                if (note.sounds[i].shape != inst.first)
                {
                    inst.frames = int(i);
                    inst.second = note.sounds[i].shape;
                    break;
                }
            }

            // A wave pattern of 64 steps plays an octave lower than one of 32, and one that repeats sounds higher.
            if (c == kWave)
            {
                const WaveShape& shape = shapes_[inst.first];
                note.key += WaveKeyOffset(shape) - (shape.steps.size() > 32 ? 24 : 12);
            }
        }

        note.program = programs_.try_emplace(inst, int(programs_.size())).first->second;
        notes.push_back(note);
    }

    const Rom& rom_;
    const DriverInfo& info_;
    const Simulation& sim_;
    const uint32_t end_;
    std::map<Instrument, int> programs_;
    std::map<NoiseSound, int> drums_;
    WaveShapes shapes_;
};

// The start of a note in a pass through a loop. FirstPassRepeats() compares the passes by these fields: the MIDI ticks
// from the pass's start, the key and program, the controllers for the levels and the bend of the note's first frame,
// and the MIDI file's tempo there.
struct NoteStart
{
    bool operator==(const NoteStart& other) const = default;

    uint32_t at = 0;
    int key = 0;
    int program = 0;
    int cc10 = 0;
    int cc11 = 0;
    double bend = 0;
    int tempo = 0;
};

// Returns true if each channel's second pass through its loop plays the same notes as its first: the same keys and
// programs at the same ticks from the pass's start, with the same levels, bend and tempo. Every note has the same
// velocity. The notes' ends aren't compared: the first pass can be a fraction of a frame longer than the next, as the
// driver drops the part of a frame it has counted when it loops, and the sound hardware's clocks, which aren't tied to
// the frames, can end a note a frame earlier or later from pass to pass. `sim` has to hold three passes of the song's
// loop, so that every channel's second pass is complete.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim);
    for (int c = 0; c < kChannels; c++)
    {
        // Where the channel's first pass starts, at its loop point, and where it and the second pass end.
        std::vector<uint32_t> bounds;
        for (size_t i = 0; i < sim.events.size() && bounds.size() < 3; i++)
        {
            const Event& e = sim.events[i];
            if (e.channel == c && e.kind == Event::kLoopPoint && bounds.size() < 2)
            {
                bounds.assign(1, sim.event_ticks[i]);
            }
            else if (e.channel == c && e.kind == Event::kLoop && !bounds.empty())
            {
                bounds.push_back(sim.event_ticks[i]);
            }
        }
        if (bounds.size() < 3)
        {
            continue;
        }

        std::array<std::vector<NoteStart>, 2> passes;
        for (const Note& n : maker.Make(c))
        {
            for (size_t k = 0; k < 2; k++)
            {
                if (n.on >= bounds[k] && n.on < bounds[k + 1])
                {
                    const auto frame = std::upper_bound(sim.frame_ticks.begin(), sim.frame_ticks.end(), n.on) - 1;
                    int cc10 = 64, cc11 = 0;
                    LevelsToControllers(n.sounds[0].left, n.sounds[0].right, cc10, cc11);
                    passes[k].push_back({n.on - bounds[k], n.key, n.program, cc10, cc11, n.sounds[0].bend,
                                         MidiTempo(sim.frames[size_t(frame - sim.frame_ticks.begin())])});
                }
            }
        }
        if (passes[0] != passes[1])
        {
            return false;
        }
    }

    return true;
}

// Returns true if each channel starts the same number of notes in the marked loop and the next pass. When a channel
// loops, the driver discards its fractional frame, which differs between channels, so a channel's later passes can
// start up to a frame earlier relative to the loop channel than its first did. The copy of a note at the loop's start
// can then fall before the end marker, causing a looping player to play it twice.
bool SeamRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    NoteMaker maker(rom, info, sim);
    const Plan& plan = sim.plan;
    for (int c = 0; c < kChannels; c++)
    {
        const std::vector<Note> notes = maker.Make(c);
        const auto starts = [&](uint32_t from, uint32_t to)
        {
            return std::count_if(notes.begin(), notes.end(), [&](const Note& n) { return n.on >= from && n.on < to; });
        };

        if (starts(plan.loop_start, plan.loop_end) != starts(plan.loop_end, plan.next_end))
        {
            return false;
        }
    }

    return true;
}

// Returns the MIDI file's tempo changes before the end, as (MIDI tick, tempo).
std::vector<std::pair<uint32_t, int>> TempoMap(const Simulation& sim)
{
    std::vector<std::pair<uint32_t, int>> tempos;
    for (size_t f = 0; f < sim.frames.size() && (f == 0 || sim.frame_ticks[f] < sim.plan.end); f++)
    {
        const int tempo = MidiTempo(sim.frames[f]);
        if (tempos.empty() || tempos.back().second != tempo)
        {
            tempos.emplace_back(sim.frame_ticks[f], tempo);
        }
    }

    if (tempos.empty())
    {
        tempos.emplace_back(0, kDefaultTempo);
    }

    return tempos;
}

// Adds the SoundFont presets of a song's instruments, each in the bank and program of `slots` that its program number
// picks. Returns the warnings for instruments with nothing to play.
std::vector<std::string> AddPresets(SoundfontBuilder& sf, const NoteMaker& maker, const std::vector<BankProgram>& slots)
{
    std::vector<std::string> warnings;
    for (const auto& [inst, program] : maker.Programs())
    {
        NamedZones made;
        switch (inst.kind)
        {
        case Instrument::kSquare:
            made = sf.SquareInstrument(inst.first, inst.frames, inst.second);
            break;

        case Instrument::kWave:
            made = sf.WaveInstrument(maker.Shapes(), inst.first, inst.frames, inst.second);
            break;

        case Instrument::kNoiseKit:
            for (const auto& [sound, key] : maker.Drums())
            {
                const int sample = sf.NoiseSample(sound);
                made.zones.push_back(RootKeyZone(sample, Looped(sf.File().samples[size_t(sample)]), key, key, key));
            }
            made.name = "Noise drums";
            break;

        case Instrument::kPcm:
            {
                const int sample = sf.PcmSample(inst.pcm);
                if (sample >= 0)
                {
                    made.zones.push_back(SampleZone(sample, inst.pcm.looped));
                }
                else
                {
                    warnings.push_back("a PCM note's sample has nothing to play");
                }

                made.name = AddressName("PCM", inst.pcm.start);
                break;
            }
        }

        const BankProgram& slot = slots[size_t(program)];
        sf.AddPreset(made.name, slot.bank, slot.program, sf.AddInstrument(made.name, std::move(made.zones)));
    }

    return warnings;
}

// Writes the MIDI file: a conductor track, then a track for each channel that plays notes. `slots` gives each program
// number its preset's bank and program.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
               const std::array<std::vector<Note>, kChannels>& notes, const std::vector<BankProgram>& slots,
               std::string& error)
{
    const Plan& plan = sim.plan;
    const uint32_t end = plan.end;
    MidiFile midi(kTicksPerQuarter);
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    conductor.TimeSignature(0, 4, 2);
    for (const auto& [tick, tempo] : TempoMap(sim))
    {
        conductor.Tempo(tick, QuarterMicros(kTicksPerQuarter, uint64_t(tempo)));
    }
    if (plan.loops)
    {
        conductor.Meta(plan.loop_start, 0x06, "loopStart");
        conductor.Meta(plan.loop_end, 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    for (int c = 0; c < kChannels; c++)
    {
        const std::vector<Note>& list = notes[size_t(c)];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name(kChannelNames[c]);
        mt.SetEnd(end);

        // The pitch bend range covers the channel's largest bend. The other drivers that use BendRangeFor() leave out
        // bends of kSmallBend or less. Quintet's channels keep every bend, however small. A PCM note's rate, a whole
        // number in the driver, can bend the note by a few thousandths of a semitone from its key's rate.
        double widest = 0;
        for (const Note& n : list)
        {
            for (const Sound& s : n.sounds)
            {
                widest = std::max(widest, std::fabs(s.bend));
                for (const SweepStep& step : s.sweeps)
                {
                    widest = std::max(widest, std::fabs(step.bend));
                }
            }
        }
        const int range = BendRangeFor(widest, 0);

        const int first = list[0].program;
        ChannelWriter writer(mt, c, first, slots[size_t(first)], range, plan.loops ? plan.loop_start : 0);
        for (const Note& n : list)
        {
            writer.SetProgram(n.on, n.program, slots[size_t(n.program)]);

            // Each frame's levels and bend.
            for (const Sound& s : n.sounds)
            {
                const uint32_t tick = s.tick;
                if (tick >= n.off)
                {
                    break;
                }

                writer.SetLevels(tick, s.left, s.right);
                if (!range)
                {
                    continue;
                }

                // The frame's bend, then the sweep's steps within the frame.
                writer.SetBend(tick, s.bend);
                for (const SweepStep& step : s.sweeps)
                {
                    writer.Reach(step.tick);
                    if (step.tick < n.off)
                    {
                        writer.SetBend(step.tick, step.bend);
                    }
                }
            }

            writer.Note(n.on, n.off, std::clamp(n.key, 0, 127));
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, writing its files if `write` is set.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
               SoundfontBuilder* shared, bool write)
{
    SongReport report;
    if (song < 0 || song >= int(info.song_addresses.size()))
    {
        report.warnings.push_back("its header can't be read");
        return report;
    }

    // Start the marked loop on the second pass if it differs from the first. The intro may leave a different octave or
    // other setting from the end of the loop. A channel may continue changing on later passes; the marker still starts
    // at the second. Compare notes on the beat, as in the default MIDI output, so `frame_timing` moves the same loops
    // rather than also moving those that differ only through rounding to frames. Also move the marker if a note at the
    // loop's start would play twice, under either timing mode.
    const Timing timing = settings.frame_timing ? Timing::kFrames : Timing::kBeat;
    Simulation sim = Simulate(rom, info, song, settings.loops, timing, LoopStart::kFirstPass);
    if (sim.plan.loops)
    {
        // The checks time one run of three passes twice: on the beat, then by frames.
        Simulation check = RunModel(rom, info, song, 3);
        PlanLoop(check, Timing::kBeat, 3, LoopStart::kFirstPass);
        bool repeats = FirstPassRepeats(rom, info, check) && SeamRepeats(rom, info, check);
        if (repeats)
        {
            PlanLoop(check, Timing::kFrames, 3, LoopStart::kFirstPass);
            repeats = SeamRepeats(rom, info, check);
        }
        if (!repeats)
        {
            sim = Simulate(rom, info, song, settings.loops, timing, LoopStart::kSecondPass);
        }
    }
    report.warnings = sim.warnings;

    NoteMaker maker(rom, info, sim);
    std::array<std::vector<Note>, kChannels> notes;
    for (int c = 0; c < kChannels; c++)
    {
        if (settings.track_mask & (1 << c))
        {
            notes[size_t(c)] = maker.Make(c);
            report.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
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

    // The SoundFont's presets, in the song's file or the shared one, in the banks and programs that it gives the song.
    // In a SoundFont for this song alone, the presets go in song 0's banks and programs.
    SoundfontBuilder own(rom);
    SoundfontBuilder& sf = shared ? *shared : own;
    const int count = int(maker.Programs().size());
    const std::vector<BankProgram> slots = sf.Banks().Place(shared ? song : 0, count);
    if (int(slots.size()) < count)
    {
        report.warnings.push_back("needs " + std::to_string(count) +
                                  " presets, more than the SoundFont's banks have free" +
                                  (shared ? "; convert the song without --single-sf2" : ""));
        return report;
    }
    for (const std::string& w : AddPresets(sf, maker, slots))
    {
        report.warnings.push_back(w);
    }

    const std::string title = rom.Title() + " #" + SongNumber(song, 0);
    char about[160];
    std::snprintf(about, sizeof about, "%s (%s) song %d, header at 0x%08X, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), song, unsigned(info.song_addresses[size_t(song)]), kProgramName);
    const std::string stem = SongStem(settings, SongNumber(song, 0));

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, sim, notes, slots, error))
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

} // namespace agbsf2::quintet
