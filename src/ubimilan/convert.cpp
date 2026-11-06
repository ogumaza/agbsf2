// SPDX-License-Identifier: MIT

// Song conversion for Ubisoft Milan's driver. The model plays a piece frame by frame, and its notes go on the song's
// beat, which BeatGrid works out from the frames they start and end on, since the driver counts frames.

#include "ubimilan/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "beat_grid.h"
#include "beat_midi.h"
#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "program.h"
#include "psg.h"
#include "rom.h"
#include "sf2.h"
#include "ubimilan/driver.h"
#include "ubimilan/sequencer.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{
namespace
{

// The channels that play notes, in the order of the MIDI file's tracks: the PSG's square 1, square 2 and wave, the kit
// on sequence channel 9, and the noise that channel 9's keys up to 11 play.
constexpr int kTracks = 5;
constexpr int kKitTrack = 3;
constexpr int kNoiseTrack = 4;

// The PSG channel of each track, or -1 for the kit.
constexpr std::array<int, kTracks> kPsgOfTrack = {0, 1, 2, -1, 3};

// The MIDI channel of each track. The kit plays on MIDI channel 11. Players treat notes on MIDI channel 10 as drums,
// and some ignore their note-offs or their samples' loops. The kit's notes require both.
constexpr std::array<int, kTracks> kMidiChannels = {0, 1, 2, 10, 3};

// The registers that set the PSG's level on each side: SOUNDCNT_L as the driver's init sets it, and SOUNDCNT_H as the
// sound init sets it. Every channel plays on both sides at a master volume of 3 of 7, and the PSG at 100%.
constexpr uint16_t kSoundcntL = 0xFF33;
constexpr uint16_t kSoundcntH = 0xA90E;

// The registers of the PSG after a frame, as the converter reads them: NR10, each channel's NRx1 and NRx2, its NRx3
// and NRx4, NR30, and the channels that the frame restarted.
struct PsgFrame
{
    uint16_t nr10 = 0;
    std::array<uint16_t, 4> control = {};
    std::array<uint16_t, 4> frequency = {};
    uint16_t nr30 = 0;
    uint8_t restarted = 0;
};

// The PSG's registers as the driver's writes leave them.
class PsgRegisterFile
{
public:
    // Takes in a write of the driver's. A write to NRx3 and NRx4 with bit 15 set restarts the channel.
    void Write(const RegisterWrite& w)
    {
        if (w.size != 2 || w.address < kNr10 || w.address > kNr43)
        {
            return;
        }

        const uint16_t value = uint16_t(w.value);
        if (w.address == kNr10)
        {
            frame_.nr10 = value;
        }
        else if (w.address == kNr30)
        {
            frame_.nr30 = value;
        }
        for (size_t h = 0; h < kControl.size(); h++)
        {
            if (w.address == kControl[h])
            {
                frame_.control[h] = value;
            }
            else if (w.address == kFrequency[h])
            {
                frame_.frequency[h] = value;
                frame_.restarted = uint8_t(frame_.restarted | ((value & 0x8000) ? 1 << h : 0));
            }
        }
    }

    // Returns the registers after a frame, and starts the next frame.
    PsgFrame EndFrame()
    {
        const PsgFrame frame = frame_;
        frame_.restarted = 0;
        return frame;
    }

private:
    // NR10 and NR30, each channel's NRx1 and NRx2, and its NRx3 and NRx4.
    static constexpr uint32_t kNr10 = 0x04000060;
    static constexpr uint32_t kNr30 = 0x04000070;
    static constexpr uint32_t kNr43 = 0x0400007C;
    static constexpr std::array<uint32_t, 4> kControl = {0x04000062, 0x04000068, 0x04000072, 0x04000078};
    static constexpr std::array<uint32_t, 4> kFrequency = {0x04000064, 0x0400006C, 0x04000074, kNr43};
    PsgFrame frame_;
};

// What the model played, up to the conversion's end, and the PSG's registers after each frame.
struct Simulation
{
    std::vector<Note> notes;
    std::vector<uint32_t> sequence_starts;
    std::vector<PsgFrame> psg;
    FramePlan plan;
    std::vector<std::string> warnings;
};

// Plays a song for the conversion. A piece that loops plays its list of sequences `loops` times from its start. From
// LoopStart::kSecondPass, it plays them `loops` times from its second pass, after the first pass. A piece that doesn't
// loop plays until its track stops and no voice plays. Notes that still sound at the end end with it. With `kit` false,
// the game's switch of channel 9's kit is off.
Simulation Simulate(const Rom& rom, const DriverInfo& info, int song, int loops, LoopStart start_pass, bool kit)
{
    Simulation sim;
    Sequencer seq(rom, info, song, kit);
    const bool looping = seq.Piece().Loops();
    const size_t count = seq.Piece().Parts();
    const bool second = start_pass == LoopStart::kSecondPass;
    const size_t passes = size_t(std::max(loops, 1)) + (second ? 1 : 0);
    bool done = false;
    PsgRegisterFile registers;
    while (!done && seq.Frame() < kMaxFrames)
    {
        for (const RegisterWrite& w : seq.Step())
        {
            registers.Write(w);
        }
        sim.psg.push_back(registers.EndFrame());
        done = looping ? seq.SequenceStarts().size() > passes * count : !seq.Playing();
    }

    sim.sequence_starts = seq.SequenceStarts();
    sim.warnings = seq.Warnings();
    if (looping && done)
    {
        sim.plan.loops = true;
        sim.plan.loop_start = sim.sequence_starts[second ? count : 0];
        sim.plan.loop_end = sim.sequence_starts[(second ? 2 : 1) * count];
        sim.plan.end = sim.sequence_starts[passes * count];
    }
    else
    {
        sim.plan.end = std::max<uint32_t>(seq.Frame(), 1);
    }
    if (!done)
    {
        sim.warnings.push_back("it was cut off after an hour");
    }

    for (Note n : seq.Notes())
    {
        if (n.on >= sim.plan.end)
        {
            continue;
        }

        if (!n.ended || n.off > sim.plan.end)
        {
            n.ended = true;
            n.off = sim.plan.end;
        }
        sim.notes.push_back(n);
    }

    return sim;
}

// Returns true if the notes of a looping song's second pass start as those of its first, in `sim`'s first two passes:
// the same keys at the same frames from the pass's start, with the same velocity, kit offset or instrument, and sample.
// The kit's offset is the setting that a pass can leave to the next.
bool FirstPassRepeats(const Simulation& sim)
{
    if (!sim.plan.loops)
    {
        return true;
    }

    PassComparison passes(sim.plan.loop_start, sim.plan.loop_end - sim.plan.loop_start);
    for (const Note& n : sim.notes)
    {
        passes.Add(n.on, n.channel, n.key, {double(n.velocity), double(n.program), double(n.sample)});
    }

    return passes.Alike();
}

// Plays a song for the conversion, with `loops` passes of its loop. A piece that loops whose first pass plays unlike
// the next marks its second pass. With `kit` false, the game's switch of channel 9's kit is off.
Simulation SimulateConversion(const Rom& rom, const DriverInfo& info, int song, int loops, bool kit)
{
    Simulation sim = Simulate(rom, info, song, loops, LoopStart::kFirstPass, kit);
    if (sim.plan.loops && !FirstPassRepeats(Simulate(rom, info, song, 2, LoopStart::kFirstPass, kit)))
    {
        sim = Simulate(rom, info, song, loops, LoopStart::kSecondPass, kit);
    }

    return sim;
}

// Returns the track of a note, or -1 if it plays nothing.
int TrackOf(const Note& n)
{
    if (n.channel < kPsgChannels)
    {
        return n.channel;
    }
    if (PlaysNoise(n.channel, n.key))
    {
        return kNoiseTrack;
    }

    return PlaysKit(n.channel, n.key) ? kKitTrack : -1;
}

// Returns the name of a track.
const char* TrackName(int t)
{
    return t == kNoiseTrack ? "Noise" : ChannelName(t == kKitTrack ? kKitChannel : t);
}

// Returns a note's MIDI velocity, or 0 if the driver plays it silently. A kit note's velocity scales its sample, 64 to
// the whole scale. A square's or the noise's starting volume scales the PSG, and an envelope that rises from 0 is taken
// at full volume. The wave's volume code plays it at 100%, 50% or 25%. SquareRootValue() turns the level into the
// velocity.
int Velocity(const Note& n)
{
    double level = 0;
    if (PlaysKit(n.channel, n.key))
    {
        level = n.velocity / 64.0;
    }
    else if (n.channel == kWaveChannel)
    {
        static const double kWaveLevels[4] = {0, 1, 0.5, 0.25};
        level = kWaveLevels[(n.envelope >> 5) & 3];
    }
    else
    {
        const int volume = n.envelope >> 4;
        const bool rises = (n.envelope & 8) && (n.envelope & 7);
        level = (volume ? volume : rises ? 15 : 0) / 15.0;
    }

    return level > 0 ? SquareRootValue(level, 1, 1) : 0;
}

// Returns the kit's notes that sound on sequence channel 9, if `track_mask` sets its bit, and adds their keys to
// `instruments`.
std::vector<Note> KitNotes(const Simulation& sim, uint32_t track_mask, InstrumentSet& instruments)
{
    std::vector<Note> notes;
    for (const Note& n : sim.notes)
    {
        if (TrackOf(n) == kKitTrack && (track_mask & (1 << n.channel)) && Velocity(n))
        {
            notes.push_back(n);
            instruments.AddKitKey(n.program, n.key, n.sample, n.skip);
        }
    }

    return notes;
}

// A PSG channel's sound in a frame.
struct Sound
{
    uint32_t frame = 0;
    double level = 0; // the level on each side, with a full-scale FIFO sample at kFifoLevel
    double bend = 0;  // semitones from the note's key
    std::vector<std::pair<double, double>> sweeps; // the frequency sweep's steps in the frame: (place 0-1, bend)
};

// A note of a PSG channel as the MIDI file plays it: one run of frames in which the channel sounds, from a note-on to
// the next on the channel.
struct PsgNote
{
    int key = 60;
    int program = 0; // the instrument, or the noise's drum
    uint32_t on = 0; // frames
    uint32_t off = 0;
    std::vector<Sound> sounds; // one for each of its frames
    NoiseSound noise;          // a noise note's drum
};

// Returns the pitch in Hz at which a square zone plays key `key`, or a wave zone of program `program`, as a square's
// pitch at the same frequency setting.
double ZoneHz(const Rom& rom, const DriverInfo& info, int channel, int key, int program)
{
    const double equal = kMiddleC * std::pow(2.0, (key - 60) / 12.0);
    if (channel != kWaveChannel)
    {
        return equal * std::pow(2.0, SquareCents(rom, info, key) / 1200.0);
    }

    const int offset = WaveKeyOffset(ProgramWave(rom, info, program));
    return 4 * kMiddleC * std::pow(2.0, (key - offset - 60) / 12.0) *
           std::pow(2.0, WaveCents(rom, info, key, offset) / 1200.0);
}

// Returns track `t`'s notes, which play PSG channel kPsgOfTrack[t], if `track_mask` sets the bit of its sequence
// channel. Adds their instruments to `instruments`. Each note-on starts a stretch of the channel, which lasts until the
// channel's next note-on or the conversion's end. A model of the PSG plays the driver's registers through each stretch:
// its envelope, length counter and sweep, and the frequency that the modulation and the glides write. The MIDI notes
// are the runs of frames in which the channel sounds.
std::vector<PsgNote> PsgNotes(const Rom& rom, const DriverInfo& info, const Simulation& sim, int t, uint32_t track_mask,
                              InstrumentSet& instruments)
{
    const int h = kPsgOfTrack[size_t(t)];
    std::vector<const Note*> starts;
    for (const Note& n : sim.notes)
    {
        if (TrackOf(n) == t && (track_mask & (1 << n.channel)))
        {
            starts.push_back(&n);
        }
    }

    std::vector<PsgNote> notes;
    PsgFeed feed(h,
                 [&sim, h](uint32_t f)
                 {
                     const PsgFrame& r = sim.psg[f];
                     return PsgRegisters{r.nr10, r.control[size_t(h)], r.frequency[size_t(h)], r.nr30,
                                         (r.restarted & (1 << h)) != 0};
                 });
    const PsgScale scale = PsgScaleOf(kSoundcntL, kSoundcntH);
    const uint32_t frames = std::min<uint32_t>(sim.plan.end, uint32_t(sim.psg.size()));
    for (size_t i = 0; i < starts.size(); i++)
    {
        const Note& n = *starts[i];
        const uint32_t last = std::min(i + 1 < starts.size() ? starts[i + 1]->on : sim.plan.end, frames);
        const double key_hz = h == 3 ? 0 : ZoneHz(rom, info, h, n.key, n.program);
        PsgNote note;
        bool open = false;
        const auto finish = [&](uint32_t off)
        {
            note.off = off;
            if (h == 3)
            {
                note.noise.Trim();
                const int number = instruments.AddNoise(note.noise);
                note.program = number / kKitDrums;
                note.key = kFirstDrumKey + number % kKitDrums;
            }
            else if (h == kWaveChannel)
            {
                instruments.AddWave(note.program);
            }
            else
            {
                instruments.AddSquare(h, note.program, uint8_t(sim.psg[note.on].control[size_t(h)]));
            }
            notes.push_back(note);
            open = false;
        };

        for (uint32_t f = n.on; f < last; f++)
        {
            // The channel sounds while it's on, at a volume above 0 or with an envelope that rises from 0. The wave
            // sounds while NR32 doesn't mute it.
            const int volume = feed.Volume(f);
            const PsgFrame& r = sim.psg[f];
            const int envelope = r.control[size_t(h)] >> 8;
            const double share = h == kWaveChannel ? WaveShare(r.control[size_t(h)]) : 1;
            const bool rises = (envelope & 8) && (envelope & 7);
            const bool sounds = h == kWaveChannel ? volume >= 0 && share > 0 : volume > 0 || (volume == 0 && rises);
            if (!sounds)
            {
                if (open)
                {
                    finish(f);
                }
                continue;
            }

            if (!open)
            {
                note = PsgNote();
                note.key = n.key;
                note.program = n.program;
                note.on = f;
                open = true;
            }

            // The level of a square follows its volume. The wave sample contains the wave steps, and NR32 scales them.
            // The noise drum contains the noise volume.
            Sound sound;
            sound.frame = f;
            const double level = h == kWaveChannel ? 2 * kPsgLevelPerVolume * 15 * share
                                 : h == 3          ? kPsgLevelPerVolume * 15
                                                   : kPsgLevelPerVolume * volume;
            sound.level = level * scale.left;
            if (h == 3)
            {
                note.noise.AddFrame(r.control[3], r.frequency[3], (r.restarted & 8) != 0);
            }
            else
            {
                const PsgModel& model = feed.Model();
                sound.bend = 12 * std::log2(SquareHz(model.Frequency()) / key_hz);
                for (const auto& [at, frequency] : model.Sweeps())
                {
                    sound.sweeps.emplace_back(at, 12 * std::log2(SquareHz(frequency) / key_hz));
                }
            }
            note.sounds.push_back(sound);
        }

        if (open)
        {
            finish(last);
        }
    }

    return notes;
}

// Returns the song's beat. It comes from the frames on which every channel's notes start and end, including the
// channels that the conversion leaves out. Conversions of different channels then line up. Each channel's first event
// is a rest at the piece's start. The files then start where the piece starts, even when it starts with silence.
BeatGrid SongGrid(const Simulation& sim)
{
    std::vector<std::vector<uint32_t>> starts(kTracks);
    for (const Note& n : sim.notes)
    {
        const int c = TrackOf(n);
        if (c >= 0 && Velocity(n))
        {
            if (starts[size_t(c)].empty())
            {
                starts[size_t(c)].push_back(0);
            }
            starts[size_t(c)].push_back(n.on);
            starts[size_t(c)].push_back(n.off);
        }
    }
    for (std::vector<uint32_t>& frames : starts)
    {
        std::sort(frames.begin(), frames.end());
    }

    std::vector<uint32_t> hints = sim.sequence_starts;
    hints.erase(std::remove_if(hints.begin(), hints.end(), [&](uint32_t f) { return f >= sim.plan.end; }), hints.end());

    return BeatGrid(starts, hints);
}

// Writes to `mt` the kit's track. The kit plays `notes`, at least one. The track holds its name, its end at tick `end`,
// and the notes at the ticks that `ticks` gives them. The kit plays on its MIDI channel in kMidiChannels, in the bank
// that PresetBank() gives sequence channel 9. At tick 0, the track sets the first note's bank and program. It sets CC7
// and CC11 to 127, CC10 to 64, and CC91 and CC93 to 0. The notes' velocities hold their levels. The mixer is mono and
// adds no reverb or chorus. The kit writes no bends. The track therefore sets no bend range. Unless the loop starts
// with the song, the kit gives its bank and program again at its first note from tick `loop_start`.
void WriteKit(MidiTrack& mt, const std::vector<Note>& notes, const BeatTicks& ticks, uint32_t loop_start, uint32_t end)
{
    const int c = kKitTrack;
    mt.Name(TrackName(c));
    mt.SetEnd(end);
    const int preset_bank = PresetBank(kKitChannel);
    const int midi_channel = kMidiChannels[c];
    int bank = preset_bank;
    int program = notes.front().program;
    mt.Bank(0, midi_channel, bank);
    mt.Program(0, midi_channel, program);
    mt.Control(0, midi_channel, cc::kVolume, 127);
    mt.Control(0, midi_channel, cc::kPan, 64);
    mt.Control(0, midi_channel, cc::kExpression, 127);
    mt.Control(0, midi_channel, cc::kReverb, 0);
    mt.Control(0, midi_channel, cc::kChorus, 0);
    bool restated = loop_start == 0;
    for (const Note& n : notes)
    {
        const uint32_t on = ticks.Channel(c, n.on, end);
        const uint32_t off = std::max(on + 1, ticks.Channel(c, n.off, end));
        if (!restated && on >= loop_start)
        {
            bank = program = -1;
            restated = true;
        }
        if (bank != preset_bank)
        {
            mt.Bank(on, midi_channel, preset_bank);
            bank = preset_bank;
        }
        if (program != n.program)
        {
            mt.Program(on, midi_channel, n.program);
            program = n.program;
        }
        mt.NoteOn(on, midi_channel, n.key, Velocity(n));
        mt.NoteOff(off, midi_channel, n.key);
    }
}

// Writes to `mt` track `t`, which plays a PSG channel. The notes `notes`, at least one, play at full velocity. In each
// frame, CC11 sets the channel level, and the bend sets the semitones from the note key to the channel frequency. The
// bend range covers the widest bend. The noise notes play drums at a fixed pitch.
void WritePsgTrack(MidiTrack& mt, int t, const std::vector<PsgNote>& notes, const BeatTicks& ticks, uint32_t loop_start,
                   uint32_t end)
{
    mt.Name(TrackName(t));
    mt.SetEnd(end);
    const int bank = t == kNoiseTrack ? kNoiseBank : PresetBank(t);
    double widest = 0;
    for (const PsgNote& n : notes)
    {
        for (const Sound& s : n.sounds)
        {
            widest = std::max(widest, std::fabs(s.bend));
            for (const auto& [at, bend] : s.sweeps)
            {
                widest = std::max(widest, std::fabs(bend));
            }
        }
    }
    const int range = BendRangeFor(widest, kSmallBend);
    const auto tick = [&](double frame)
    {
        return ticks.Channel(t, frame, end);
    };

    ChannelWriter writer(mt, kMidiChannels[size_t(t)], notes[0].program, {bank, notes[0].program}, range, loop_start);
    for (const PsgNote& n : notes)
    {
        const uint32_t on = tick(n.on);
        const uint32_t off = tick(n.off);
        if (on >= off)
        {
            continue;
        }

        writer.SetProgram(on, n.program, {bank, n.program});
        for (const Sound& s : n.sounds)
        {
            const uint32_t at = tick(s.frame);
            writer.SetLevels(at, s.level, s.level);
            writer.SetBend(at, s.bend);
            for (const auto& [place, bend] : s.sweeps)
            {
                const uint32_t step_at = tick(s.frame + place);
                writer.Reach(step_at);
                if (s.frame + place < n.off)
                {
                    writer.SetBend(step_at, bend);
                }
            }
        }
        writer.Note(on, off, std::clamp(n.key, 0, 127));
    }
}

// Writes the MIDI file: a conductor track with the tempo map, then a track for each channel that plays notes: the
// PSG's three, the kit and the noise. Each event goes on the song's beat, or with `frame_timing`, on the frame the
// driver plays it in.
bool WriteMidi(const std::string& path, const std::string& title, const std::string& about, const FramePlan& plan,
               const BeatGrid& grid, bool frame_timing, const std::array<std::vector<PsgNote>, kTracks>& psg,
               const std::vector<Note>& kit, std::string& error)
{
    const BeatTicks ticks(grid, frame_timing);
    const uint32_t end = ticks.Song(plan.end);
    MidiFile midi(kBeatQuarterTicks);
    WriteBeatConductor(midi.AddTrack(), title, about, ticks, plan, end);

    const uint32_t loop_start = plan.loops ? ticks.Marker(plan.loop_start) : 0;
    for (int t = 0; t < kTracks; t++)
    {
        if (t == kKitTrack)
        {
            if (!kit.empty())
            {
                WriteKit(midi.AddTrack(), kit, ticks, loop_start, end);
            }
        }
        else if (!psg[size_t(t)].empty())
        {
            WritePsgTrack(midi.AddTrack(), t, psg[size_t(t)], ticks, loop_start, end);
        }
    }

    return WriteFile(path, midi.Encode(), error);
}

// Converts a song, or with `write` false, works out what the conversion would be.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write)
{
    SongReport report;
    MusicPiece piece;
    if (!ReadMusic(rom, info, song, piece))
    {
        report.warnings.push_back("its list of sequences can't be read");
        return report;
    }

    // The model's notes, those that the files hold and their instruments, and the song's beat. The simulation's
    // warnings come first.
    const Simulation sim = SimulateConversion(rom, info, song, settings.loops, KitPlays(info, song, settings.version));
    report.warnings = sim.warnings;
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    std::array<std::vector<PsgNote>, kTracks> psg;
    for (int t = 0; t < kTracks; t++)
    {
        if (t != kKitTrack)
        {
            psg[size_t(t)] = PsgNotes(rom, info, sim, t, settings.track_mask, instruments);
            report.tracks += psg[size_t(t)].empty() ? 0 : 1;
        }
    }
    const std::vector<Note> kit = KitNotes(sim, settings.track_mask, instruments);
    report.tracks += kit.empty() ? 0 : 1;
    const BeatGrid grid = SongGrid(sim);

    // The length, the loop and the first tempo.
    report.seconds = FrameSeconds(sim.plan.end);
    if (sim.plan.loops)
    {
        report.loop_start = FrameSeconds(sim.plan.loop_start);
        report.loop_end = FrameSeconds(sim.plan.loop_end);
    }
    report.bpm = 60 / FrameSeconds(grid.Tempos().front().frames);

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description.
    const std::vector<SongVersion> versions = SongVersions(info, song);
    const SongVersion& picked = versions[size_t(std::clamp(settings.version, 0, int(versions.size()) - 1))];
    const std::string named = picked.name.empty() ? "" : " " + picked.name;
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number + named;
    char about[192];
    std::snprintf(about, sizeof about, "%s (%s) music %d%s, resource %d at 0x%08X, converted by %s",
                  rom.Title().c_str(), rom.GameCode().c_str(), song, named.c_str(), info.songs[size_t(song)],
                  unsigned(piece.address), kProgramName);
    const std::string stem = SongStem(settings, number) + picked.suffix;

    std::string error;
    report.midi_path = stem + ".mid";
    if (!WriteMidi(report.midi_path, title, about, sim.plan, grid, settings.frame_timing, psg, kit, error))
    {
        report.warnings.push_back(error);
        return report;
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        Sf2File file;
        for (const std::string& w : own.Build(rom, info, file))
        {
            report.warnings.push_back(w);
        }
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
    if (!KitPlays(info, song, 0))
    {
        versions.push_back({"with the kit", "_kit"});
    }

    return versions;
}

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

} // namespace agbsf2::ubimilan
