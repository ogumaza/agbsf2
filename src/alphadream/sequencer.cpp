// SPDX-License-Identifier: MIT

// The model of AlphaDream's sound driver. Each routine's address in A88E is given where the model follows it.

#include "alphadream/sequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "alphadream/driver.h"
#include "rom.h"

namespace agbsf2::alphadream
{
namespace
{

// The player that the game starts its music on.
constexpr uint8_t kMusicPlayer = 1;

// The level of a full-scale sample at volume 127 and the envelope's full level, with a full-scale FIFO sample at 128.
// The mixer writes it as 63 of the FIFO's 128. The SoundFont's PSG samples hold the PSG's level at volume 15 against
// it, so a PSG channel at volume 15 gets the same level.
constexpr double kFullSampleLevel = 63;

// The envelope's full level.
constexpr double kFullEnvelope = 255;

// The points that the mixer makes each frame, at 15768 Hz.
constexpr uint32_t kFramePoints = 264;

// The mixer's positions count 1024ths of a point.
constexpr int kPositionShift = 10;

// The literal that the step routine multiplies by: the share of 2^32 by which the mixer's rate falls short of
// 16384 Hz.
constexpr uint64_t kStepScale = 0x0A080000;

// The PSG channels of the wave and the noise.
constexpr int kWaveChannel = 2;
constexpr int kNoiseChannel = 3;

// The hardware's volume that the driver works out for a PSG channel from the envelope setting it last kept and the
// frames since (0x0819AC14). The hardware moves the volume by 1 every `step` 64ths of a second. The driver counts
// `step` frames for each move. Its division (0x081DAAA4) gives 0 for a step of 0.
int DriverVolume(const PsgChannel& c)
{
    const int start = c.written >> 4;
    if (!(c.written & 0xF))
    {
        return start;
    }
    const int step = c.written & 7;
    const int moved = step ? c.count / step : 0;
    return (c.written & 8) ? std::min(start + moved, 15) : std::max(start - moved, 0);
}

// Returns the hardware envelope's step that an envelope byte gives: the 64ths of a second for each move of the volume,
// or 0 for none.
uint8_t EnvelopeStep(uint8_t byte)
{
    return uint8_t((0xFF - byte) >> 5);
}

} // namespace

std::array<uint8_t, 32> TrackState::Bytes() const
{
    std::array<uint8_t, 32> b{};
    b[0] = uint8_t(flags);
    b[1] = uint8_t(flags >> 8);
    b[2] = tempo;
    b[3] = count;
    for (int i = 0; i < 4; i++)
    {
        b[size_t(4 + i)] = uint8_t(data >> (8 * i));
    }
    b[8] = uint8_t(at);
    b[9] = uint8_t(at >> 8);
    b[0xA] = wait;
    b[0xB] = key;
    b[0xC] = instrument;
    b[0xD] = volume;
    b[0xE] = pan;
    b[0xF] = bend_range;
    b[0x10] = uint8_t(bend);
    b[0x11] = uint8_t(fine);
    b[0x12] = channel;
    b[0x13] = player;
    b[0x14] = level_lfo.depth;
    b[0x15] = level_lfo.period;
    b[0x16] = level_lfo.delay;
    b[0x17] = level_lfo.count;
    b[0x18] = pitch_lfo.depth;
    b[0x19] = pitch_lfo.period;
    b[0x1A] = pitch_lfo.delay;
    b[0x1B] = pitch_lfo.count;
    for (size_t i = 0; i < 4; i++)
    {
        b[0x1C + i] = loops[i];
    }
    return b;
}

std::array<uint8_t, 12> SampleChannel::TraceBytes() const
{
    return {status,      level,       uint8_t(sample), uint8_t(sample >> 8),
            right,       left,        uint8_t(pitch),  uint8_t(pitch >> 8),
            envelope[0], envelope[1], envelope[2],     envelope[3]};
}

std::array<uint8_t, 12> PsgChannel::TraceBytes() const
{
    return {envelope[0],
            envelope[1],
            envelope[2],
            envelope[3],
            volume,
            count,
            duty,
            state,
            uint8_t(frequency),
            uint8_t(frequency >> 8),
            written,
            0};
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, const Song& song) : rom_(rom), info_(info)
{
    open_.fill(-1);
    held_.fill(-1);
    // A84J's init leaves the player's volume at 0. Only the game's fades set it.
    if (info.version == Version::kHamtaro4)
    {
        player_volume_ = 0;
    }
    // The driver's init gives each track its channel and player (0x0819AFA8). The start gives each of the song's
    // tracks its first settings (0x0819B040). The flags start with kMuted, so the first frame restores the channel.
    for (int t = 0; t < kTracks; t++)
    {
        TrackState& s = tracks_[size_t(t)];
        s.channel = uint8_t(t);
        s.player = kMusicPlayer;
        if (!(song.mask & (1 << t)))
        {
            continue;
        }
        // B85A's driver starts a track without a note, so the first frame starts none.
        s.flags =
            info.version == Version::kHamSports ? flag::kMuted | flag::kUsed : flag::kNote | flag::kMuted | flag::kUsed;
        s.data = song.tracks[size_t(t)];
        s.at = 0;
        s.tempo = 0x78;
        s.count = 0;
        s.instrument = 0;
        s.pan = 0x7F;
        s.wait = 1;
        s.volume = 0xC8;
        s.bend = 0;
        s.bend_range = 2;
        s.fine = 0;
    }
}

uint8_t Sequencer::Next(TrackState& t)
{
    const uint8_t b = rom_.U8(t.data + t.at);
    t.at = uint16_t(t.at + 1);
    return b;
}

bool Sequencer::Playing() const
{
    for (const TrackState& t : tracks_)
    {
        if (t.flags & flag::kUsed)
        {
            return true;
        }
    }
    for (const SampleChannel& c : samples_)
    {
        if (c.status)
        {
            return true;
        }
    }
    return PsgSounds(0) || PsgSounds(1) || PsgSounds(kNoiseChannel);
}

bool Sequencer::Looped(int passes) const
{
    // A track that plays on has to have gone back. A track that has ended doesn't count.
    bool any = false;
    for (int t = 0; t < kTracks; t++)
    {
        if (tracks_[size_t(t)].flags & flag::kUsed)
        {
            if (int(goto_frames_[size_t(t)].size()) < passes)
            {
                return false;
            }
            any = true;
        }
    }
    return any;
}

uint32_t Sequencer::LoopFrame(int pass) const
{
    uint32_t frame = 0;
    for (const std::vector<uint32_t>& frames : goto_frames_)
    {
        if (int(frames.size()) > pass)
        {
            frame = std::max(frame, frames[size_t(pass)]);
        }
    }
    return frame;
}

// The pitch of a sample channel (0x0819A79C) or a PSG channel (0x0819AA38, 0x0819AA9C). With `square`, the noise
// takes the squares' setting too, as the restore of a PSG channel gives it (0x0819ABF8).
void Sequencer::UpdatePitch(int index, bool square)
{
    TrackState& t = tracks_[size_t(index)];
    const int scale = index < kSampleTracks ? 2 : 1;
    uint16_t p = uint16_t((t.key << 8) + t.bend * (t.bend_range * scale));
    p = uint16_t(p + t.fine);
    if (t.flags & flag::kPitchLfo)
    {
        if (t.flags & flag::kPitchLfoOn)
        {
            p = uint16_t(p + ((t.flags & flag::kPitchLfoDown) ? -int(t.pitch_lfo.depth) : int(t.pitch_lfo.depth)));
        }
    }
    if (index < kSampleTracks)
    {
        samples_[t.channel & 7].pitch = p;
        return;
    }

    PsgChannel& c = psg_[t.channel & 3];
    if ((t.channel & 3) == 3 && !square)
    {
        // The noise's setting for the key alone (0x0819A9B4).
        c.frequency = t.key <= 0x14 ? 0xD7 : t.key <= 0x50 ? rom_.U8(info_.noise_table + t.key - 0x15) : 0;
        return;
    }
    const int key = (p >> 8) - 36;
    if (key < 0)
    {
        c.frequency = 0;
        return;
    }
    const int lo = rom_.U16(info_.psg_pitch_table + 2 * uint32_t(key));
    const int hi = rom_.U16(info_.psg_pitch_table + 2 * uint32_t(key + 1));
    c.frequency = uint16_t(0x800 - (lo + (((hi - lo) * (p & 0xFF)) >> 8)));
}

// A sample channel's volumes (0x0819A834), or a PSG channel's volume and sides (0x0819ABB8, 0x0819AB78).
void Sequencer::UpdateLevels(int index)
{
    TrackState& t = tracks_[size_t(index)];
    // A84J's driver applies the player's volume only while it fades the player.
    const int player = info_.version == Version::kHamtaro4 ? 0x100 : (player_volume_ >> 8) + 1;
    if (index >= kSampleTracks)
    {
        PsgChannel& c = psg_[t.channel & 3];
        c.volume = uint8_t((t.volume * player) >> 12);
        c.right = t.pan <= 0x80;
        c.left = t.pan >= 0x7F;
        return;
    }

    int near = 0x7F;
    int far = t.pan;
    if (int8_t(t.pan) < 0)
    {
        near = (0xFF - t.pan) & 0xFF;
        far = 0x7F;
    }
    int v = t.volume;
    if ((t.flags & flag::kLevelLfo) && (t.flags & flag::kLevelLfoOn))
    {
        v += (t.flags & flag::kLevelLfoDown) ? -int(t.level_lfo.depth) : int(t.level_lfo.depth);
    }
    v = std::clamp(v, 0, 0xFF);
    v = (v * player) >> 8;
    SampleChannel& c = samples_[t.channel & 7];
    c.right = uint8_t(((v + 1) * near) >> 8);
    c.left = uint8_t(((v + 1) * far) >> 8);
}

// Starts a note on the track's channel (0x0819A7EC, 0x0819AAE4, 0x0819AB30).
void Sequencer::NoteOn(int index)
{
    TrackState& t = tracks_[size_t(index)];
    Region region;
    if (index < kSampleTracks)
    {
        SampleChannel& c = samples_[t.channel & 7];
        c.status = 0;
        EndNote(index, frame_);
        LetGo(index, frame_);
        if (!FindRegion(rom_, info_, t.instrument, t.key, region))
        {
            return;
        }
        UpdatePitch(index);
        c.sample = region.sample;
        c.envelope = region.envelope;
        c.position = 0;
        c.status = 0x80;
    }
    else
    {
        if (!FindRegion(rom_, info_, t.instrument, t.key, region))
        {
            return;
        }
        EndNote(index, frame_);
        LetGo(index, frame_);
        // The wave's note-on (0x0819AB30) loads the region's pattern and keeps the record's other bytes.
        PsgChannel& c = psg_[t.channel & 3];
        if ((t.channel & 3) != kWaveChannel)
        {
            c.envelope = region.envelope;
            c.duty = uint8_t(region.sample);
        }
        c.state = 1;
    }

    Note n;
    n.track = index;
    n.on = frame_;
    n.key = t.key;
    n.instrument = t.instrument;
    n.region = region;
    open_[size_t(index)] = int(notes_.size());
    held_[size_t(index)] = int(notes_.size());
    notes_.push_back(n);
    last_[size_t(index)] = Change{index, frame_, -1000, -1, -1};
}

// Releases the track's note (0x0819A8C0, 0x0819AAC0). A square's or the noise's note sounds on through the release,
// until the hardware's envelope falls silent. The wave falls silent on the same frame.
void Sequencer::NoteOff(int index)
{
    TrackState& t = tracks_[size_t(index)];
    LetGo(index, frame_);
    if (index < kSampleTracks)
    {
        // A note that starts on the same frame plays on through the release until its channel stops.
        samples_[t.channel & 7].status |= 0x40;
        const int open = open_[size_t(index)];
        if (open >= 0 && notes_[size_t(open)].on == frame_)
        {
            return;
        }
    }
    else
    {
        psg_[t.channel & 3].state = 5;
        if ((t.channel & 3) != kWaveChannel)
        {
            return;
        }
    }
    EndNote(index, frame_);
}

void Sequencer::EndNote(int index, uint32_t frame)
{
    const int open = open_[size_t(index)];
    if (open >= 0)
    {
        notes_[size_t(open)].off = frame;
        notes_[size_t(open)].ended = true;
        open_[size_t(index)] = -1;
    }
}

// Records the frame on which the track lets its note go: the note's release, the track's next note or the end of a
// sample. A square's note can sound on after it, or fall silent before it.
void Sequencer::LetGo(int index, uint32_t frame)
{
    const int held = held_[size_t(index)];
    if (held >= 0)
    {
        notes_[size_t(held)].released = frame;
        held_[size_t(index)] = -1;
    }
}

// Runs a track for a frame (0x0819B450).
void Sequencer::StepTrack(int index)
{
    TrackState& t = tracks_[size_t(index)];
    if (t.flags & flag::kNote)
    {
        // The square LFOs (0x0819A72C, 0x0819A6A8). Each turns on after its delay and swings each period after that.
        if (t.flags & flag::kPitchLfo)
        {
            if (t.pitch_lfo.count)
            {
                t.pitch_lfo.count--;
            }
            else
            {
                t.pitch_lfo.count = t.pitch_lfo.period;
                if (t.flags & flag::kPitchLfoOn)
                {
                    t.flags ^= flag::kPitchLfoDown;
                    UpdatePitch(index);
                }
                else
                {
                    t.flags |= flag::kPitchLfoOn;
                }
            }
        }
        if (t.flags & flag::kLevelLfo)
        {
            if (t.level_lfo.count)
            {
                t.level_lfo.count--;
            }
            else
            {
                t.level_lfo.count = t.level_lfo.period;
                if (t.flags & flag::kLevelLfoOn)
                {
                    t.flags ^= flag::kLevelLfoDown;
                    UpdateLevels(index);
                }
                else
                {
                    t.flags |= flag::kLevelLfoOn;
                }
            }
        }
    }

    uint32_t count = uint32_t(t.count) + t.tempo;
    int ticks = 0;
    while (count > divisor_)
    {
        ticks++;
        count -= divisor_;
    }
    t.count = uint8_t(count);
    if (ticks == 0)
    {
        return;
    }

    enum class At
    {
        kWait,
        kNoteEnd,
        kEvent
    };
    At next = At::kWait;
    for (int guard = 0; guard < 100000; guard++)
    {
        if (next == At::kWait)
        {
            ticks -= t.wait;
            if (ticks < 0)
            {
                t.wait = uint8_t(-ticks);
                return;
            }
            next = At::kNoteEnd;
        }
        if (next == At::kNoteEnd)
        {
            if ((t.flags & (flag::kNote | flag::kTie)) == flag::kNote)
            {
                t.flags &= uint16_t(~(flag::kNote | flag::kTie));
                NoteOff(index);
            }
            if (t.flags & flag::kRests)
            {
                t.wait = Next(t);
                next = At::kWait;
                continue;
            }
            next = At::kEvent;
        }

        const uint32_t event_at = t.at;
        const uint8_t b = Next(t);
        switch (b)
        {
        case 0xE0:
            t.pitch_lfo.depth = Next(t);
            t.pitch_lfo.period = Next(t);
            t.pitch_lfo.delay = Next(t);
            continue;
        case 0xE1:
            t.flags |= flag::kPitchLfo;
            continue;
        case 0xE2:
            t.flags &= uint16_t(~flag::kPitchLfo);
            continue;
        case 0xE3:
            t.level_lfo.depth = Next(t);
            t.level_lfo.period = Next(t);
            t.level_lfo.delay = Next(t);
            continue;
        case 0xE4:
            t.flags |= flag::kLevelLfo;
            continue;
        case 0xE5:
            t.flags &= uint16_t(~flag::kLevelLfo);
            continue;
        case 0xE8:
        case 0xE9:
        case 0xEA:
        case 0xEB:
        case 0xEC:
        case 0xED:
        case 0xEE:
        case 0xEF:
            {
                // A count, then a 16-bit jump from after the command, with one of four counters. Each command adds 1 to
                // its counter. While the count is at least the counter, E8-EB clear the counter, and EC-EF jump and
                // clear it. Once the counter passes the count, E8-EB jump and EC-EF go on. No song of A88E uses them.
                const int times = Next(t);
                const int lo = Next(t);
                const uint16_t jump = uint16_t(lo | (Next(t) << 8));
                uint8_t& counter = t.loops[b & 3];
                const int now = counter + 1;
                counter = uint8_t(now);
                const bool within = times >= now;
                if (within)
                {
                    counter = 0;
                }
                if (within == (b >= 0xEC))
                {
                    t.at = uint16_t(t.at + jump);
                }
                continue;
            }
        case 0xF0:
            t.instrument = Next(t);
            continue;
        case 0xF1:
            // A note's length in B85A and A84J.
            if (info_.version != Version::kSuperstarSaga)
            {
                break;
            }
            t.volume = Next(t);
            UpdateLevels(index);
            continue;
        case 0xF2:
            t.pan = Next(t);
            UpdateLevels(index);
            continue;
        case 0xF3:
            t.fine = int8_t(Next(t));
            continue;
        case 0xF4:
            t.bend_range = Next(t);
            UpdatePitch(index);
            continue;
        case 0xF5:
            t.bend = int8_t(Next(t));
            UpdatePitch(index);
            continue;
        case 0xF6:
            t.wait = Next(t);
            next = At::kWait;
            continue;
        case 0xF7:
            next = At::kNoteEnd;
            continue;
        case 0xF8:
            {
                const int lo = Next(t);
                const uint16_t jump = uint16_t(lo | (Next(t) << 8));
                const uint16_t to = uint16_t(t.at + jump);
                if (to <= event_at)
                {
                    goto_frames_[size_t(index)].push_back(frame_);
                }
                t.at = to;
                continue;
            }
        case 0xF9:
            t.tempo = Next(t);
            continue;
        case 0xFA:
            // B85A's driver keeps the byte as the channel's priority for sound effects. It's a note's length in A88E
            // and A84J.
            if (info_.version != Version::kHamSports)
            {
                break;
            }
            Next(t);
            continue;
        case 0xFF:
            if (t.flags & flag::kNote)
            {
                NoteOff(index);
            }
            t.flags = 0;
            return;
        default:
            break;
        }

        // A note: its length, then its key with bit 7 set if a rest follows. A length of 0 ties the note to the next.
        // In B85A and A84J, the track's volume follows the key. A84J's driver works the levels out at once. B85A's
        // driver does so only for a note that isn't tied.
        const uint8_t length = b;
        const uint8_t k = Next(t);
        t.key = k & 0x7F;
        if (info_.version != Version::kSuperstarSaga)
        {
            t.volume = Next(t);
        }
        if (info_.version == Version::kHamtaro4)
        {
            UpdateLevels(index);
        }
        if (t.flags & flag::kTie)
        {
            UpdatePitch(index);
            t.flags &= uint16_t(~flag::kTie);
        }
        else
        {
            if (t.flags & flag::kLevelLfo)
            {
                t.flags &= uint16_t(~(flag::kLevelLfoOn | flag::kLevelLfoDown));
                t.level_lfo.count = t.level_lfo.delay;
            }
            if (t.flags & flag::kPitchLfo)
            {
                t.flags &= uint16_t(~(flag::kPitchLfoOn | flag::kPitchLfoDown));
                t.pitch_lfo.count = t.pitch_lfo.delay;
            }
            if (info_.version == Version::kHamSports)
            {
                UpdateLevels(index);
            }
            UpdatePitch(index);
            NoteOn(index);
            t.flags |= flag::kNote;
        }
        if (length == 0)
        {
            t.flags |= flag::kTie;
            if (k & 0x80)
            {
                t.wait = Next(t);
                next = At::kWait;
            }
            continue;
        }
        if (k & 0x80)
        {
            t.flags |= flag::kRests;
        }
        t.wait = length;
        next = At::kWait;
    }
    warnings_.push_back("track " + std::to_string(index) + " runs commands without end");
    t.flags = 0;
}

// The mixer's envelopes and positions (0x0819A2F0, 0x0819A398).
void Sequencer::RunEnvelopes()
{
    for (SampleChannel& c : samples_)
    {
        uint8_t status = c.status;
        int level = c.level;
        if (status == 0)
        {
            continue;
        }
        if (status == 0x80)
        {
            status = 0x81;
            level = c.envelope[0];
        }
        else if (status == 0x81)
        {
            level += c.envelope[0];
            if (level >= 0xFF)
            {
                level = 0xFF;
                status = 0x82;
            }
        }
        else if (status == 0x82)
        {
            // The driver compares the status, not the level, with the sustain level. A level at or above the decay's
            // step goes to the sustain level at once.
            if (level >= c.envelope[1] || status <= c.envelope[2])
            {
                status = 0x83;
                level = c.envelope[2];
            }
            else
            {
                level = (level - c.envelope[1]) & 0xFF;
            }
        }
        else if (status != 0x83)
        {
            if (level > c.envelope[3])
            {
                level -= c.envelope[3];
            }
            else
            {
                c.status = 0;
                continue;
            }
        }
        c.status = status;
        c.level = uint8_t(level);

        // The position, which ends a sample without a loop.
        SampleHeader s;
        if (!ReadSample(rom_, info_, c.sample & 0x7FFF, s))
        {
            c.status = 0;
            continue;
        }
        const bool fixed = c.sample & 0x8000;
        const int key = fixed ? kRootKey : c.pitch >> 8;
        const int fraction = fixed ? 0 : c.pitch & 0xFF;
        const uint8_t code = rom_.U8(info_.key_table + uint32_t(key));
        const uint32_t note = code & 0xF;
        const uint32_t a = rom_.U32(info_.step_table + 4 * note);
        const uint32_t b = rom_.U32(info_.step_table + 4 * note + 4);
        const uint32_t m = (a + uint32_t(((b - a) * uint32_t(fraction)) >> 8)) >> (code >> 4);
        const uint32_t high = uint32_t((uint64_t(s.rate) * m) >> 32);
        const uint32_t step = high + uint32_t((uint64_t(high) * kStepScale) >> 32);
        const uint32_t end = s.end << kPositionShift;
        uint32_t left = kFramePoints;
        for (int guard = 0; guard < 4096 && left; guard++)
        {
            const uint32_t to_end = end - c.position + step - 1;
            uint32_t n = left;
            if (uint64_t(step) * left >= to_end && step)
            {
                n = to_end / step;
            }
            left -= n;
            c.position += n * step;
            if (int32_t(c.position) < int32_t(end))
            {
                break;
            }
            if (!s.loops)
            {
                c.status = 0;
                break;
            }
            c.position += (s.loop_start - s.end) << kPositionShift;
        }
    }
}

// Runs a square's or the noise's envelope for a frame (0x0819AC84). The driver works the hardware's envelope in turns:
// a rise from 0 to the channel's volume, a fall to the sustain level, the sustain, and a fall to 0 after the note's
// end. Each turn restarts the channel with a new envelope setting, once the volume that the driver works out reaches
// the turn's end. In between, the frequency goes to the hardware each frame.
void Sequencer::RunPsgEnvelope(int channel)
{
    PsgChannel& c = psg_[size_t(channel)];
    uint16_t& control = registers_.control[size_t(channel)];
    uint16_t& frequency = registers_.frequency[size_t(channel)];
    // A84J's driver counts the frames whether or not it has written a setting (0x0814A844).
    if ((c.written || info_.version == Version::kHamtaro4) && c.count != 0xFF)
    {
        c.count++;
    }

    // Sets the hardware's envelope, keeps it in the record, and restarts the channel. The squares take their duty
    // again with the first two turns.
    const auto restart = [&](uint8_t setting, bool duty)
    {
        c.count = 0;
        c.written = setting;
        control = uint16_t(setting << 8 | (duty && channel != kNoiseChannel ? c.duty << 6 : control & 0xFF));
        frequency = uint16_t(c.frequency | 0x8000);
        registers_.restarted |= uint8_t(1 << channel);
    };
    const auto play = [&]()
    {
        frequency = c.frequency;
    };
    const auto silence = [&](bool keep)
    {
        if (!keep)
        {
            c.written = 0;
        }
        control &= 0xFF;
        c.state = 0;
    };

    switch (c.state)
    {
    case 1:
        c.state = 2;
        if (c.envelope[0] != 0xFF)
        {
            restart(uint8_t(EnvelopeStep(c.envelope[0]) | 8), true);
            return;
        }
        break;
    case 2:
        if (DriverVolume(c) < c.volume)
        {
            play();
            return;
        }
        break;
    case 3:
        {
            const int sustain = (c.volume * c.envelope[2]) >> 8;
            if (DriverVolume(c) > sustain)
            {
                play();
                return;
            }
            restart(uint8_t(sustain << 4), false);
            c.state = 4;
            return;
        }
    case 4:
        play();
        return;
    case 5:
        {
            // The release starts from the volume that the driver works out. Without a step, or from 0, the channel
            // goes silent at once, and the record keeps the old setting. A84J's driver checks only the step
            // (0x0814AA18): from 0, it restarts the channel at 0 with the release's step.
            const int volume = DriverVolume(c);
            c.count = 0;
            const uint8_t step = EnvelopeStep(c.envelope[3]);
            if (!step || (!volume && info_.version != Version::kHamtaro4))
            {
                silence(true);
                return;
            }
            restart(uint8_t(volume << 4 | step), false);
            c.state = 6;
            return;
        }
    case 6:
        if (DriverVolume(c))
        {
            play();
            return;
        }
        // A84J's driver keeps the setting in the record (0x0814AA78).
        silence(info_.version == Version::kHamtaro4);
        return;
    default:
        return;
    }

    // The fall from the channel's volume, after the rise or at once.
    restart(uint8_t(EnvelopeStep(c.envelope[1]) | c.volume << 4), true);
    c.state = 3;
}

// Runs the wave for a frame (0x0819AF18). A note's start restarts the wave at the NR32 setting for the channel's
// volume over 4, and the note's end mutes it. A change of the channel's volume reaches the hardware with the next note.
// The record of the wave keeps NR32 in the high byte, as SOUND3CNT_H does.
void Sequencer::RunWave()
{
    PsgChannel& c = psg_[kWaveChannel];
    uint16_t& control = registers_.control[kWaveChannel];
    uint16_t& frequency = registers_.frequency[kWaveChannel];
    switch (c.state)
    {
    case 1:
        frequency = uint16_t(c.frequency | 0x8000);
        control = uint16_t(rom_.U8(info_.wave_levels + uint32_t(c.volume >> 2)) << 8);
        c.state = 2;
        break;
    case 2:
        frequency = c.frequency;
        break;
    case 5:
        control &= 0xFF;
        c.state = 0;
        break;
    default:
        break;
    }
}

// The PSG's output for a frame (0x0819AF88): the squares' and the noise's envelopes, then the hardware's response,
// from a model of the PSG. The wave's routine runs between the squares and the noise.
void Sequencer::RunPsg()
{
    registers_.restarted = 0;
    RunWave();
    for (int channel : {0, 1, kNoiseChannel})
    {
        RunPsgEnvelope(channel);
        const PsgRegisters r{0, registers_.control[size_t(channel)], registers_.frequency[size_t(channel)], 0,
                             (registers_.restarted & (1 << channel)) != 0};
        PsgModel& model = psg_models_[size_t(channel)];
        const int start = model.Frame(r);
        psg_volumes_[size_t(channel)] = start;

        // The frame's level is the mean of the volumes at its start and its end. The volume at the end is the one
        // that the next frame would start with if the driver wrote nothing. The envelope moves within the frame, and
        // the level of the frame's start would lag it by half a frame.
        PsgModel ahead = model;
        const int end = ahead.Frame(PsgRegisters{0, r.envelope, uint16_t(r.frequency & 0x7FFF), 0, false});
        psg_levels_[size_t(channel)] = start < 0 ? 0 : (start + std::max(end, 0)) / 2.0;
    }
}

// Returns true while a square or the noise sounds: on, at a volume above 0 or with an envelope that rises from 0.
bool Sequencer::PsgSounds(int channel) const
{
    const int volume = psg_volumes_[size_t(channel)];
    const uint16_t envelope = registers_.control[size_t(channel)] >> 8;
    return volume > 0 || (volume == 0 && (envelope & 8) && (envelope & 7));
}

void Sequencer::Record(int index)
{
    if (open_[size_t(index)] < 0)
    {
        return;
    }
    const Note& n = notes_[size_t(open_[size_t(index)])];
    const TrackState& t = tracks_[size_t(index)];
    Change c{index, frame_, 0, 0, 0};
    if (index < kSampleTracks)
    {
        // A note that its track releases on the frame it starts plays through the release, from the level that the
        // channel had. The SoundFont's envelope can't follow that. The note's levels take the envelope's level in.
        const SampleChannel& s = samples_[t.channel & 7];
        const double envelope = n.released == n.on ? s.level / kFullEnvelope : 1;
        c.pitch = (s.sample & 0x8000) ? 0 : (int(s.pitch) - n.key * 256) / 256.0;
        c.left = s.left * kFullSampleLevel / 127 * envelope;
        c.right = s.right * kFullSampleLevel / 127 * envelope;
    }
    else
    {
        // A square and the noise play at the hardware's level. The wave plays at the share of 15 that NR32 sets.
        // A square and the wave play the frequency setting that the driver keeps, from its pitch table (0x0819A96C).
        // The table truncates 131072 over each key's frequency, and a key below 36 gets the setting 0: 64 Hz on a
        // square. The pitch is the bend from the note's key to that setting. The wave plays an octave below a square,
        // and so does its sample.
        const int h = t.channel & 3;
        const PsgChannel& p = psg_[size_t(h)];
        const double key_hz = kMiddleC * std::exp2((n.key - 60) / 12.0);
        c.pitch = h == kNoiseChannel ? 0 : 12 * std::log2(SquareHz(std::min<int>(p.frequency, 2047)) / key_hz);
        const double volume =
            h == kWaveChannel ? 15 * WaveShare(registers_.control[kWaveChannel]) : psg_levels_[size_t(h)];
        c.left = p.left ? volume * kFullSampleLevel / 15 : 0;
        c.right = p.right ? volume * kFullSampleLevel / 15 : 0;
    }
    const Change& last = last_[size_t(index)];
    if (c.pitch != last.pitch || c.left != last.left || c.right != last.right)
    {
        changes_.push_back(c);
        last_[size_t(index)] = c;
    }
}

// Runs the players (0x0819B864) for the music's player alone, the PSG's output, then the mixer.
void Sequencer::Step()
{
    for (int index = 0; index < kTracks; index++)
    {
        TrackState& t = tracks_[size_t(index)];
        if (!(t.flags & flag::kUsed))
        {
            continue;
        }
        if (t.flags & flag::kMuted)
        {
            // The channel comes back (the restore handler, then the note-on if a note sounds). The restore handler of a
            // PSG channel uses the squares' pitch routine, for the noise too. A84J's driver doesn't start the note
            // again. B85A's driver turns the channel off first. The model restores a channel only when
            // a song starts, and nothing sounds on it then.
            t.flags &= uint16_t(~flag::kMuted);
            UpdatePitch(index, true);
            UpdateLevels(index);
            if ((t.flags & flag::kNote) && info_.version != Version::kHamtaro4)
            {
                NoteOn(index);
            }
        }
        StepTrack(index);
    }
    RunPsg();
    RunEnvelopes();
    for (int index = 0; index < kTracks; index++)
    {
        if (open_[size_t(index)] >= 0)
        {
            const int h = tracks_[size_t(index)].channel & 3;
            if (index < kSampleTracks && samples_[size_t(index)].status == 0)
            {
                // A note that its track released on the frame it started and whose channel stopped on that frame
                // never sounded: the channel's level was at most the release's step.
                const Note& n = notes_[size_t(open_[size_t(index)])];
                EndNote(index, n.on == frame_ && n.released == frame_ ? frame_ : frame_ + 1);
                LetGo(index, frame_ + 1);
            }
            else if (index >= kSampleTracks && h != kWaveChannel && !PsgSounds(h))
            {
                EndNote(index, frame_);
            }
        }
        Record(index);
    }
    frame_++;
}

} // namespace agbsf2::alphadream
