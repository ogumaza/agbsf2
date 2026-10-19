// SPDX-License-Identifier: MIT

#include "konami/sequencer.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace agbsf2::konami
{
namespace
{

// The number of commands a track can run without a delay before it's stopped.
constexpr int kMaxCommandsPerFrame = 4096;

// The Ultimate Masters and WCT 2004 revisions scale every delay by this tempo: frames = (delay * tempo + frac) >> 8.
// Here frac is the low byte of the previous result. The tempo starts at 0x100, and no command changes it. So frac stays
// 0. The Rave Master, Eternal Duelist and Dungeon Dice Monsters revisions don't scale their delays. RunTrack() scales
// theirs by kTempo too. At 0x100 that leaves them as they are.
constexpr uint32_t kTempo = 0x100;

// The flags that a song loop keeps in the Ultimate Masters and WCT 2004 revisions. Besides kTrackOn and kTrackRestarts,
// they're bits 0x10 and 0x08. Nothing sets those.
constexpr uint8_t kLoopKeptFlags = kTrackOn | kTrackRestarts | 0x10 | 0x08;

// The first quarter of the driver's vibrato waveform, trunc(4096 * sin(2 * pi * i / 256)) for i = 0 to 64; the rest
// follows by symmetry. The values are written out because a table computed with std::sin would only reach 4096 if sin()
// returned exactly 1 at the peak, which isn't guaranteed on every platform.
constexpr int16_t kQuarterSine[65] = {0,    100,  200,  301,  401,  501,  601,  700,  799,  897,  995,  1092, 1189,
                                      1284, 1379, 1474, 1567, 1659, 1751, 1841, 1930, 2018, 2105, 2191, 2275, 2358,
                                      2439, 2519, 2598, 2675, 2750, 2824, 2896, 2966, 3034, 3101, 3166, 3229, 3289,
                                      3348, 3405, 3460, 3513, 3563, 3612, 3658, 3702, 3744, 3784, 3821, 3856, 3889,
                                      3919, 3947, 3973, 3996, 4017, 4035, 4051, 4065, 4076, 4084, 4091, 4094, 4096};

// Returns the driver's vibrato waveform at `phase`, which runs through one cycle in 256 steps.
int VibratoSine(uint8_t phase)
{
    const int i = phase & 0x7F;
    const int value = kQuarterSine[i <= 64 ? i : 128 - i];
    return (phase & 0x80) ? -value : value;
}

// Routes DirectSound voice `voice` to echo bus `bus` - 1, or takes it off buses 0 and 1 if `bus` is 0, as F9 does in
// the Ultimate Masters and WCT 2004 revisions.
void RouteEcho(std::array<EchoBus, 3>& echo, int voice, int bus)
{
    if (voice > 11)
    {
        return;
    }

    if (bus == 0)
    {
        echo[0].voices &= uint16_t(~(1u << voice));
        echo[1].voices &= uint16_t(~(1u << voice));
    }
    else if (bus <= 3)
    {
        echo[size_t(bus - 1)].voices |= uint16_t(1u << voice);
    }
}

// Returns a track's flags as a song loop starts the track again: a track that can restart runs again, from its first
// frame. Each revision then keeps only some of the flags.
uint8_t RestartFlags(uint8_t flags)
{
    uint8_t f = flags & uint8_t(~kTrackStarted);
    if (f & kTrackRestarts)
    {
        f |= kTrackOn;
    }

    return f;
}

// Moves the vibrato of track `t` on by a frame in every revision but Dungeon Dice Monsters, and returns the vibrato's
// pitch offset in 1/32 semitones. At depth 0, the vibrato turns off and goes back to the start of its cycle.
template <typename Track>
int StepVibrato(Track& t)
{
    if (t.vib_depth == 0)
    {
        t.flags &= uint8_t(~kTrackVibrato);
        t.vib_phase = 0;
        return 0;
    }

    t.vib_phase = uint8_t(t.vib_phase + 0x18);
    return (VibratoSine(t.vib_phase) * t.vib_depth) >> 12;
}

// Silences the channel of track `t` through its output `o`. A stop or the end of a track does that in every revision
// but Dungeon Dice Monsters.
template <typename Track>
void Silence(Track& t, TrackOutput& o)
{
    t.vol = 0;
    o.b2 = 0;
    o.vol = 0;
    o.pitch = t.pitch;
    o.flags = kOutStop;
    o.trig = kTrigStop;
}

// Outputs a rest on the channel of track `t` through its output `o`, in every revision but Dungeon Dice Monsters. The
// first frame of a track starts with one too.
template <typename Track>
void Rest(Track& t, TrackOutput& o)
{
    o.pitch = t.pitch;
    o.flags = kOutStop;
    t.vol = 0;
    o.b2 = t.b2;
    o.trig = 1;
}

// Outputs the pitch of Dungeon Dice Monsters track `t` at volume `vol` through its output `o`, for notes and rests. A
// rest plays at volume 0. The first frame of a track starts with one.
void Play(DungeonDiceTrack& t, TrackOutput& o, int vol)
{
    o.pitch = t.pitch;
    o.flags = kOutDungeonDiceNote;
    t.vol = uint8_t(vol);
    o.b2 = t.b2;
    o.trig = 1;
}

// Counts a command that a call of track `t` plays. After the call's last command, the track's start and its read
// position `pos` go back to those before the call. With `keep_return`, the track keeps the return position and counts
// on. Every 256 commands it then jumps back there again.
template <typename Track>
void CountCallCommand(Track& t, uint32_t& pos, bool keep_return)
{
    if (t.ret == 0)
    {
        return;
    }

    t.count--;
    if (t.count == 0)
    {
        t.start = t.saved_start;
        pos = t.ret;
        t.ret = keep_return ? t.ret : 0;
    }
}

} // namespace

std::unique_ptr<Sequencer> Sequencer::Create(const Rom& rom, const DriverInfo& info, int song)
{
    switch (info.revision)
    {
    case Revision::kUltimateMasters:
        return std::make_unique<UltimateMastersSequencer>(rom, info, song);
    case Revision::kDungeonDiceMonsters:
        return std::make_unique<DungeonDiceSequencer>(rom, info, song);
    default:
        return std::make_unique<Wct2004Sequencer>(rom, info, song);
    }
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song) : rom_(rom)
{
    loop_frame_.fill(-1);

    // A song whose entry isn't in the ROM keeps an empty header, so it plays nothing.
    ReadSongHeader(rom, info.song_table, song, info.revision, header_);
}

bool Sequencer::AnyTrackActive() const
{
    for (int t = 0; t < kTracks; t++)
    {
        if (TrackActive(t))
        {
            return true;
        }
    }

    return false;
}

int Sequencer::LoopStartFrame() const
{
    if (loop_track_ < 0 || loop_frame_[size_t(loop_track_)] < 0)
    {
        return 0;
    }

    const int looping = loop_frame_[size_t(loop_track_)];
    int start = looping;
    for (int frame : loop_frame_)
    {
        if (frame >= 0 && frame >= looping - kLoopPointWindow)
        {
            start = std::min(start, frame);
        }
    }

    return start;
}

bool Sequencer::LoopPointDelayed() const
{
    for (int t = 0; t < kTracks; t++)
    {
        if (loop_frame_[size_t(t)] >= 0 && loop_delay_[size_t(t)] > 0)
        {
            return true;
        }
    }

    return false;
}

void Sequencer::Warn(int track, uint32_t addr, const std::string& what)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "frame %u, track %d, 0x%08X: ", frame_, track, addr);
    warnings_.push_back(buf + what);
}

bool Sequencer::HasNextTrack(int track, const Command& c)
{
    if (track + 1 < header_.tracks)
    {
        return true;
    }

    char buf[80];
    std::snprintf(buf, sizeof buf, "%02X on the last track writes into the driver's variables; ignored", c.opcode);
    Warn(track, c.addr, buf);

    return false;
}

void Sequencer::PassLoopPoint(int track, int delay)
{
    // Each loop point moves the track's start, so a loop takes the track back to the last one it passed. Those it
    // passes once the song has looped don't change where the loop starts.
    if (loops_ == 0)
    {
        loop_frame_[size_t(track)] = int(frame_);
        loop_delay_[size_t(track)] = delay;
    }
}

void Sequencer::EndForGood(int track)
{
    if (loops_ == 0)
    {
        loop_frame_[size_t(track)] = -1;
        loop_delay_[size_t(track)] = 0;
    }
}

const std::array<TrackOutput, kTracks>& Sequencer::Step()
{
    looped_last_frame_ = false;
    if (!valid_ || stopped_)
    {
        out_.fill(TrackOutput());
        frame_++;
        return out_;
    }

    // Tracks run from 15 down to 0. A loop command resets every track and the whole frame is run again from track 15,
    // as the driver does.
    bool hung = false;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        out_.fill(TrackOutput());
        bool restarted = false;
        for (int t = kTracks - 1; t >= 0; t--)
        {
            if (StepTrack(t) == Result::kRestart)
            {
                // Looping again in the rerun means the loop takes no time, and the driver would never finish the frame.
                // The song stops there, the frame plays nothing, and the loop doesn't count.
                if (attempt == 1)
                {
                    Warn(t, header_.base + TrackStart(t),
                         "song loops without waiting a frame, which would hang the driver; song stopped");
                    hung = true;
                    stopped_ = true;
                    looped_last_frame_ = false;
                    loops_--;
                    loop_track_ = loops_ > 0 ? loop_track_ : -1;
                    break;
                }

                if (loop_track_ < 0)
                {
                    loop_track_ = t;
                }

                ResetTracks();
                loops_++;
                looped_last_frame_ = true;
                restarted = true;
                break;
            }
        }
        if (!restarted)
        {
            break;
        }
    }

    // Clear output from a hung frame. Doing this where the loop is detected triggers a false array-bounds warning in
    // GCC 13 at -O3.
    if (hung)
    {
        out_.fill(TrackOutput());
    }

    for (int t = 0; t < kTracks; t++)
    {
        if (out_[t].active)
        {
            out_[t].note_pitch = NotePitch(t);
        }
    }

    frame_++;

    return out_;
}

template <typename TrackState>
Sequencer::Result TrackSequencer<TrackState>::RunTrack(int track, Revision revision)
{
    Track& t = tracks_[track];
    uint32_t pos;
    bool read_command;
    if (!(t.flags & kTrackStarted))
    {
        // First frame of the track: silence the channel, then read the initial delay.
        t.flags |= kTrackStarted;
        pos = 0;
        FirstFrame(t, out_[track]);
        read_command = false;
    }
    else
    {
        t.delay = uint16_t(t.delay - 1);
        if (t.delay != 0)
        {
            PerFrame(track);
            return Result::kContinue;
        }

        pos = t.pos;
        read_command = true;
    }

    for (int guard = 0;; guard++)
    {
        if (guard > kMaxCommandsPerFrame)
        {
            Warn(track, header_.base + t.start + pos, "too many commands without a delay; track stopped");
            t.flags = 0;
            return Result::kContinue;
        }

        if (read_command)
        {
            const Command c = DecodeCommand(rom_, header_.base + t.start + pos, track, revision);
            pos += c.length;
            const Next next = RunCommand(track, c, pos);
            if (next == Next::kStop || next == Next::kRestart)
            {
                return next == Next::kRestart ? Result::kRestart : Result::kContinue;
            }
            if (next == Next::kReadDelay)
            {
                CountInCall(t, pos);
            }
        }

        read_command = true;

        const uint32_t daddr = header_.base + t.start + pos;
        if (!rom_.Contains(daddr, 1))
        {
            Warn(track, daddr, "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Result::kContinue;
        }

        uint32_t delay;
        pos += ReadDelay(rom_, daddr, revision, delay);
        t.pos = uint16_t(pos);
        pos = t.pos;
        t.delay = uint16_t((kTempo * delay) >> 8);
        if (delay == 0)
        {
            continue;
        }

        PerFrame(track);
        return Result::kContinue;
    }
}

UltimateMastersSequencer::UltimateMastersSequencer(const Rom& rom, const DriverInfo& info, int song)
    : TrackSequencer(rom, info, song), bend_kept_(info.bend_kept)
{
    if (!rom.Contains(header_.base))
    {
        return;
    }

    // Every track starts active and restartable by a song loop. That's the state a loop resets it to.
    StartTracks();
    ResetTracks();

    valid_ = true;
}

void UltimateMastersSequencer::ResetTracks()
{
    // Tracks that can restart run again. Every track starts over from its first frame and without vibrato.
    for (Track& t : tracks_)
    {
        t.flags = RestartFlags(t.flags) & kLoopKeptFlags;
        t.pos = 0;
        t.b2 = 0;
        t.vol = 0;
        t.vib_phase = 0;
        t.vib_depth = 0;
        t.bend = 0;
    }

    // The square channels restart at 50% duty.
    tracks_[0].b2 = 0x80;
    tracks_[1].b2 = 0x80;
}

Sequencer::Result UltimateMastersSequencer::StepTrack(int track)
{
    const Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    if (!(t.flags & kTrackOn))
    {
        return Result::kContinue;
    }

    o.active = true;
    o.pan_r = t.pan_r;
    o.pan_l = t.pan_l;

    return RunTrack(track, Revision::kUltimateMasters);
}

void UltimateMastersSequencer::FirstFrame(Track& t, TrackOutput& o)
{
    Rest(t, o);
}

UltimateMastersSequencer::Next UltimateMastersSequencer::RunCommand(int track, const Command& c, uint32_t& pos)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    switch (c.op)
    {
    case Op::kJump:
        if (c.value != 0)
        {
            return Next::kRestart;
        }

        stopped_ = true;
        Silence(t, o);
        return Next::kStop;

    case Op::kEndTrack:
    case Op::kEndTrackHard:
        t.flags &= kTrackRestarts;
        if (c.op == Op::kEndTrackHard)
        {
            t.flags = 0;
            EndForGood(track);
        }

        if (!o.Any())
        {
            Silence(t, o);
        }

        return Next::kStop;

    case Op::kEchoRoute:
        RouteEcho(echo_, c.value >> 4, c.value & 0x0F);
        break;

    case Op::kEchoDelay:
        if (c.value > 0x7F)
        {
            Warn(track, c.addr, "echo delay above 0x7F (unverified behaviour)");
        }

        echo_[0].delay = c.value;
        break;

    case Op::kEchoFeedback:
        if (c.value > 0x7F)
        {
            Warn(track, c.addr, "echo feedback above 0x7F (unverified behaviour)");
        }

        echo_[0].feedback = c.value;
        break;

    case Op::kLoopPoint:
        t.start = uint16_t(t.start + pos);
        pos = 0;
        PassLoopPoint(track);
        break;

    case Op::kPitchBend:
        {
            // The bend changes the pitch of the note that's playing. Some builds of the driver also keep it, and add it
            // to the notes after it and to vibrato.
            const int16_t bend = int16_t(c.value);
            o.key = t.key;
            o.pitch = int16_t(t.pitch + bend);
            o.flags |= kOutPitch;
            if (bend_kept_)
            {
                t.bend = bend;
            }
            break;
        }

    case Op::kVibrato:
        t.vib_depth = uint8_t(c.value >> 1);
        t.flags |= kTrackVibrato;
        break;

    case Op::kPan:
        if (c.value <= 0x3F)
        {
            t.pan_r = uint8_t(c.value);
            t.pan_l = 0x3F;
        }
        else
        {
            t.pan_r = 0x3F;
            t.pan_l = uint8_t(0x7F - c.value);
        }

        o.pan_r = t.pan_r;
        o.pan_l = t.pan_l;
        o.flags |= kOutPan;
        break;

    case Op::kRest:
        Rest(t, o);
        break;

    case Op::kPsgNote:
        if (track <= 2)
        {
            t.pitch = int16_t(c.note << 5);
            o.pitch = int16_t(t.bend + t.pitch);
        }
        else
        {
            t.pitch = int16_t(c.note);
            o.pitch = t.pitch;
        }

        o.flags = kOutPsgNote;
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kVolume:
        o.pitch = t.pitch;
        o.flags = kOutPsgNote;
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kNote:
        t.vol = uint8_t(c.vol);
        t.key = uint16_t(c.sample);
        t.b2 = uint8_t(c.sample);
        o.key = t.key;
        o.flags = kOutNoteOn;
        t.pitch = int16_t(c.semitone * 32);
        o.pitch = int16_t(t.bend + t.pitch);
        o.b2 = t.b2;
        o.vol = t.vol;
        break;

    case Op::kDuty:
        o.b2 = uint8_t(c.value);
        t.b2 = uint8_t(c.value);
        break;

    default:
        {
            // Opcodes 90-9F, F4-F6 and FA-FC, notes of sample numbers above EF and commands past the end of the ROM
            // decode as unknown ones, and the other ops belong to the older revisions.
            char buf[80];
            if (c.opcode >= 0xA0 && c.opcode < 0xF0 && c.sample > 0xEF)
            {
                std::snprintf(buf, sizeof buf, "note of sample %02X: samples above EF are unsupported; track stopped",
                              c.sample);
            }
            else
            {
                std::snprintf(buf, sizeof buf, "unsupported opcode %02X; track stopped", c.opcode);
            }
            Warn(track, c.addr, rom_.Contains(c.addr) ? buf : "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Next::kStop;
        }
    }

    return Next::kReadDelay;
}

void UltimateMastersSequencer::PerFrame(int track)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    if (t.flags & kTrackVibrato)
    {
        const int vib = StepVibrato(t);
        o.pitch = int16_t(t.bend + t.pitch + vib);
        o.b2 = t.b2;
        o.key = t.key;
        o.flags |= kOutPitch;
    }

    o.vol = t.vol;
}

Wct2004Sequencer::Wct2004Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : TrackSequencer(rom, info, song),
      revision_(info.revision),
      eternal_duelist_(info.revision == Revision::kEternalDuelist)
{
    if (!rom.Contains(header_.base))
    {
        return;
    }

    // Each of the song's tracks starts in the state that a loop resets it to: active and restartable by a song loop,
    // panned to the centre and with no attack or decay. The echo buses start with no voices and no feedback.
    StartTracks();
    ResetTracks();

    valid_ = true;
}

void Wct2004Sequencer::ResetTracks()
{
    // Tracks that can restart run again from their first frame and without vibrato. A loop keeps their attack and decay
    // rates. WCT 2004 also keeps their pan, and Rave Master centres it again.
    for (Track& t : tracks_)
    {
        t.flags = RestartFlags(t.flags) & kLoopKeptFlags;
        t.pos = 0;
        t.ret = 0;
        t.b2 = 0;
        t.vol = 0;
        t.vib_phase = 0;
        t.vib_depth = 0;
        t.attack = 0;
        t.decay = 0;
        t.pan = revision_ == Revision::kWct2004 ? t.pan : 0xFF;
    }

    // Eternal Duelist sets each PSG track's NR51 bits for both sides of its own channel, and each sample track's pan to
    // 33.
    if (eternal_duelist_)
    {
        for (int t = 0; t < kTracks; t++)
        {
            tracks_[t].pan = IsPsgTrack(t) ? uint8_t(0x11 << t) : 0x33;
        }
    }

    // The square channels restart at 50% duty.
    tracks_[0].b2 = 0x80;
    tracks_[1].b2 = 0x80;
}

Sequencer::Result Wct2004Sequencer::StepTrack(int track)
{
    const Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    // Eternal Duelist's NR51 takes the pan of every PSG track, running or not.
    if (eternal_duelist_)
    {
        o.pan = t.pan;
    }

    if (!(t.flags & kTrackOn))
    {
        return Result::kContinue;
    }

    o.active = true;
    o.pan = t.pan;
    o.pan_start = t.pan;

    return RunTrack(track, revision_);
}

void Wct2004Sequencer::FirstFrame(Track& t, TrackOutput& o)
{
    t.attack = 0;
    t.decay = 0;
    Rest(t, o);
}

void Wct2004Sequencer::CountInCall(Track& t, uint32_t& pos)
{
    CountCallCommand(t, pos, eternal_duelist_);
}

Wct2004Sequencer::Next Wct2004Sequencer::RunCommand(int track, const Command& c, uint32_t& pos)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    switch (c.op)
    {
    case Op::kJump:
        if (c.value != 0)
        {
            return Next::kRestart;
        }

        // The driver silences every track on the next frame, and agbsf2 ends the song here.
        stopped_ = true;
        Silence(t, o);
        return Next::kStop;

    case Op::kEndTrack:
        t.flags &= kTrackRestarts;
        if (!o.Any())
        {
            Silence(t, o);
        }

        return Next::kStop;

    case Op::kCall:
        // The call plays commands from the track's data, from its start in the song table, with the delay before each.
        // 9F plays `count` commands. Like 00-8F, the other opcodes also set the duty byte or the wave. That counts as
        // the call's first command.
        t.saved_start = t.start;
        t.ret = uint16_t(pos);
        t.start = header_.offsets[track];
        pos = uint32_t(c.value);
        if (!CallSetsFirst(c.opcode, revision_))
        {
            t.count = uint8_t(c.count);
            return Next::kReadDelayUncounted;
        }

        t.count = uint8_t(c.count + 1);
        t.b2 = uint8_t(DutyOrWave(c.opcode, revision_).value);
        o.b2 = t.b2;
        break;

    case Op::kEchoRoute:
        RouteEcho(echo_, c.value >> 4, c.value & 0x0F);
        break;

    case Op::kEchoDelay:
        echo_[size_t(c.value >> 7)].delay = c.value & 0x7F;
        break;

    case Op::kEchoFeedback:
        echo_[0].feedback = c.value & 0x7F;
        break;

    case Op::kLoopPoint:
        t.start = uint16_t(t.start + pos);
        pos = 0;
        PassLoopPoint(track);
        break;

    case Op::kPitchBend:
        // The bend changes the pitch of the note that's playing, and isn't kept. Eternal Duelist marks the change with
        // a flag of 1, in place of the others.
        o.key = t.b2;
        o.pitch = int16_t(t.pitch + c.value);
        o.flags = eternal_duelist_ ? kOutEternalDuelistPitch : o.flags | kOutPitch;
        break;

    case Op::kVibrato:
        t.vib_depth = uint8_t(c.value >> 1);
        t.flags |= kTrackVibrato;
        break;

    case Op::kPanLevels:
    case Op::kPsgPan:
        t.pan = uint8_t(c.value);
        o.pan = t.pan;
        break;

    case Op::kPairVolumes:
        // Put the high nibble in the next track's output record to set its voice's volume. That track has already run
        // this frame. After the last track, this would overwrite the driver's variables.
        t.vol = uint8_t(c.value & 15);
        if (HasNextTrack(track, c))
        {
            out_[size_t(track + 1)].vol = uint8_t(c.value >> 4);
            out_[size_t(track + 1)].trig = 1;
        }

        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kAttack:
        t.attack_rate = uint8_t(c.value);
        break;

    case Op::kDecay:
        t.decay_rate = uint8_t(c.value);
        break;

    case Op::kInstrument:
    case Op::kVolumeScale:
    case Op::kNop:
        // F6 and FA take effect only with tables and settings that the game gives the driver. agbsf2 leaves them out.
        break;

    case Op::kRest:
        t.attack = 0;
        t.decay = 0;
        Rest(t, o);
        break;

    case Op::kPsgNote:
        // The noise track's notes are pitches too, and the driver reads its noise table at them. Eternal Duelist only
        // retriggers the channel for a new volume, and otherwise changes the pitch of the note that's playing.
        t.pitch = int16_t(c.note << 5);
        o.pitch = t.pitch;
        o.flags = kOutPsgNote;
        if (eternal_duelist_ && c.vol == t.vol)
        {
            break;
        }

        t.attack = t.attack_rate ? 0xFF : t.attack;
        t.decay = t.decay_rate ? 0xFF : t.decay;
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kVolume:
        o.pitch = t.pitch;
        o.flags = kOutPsgNote;
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kNote:
        t.vol = uint8_t(c.vol);
        t.b2 = uint8_t(c.sample);
        o.key = uint16_t(c.sample);
        o.flags = kOutNoteOn;
        t.pitch = int16_t(c.semitone * 32);
        o.pitch = t.pitch;
        o.b2 = t.b2;
        o.vol = t.vol;
        break;

    case Op::kDuty:
    case Op::kWave:
        // A wave also goes into the wave channel's RAM at once, at the track's volume. agbsf2 leaves that out.
        t.b2 = uint8_t(c.value);
        o.b2 = t.b2;
        break;

    default:
        {
            // A command past the end of the ROM decodes as an unknown one, and the others belong to the Ultimate
            // Masters and Dungeon Dice Monsters revisions.
            char buf[80];
            std::snprintf(buf, sizeof buf, "unsupported opcode %02X; track stopped", c.opcode);
            Warn(track, c.addr, rom_.Contains(c.addr) ? buf : "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Next::kStop;
        }
    }

    return Next::kReadDelay;
}

void Wct2004Sequencer::PerFrame(int track)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    if (t.flags & kTrackVibrato)
    {
        const int vib = StepVibrato(t);

        // Eternal Duelist's flag of 1 replaces the flags the commands set, a note's start included.
        o.pitch = int16_t(t.pitch + vib);
        o.b2 = t.b2;
        o.key = t.b2;
        o.flags = eternal_duelist_ ? kOutEternalDuelistPitch : o.flags | kOutPitch;
    }

    // A PSG note's attack takes its volume from 1/16 of the note's volume up to 1.06 times it, and the decay then takes
    // it back down to 0.06 times it. When the level changes, the channel is retriggered at it.
    int level = t.vol;
    int before = level;
    if (t.attack)
    {
        const int from = t.attack;
        t.attack = uint8_t(std::max(0, from - t.attack_rate));
        before = ((0x10F - from) * t.vol) >> 8;
        level = ((0x10F - t.attack) * t.vol) >> 8;
    }
    else if (t.decay)
    {
        const int from = t.decay;
        t.decay = uint8_t(std::max(0, from - t.decay_rate));
        before = ((from + 0xF) * t.vol) >> 8;
        level = ((t.decay + 0xF) * t.vol) >> 8;
    }

    if (level != before)
    {
        if (o.flags == 0)
        {
            o.pitch = t.pitch;
        }

        o.b2 = t.b2;
        o.trig = 1;
    }

    o.vol = uint8_t(level);
}

DungeonDiceSequencer::DungeonDiceSequencer(const Rom& rom, const DriverInfo& info, int song)
    : TrackSequencer(rom, info, song),
      sample_map_(info.sample_map),
      sample_periods_(info.sample_period_table),
      vibrato_entry_(info.vibrato_entry)
{
    if (!rom.Contains(header_.base))
    {
        return;
    }

    // Start all tracks active and restartable on a song loop. That's the state a loop resets them to.
    StartTracks();
    ResetTracks();

    valid_ = true;
}

void DungeonDiceSequencer::ResetTracks()
{
    // Tracks that can restart run again from their first frame and without vibrato or a fade. A loop keeps each track's
    // note and its loop point.
    for (Track& t : tracks_)
    {
        t.flags = RestartFlags(t.flags) & (kTrackOn | kTrackRestarts);
        t.pos = 0;
        t.ret = 0;
        t.pitch = 0;
        t.b2 = 0;
        t.vol = 0;
        t.vib_depth = 0;
        t.vib_count = 0;
    }

    // The square channels restart at 50% duty, the PSG at full volume on both sides, and the wave RAM with wave 0.
    tracks_[0].b2 = 0x80;
    tracks_[1].b2 = 0x80;
    psg_volume_ = 0x77;
    loaded_wave_ = 0;
    wave_loads_++;
}

Sequencer::Result DungeonDiceSequencer::StepTrack(int track)
{
    if (track >= header_.tracks)
    {
        return Result::kContinue;
    }

    if (!(tracks_[track].flags & kTrackOn))
    {
        PerFrame(track);
        return Result::kContinue;
    }

    out_[track].active = true;

    return RunTrack(track, Revision::kDungeonDiceMonsters);
}

void DungeonDiceSequencer::FirstFrame(Track& t, TrackOutput& o)
{
    Play(t, o, 0);
}

void DungeonDiceSequencer::CountInCall(Track& t, uint32_t& pos)
{
    CountCallCommand(t, pos, true);
}

DungeonDiceSequencer::Next DungeonDiceSequencer::RunCommand(int track, const Command& c, uint32_t& pos)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    // Returns the sample that sample map entry `entry` names.
    auto sample_of = [&](int entry)
    {
        const uint32_t a = sample_map_ + uint32_t(entry);
        return sample_map_ && rom_.Contains(a) ? rom_.U8(a) : uint8_t(entry);
    };

    switch (c.op)
    {
    case Op::kJump:
        if (c.value != 0)
        {
            return Next::kRestart;
        }

        // The driver silences every track on the next frame, and agbsf2 ends the song here. The track ends first.
        stopped_ = true;
        [[fallthrough]];

    case Op::kEndTrack:
        t.flags &= kTrackRestarts;
        if (!o.Any())
        {
            t.vol = 0;
            o.b2 = 0;
            o.vol = 0;
            o.pitch = t.pitch;
            o.flags = kOutDungeonDiceNote;
            o.trig = 1;
        }

        return Next::kStop;

    case Op::kRest:
        Play(t, o, 0);
        break;

    case Op::kRelease:
        // A release is a rest during vibrato. Otherwise it fades the track out, and gives a square channel an envelope
        // that takes it down a step every 1/64 second.
        if (t.flags & kTrackVibrato)
        {
            Play(t, o, 0);
            break;
        }

        t.flags |= kTrackFade;
        t.fade = 4;
        t.fade_from = t.vol;
        o.key = 0x100;
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kFade:
        {
            Track* u = c.opcode & 1 ? NextTrack(track, c) : &t;
            if (u)
            {
                u->flags |= kTrackFade;
                u->fade = 4;
                u->fade_from = u->vol;
            }
            break;
        }

    case Op::kLoopPoint:
        // The loop starts with FA's two bytes, a delay and a command, which the track skips until the song loops.
        t.start = uint16_t(t.start + pos - 2);
        pos = 2;
        PassLoopPoint(track, rom_.U8(header_.base + t.start));
        break;

    case Op::kCall:
        // The call plays commands from the track's data, from its start in the song table, with the delay before each.
        // F5 starts with the command in its last byte, which counts as the call's first, and the delay after the call
        // is read from that byte too.
        t.saved_start = t.start;
        t.ret = uint16_t(pos);
        t.start = header_.offsets[track];
        t.count = uint8_t(c.count);
        pos = uint32_t(c.value);
        if (!CallSetsFirst(c.opcode, Revision::kDungeonDiceMonsters))
        {
            return Next::kReadDelayUncounted;
        }

        t.count++;
        RunDutyOrWave(track, rom_.U8(c.addr + 4));
        break;

    case Op::kPitchBend:
        t.pitch = int16_t(t.note * 16 + c.value);
        Play(t, o, t.vol);
        break;

    case Op::kSampleBend:
        {
            // The bend is from the note's pitch, or from note 24, a sample's rate, after a sample map note. A note that
            // starts this frame starts at the pitch, and otherwise the track's pitch becomes the period, which the
            // output stage hands to the voice's FIFO at once.
            Track* u = c.opcode & 1 ? NextTrack(track, c) : &t;
            if (!u)
            {
                break;
            }

            TrackOutput& uo = out_[size_t(u - tracks_.data())];
            const int pitch = (u->note ? u->note : 24) * 16 + 1 + c.value;
            if (uo.flags & kOutDungeonDiceNote)
            {
                u->pitch = int16_t(pitch);
            }
            else
            {
                u->pitch = int16_t(TableEntry(rom_, sample_periods_, pitch).value_or(0));
                uo.flags = kOutDungeonDicePeriod;
            }

            uo.pitch = u->pitch;
            break;
        }

    case Op::kPsgVolume:
        psg_volume_ = uint8_t(c.value);
        break;

    case Op::kPsgNote:
        t.note = uint8_t(c.note);
        t.pitch = int16_t(c.note < kDungeonDiceNoiseNote ? c.note << 4 : c.note + 15 * kDungeonDiceNoiseNote);
        t.flags &= uint8_t(~kTrackFade);
        Play(t, o, c.vol);
        break;

    case Op::kSampleAtNote:
        t.note = uint8_t(c.note);
        t.pitch = int16_t((c.note << 4) + 1);
        t.flags &= uint8_t(~(kTrackVibrato | kTrackFade));
        Play(t, o, c.vol);
        break;

    case Op::kNote:
        t.b2 = sample_of(c.sample);
        t.note = 0;
        t.pitch = 0;
        t.flags &= uint8_t(~(kTrackVibrato | kTrackFade));
        Play(t, o, c.vol);
        break;

    case Op::kNextNote:
        {
            // The next track has had its turn this frame, so the note goes straight into its record.
            Track* u = NextTrack(track, c);
            if (u)
            {
                TrackOutput& uo = out_[size_t(track + 1)];
                u->vol = uint8_t(c.vol);
                u->b2 = sample_of(c.sample);
                u->note = 0;
                u->pitch = 0;
                u->flags &= uint8_t(~(kTrackVibrato | kTrackFade));
                uo.vol = u->vol;
                uo.b2 = u->b2;
                uo.pitch = 0;
                uo.flags = kOutDungeonDiceNote;
                uo.trig = 1;
            }
            break;
        }

    case Op::kVolume:
        Play(t, o, c.vol);
        break;

    case Op::kSetVolume:
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kVibrato:
        t.flags &= uint8_t(~(kTrackVibrato | kTrackFade));
        t.vib_depth = uint8_t(c.value);
        if (c.value)
        {
            t.flags |= kTrackVibrato;
        }
        break;

    case Op::kDuty:
    case Op::kWave:
        RunDutyOrWave(track, c.opcode);
        break;

    case Op::kNop:
        break;

    default:
        {
            char buf[80];
            std::snprintf(buf, sizeof buf, "unsupported opcode %02X; track stopped", c.opcode);
            Warn(track, c.addr, rom_.Contains(c.addr) ? buf : "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Next::kStop;
        }
    }

    return Next::kReadDelay;
}

void DungeonDiceSequencer::RunDutyOrWave(int track, uint8_t op)
{
    // A wave goes into the wave RAM at once.
    const Command set = DutyOrWave(op, Revision::kDungeonDiceMonsters);
    if (set.op == Op::kWave)
    {
        loaded_wave_ = set.value;
        wave_loads_++;
    }

    tracks_[size_t(track)].b2 = uint8_t(set.value);
    out_[size_t(track)].b2 = uint8_t(set.value);
}

DungeonDiceSequencer::Track* DungeonDiceSequencer::NextTrack(int track, const Command& c)
{
    return HasNextTrack(track, c) ? &tracks_[size_t(track + 1)] : nullptr;
}

void DungeonDiceSequencer::PerFrame(int track)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    const uint8_t flags = t.flags;

    // Each frame of a fade subtracts a quarter of the starting volume. At the end, output duty byte 0 and volume 0.
    if (flags & kTrackFade)
    {
        o.pitch = t.pitch;
        t.fade--;
        if (t.fade == 0)
        {
            t.flags &= uint8_t(~kTrackFade);
            o.b2 = 0;
            o.trig = 1;
        }

        t.vol = uint8_t((t.fade * t.fade_from) >> 2);
    }

    // Vibrato outputs a pitch every 4th frame, alternating normal and lowered pitch. The vibrato section of the
    // frequency table has 4 entries per whole note.
    if (flags & kTrackVibrato)
    {
        int pitch = t.pitch;
        if ((pitch & 15) == 0)
        {
            pitch = pitch / 4 + vibrato_entry_;
        }

        t.vib_count++;
        if (t.vol != 0 && (t.vib_count & 3) == 0)
        {
            if (t.vib_count & 4)
            {
                pitch -= t.vib_depth;
            }

            o.pitch = int16_t(pitch);
            o.b2 = t.b2;
            o.flags = kOutDungeonDiceNote;
        }
    }

    o.vol = t.vol;
}

} // namespace agbsf2::konami
