// SPDX-License-Identifier: MIT

#include "mp2k/sequencer.h"

#include <algorithm>

#include "warnings.h"

namespace agbsf2::mp2k
{
namespace
{

// A track's flags.
constexpr uint8_t kFlagVolSet = 0x01;
constexpr uint8_t kFlagVolChange = 0x03;
constexpr uint8_t kFlagPitSet = 0x04;
constexpr uint8_t kFlagPitChange = 0x0C;
constexpr uint8_t kFlagVolPitChange = kFlagVolChange | kFlagPitChange;
constexpr uint8_t kFlagStart = 0x40;
constexpr uint8_t kFlagExist = 0x80;

// The settings that a track sets, for the conversion's reports.
constexpr uint8_t kSetVolume = 0x01;
constexpr uint8_t kSetPan = 0x02;
constexpr uint8_t kSetPitch = 0x04;

// 2^31 * 2^(n/12), rounded: a semitone's steps in the driver's table of sample rates, an octave below key 180.
constexpr uint32_t kFreqTable[12] = {2147483648u, 2275179671u, 2410468894u, 2553802834u, 2705659852u, 2866546760u,
                                     3037000500u, 3217589947u, 3408917802u, 3611622603u, 3826380858u, 4053909305u};

// The PSG's periods for the notes C2 to B2 at A4 = 440 Hz, negated: -131072 / f, rounded. A frequency setting is 2048
// less the period.
constexpr int16_t kPsgFreqTable[12] = {-2004, -1891, -1785, -1685, -1591, -1501,
                                       -1417, -1337, -1262, -1192, -1125, -1062};

// Returns (a * b) >> 32.
uint32_t MulHigh(uint32_t a, uint32_t b)
{
    return uint32_t((uint64_t(a) * b) >> 32);
}

// Returns a key's entry in the driver's table of sample rates: the semitone in the low 4 bits, and the octaves below
// keys 168-179 in the high 4.
uint32_t ScaleEntry(int key)
{
    return uint32_t(((14 - key / 12) << 4) | (key % 12));
}

// Sets a PSG channel's pan and its envelope's full and sustain levels from its volumes.
void SetPsgVolume(Channel& c)
{
    // A note panned at least two to one plays on one side only, at up to the full level. Otherwise it plays on both.
    bool one_side = false;
    if (c.right >= c.left && c.right / 2 >= c.left)
    {
        c.pan = 0x0F;
        one_side = true;
    }
    else if (c.right < c.left && c.left / 2 >= c.right)
    {
        c.pan = 0xF0;
        one_side = true;
    }

    const uint32_t sum = uint32_t(c.left) + c.right;
    c.goal = uint8_t(sum / 16);
    if (!one_side)
    {
        c.pan = 0xFF;
    }
    else if (c.goal > 15)
    {
        c.goal = 15;
    }

    c.sustain_goal = uint8_t((c.goal * c.sustain + 15) >> 4);
    c.pan &= c.pan_mask;
}

// Each part of a PSG channel's envelope ends in a step of its counter, the end of the frame's envelope work, or the
// channel's end.
enum class Next
{
    kStep,
    kDone,
    kOff
};

// Starts the echo after a PSG note's release. A silent echo ends the channel.
Next StartEcho(Channel& c)
{
    c.envelope = uint8_t((c.goal * c.echo_volume + 0xFF) >> 8);
    if (!c.envelope)
    {
        return Next::kOff;
    }

    c.status |= kStatusEcho;

    return Next::kDone;
}

// Starts a PSG channel's sustain, or its echo if the voice's sustain is 0.
Next StartSustain(Channel& c)
{
    if (c.sustain == 0)
    {
        c.status &= uint8_t(~kStatusEnvelope);
        return StartEcho(c);
    }

    c.status--;
    c.envelope = c.sustain_goal;
    c.counter = 7;

    return Next::kStep;
}

// Starts a PSG channel's decay, or its sustain if the voice's decay is 0.
Next StartDecay(Channel& c)
{
    c.status--;
    c.counter = c.decay;
    if (!c.counter)
    {
        return StartSustain(c);
    }

    c.envelope = c.goal;

    return Next::kStep;
}

// Moves a PSG channel's envelope a level on in its phase once the counter has run out, and sets the counter again.
Next StepEnvelope(Channel& c)
{
    if (c.counter != 0)
    {
        return Next::kStep;
    }

    SetPsgVolume(c);

    const uint8_t phase = c.status & kStatusEnvelope;
    if (phase == kPhaseRelease)
    {
        c.envelope--;
        if (int8_t(c.envelope) <= 0)
        {
            return StartEcho(c);
        }

        c.counter = c.release;
    }
    else if (phase == kPhaseSustain)
    {
        c.envelope = c.sustain_goal;
        c.counter = 7;
    }
    else if (phase == kPhaseDecay)
    {
        c.envelope--;
        if (int8_t(c.envelope) <= int8_t(c.sustain_goal))
        {
            return StartSustain(c);
        }

        c.counter = c.decay;
    }
    else
    {
        c.envelope++;
        if (c.envelope >= c.goal)
        {
            return StartDecay(c);
        }

        c.counter = c.attack;
    }

    return Next::kStep;
}

} // namespace

uint32_t KeyToFrequency(uint32_t frequency, int key, int fine)
{
    key &= 0xFF;
    uint32_t fraction = uint32_t(fine & 0xFF) << 24;
    if (key > 178)
    {
        key = 178;
        fraction = 255u << 24;
    }

    const uint32_t low = kFreqTable[ScaleEntry(key) & 0xF] >> (ScaleEntry(key) >> 4);
    const uint32_t high = kFreqTable[ScaleEntry(key + 1) & 0xF] >> (ScaleEntry(key + 1) >> 4);
    return MulHigh(frequency, low + MulHigh(high - low, fraction));
}

uint32_t CamelotKeyToFrequency(uint32_t frequency, int key, int fine)
{
    key &= 0xFF;
    uint32_t fraction = uint32_t(fine & 0xFF) << 24;
    if (key > 178)
    {
        key = 178;
        fraction = 255u << 24;
    }

    // The table holds each semitone's step to the next, which from B is to 2^32, and wraps to the same in 32 bits.
    const uint32_t semitone = ScaleEntry(key) & 0xF;
    const uint32_t octaves = ScaleEntry(key) >> 4;
    const uint32_t step = (semitone == 11 ? 0 : kFreqTable[semitone + 1]) - kFreqTable[semitone];
    return MulHigh(frequency, (kFreqTable[semitone] >> octaves) + MulHigh(step >> octaves, fraction));
}

uint32_t SampleFrequency(const DriverInfo& info, uint32_t frequency, int key, int fine)
{
    return info.camelot_sequencer ? CamelotKeyToFrequency(frequency, key, fine) : KeyToFrequency(frequency, key, fine);
}

uint32_t KeyToPsgFrequency(int channel, int key, int fine)
{
    key &= 0xFF;
    fine &= 0xFF;

    // The noise settings go up a step a key from key 21: through ratios 7 to 4 of each clock shift from 13 down to 0,
    // then ratios 3 to 0 of shift 0 at keys 77 to 80.
    if (channel == 4)
    {
        const int index = key <= 20 ? 0 : std::min(key - 21, 59);
        return index < 56 ? uint32_t(((13 - index / 4) << 4) | (7 - index % 4)) : uint32_t(59 - index);
    }

    if (key <= 35)
    {
        fine = 0;
        key = 0;
    }
    else
    {
        key -= 36;
        if (key > 130)
        {
            key = 130;
            fine = 255;
        }
    }

    const int low = kPsgFreqTable[key % 12] >> (key / 12);
    const int high = kPsgFreqTable[(key + 1) % 12] >> ((key + 1) / 12);
    return uint32_t(low + ((fine * (high - low)) >> 8) + 2048);
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song) : rom_(rom), info_(info)
{
    if (!ReadSong(rom, info, song, header_))
    {
        return;
    }

    // The song's tracks go on the player's, as far as it has room for them.
    valid_ = true;
    const int count = PlayedTracks(rom, info, song, header_);
    tracks_.resize(size_t(count));
    for (int t = 0; t < count; t++)
    {
        tracks_[size_t(t)].flags = kFlagExist | kFlagStart;
        tracks_[size_t(t)].position = header_.tracks[size_t(t)];
    }
    if (count < header_.track_count)
    {
        Warn("the song has " + std::to_string(header_.track_count) + " tracks, and its music player plays only " +
             std::to_string(count));
    }

    priority_ = header_.priority;

    for (int n = 0; n < 4; n++)
    {
        Channel& c = channels_[size_t(kFirstPsgChannel + n)];
        c.type = uint8_t(n + 1);
        c.pan_mask = uint8_t(0x11 << n);
    }
}

const std::vector<Action>& Sequencer::Step()
{
    actions_.clear();

    // Each 150 of the tempo is a tick. The tracks that existed at the start of a tick count as playing, so the player
    // stops in the tick after its last track ended, and then skips the update of the channels.
    if (valid_ && !(status_ & kPlayerStopped))
    {
        tempo_c_ = uint16_t(tempo_c_ + tempo_i_);
        bool stopped = false;
        while (tempo_c_ >= kTickCounter)
        {
            if (clock_ == restate_from_)
            {
                for (Track& track : tracks_)
                {
                    track.restate = kSetVolume | kSetPan | kSetPitch;
                }
            }

            uint32_t playing = 0;
            for (int t = 0; t < TrackCount(); t++)
            {
                if (tracks_[size_t(t)].flags & kFlagExist)
                {
                    playing |= 1u << t;
                    RunTrack(t);
                }
            }

            clock_++;
            if (!playing)
            {
                status_ = kPlayerStopped;
                stopped = true;
                break;
            }

            status_ = playing;
            tempo_c_ = uint16_t(tempo_c_ - kTickCounter);
        }
        if (!stopped)
        {
            UpdateChannels();
        }
    }

    RunPsg();
    RunDirect();
    frame_++;

    return actions_;
}

bool Sequencer::Ended() const
{
    if (!TracksEnded() && valid_)
    {
        return false;
    }

    for (int c = 0; c < kChannelCount; c++)
    {
        if ((c < info_.max_channels || c >= kFirstPsgChannel) && (channels_[size_t(c)].status & kStatusOn))
        {
            return false;
        }
    }

    return true;
}

void Sequencer::RunTrack(int t)
{
    Track& track = tracks_[size_t(t)];

    // Count down the notes' lengths, and take channels that have stopped off the track's list.
    for (int c = track.chan; c >= 0;)
    {
        Channel& ch = channels_[size_t(c)];
        const int next = ch.next;
        if (!(ch.status & kStatusOn))
        {
            UnlinkChannel(c);
        }
        else if (ch.gate && --ch.gate == 0)
        {
            ReleaseNote(t, c);
        }

        c = next;
    }

    if (track.flags & kFlagStart)
    {
        StartTrack(track);
    }

    // Run the commands that are due, up to the next wait. A track that ends doesn't move its LFO on.
    while (track.wait == 0)
    {
        if (!RunCommand(t))
        {
            return;
        }
    }

    track.wait--;

    // The LFO's shape is a triangle wave from -64 to 64, scaled by the depth. Camelot's sequencer counts the LFO's
    // delay down before it looks at the speed, so it counts it down at a speed of 0 too.
    if ((track.lfo_speed || info_.camelot_sequencer) && track.mod)
    {
        if (track.lfo_delay_c)
        {
            track.lfo_delay_c--;
        }
        else if (track.lfo_speed)
        {
            const uint32_t phase = uint32_t(track.lfo_speed_c) + track.lfo_speed;
            track.lfo_speed_c = uint8_t(phase);
            const int32_t shape = int8_t(uint8_t(phase - 64)) >= 0 ? 128 - int32_t(phase) : int8_t(uint8_t(phase));
            const int32_t depth = (int32_t(track.mod) * shape) >> 6;
            if (uint8_t(depth) != uint8_t(track.mod_m))
            {
                track.mod_m = int8_t(uint8_t(depth));
                track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
            }
        }
    }

    ReportTrack(t);
}

bool Sequencer::RunCommand(int t)
{
    Track& track = tracks_[size_t(t)];
    if (!rom_.Contains(track.position))
    {
        Warn("track " + std::to_string(t) + " runs past the end of the ROM, so it stops there");
        EndTrack(t);
        return false;
    }

    // A byte below 0x80 repeats the running status, with that byte as its first argument.
    uint8_t c = rom_.U8(track.position);
    if (c < 0x80)
    {
        c = track.running;
    }
    else
    {
        track.position++;
        if (c >= kCmdVoice)
        {
            track.running = c;
        }
    }

    auto arg = [&]()
    {
        return rom_.U8(track.position++);
    };

    if (c >= kCmdTie)
    {
        Note(t, c - kCmdTie);
        return true;
    }
    if (c < kCmdWait)
    {
        Warn("track " + std::to_string(t) +
             " has a byte below 0x80 before any command it could repeat, so it stops there");
        EndTrack(t);
        return false;
    }
    if (c <= kCmdLastWait)
    {
        track.wait = uint8_t(ClockLength(c - kCmdWait));
        return true;
    }

    switch (c)
    {
    case kCmdGoto:
        Jump(track);
        break;

    case kCmdPatt:
        // A pattern can call another, three deep. One more ends the track.
        if (track.pattern_level < 3)
        {
            track.stack[track.pattern_level++] = track.position + 4;
            Jump(track);
        }
        else
        {
            EndTrack(t);
        }
        break;

    case kCmdPend:
        if (track.pattern_level)
        {
            track.position = track.stack[--track.pattern_level];
        }
        break;

    case kCmdRept:
        {
            // A count of 0 repeats for ever. The track has one count, for all of its repeats.
            const uint8_t count = rom_.U8(track.position);
            if (count == 0 || ++track.repeats < count)
            {
                track.position++;
                Jump(track);
            }
            else
            {
                track.repeats = 0;
                track.position += 5;
            }
            break;
        }

    case kCmdMemAcc:
        MemAcc(t);
        break;

    case kCmdPrio:
        track.priority = arg();
        break;

    case kCmdTempo:
        {
            // The frame's progress has already been counted at the old tempo, and so has any of it left over for the
            // next tick.
            Action a;
            a.kind = Action::kTempo;
            a.track = uint8_t(t);
            a.old_tempo = tempo_i_;
            a.counter = tempo_c_ - kTickCounter;
            tempo_d_ = uint16_t(arg() * 2);
            tempo_i_ = uint16_t((uint32_t(tempo_d_) * tempo_u_) >> 8);
            a.value = tempo_i_;
            AddAction(a);
            break;
        }

    case kCmdKeySh:
        track.key_shift = int8_t(arg());
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case kCmdVoice:
        {
            // The track keeps a copy of the voice.
            const int voice = arg();
            ReadVoice(rom_, header_.voices + 12 * uint32_t(voice), track.tone);
            track.voice = voice;
            track.modified = false;
            Action a;
            a.kind = Action::kVoice;
            a.track = uint8_t(t);
            a.value = voice;
            AddAction(a);
            break;
        }

    case kCmdVol:
        track.vol = arg();
        track.flags |= kFlagVolChange;
        track.sets |= kSetVolume;
        break;

    case kCmdPan:
        track.pan = int8_t(uint8_t(arg() - 0x40));
        track.flags |= kFlagVolChange;
        track.sets |= kSetPan;
        break;

    case kCmdBend:
        track.bend = int8_t(uint8_t(arg() - 0x40));
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case kCmdBendR:
        track.bend_range = arg();
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case kCmdLfoS:
        track.lfo_speed = arg();
        if (!track.lfo_speed)
        {
            ClearModulation(track);
        }
        break;

    case kCmdLfoDl:
        track.lfo_delay = arg();
        break;

    case kCmdMod:
        track.mod = arg();
        if (!track.mod)
        {
            ClearModulation(track);
        }
        break;

    case kCmdModT:
        {
            const uint8_t type = arg();
            if (track.mod_t != type)
            {
                track.mod_t = type;
                track.flags |= kFlagVolPitChange;
            }
            break;
        }

    case kCmdTune:
        track.tune = int8_t(uint8_t(arg() - 0x40));
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case kCmdPort:
        track.position += 2;
        Warn("track " + std::to_string(t) + " writes to the sound registers, which the conversion leaves out");
        break;

    case kCmdXcmd:
        ExtendedCommand(t);
        break;

    case kCmdEot:
        EndTie(t);
        break;

    default:
        // kCmdFine, and the command numbers that the driver's command table gives the same handler.
        EndTrack(t);
        break;
    }

    return track.flags != 0;
}

void Sequencer::StartTrack(Track& track)
{
    // The driver clears the first 64 bytes of the track's state, which leaves its position and pattern stack.
    Track fresh;
    fresh.position = track.position;
    fresh.stack = track.stack;
    fresh.flags = kFlagExist;
    fresh.bend_range = 2;
    fresh.vol_x = 64;
    fresh.lfo_speed = 22;
    fresh.tone.type = 1;
    track = fresh;
}

void Sequencer::Note(int t, int length_index)
{
    Track& track = tracks_[size_t(t)];

    // The key, velocity and extra length can each be left out. The last key and velocity given carry on, and the extra
    // length is 0 if it's left out.
    track.gate = uint8_t(ClockLength(length_index));
    if (rom_.U8(track.position) < 0x80)
    {
        track.key = rom_.U8(track.position++);
        if (rom_.U8(track.position) < 0x80)
        {
            track.velocity = rom_.U8(track.position++);
            if (rom_.U8(track.position) < 0x80)
            {
                track.gate = uint8_t(track.gate + rom_.U8(track.position++));
            }
        }
    }

    Action a;
    a.kind = Action::kNoteOn;
    a.track = uint8_t(t);
    a.key = track.key;
    a.velocity = track.velocity;
    a.program = track.voice;
    a.modified = track.modified || track.offset != 0;
    a.voice = track.voice >= 0 ? header_.voices + 12 * uint32_t(track.voice) : 0;

    // A key split or drum kit picks a voice for the key from its table. A drum kit's voice plays at its own key and
    // pan.
    Voice voice = track.tone;
    uint8_t key = track.key;
    int8_t rhythm_pan = 0;
    if (track.tone.IsSplit())
    {
        const uint32_t index =
            track.tone.type & kVoiceKeySplit ? rom_.U8(track.tone.key_table + track.key) : uint32_t(track.key);
        a.voice = track.tone.wave + 12 * index;
        ReadVoice(rom_, a.voice, voice);
        if (voice.IsSplit())
        {
            a.kind = Action::kNoteDropped;
            AddAction(a);
            return;
        }

        if (track.tone.type & kVoiceDrumKit)
        {
            rhythm_pan = voice.RhythmPan();
            key = voice.key;
        }
    }

    const int priority = std::min(255, priority_ + track.priority);
    const int psg = voice.PsgChannel();
    int c = -1;
    if (psg <= 4)
    {
        c = psg || !info_.camelot_sequencer ? FindChannel(t, psg, priority) : FindCamelotChannel(t, priority);
    }
    if (c < 0)
    {
        a.kind = Action::kNoteDropped;
        AddAction(a);
        return;
    }

    // The note takes the channel from any note that had it, and goes first in the track's list.
    Channel& ch = channels_[size_t(c)];
    if (ch.status & kStatusOn)
    {
        EndNote(c, !(ch.status & kStatusStart));
    }
    UnlinkChannel(c);
    LinkChannel(c, t);

    track.lfo_delay_c = track.lfo_delay;
    if (track.lfo_delay)
    {
        ClearModulation(track);
    }
    UpdateTrack(track);

    ch.gate = track.gate;
    ch.midi_key = track.key;
    ch.velocity = track.velocity;
    ch.priority = uint8_t(priority);
    ch.key = key;
    ch.rhythm_pan = rhythm_pan;
    ch.type = voice.type;
    ch.wave = voice.wave;
    ch.attack = voice.attack;
    ch.decay = voice.decay;
    ch.sustain = voice.sustain;
    ch.release = voice.release;
    ch.echo_volume = track.echo_volume;
    ch.echo_length = track.echo_length;
    SetChannelVolume(ch, track);

    SetChannelPitch(ch, track);
    if (!psg)
    {
        ch.count = int32_t(track.offset);
    }

    ch.status = kStatusStart;
    ch.synth = false;
    ch.audible = false;
    track.flags &= uint8_t(~kFlagVolPitChange);

    a.channel = c;
    a.play_key = key;
    AddAction(a);
}

void Sequencer::EndTie(int t)
{
    Track& track = tracks_[size_t(t)];
    if (rom_.U8(track.position) < 0x80)
    {
        track.key = rom_.U8(track.position++);
    }

    // Release the newest of the track's notes with the key that hasn't been released yet.
    for (int c = track.chan; c >= 0; c = channels_[size_t(c)].next)
    {
        Channel& ch = channels_[size_t(c)];
        if ((ch.status & (kStatusStart | kStatusEnvelope)) && !(ch.status & kStatusStop) && ch.midi_key == track.key)
        {
            ReleaseNote(t, c);
            break;
        }
    }
}

void Sequencer::EndTrack(int t)
{
    // The track's notes are released, and it lets go of its channels.
    Track& track = tracks_[size_t(t)];
    for (int c = track.chan; c >= 0;)
    {
        Channel& ch = channels_[size_t(c)];
        const int next = ch.next;
        if ((ch.status & kStatusOn) && !(ch.status & kStatusStop))
        {
            ReleaseNote(t, c);
        }

        UnlinkChannel(c);
        c = next;
    }

    track.flags = 0;
}

void Sequencer::ExtendedCommand(int t)
{
    Track& track = tracks_[size_t(t)];
    const uint8_t command = rom_.U8(track.position++);

    // A key split keeps its key table where other voices keep their envelope.
    auto set_envelope = [&](uint8_t& field)
    {
        field = rom_.U8(track.position++);
        track.tone.key_table = track.tone.attack | (track.tone.decay << 8) | (track.tone.sustain << 16) |
                               (uint32_t(track.tone.release) << 24);
        track.modified = true;
    };

    switch (command)
    {
    case kXcmdWave:
        track.tone.wave = rom_.U32(track.position);
        track.position += 4;
        track.modified = true;
        break;

    case kXcmdType:
        track.tone.type = rom_.U8(track.position++);
        track.modified = true;
        break;

    case kXcmdAttack:
        set_envelope(track.tone.attack);
        break;

    case kXcmdDecay:
        set_envelope(track.tone.decay);
        break;

    case kXcmdSustain:
        set_envelope(track.tone.sustain);
        break;

    case kXcmdRelease:
        set_envelope(track.tone.release);
        break;

    case kXcmdEchoVol:
        track.echo_volume = rom_.U8(track.position++);
        break;

    case kXcmdEchoLen:
        track.echo_length = rom_.U8(track.position++);
        break;

    case kXcmdLength:
        track.tone.length = rom_.U8(track.position++);
        track.modified = true;
        break;

    case kXcmdSweep:
        track.tone.pan_sweep = rom_.U8(track.position++);
        track.modified = true;
        break;

    case kXcmdWait:
        {
            // The command runs again on each tick until its count runs out.
            const uint16_t length = rom_.U16(track.position);
            if (track.timer < length)
            {
                track.timer++;
                track.position -= 2;
                track.wait = 1;
            }
            else
            {
                track.timer = 0;
                track.position += 2;
            }
            break;
        }

    case kXcmdOffset:
        track.offset = rom_.U32(track.position);
        track.position += 4;
        break;

    default:
        if (command >= kXcmdCount)
        {
            Warn("track " + std::to_string(t) + " has an unknown extended command, so it stops there");
        }
        EndTrack(t);
        break;
    }
}

void Sequencer::MemAcc(int t)
{
    Track& track = tracks_[size_t(t)];
    const uint8_t op = rom_.U8(track.position);
    uint8_t& byte = memory_[rom_.U8(track.position + 1)];
    const uint8_t value = rom_.U8(track.position + 2);
    track.position += 3;

    // Operations 0-5 change the byte, with the value or with the byte the value names. Operations 6-17 jump if the byte
    // compares with them as each one says.
    if (op < 6)
    {
        const uint8_t operand = op >= 3 ? memory_[value] : value;
        byte = op % 3 == 0 ? operand : op % 3 == 1 ? uint8_t(byte + operand) : uint8_t(byte - operand);
        return;
    }
    if (op > 17)
    {
        return;
    }

    const uint8_t operand = op >= 12 ? memory_[value] : value;
    bool jump = false;
    switch (op % 6)
    {
    case 0:
        jump = byte == operand;
        break;
    case 1:
        jump = byte != operand;
        break;
    case 2:
        jump = byte > operand;
        break;
    case 3:
        jump = byte >= operand;
        break;
    case 4:
        jump = byte <= operand;
        break;
    default:
        jump = byte < operand;
        break;
    }

    if (jump)
    {
        Jump(track);
    }
    else
    {
        track.position += 4;
    }
}

void Sequencer::Jump(Track& track)
{
    track.position = rom_.U32(track.position);
}

void Sequencer::ClearModulation(Track& track)
{
    track.mod_m = 0;
    track.lfo_speed_c = 0;
    track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
    track.sets |= track.mod_t == 0 ? kSetPitch : track.mod_t == 1 ? kSetVolume : kSetPan;
}

void Sequencer::UpdateTrack(Track& track)
{
    // The volume and pan give each side a level, with the pan in a straight line from one side to the other.
    if (track.flags & kFlagVolSet)
    {
        uint32_t level = (uint32_t(track.vol) * track.vol_x) >> 5;
        if (track.mod_t == 1)
        {
            level = (level * uint32_t(track.mod_m + 128)) >> 7;
        }

        int32_t pan = 2 * track.pan;
        if (track.mod_t == 2)
        {
            pan += track.mod_m;
        }
        pan = std::clamp(pan, -128, 127);

        track.vol_mr = uint8_t((uint32_t(pan + 128) * level) >> 8);
        track.vol_ml = uint8_t((uint32_t(127 - pan) * level) >> 8);
    }

    // The pitch is in 256ths of a semitone: the key offset in the high bits, and the fine tune in the low 8.
    if (track.flags & kFlagPitSet)
    {
        const int32_t pitch = TrackPitch(track);
        track.key_m = int8_t(uint8_t(pitch >> 8));
        track.pit_m = uint8_t(pitch);
    }

    track.flags &= uint8_t(~(kFlagPitSet | kFlagVolSet));
}

void Sequencer::SetChannelVolume(Channel& c, const Track& track) const
{
    // Camelot's sequencer works out the left side's level from 128 less a drum kit voice's pan, where the driver takes
    // it from 127, so every note's left side comes out a little louder.
    const int right = (c.velocity * (128 + c.rhythm_pan) * track.vol_mr) >> 14;
    const int left = (c.velocity * ((info_.camelot_sequencer ? 128 : 127) - c.rhythm_pan) * track.vol_ml) >> 14;
    c.right = uint8_t(std::min(right, 255));
    c.left = uint8_t(std::min(left, 255));
}

void Sequencer::SetChannelPitch(Channel& c, const Track& track) const
{
    const int key = std::max(0, c.key + track.key_m);
    const int psg = c.type & kVoicePsg;
    if (psg)
    {
        c.frequency = KeyToPsgFrequency(psg, key, track.pit_m);
    }
    else
    {
        c.frequency = SampleFrequency(info_, rom_.U32(c.wave + 4), key, track.pit_m);
    }
}

void Sequencer::UpdateChannels()
{
    for (int t = 0; t < TrackCount(); t++)
    {
        Track& track = tracks_[size_t(t)];
        if (!(track.flags & kFlagExist) || !(track.flags & kFlagVolPitChange))
        {
            continue;
        }

        UpdateTrack(track);
        for (int c = track.chan; c >= 0;)
        {
            Channel& ch = channels_[size_t(c)];
            const int next = ch.next;
            if (!(ch.status & kStatusOn))
            {
                UnlinkChannel(c);
                c = next;
                continue;
            }

            if (track.flags & kFlagVolChange)
            {
                SetChannelVolume(ch, track);
            }
            if (track.flags & kFlagPitChange)
            {
                SetChannelPitch(ch, track);
            }

            c = next;
        }

        track.flags &= uint8_t(~kFlagVolPitChange);
    }
}

int Sequencer::FindChannel(int t, int psg, int priority) const
{
    // A PSG voice has one channel. It takes it if it's free, or plays a released note, or a note of lower priority, or
    // one of the same priority from this track or a later one.
    if (psg)
    {
        const int c = kFirstPsgChannel + psg - 1;
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn) || (ch.status & kStatusStop) || ch.priority < priority)
        {
            return c;
        }

        return ch.priority == priority && ch.track >= t ? c : -1;
    }

    // A sample takes the first free channel. Failing that, it takes a released note, or else a note of lower priority,
    // or of the same priority from this track or a later one: of those, the one with the lowest priority, from the
    // latest track, and the last in the list of channels.
    int found = -1;
    int lowest = priority;
    int latest = t + 1;
    bool released = false;
    for (int c = 0; c < info_.max_channels; c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn))
        {
            return c;
        }

        const int order = ch.track + 1;
        if ((ch.status & kStatusStop) && !released)
        {
            released = true;
            lowest = ch.priority;
            latest = order;
            found = c;
            continue;
        }
        if (!(ch.status & kStatusStop) && released)
        {
            continue;
        }

        if (ch.priority < lowest)
        {
            lowest = ch.priority;
            latest = order;
            found = c;
        }
        else if (ch.priority == lowest && order >= latest)
        {
            latest = order;
            found = c;
        }
    }

    return found;
}

int Sequencer::FindCamelotChannel(int t, int priority) const
{
    // A sample takes the third free channel, in the list's order. With only one or two free, it takes the last of them,
    // unless one of this track's released notes has a channel: then it takes the last of those.
    const int count = info_.max_channels;
    int free_count = 0;
    int last_free = -1;
    uint32_t released = 0;
    for (int c = 0; c < count; c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn))
        {
            if (++free_count == 3)
            {
                return c;
            }

            last_free = c;
        }
        else if (ch.status & kStatusStop)
        {
            released |= 1u << c;
        }
    }

    if (last_free >= 0)
    {
        for (int c = count - 1; c >= 0; c--)
        {
            if ((released & (1u << c)) && channels_[size_t(c)].track == t)
            {
                return c;
            }
        }

        return last_free;
    }

    // With none free, it takes a released note if there is one, or else a note of lower priority, or of the same
    // priority from this track or a later one: of those, the one with the lowest priority, from the latest track, and
    // the first in the list of channels.
    const uint32_t candidates = released ? released : (1u << count) - 1;
    int found = -1;
    int lowest = released ? 256 : priority;
    int latest = released ? 0 : t + 1;
    for (int c = count - 1; c >= 0; c--)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(candidates & (1u << c)))
        {
            continue;
        }

        const int order = ch.track + 1;
        if (ch.priority < lowest)
        {
            lowest = ch.priority;
            latest = order;
            found = c;
        }
        else if (ch.priority == lowest && order >= latest)
        {
            latest = order;
            found = c;
        }
    }

    return found;
}

void Sequencer::LinkChannel(int index, int t)
{
    Channel& ch = channels_[size_t(index)];
    Track& track = tracks_[size_t(t)];
    ch.prev = -1;
    ch.next = track.chan;
    if (track.chan >= 0)
    {
        channels_[size_t(track.chan)].prev = index;
    }

    track.chan = index;
    ch.track = t;
}

void Sequencer::UnlinkChannel(int index)
{
    Channel& ch = channels_[size_t(index)];
    if (ch.track < 0)
    {
        return;
    }

    if (ch.prev >= 0)
    {
        channels_[size_t(ch.prev)].next = ch.next;
    }
    else
    {
        tracks_[size_t(ch.track)].chan = ch.next;
    }
    if (ch.next >= 0)
    {
        channels_[size_t(ch.next)].prev = ch.prev;
    }

    ch.track = -1;
}

void Sequencer::RunPsg()
{
    // The steps of the hardware's envelopes are 64 a second, so every 15th frame takes two.
    c15_ = c15_ ? uint8_t(c15_ - 1) : 14;

    for (int n = 1; n <= 4; n++)
    {
        const int index = kFirstPsgChannel + n - 1;
        Channel& c = channels_[size_t(index)];
        if (!(c.status & kStatusOn))
        {
            continue;
        }

        // A note released before the channel started it never sounds.
        Next next = Next::kStep;
        const bool never_started = (c.status & kStatusStart) && (c.status & kStatusStop);
        if (never_started)
        {
            next = Next::kOff;
        }
        else if (c.status & kStatusStart)
        {
            c.status = kPhaseAttack;
            SetPsgVolume(c);
            c.counter = c.attack;
            if (c.attack)
            {
                c.envelope = 0;
            }
            else
            {
                next = StartDecay(c);
            }
        }
        else if (c.status & kStatusEcho)
        {
            c.echo_length--;
            next = int8_t(c.echo_length) <= 0 ? Next::kOff : Next::kDone;
        }
        else if ((c.status & kStatusStop) && (c.status & kStatusEnvelope))
        {
            c.status &= uint8_t(~kStatusEnvelope);
            c.counter = c.release;
            if (!c.release)
            {
                next = StartEcho(c);
            }
        }
        else
        {
            next = StepEnvelope(c);
        }

        // The counter steps once a frame, and twice every 15th frame.
        int c15 = c15_;
        while (next == Next::kStep)
        {
            c.counter--;
            if (c15 != 0)
            {
                next = Next::kDone;
                break;
            }

            c15--;
            next = StepEnvelope(c);
        }
        if (next == Next::kOff)
        {
            c.status = 0;
            EndNote(index, !never_started);
            continue;
        }

        CheckAudible(index);

        // A fixed-pitch voice's frequency is rounded to suit the output's resolution. The driver rounds it when it
        // changes, and rounding it again changes nothing.
        if (n < 4 && (c.type & kVoiceFixed))
        {
            if (info_.dac_bits < 0x40)
            {
                c.frequency = (c.frequency + 2) & 0x7FC;
            }
            else if (info_.dac_bits < 0x80)
            {
                c.frequency = (c.frequency + 1) & 0x7FE;
            }
        }
    }
}

void Sequencer::RunDirect()
{
    for (int i = 0; i < info_.max_channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!(c.status & kStatusOn))
        {
            continue;
        }

        // The mixer starts a note's sample, then moves its envelope on: up by the attack each frame, down by the decay
        // to the sustain level, and down by the release after the note's release, each a fraction out of 256. After the
        // release, the note can carry on at its echo's level for the echo's length.
        Wave wave;
        ReadWave(rom_, c.wave, wave);
        int envelope = c.envelope;
        bool echo = false;
        if (c.status & kStatusStart)
        {
            if (c.status & kStatusStop)
            {
                c.status = 0;
                EndNote(i, false);
                continue;
            }

            c.status = kPhaseAttack;
            c.position = wave.Data() + uint32_t(c.count);
            c.count = int32_t(wave.size - uint32_t(c.count));
            c.fraction = 0;
            envelope = 0;
            if (wave.loops)
            {
                c.status |= kStatusLoop;
            }
        }
        else if (c.status & kStatusEcho)
        {
            if (c.echo_length-- <= 1)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }
        }
        else if (c.status & kStatusStop)
        {
            // Camelot's mixer takes 256 less the release off the level each frame, in a straight line.
            envelope = info_.camelot_mixer ? envelope + c.release - 256 : (envelope * c.release) >> 8;
            echo = envelope <= c.echo_volume;
        }
        else if ((c.status & kStatusEnvelope) == kPhaseDecay)
        {
            envelope = (envelope * c.decay) >> 8;
            if (envelope <= c.sustain)
            {
                envelope = c.sustain;
                if (envelope == 0)
                {
                    echo = true;
                }
                else
                {
                    c.status--;
                }
            }
        }

        if ((c.status & (kStatusStop | kStatusEcho | kStatusEnvelope)) == kPhaseAttack)
        {
            envelope += c.attack;
            if (envelope >= 255)
            {
                envelope = 255;
                c.status--;
            }
        }
        if (echo)
        {
            envelope = c.echo_volume;
            if (envelope == 0)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }

            c.status |= kStatusEcho;
        }

        c.envelope = uint8_t(envelope);
        CheckAudible(i);

        // Camelot's mixer plays each side at 9/8 of the envelope, and leaves out a note that comes out silent on both,
        // which doesn't move on through its sample.
        if (info_.camelot_mixer)
        {
            const int level = envelope + (envelope >> 3);
            if (((c.right * level) >> 9) == 0 && ((c.left * level) >> 9) == 0)
            {
                continue;
            }
        }

        MixChannel(c, wave);
        if (!c.status)
        {
            EndNote(i, true);
        }
    }
}

void Sequencer::MixChannel(Channel& c, const Wave& wave)
{
    const int samples = info_.samples_per_frame;

    // Camelot's mixer takes a sample that has no points left when it comes to mix it, as one of no length is from the
    // start, as a synth voice from then on, unless it plays at a fixed pitch.
    if (info_.camelot_mixer && !(c.type & kVoiceFixed) && (c.synth || c.count == 0))
    {
        c.synth = true;
        MixSynth(c);
        return;
    }

    // A newer driver's mixer plays a reversed sample backwards from its end, and counts its way through a compressed
    // sample by the point rather than the address. A compressed voice with an uncompressed sample isn't mixed.
    const bool special = info_.special_samples && (c.type & (kVoiceReverse | kVoiceCompressed));
    const bool reverse = special && (c.type & kVoiceReverse);
    if (special && !(c.status & kStatusSpecial))
    {
        c.status |= kStatusSpecial;
        if (reverse)
        {
            c.position = 2 * c.wave + 32 + wave.size - c.position;
        }
        if (wave.type != 0)
        {
            c.position -= c.wave + 16;
        }
    }
    if (special && !reverse && wave.type == 0)
    {
        return;
    }

    // Otherwise a fixed-pitch sample plays a point for each point of the output. The driver takes a loop of no length
    // as no loop.
    const uint32_t loop_start = special ? wave.loop_start : wave.Data() + wave.loop_start;
    const int32_t loop_length = int32_t(wave.size - wave.loop_start);
    const bool loops = !reverse && (c.status & kStatusLoop) && loop_length > 0;
    if (!special && (c.type & kVoiceFixed))
    {
        for (int i = 0; i < samples; i++)
        {
            c.position++;
            if (--c.count == 0)
            {
                if (!loops)
                {
                    c.status = 0;
                    return;
                }

                c.position = loop_start;
                c.count = loop_length;
            }
        }

        return;
    }

    // Other samples move on by their rate, in 2^-23 points, for each point of the output. A sample that ends stops the
    // channel there, without saving its progress, and a loop goes back by its length as many times as it takes. A
    // reversed sample doesn't loop.
    const uint32_t step = special && (c.type & kVoiceFixed) ? 0x800000u : c.frequency * info_.step_scale;
    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t position = c.position;
    for (int i = 0; i < samples; i++)
    {
        fraction += step;
        const int32_t advance = int32_t(fraction >> 23);
        if (advance == 0)
        {
            continue;
        }

        fraction &= ~0x3F800000u;
        count -= advance;
        if (count > 0)
        {
            position = reverse ? position - uint32_t(advance) : position + uint32_t(advance);
            continue;
        }
        if (!loops)
        {
            c.status = 0;
            return;
        }

        int32_t overshoot = -count;
        count += loop_length;
        while (count <= 0)
        {
            overshoot -= loop_length;
            count += loop_length;
        }

        position = loop_start + uint32_t(overshoot);
    }

    c.fraction = fraction;
    c.count = count;
    c.position = position;
}

void Sequencer::MixSynth(Channel& c) const
{
    // A synth voice's data follows its sample's 16-byte header. It starts 0x80 and its type: 0 for a pulse wave, 1 for
    // a saw wave, and anything else for a triangle wave.
    const uint32_t data = c.wave + 16;
    const int type = int8_t(rom_.U8(data + 1));
    const uint32_t step = SynthStep(c.frequency, info_.step_scale);
    const int samples = info_.samples_per_frame;

    // A pulse wave's duty follows a triangle wave whose phase is the top byte of the count, which moves on by the
    // fourth byte of the sample's data each frame.
    if (type == 0)
    {
        c.count = int32_t(uint32_t(c.count) + DutyStep(rom_, data));
        c.fraction += step * uint32_t(samples);
        return;
    }

    // A saw wave goes through a filter that adds half its last output to each point, which the count keeps.
    if (type == 1)
    {
        int32_t filter = c.count;
        for (int i = 0; i < samples; i++)
        {
            c.fraction += step;
            filter = FilterSaw(SawPoint(c.fraction), filter);
        }

        c.count = filter;
        return;
    }

    c.fraction += step * uint32_t(samples);
}

void Sequencer::ReportTrack(int t)
{
    Track& track = tracks_[size_t(t)];
    Action a;
    a.track = uint8_t(t);
    const uint8_t again = track.sets & track.restate;
    track.sets = 0;

    // The conversion's volume and pan, with the LFO's swing when it modulates them.
    int volume = track.vol;
    if (track.mod_t == 1)
    {
        volume = std::min(127, (volume * (track.mod_m + 128)) >> 7);
    }
    if (volume != track.last_volume || (again & kSetVolume))
    {
        track.restate &= uint8_t(~kSetVolume);
        track.last_volume = volume;
        a.kind = Action::kVolume;
        a.value = volume;
        AddAction(a);
    }

    const int pan = std::clamp(64 + track.pan + (track.mod_t == 2 ? track.mod_m / 2 : 0), 0, 127);
    if (pan != track.last_pan || (again & kSetPan))
    {
        track.restate &= uint8_t(~kSetPan);
        track.last_pan = pan;
        a.kind = Action::kPan;
        a.value = pan;
        AddAction(a);
    }

    const int pitch = TrackPitch(track);
    if (pitch != track.last_pitch || (again & kSetPitch))
    {
        track.restate &= uint8_t(~kSetPitch);
        track.last_pitch = pitch;
        a.kind = Action::kPitch;
        a.value = pitch;
        AddAction(a);
    }
}

int32_t Sequencer::TrackPitch(const Track& track)
{
    int32_t pitch = (track.tune + track.bend * track.bend_range) * 4 + track.key_shift * 256;
    if (track.mod_t == 0)
    {
        pitch += 16 * track.mod_m;
    }

    return pitch;
}

void Sequencer::AddAction(Action action)
{
    action.tick = clock_;
    action.frame = frame_;
    actions_.push_back(action);
}

void Sequencer::ReleaseNote(int t, int channel)
{
    channels_[size_t(channel)].status |= kStatusStop;

    Action a;
    a.kind = Action::kRelease;
    a.track = uint8_t(t);
    a.channel = channel;
    AddAction(a);
}

void Sequencer::EndNote(int channel, bool sounded)
{
    Action a;
    a.kind = Action::kNoteEnd;
    a.channel = channel;
    a.sounded = sounded;
    AddAction(a);
}

void Sequencer::CheckAudible(int channel)
{
    Channel& c = channels_[size_t(channel)];
    if (!c.audible && c.envelope > 0)
    {
        c.audible = true;
        Action a;
        a.kind = Action::kAudible;
        a.channel = channel;
        AddAction(a);
    }
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace agbsf2::mp2k
