// SPDX-License-Identifier: MIT

#include "krawall/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "files.h"
#include "krawall/driver.h"
#include "krawall/player.h"
#include "krawall/soundfont.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::krawall
{
namespace
{

// Target tempo range in BPM, with the upper bound excluded. Doubling the rows per beat halves the tempo, so exactly one
// power-of-two choice falls within a twofold range.
constexpr double kSlowestBeat = 105;
constexpr double kFastestBeat = 210;

// The most rows that a beat, or an eighth note in 12/8, can have.
constexpr uint32_t kMaxBeatRows = 64;

// The quarter notes in a bar of 4/4, and in one of 12/8.
constexpr uint32_t kSimpleBarQuarters = 4;
constexpr uint32_t kCompoundBarQuarters = 6;

// The cents by which a note's pitch can miss its key before its bend makes up the difference: more than the table of
// periods misses equal temperament by in the octaves that modules use.
constexpr double kTuningCents = 3;

// The longest conversion, in seconds.
constexpr uint64_t kMaxSeconds = 60 * 60;

// A channel's mixer channel after a tick.
struct Voice
{
    bool playing = false;
    uint32_t handle = 0;
    uint32_t moves = 0;
    uint32_t sample = 0;
    int sample_index = -1;
    uint32_t offset = 0; // the point it plays, from the sample's start
    double rate = 0;     // Hz
    double left = 0;     // the level of each side, where 128 is full scale
    double right = 0;
};

// What the conversion keeps of a tick.
struct TickData
{
    uint64_t time = 0;   // the mixer's samples from the module's first tick
    uint32_t length = 0; // the samples to the next tick
    bool row = false;    // a row starts on the tick
    int order = 0;       // the row's order
    int pattern_row = 0; // the row's number in its pattern
    int speed = 0;       // the ticks of its row, at least 1
    std::array<Voice, kPlayerChannels> voices;
};

// The stretch of a module that the conversion covers, in ticks.
struct Plan
{
    bool loops = false;
    size_t loop_start = 0; // the first row whose place in the module comes back
    size_t loop_end = 0;   // where it comes back
    size_t end = 0;
};

// The model's output for the stretch of a module that the plan covers.
struct Simulation
{
    std::vector<TickData> ticks; // up to and including the plan's end
    Plan plan;
    int channels = 0;
    int mix_rate = 0;
    std::vector<std::string> warnings;
};

// Returns a channel's mixer channel after a tick. `scale` is the output's level for a mixer level of 1, where 128 is
// full scale.
Voice VoiceOf(const Player& player, const DriverInfo& info, const Channel& ch, double scale)
{
    Voice v;
    v.playing = player.Sounding(ch);
    if (!v.playing)
    {
        return v;
    }

    const MixChannel& m = player.MixerOf(ch);
    v.handle = ch.handle;
    v.moves = m.moves;
    v.sample = m.sample;
    v.sample_index = m.sample_index;
    v.offset = m.pos - m.start;
    v.rate = std::fabs(double(m.inc)) * info.mix_rate / 65536.0;
    v.left = m.left * scale;
    v.right = (info.stereo ? m.right : m.left) * scale;

    return v;
}

// Runs the model until a row revisits an earlier module position with the same speed, tempo and pattern-loop state, and
// the module has played the loop between them `loops` times in all. From LoopStart::kSecondPass, the loop starts at the
// module's second pass through it. The module then plays the first pass before it. A song of a module that separates
// songs with +++ plays in song mode. Song mode loops back to the song's start at the song's end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, const ModuleSong& song, int loops, LoopStart start_pass)
{
    Simulation sim;
    Player player(rom, info, song);
    sim.channels = player.Module().channels;
    sim.mix_rate = info.mix_rate;
    if (!player.Valid())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    const double scale = player.OutputScale();
    std::map<std::tuple<int, int, uint32_t, int, int>, size_t> rows;
    uint64_t first = 0;
    size_t end = SIZE_MAX;
    const auto record = [&](uint64_t time)
    {
        if (sim.ticks.size() > end)
        {
            return;
        }

        if (sim.ticks.empty())
        {
            first = time;
        }
        TickData d;
        d.time = time - first;
        d.length = player.TickSamples();
        d.row = player.TickInRow() == 0;
        d.order = player.RowOrder();
        d.pattern_row = player.RowNumber();
        d.speed = std::max(player.Speed(), 1);
        for (int c = 0; c < sim.channels; c++)
        {
            d.voices[size_t(c)] = VoiceOf(player, info, player.Channels()[size_t(c)], scale);
        }

        // The first row that comes back to an earlier row's place starts the loop's second pass.
        if (d.row && end == SIZE_MAX)
        {
            const auto key = std::make_tuple(player.RowOrder(), player.RowNumber(), player.LoopState(), player.Speed(),
                                             player.Tempo());
            const auto [it, added] = rows.emplace(key, sim.ticks.size());
            if (!added)
            {
                const size_t length = sim.ticks.size() - it->second;
                const size_t skip = start_pass == LoopStart::kSecondPass ? length : 0;
                sim.plan.loops = true;
                sim.plan.loop_start = it->second + skip;
                sim.plan.loop_end = sim.ticks.size() + skip;
                end = sim.plan.loop_start + size_t(std::max(loops, 1)) * length;
            }
        }

        sim.ticks.push_back(d);
    };
    player.OnTick(record);

    const uint64_t most = kMaxSeconds * uint64_t(info.mix_rate);
    while (player.Playing() && sim.ticks.size() <= end && player.Elapsed() < most)
    {
        player.Frame();
    }

    if (sim.ticks.empty())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    if (end == SIZE_MAX)
    {
        // A module that stops ends where it stops, and one that neither stops nor loops is cut off.
        end = sim.ticks.size() - 1;
        if (player.Playing())
        {
            sim.warnings.push_back("found no loop within an hour, so the module is cut off there");
        }
    }

    sim.ticks.resize(std::min(sim.ticks.size(), end + 1));
    sim.plan.end = sim.ticks.size() - 1;
    sim.warnings.insert(sim.warnings.end(), player.Warnings().begin(), player.Warnings().end());

    return sim;
}

// A tick of a note: how far its pitch is from the note's key, in semitones, and its levels.
struct Sound
{
    size_t tick = 0;
    double bend = 0;
    double left = 0;
    double right = 0;
};

// A note: a sample that a channel's mixer channel plays from the tick a note or an effect starts it until it stops,
// another starts, or the module ends.
struct Note
{
    size_t on = 0;
    size_t off = 0;
    int key = 0;
    int program = 0;
    std::vector<Sound> sounds;
};

// Returns the notes of channel `c`, adding their instruments to `instruments`.
std::vector<Note> MakeNotes(const Rom& rom, const DriverInfo& info, const Simulation& sim, int c,
                            InstrumentSet& instruments)
{
    std::vector<Note> notes;
    bool open = false;
    double start_rate = 0, tuning = 0;
    for (size_t i = 0; i <= sim.plan.end; i++)
    {
        const Voice& v = sim.ticks[i].voices[size_t(c)];
        const Voice* last = i > 0 ? &sim.ticks[i - 1].voices[size_t(c)] : nullptr;
        const bool started =
            v.playing && (!last || !last->playing || v.handle != last->handle || v.moves != last->moves);
        if (open && (!v.playing || started || i == sim.plan.end))
        {
            notes.back().off = i;
            open = false;
        }
        if (i == sim.plan.end)
        {
            break;
        }

        // A note's key is the one nearest its pitch, and a pitch that misses it by more than the table of periods does
        // starts with a bend. A sample that starts at no pitch starts its note when it has one.
        if (v.playing && !open && v.rate > 0)
        {
            Note n;
            n.on = i;
            n.program = instruments.Program(v.sample, v.sample_index, v.offset);
            const double semitones = 12 * std::log2(v.rate / SampleRate(rom, info, v.sample));
            n.key = std::clamp(int(std::lround(kRootKey + semitones)), 0, 127);
            tuning = semitones - (n.key - kRootKey);
            if (std::fabs(tuning) * 100 < kTuningCents)
            {
                tuning = 0;
            }
            start_rate = v.rate;
            notes.push_back(n);
            open = true;
        }

        // Each tick of the note: its pitch, as a bend from its key, and its levels.
        if (open)
        {
            Sound s;
            s.tick = i;
            s.bend = notes.back().sounds.empty() ? tuning : notes.back().sounds.back().bend;
            if (v.rate > 0)
            {
                s.bend = tuning + 12 * std::log2(v.rate / start_rate);
            }
            s.left = v.left;
            s.right = v.right;
            notes.back().sounds.push_back(s);
        }
    }

    return notes;
}

// Returns true if the notes of the module's second pass through its loop start as those of its first do: on the same
// channels and ticks from the pass's start, with the same sample, start point, key, bend and levels, and ticks of the
// same length. The module's channels keep their pan and volume when it comes back to its loop, so the first pass can
// play unlike the later ones. `sim` has to hold two passes of the loop.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    const Plan& plan = sim.plan;
    PassComparison passes(plan.loop_start, plan.loop_end - plan.loop_start);
    InstrumentSet instruments;
    for (int c = 0; c < sim.channels; c++)
    {
        for (const Note& n : MakeNotes(rom, info, sim, c, instruments))
        {
            const Sound& s = n.sounds.front();
            passes.Add(n.on, c, n.key, {double(n.program), s.bend, s.left, s.right, double(sim.ticks[n.on].length)});
        }
    }

    return passes.Alike();
}

// A run of rows within one order. A new run starts when the order changes or playback returns to the same or an earlier
// row.
struct OrderRun
{
    size_t start = 0; // the tick of its first row
    size_t ticks = 0; // the ticks to the next run, or to the end
    int order = 0;
    int first_row = 0; // its first row's number in the pattern, which a break can make more than 0
};

// Returns the runs of rows that the module's orders play.
std::vector<OrderRun> OrderRuns(const Simulation& sim)
{
    std::vector<OrderRun> runs;
    int last_row = -1;
    for (size_t i = 0; i < sim.ticks.size(); i++)
    {
        const TickData& t = sim.ticks[i];
        if (!t.row)
        {
            continue;
        }

        if (runs.empty() || t.order != runs.back().order || t.pattern_row <= last_row)
        {
            if (!runs.empty())
            {
                runs.back().ticks = i - runs.back().start;
            }
            runs.push_back({i, 0, t.order, t.pattern_row});
        }
        last_row = t.pattern_row;
    }
    if (!runs.empty())
    {
        runs.back().ticks = sim.ticks.size() - runs.back().start;
    }

    return runs;
}

// MIDI ticks per quarter note and the choice of 4/4 or 12/8. In 12/8, the beat is a dotted quarter note.
struct Meter
{
    uint32_t quarter_ticks = 1;
    bool compound = false;
};

// Returns the ticks in a full bar of `meter`.
size_t BarTicks(const Meter& meter)
{
    return size_t(meter.quarter_ticks) * (meter.compound ? kCompoundBarQuarters : kSimpleBarQuarters);
}

// Returns the key with the largest count, the first of those that tie.
template <typename K>
K Commonest(const std::map<K, size_t>& counts)
{
    K best{};
    size_t most = 0;
    for (const auto& [key, count] : counts)
    {
        if (count > most)
        {
            best = key;
            most = count;
        }
    }

    return best;
}

// Returns whether one speed is a power-of-two multiple of the other. These speeds share a beat grid: rows divide or
// combine evenly.
bool SameGrid(uint32_t speed, uint32_t main)
{
    const uint32_t high = std::max(speed, main), low = std::min(speed, main);
    return low > 0 && high % low == 0 && ((high / low) & (high / low - 1)) == 0;
}

// Chooses a beat and meter using row lengths and pattern boundaries. In 4/4, a quarter note spans a power-of-two number
// of rows; in 12/8, each eighth note does. The target beat tempo is at least kSlowestBeat and below kFastestBeat, with
// `row_ticks` MIDI ticks per row and `tick_seconds` seconds per MIDI tick. 12/8 wins when more of the runs' MIDI ticks,
// in `ticks`, fit whole bars of it than of 4/4, as 48-row patterns do at two rows per eighth note. The count leaves out
// the first run, which may be a lead-in, and the last, which may end early.
Meter ChooseMeter(const std::vector<uint32_t>& ticks, const std::vector<OrderRun>& runs, uint32_t row_ticks,
                  double tick_seconds)
{
    const auto bpm = [&](uint32_t beat_ticks)
    {
        return 60 / (beat_ticks * tick_seconds);
    };

    // Double the rows per quarter note (4/4) or eighth note (12/8) until the beat tempo falls below kFastestBeat, or
    // the row limit is reached.
    uint32_t quarter_rows = 1, eighth_rows = 1;
    while (bpm(quarter_rows * row_ticks) >= kFastestBeat && quarter_rows < kMaxBeatRows)
    {
        quarter_rows *= 2;
    }
    while (bpm(3 * eighth_rows * row_ticks) >= kFastestBeat && eighth_rows < kMaxBeatRows)
    {
        eighth_rows *= 2;
    }
    const Meter simple = {quarter_rows * row_ticks, false};
    const Meter compound = {2 * eighth_rows * row_ticks, true};

    // The MIDI ticks of the runs that make whole bars.
    const auto whole = [&](const Meter& meter)
    {
        size_t sum = 0;
        for (size_t r = 1; r + 1 < runs.size(); r++)
        {
            const size_t length = ticks[runs[r].start + runs[r].ticks] - ticks[runs[r].start];
            if (length % BarTicks(meter) == 0)
            {
                sum += length;
            }
        }

        return sum;
    };

    if (bpm(3 * eighth_rows * row_ticks) >= kSlowestBeat && whole(compound) > whole(simple))
    {
        return compound;
    }

    return simple;
}

// Returns bar start positions in MIDI ticks, with `bar` MIDI ticks in a full bar. Bars align to each pattern's first
// row, and any rows that a run skips count as long as its first row. A pattern that ends early has a short final bar.
// If a break or jump cuts the first order short, as with a game's lead-in, its partial bar comes at the start instead,
// as a pickup, each time the first order plays.
std::vector<uint32_t> BarStarts(const Simulation& sim, const std::vector<uint32_t>& ticks,
                                const std::vector<OrderRun>& runs, size_t bar)
{
    std::vector<uint32_t> starts;
    if (runs.empty())
    {
        return starts;
    }

    const OrderRun& first = runs[0];
    const bool cut = runs.size() > 1 && runs[1].order != first.order;
    const size_t pickup = cut ? (ticks[first.start + first.ticks] - ticks[first.start]) % bar : 0;
    for (const OrderRun& run : runs)
    {
        const size_t row = size_t(ticks[run.start + 1] - ticks[run.start]) * size_t(sim.ticks[run.start].speed);
        const size_t from = run.order == first.order ? pickup : 0;
        size_t at = size_t(run.first_row) * row;
        for (size_t i = run.start; i < run.start + run.ticks; i++)
        {
            if (sim.ticks[i].row && (i == 0 || at == 0 || (at >= from && (at - from) % bar == 0)))
            {
                starts.push_back(ticks[i]);
            }
            at += ticks[i + 1] - ticks[i];
        }
    }

    return starts;
}

// MIDI timing based on the most common speed. At that speed, and at power-of-two multiples or fractions of it, each
// player tick occupies the same number of MIDI ticks. This lets a module halve its speed for finer rows without
// changing the MIDI tempo. At other speeds, each row gets the usual number of MIDI ticks, and the tempo changes to
// preserve its playback duration, subject to the resolution limit below.
struct Timing
{
    Meter meter;                                       // in MIDI ticks
    std::vector<uint32_t> ticks;                       // the MIDI tick of each of the module's ticks, and of the end
    std::vector<std::pair<uint32_t, uint32_t>> tempos; // MIDI tick and microseconds a quarter note
    std::vector<uint32_t> bars;                        // the MIDI ticks that start a bar
};

Timing MakeTiming(const Simulation& sim)
{
    // The rows at each speed, and the ticks of each length.
    Timing t;
    std::map<int, size_t> speeds;
    std::map<uint32_t, size_t> lengths;
    for (const TickData& d : sim.ticks)
    {
        lengths[d.length]++;
        if (d.row)
        {
            speeds[d.speed]++;
        }
    }
    const uint32_t main = uint32_t(std::max(Commonest(speeds), 1));

    // Increase the MIDI resolution so rows at other speeds can be divided evenly into player ticks. The ticks per
    // quarter note must fit in 15 bits. If a speed would require a higher resolution, leave its player ticks on the
    // normal grid.
    uint32_t scale = 1;
    std::set<uint32_t> stretched;
    for (const auto& entry : speeds)
    {
        const uint32_t s = uint32_t(entry.first);
        const uint32_t need = std::lcm(scale, s / std::gcd(s, main));
        if (!SameGrid(s, main) && 2 * kMaxBeatRows * main * need <= 32767)
        {
            scale = need;
            stretched.insert(s);
        }
    }

    // Each tick's MIDI tick.
    uint32_t at = 0;
    for (const TickData& d : sim.ticks)
    {
        t.ticks.push_back(at);
        const uint32_t s = uint32_t(d.speed);
        at += stretched.count(s) ? main * scale / s : scale;
    }
    t.ticks.push_back(at);

    // The beat, from the commonest length of tick, and the bars.
    const std::vector<OrderRun> runs = OrderRuns(sim);
    t.meter = ChooseMeter(t.ticks, runs, main * scale, double(Commonest(lengths)) / sim.mix_rate / scale);
    t.bars = BarStarts(sim, t.ticks, runs, BarTicks(t.meter));

    // A tempo that gives each tick its length.
    uint32_t tempo = 0;
    for (size_t i = 0; i < sim.ticks.size(); i++)
    {
        const uint32_t s = uint32_t(sim.ticks[i].speed);
        const double stretch = stretched.count(s) ? double(s) / main : 1;
        const double seconds = double(sim.ticks[i].length) / sim.mix_rate * t.meter.quarter_ticks / scale * stretch;
        const uint32_t micros = uint32_t(std::lround(seconds * 1e6));
        if (micros != tempo)
        {
            t.tempos.push_back({t.ticks[i], micros});
            tempo = micros;
        }
    }

    return t;
}

// The MIDI channels other than the drum channel.
constexpr size_t kMelodicChannels = 15;

// Returns the mixer's samples in which the notes of two channels sound at once. A channel plays one note at a time, so
// each list is in order and its notes don't overlap.
uint64_t Overlap(const Simulation& sim, const std::vector<Note>& a, const std::vector<Note>& b)
{
    uint64_t total = 0;
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        const uint64_t start = sim.ticks[std::max(a[i].on, b[j].on)].time;
        const uint64_t end = sim.ticks[std::min(a[i].off, b[j].off)].time;
        total += end > start ? end - start : 0;
        if (a[i].off < b[j].off)
        {
            i++;
        }
        else
        {
            j++;
        }
    }

    return total;
}

// Returns each channel's MIDI channel, from the channels other than the drum channel. In a module with up to 15
// channels, channel c takes the c-th. In a module with more, channels share them. A MIDI channel's controllers and bend
// apply to all its notes. The sharing therefore follows the notes. The 15 channels whose notes last longest take one
// each, in the channels' order. Each other channel then takes, longest first, the one whose notes it overlaps least. A
// tie goes to the lower channel.
std::vector<int> MidiChannels(const Simulation& sim, const std::vector<std::vector<Note>>& notes)
{
    std::vector<int> midi(notes.size());
    if (notes.size() <= kMelodicChannels)
    {
        for (size_t c = 0; c < notes.size(); c++)
        {
            midi[c] = MelodicChannel(int(c));
        }
        return midi;
    }

    std::vector<uint64_t> length(notes.size());
    for (size_t c = 0; c < notes.size(); c++)
    {
        for (const Note& n : notes[c])
        {
            length[c] += sim.ticks[n.off].time - sim.ticks[n.on].time;
        }
    }

    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return length[a] > length[b]; });

    std::vector<size_t> first(order.begin(), order.begin() + kMelodicChannels);
    std::sort(first.begin(), first.end());
    std::vector<std::vector<size_t>> groups;
    for (size_t c : first)
    {
        midi[c] = MelodicChannel(int(groups.size()));
        groups.push_back({c});
    }

    for (size_t k = kMelodicChannels; k < order.size(); k++)
    {
        const size_t c = order[k];
        size_t best = 0;
        uint64_t least = UINT64_MAX;
        for (size_t g = 0; g < groups.size(); g++)
        {
            uint64_t overlap = 0;
            for (size_t other : groups[g])
            {
                overlap += Overlap(sim, notes[c], notes[other]);
            }
            if (overlap < least)
            {
                best = g;
                least = overlap;
            }
        }

        midi[c] = MelodicChannel(int(best));
        groups[best].push_back(c);
    }

    return midi;
}

// Writes the time signature at `tick` for a bar of `ticks` MIDI ticks. Full bars use 4/4 or 12/8. A shorter bar counts
// the largest note value that divides it evenly, from quarter notes in 4/4 or eighth notes in 12/8 down to 128th notes.
void BarSignature(MidiTrack& conductor, uint32_t tick, size_t ticks, const Meter& meter)
{
    size_t note = meter.compound ? meter.quarter_ticks / 2 : meter.quarter_ticks;
    int pow2 = meter.compound ? 3 : 2;
    while (ticks % note != 0 && note % 2 == 0 && pow2 < 7)
    {
        note /= 2;
        pow2++;
    }
    conductor.TimeSignature(tick, int(ticks / note), pow2);
}

// Writes the MIDI file: a conductor track with the tempo map, then a track for each channel that plays notes, on the
// MIDI channel that `channels` gives it.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const Simulation& sim,
               const Timing& timing, const std::vector<std::vector<Note>>& notes, const std::vector<int>& channels,
               std::string& error)
{
    const Plan& plan = sim.plan;
    const uint32_t end = timing.ticks[plan.end];
    MidiFile midi(uint16_t(timing.meter.quarter_ticks));
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);

    // Write a time signature wherever the bar length changes. The last bar keeps the previous signature, even if the
    // file ends partway through it.
    size_t bar_ticks = 0;
    for (size_t b = 0; b + 1 < timing.bars.size(); b++)
    {
        const size_t ticks = size_t(timing.bars[b + 1] - timing.bars[b]);
        if (ticks != bar_ticks)
        {
            BarSignature(conductor, timing.bars[b], ticks, timing.meter);
            bar_ticks = ticks;
        }
    }
    if (bar_ticks == 0)
    {
        BarSignature(conductor, 0, BarTicks(timing.meter), timing.meter);
    }

    for (const auto& [tick, micros] : timing.tempos)
    {
        conductor.Tempo(tick, micros);
    }
    if (plan.loops)
    {
        conductor.Meta(timing.ticks[plan.loop_start], 0x06, "loopStart");
        conductor.Meta(timing.ticks[plan.loop_end], 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the module.
    const uint32_t loop_start = plan.loops ? timing.ticks[plan.loop_start] : 0;

    for (size_t c = 0; c < notes.size(); c++)
    {
        const std::vector<Note>& list = notes[c];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        const int ch = channels[c];
        mt.Name("Channel " + std::to_string(c + 1));
        mt.SetEnd(end);

        // The pitch bend range covers the largest bend of the channels on the MIDI channel.
        double widest = 0;
        bool shared = false;
        for (size_t other = 0; other < notes.size(); other++)
        {
            if (channels[other] != ch || notes[other].empty())
            {
                continue;
            }

            shared = shared || other != c;
            for (const Note& n : notes[other])
            {
                for (const Sound& s : n.sounds)
                {
                    widest = std::max(widest, std::fabs(s.bend));
                }
            }
        }
        const int range = BendRangeFor(widest, kSmallBend);

        ChannelWriter writer(mt, ch, list[0].program, NumberedPreset(list[0].program), range, loop_start, shared);
        for (const Note& n : list)
        {
            const uint32_t on = timing.ticks[n.on];
            const uint32_t off = timing.ticks[n.off];
            if (on >= off)
            {
                continue;
            }

            writer.SetProgram(on, n.program, NumberedPreset(n.program));

            // Each tick's levels and bend.
            for (const Sound& s : n.sounds)
            {
                const uint32_t at = timing.ticks[s.tick];
                writer.SetLevels(at, s.left, s.right);
                writer.SetBend(at, s.bend);
            }

            writer.Note(on, off, n.key);
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a module, writing its files if `write` is set.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write)
{
    SongReport report;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        report.warnings.push_back("there's no module " + std::to_string(song));
        return report;
    }

    // The marked loop starts at the module's second pass if its first plays unlike it, as when the channels come back
    // to the loop with the pan or volume that its end left them.
    const ModuleSong& entry = info.songs[size_t(song)];
    const uint32_t module = entry.module;
    Simulation sim = Simulate(rom, info, entry, settings.loops, LoopStart::kFirstPass);
    if (sim.plan.loops && !FirstPassRepeats(rom, info, Simulate(rom, info, entry, 2, LoopStart::kFirstPass)))
    {
        sim = Simulate(rom, info, entry, settings.loops, LoopStart::kSecondPass);
    }
    report.warnings = sim.warnings;
    if (sim.ticks.empty())
    {
        return report;
    }

    // The notes of each channel chosen.
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    std::vector<std::vector<Note>> notes(size_t(sim.channels));
    for (int c = 0; c < sim.channels; c++)
    {
        if ((settings.track_mask >> c) & 1)
        {
            notes[size_t(c)] = MakeNotes(rom, info, sim, c, instruments);
            report.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
    }

    // In a module with more than 15 channels, channels share MIDI channels. Only notes that sound at once matter.
    const std::vector<int> midi = MidiChannels(sim, notes);
    for (size_t a = 0; a < notes.size(); a++)
    {
        for (size_t b = a + 1; b < notes.size(); b++)
        {
            const uint64_t overlap = midi[a] == midi[b] ? Overlap(sim, notes[a], notes[b]) : 0;
            if (overlap > 0)
            {
                char text[128];
                std::snprintf(text, sizeof text,
                              "channels %zu and %zu share a MIDI channel, and their notes overlap for %.1f s", a + 1,
                              b + 1, double(overlap) / sim.mix_rate);
                report.warnings.push_back(text);
            }
        }
    }

    const Timing timing = MakeTiming(sim);
    const double rate = sim.mix_rate;
    report.seconds = double(sim.ticks[sim.plan.end].time) / rate;
    if (sim.plan.loops)
    {
        report.loop_start = double(sim.ticks[sim.plan.loop_start].time) / rate;
        report.loop_end = double(sim.ticks[sim.plan.loop_end].time) / rate;
    }
    report.bpm = 60e6 / timing.tempos.front().second;

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description. The description names the module by its place in
    // the list.
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number;
    const int place = int(std::find(info.modules.begin(), info.modules.end(), module) - info.modules.begin());
    char about[192];
    if (entry.song < 0)
    {
        std::snprintf(about, sizeof about, "%s (%s) module %d at 0x%08X, converted by %s", rom.Title().c_str(),
                      rom.GameCode().c_str(), place, unsigned(module), kProgramName);
    }
    else
    {
        std::snprintf(about, sizeof about, "%s (%s) song %d of module %d at 0x%08X, converted by %s",
                      rom.Title().c_str(), rom.GameCode().c_str(), entry.song, place, unsigned(module), kProgramName);
    }
    const std::string stem = SongStem(settings, number);

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, sim, timing, notes, midi, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    // The module's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        Sf2File file = own.Build(rom, info);
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
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

} // namespace agbsf2::krawall
