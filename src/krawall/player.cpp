// SPDX-License-Identifier: MIT

#include "krawall/player.h"

#include <algorithm>
#include <string>
#include <utility>

namespace supergbamidi::krawall
{
namespace
{

// The effects of the effect column, as the module's patterns number them.
enum EffectNumber : int
{
    kSpeed = 1,
    kBpm = 2,
    kSpeedBpm = 3,
    kPatternJump = 4,
    kPatternBreak = 5,
    kVolumeSlideS3m = 6,
    kVolumeSlideXm = 7,
    kVolumeSlideDownFine = 8,
    kVolumeSlideUpFine = 9,
    kPortaDownXm = 10,
    kPortaDownS3m = 11,
    kPortaDownFine = 12,
    kPortaDownExtraFine = 13,
    kPortaUpXm = 14,
    kPortaUpS3m = 15,
    kPortaUpFine = 16,
    kPortaUpExtraFine = 17,
    kVolume = 18,
    kPortaNote = 19,
    kVibrato = 20,
    kTremor = 21,
    kArpeggio = 22,
    kVolumeSlideVibrato = 23,
    kVolumeSlidePorta = 24,
    kChannelVolume = 25,
    kChannelVolumeSlide = 26,
    kOffset = 27,
    kPanSlide = 28,
    kRetrig = 29,
    kTremolo = 30,
    kFineVibrato = 31,
    kGlobalVolume = 32,
    kGlobalVolumeSlide = 33,
    kPan = 34,
    kPanbrello = 35,
    kMark = 36,
    kGlissando = 37,
    kVibratoWave = 38,
    kTremoloWave = 39,
    kPanbrelloWave = 40,
    kPatternLoop = 43,
    kNoteCut = 44,
    kNoteDelay = 45,
    kVolumeSlideVibratoXm = 49,
    kVolumeSlidePortaXm = 50,
    kEffectCount = 51,
};

// Whether each effect runs on the ticks between rows as well as on the row's tick, and whether it has a second part
// that runs only between rows, as the player's table of effects gives them.
struct EffectEntry
{
    bool between = false;
    bool second = false;
};

constexpr EffectEntry EntryOf(int effect)
{
    switch (effect)
    {
    case kVolumeSlideS3m:
    case kVolumeSlideXm:
    case kPortaDownXm:
    case kPortaDownS3m:
    case kPortaUpXm:
    case kPortaUpS3m:
    case kPortaNote:
    case kVibrato:
    case kTremor:
    case kArpeggio:
    case kChannelVolumeSlide:
    case kPanSlide:
    case kRetrig:
    case kTremolo:
    case kFineVibrato:
    case kGlobalVolumeSlide:
    case kPanbrello:
    case kNoteCut:
    case kNoteDelay:
        return {true, false};
    case kVolumeSlideVibrato:
    case kVolumeSlidePorta:
    case kVolumeSlideVibratoXm:
    case kVolumeSlidePortaXm:
        return {true, true};
    default:
        return {false, false};
    }
}

// Whether each of the volume column's effects runs on the ticks between rows.
constexpr bool kColumnBetween[10] = {true, true, false, false, false, true, false, true, true, true};

// The frequency that the period of an Amiga module divides: 8363 Hz times 1712, the period of C-4.
constexpr int32_t kAmigaClock = 14317456;

// The slowest tempo the sound timer takes: 16 times 24 ticks a minute.
constexpr int kMinTimerBpm = 16 * 24;

// The samples that the timer's step divides: 15 seconds of the mix.
constexpr int32_t kTimerSamples = 15 * 16384;

// Returns a word's bytes, least significant first, into `out` at `at`.
template <size_t N>
void Put(std::array<uint8_t, N>& out, size_t at, uint32_t value, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        out[at + i] = uint8_t(value >> (8 * i));
    }
}

// Returns the quotient and remainder from the BIOS's signed Div operation. For a zero divisor, the player skips the
// call and uses `n` and 0.
std::pair<int32_t, int32_t> Div(int32_t n, int32_t d)
{
    if (d == 0)
    {
        return {n, 0};
    }
    if (n == INT32_MIN && d == -1)
    {
        return {n, 0};
    }

    return {n / d, n % d};
}

// Returns `v` shifted right by the bottom byte of `amount`, as an ARM register shift does.
uint32_t ShiftRight(uint32_t v, uint32_t amount)
{
    amount &= 0xFF;
    return amount >= 32 ? 0 : v >> amount;
}

} // namespace

Player::Player(const Rom& rom, const DriverInfo& info, uint32_t module, int mode, int song) : rom_(rom), info_(info)
{
    if (!ReadModule(rom, module, module_))
    {
        Warn("the module at " + Hex(module) + " can't be read");
        return;
    }
    if (module_.channels > kPlayerChannels)
    {
        Warn("the module has " + std::to_string(module_.channels) + " channels, more than the player's " +
             std::to_string(kPlayerChannels));
        return;
    }

    // The mixer as the game's call to kramQualityMode() leaves it.
    hq_ramp_ = (info.quality & 0x10) ? uint8_t(1) : uint8_t(0);
    hq_mode_ = (info.quality & 0xF) > 2 ? uint8_t(0) : uint8_t(info.quality & 0xF);

    // krapPlay(), with the player stopped: a jingle then plays as a module of its own. The first tick plays the first
    // row, at the module's volume and tempo.
    if (mode & kModeJingle)
    {
        mode &= ~kModeJingle;
    }
    mode_ = uint32_t(mode);
    speed_ = tick_ = module_.speed;
    volume_ = 64;
    volume_slide_ = 0;
    SetMusicVolume((int32_t(module_.global_volume) << 6) * music_volume_ >> 13);
    tempo_ = module_.tempo;
    SetTimerBpm(tempo_ * 24);

    // The song's first order, and the order that the module goes back to after its last.
    if (song > 63)
    {
        song = 0;
    }
    order_ = module_.song_starts[size_t(song)];
    song_start_ = order_;
    if (!(mode_ & kModeSong))
    {
        song_start_ = module_.restart;
    }
    row_ = 0;
    FindRow();

    goto_row_ = goto_order_ = -1;
    loop_row_ = loop_count_ = loop_active_ = 0;
    instrument_based_ = module_.instruments ? uint8_t(1) : uint8_t(0);
    fast_slides_ = module_.fast_slides ? uint8_t(1) : uint8_t(0);
    vibrato_offset_ = instrument_based_ ? uint8_t(32) : uint8_t(0);

    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        c.vibrato_pos = vibrato_offset_;
        c.vibrato_reset = 1;
        c.vibrato_wave = info.sine;
        c.tremolo_reset = 1;
        c.tremolo_wave = info.sine;
        c.panbrello_reset = 0;
        c.panbrello_wave = info.sine;
        c.panning = module_.channel_pan[size_t(i)];
        c.envelope_volume = 64;
        c.envelope_pan = 0;
        c.channel_volume = 64;
    }

    status_ = 2;
    SetTimer(true);
    valid_ = true;
}

void Player::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

double Player::OutputScale() const
{
    // The mix adds each point times the side's volume, shifted down 3 bits, and its output shifts that down by the
    // master volume's shift and scales it by the master volume's level over 128.
    const uint32_t shift = (info_.master_volume >> 16) ? (info_.master_volume >> 16) : 5;
    return double(info_.master_volume & 0xFF) / double(1u << (3 + shift));
}

// ---------------------------------------------------------------------------------------------------------------------
// The mixer

uint32_t Player::Play(uint32_t sample, int sample_index, int sfx, uint32_t handle, uint32_t freq, uint32_t volume,
                      int32_t pan)
{
    // With ramping, the channel's note ramps down first, if it's still the channel's.
    MixChannel* m = &mixer_[handle & 0x1F];
    if (hq_ramp_ && m->handle == handle)
    {
        Stop(handle);
    }

    // A channel whose play is another's is left alone, and the play takes the first free channel instead.
    uint32_t channel = handle & 0xFF;
    if (channel >= uint32_t(kMixChannels) || m->handle != handle)
    {
        channel = 0;
        while (channel < uint32_t(kMixChannels) && mixer_[channel].status != 0)
        {
            channel++;
        }
        if (channel >= uint32_t(kMixChannels))
        {
            return 0;
        }
        m = &mixer_[channel];
    }

    plays_++;
    m->sfx = uint8_t(sfx & 1);
    m->hq = hq_mode_ == 2 ? uint8_t(8) : uint8_t((m->hq_sample & hq_mode_) << 3);
    m->handle = channel | (plays_ << 8);
    SetPanning(*m, pan);
    m->volume = uint8_t(volume);
    m->inc = int32_t(freq << 2);
    SetSides(*m);
    m->hq_sample = rom_.U8(sample + 0x11);
    m->pos = m->start = sample + 0x12;
    m->loop_length = rom_.U32(sample);
    m->end = rom_.U32(sample + 4);
    m->loop = rom_.U8(sample + 0x10);
    m->status = 1;
    m->loop_from = m->end - m->loop_length;
    m->frac = 0;
    m->moves++;
    m->sample = sample;
    m->sample_index = sample_index;

    return m->handle;
}

bool Player::Stop(uint32_t handle)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if ((handle & 0xFF) >= uint32_t(kMixChannels) || m.handle != handle)
    {
        return false;
    }

    // Without ramping a stop is at once. With it, a channel that still plays loud enough ramps down from its point.
    if (!hq_ramp_ || m.status != 1 || m.volume <= 5)
    {
        m.status = 0;
        return true;
    }

    const uint8_t point = rom_.U8(m.pos);
    m.sfx = 16;
    m.loop_from = uint32_t(uint16_t(point << 4)) | (uint32_t(uint16_t(0x80 - point)) << 16);
    m.mix = 7;
    m.inc = 0;
    m.hq = 0;
    m.handle = 0;
    m.pos = m.start;

    return true;
}

bool Player::SetFreq(uint32_t handle, uint32_t freq)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if ((handle & 0xFF) >= uint32_t(kMixChannels) || m.handle != handle)
    {
        return false;
    }

    m.inc = m.inc >= 0 ? int32_t(freq << 2) : int32_t(0u - (freq << 2));

    return true;
}

bool Player::SetVol(uint32_t handle, uint32_t volume)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if ((handle & 0xFF) >= uint32_t(kMixChannels) || m.handle != handle)
    {
        return false;
    }

    m.volume = uint8_t(volume);
    SetSides(m);

    return true;
}

bool Player::SetPan(uint32_t handle, int32_t pan)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if ((handle & 0xFF) >= uint32_t(kMixChannels) || m.handle != handle)
    {
        return false;
    }

    SetPanning(m, pan);
    SetSides(m);

    return true;
}

bool Player::SetPos(uint32_t handle, uint32_t pos)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if ((handle & 0xFF) >= uint32_t(kMixChannels) || m.handle != handle)
    {
        return false;
    }

    if (m.start + pos < m.end)
    {
        m.pos = m.start + pos;
        m.frac = 0;
        m.moves++;
    }

    return true;
}

void Player::SetSides(MixChannel& m) const
{
    // The master volume of a ramping channel's count is past the two that the mixer keeps, so it reads as 0.
    const uint32_t master = size_t(m.sfx) < master_.size() ? master_[m.sfx] : 0;
    const uint32_t level = master * m.volume;
    m.left = uint8_t((level * uint32_t(64 - m.pan)) >> 13);
    m.right = uint8_t((level * uint32_t(64 + m.pan)) >> 13);
}

void Player::SetPanning(MixChannel& m, int32_t pan) const
{
    // In mono, every channel mixes to the left buffer alone.
    if (!info_.stereo)
    {
        m.mix = 1;
        m.pan = -64;
        return;
    }

    m.mix = pan == 0 ? uint8_t(0) : uint8_t(pan > 0 ? (pan >> 6) + 2 : (-pan) >> 6);
    m.pan = int8_t(pan);
}

void Player::ResetChannels()
{
    for (MixChannel& m : mixer_)
    {
        if (m.status != 0)
        {
            SetSides(m);
        }
    }
}

void Player::SetMusicVolume(int32_t volume)
{
    master_[0] = uint32_t(volume) > 255 ? uint8_t(255) : uint8_t(volume);
    ResetChannels();
}

void Player::SetTimerBpm(int bpm)
{
    bpm = std::max(bpm, kMinTimerBpm);
    timer_speed_ = timer_count_ = timer_backup_ = uint16_t((kTimerSamples / bpm) << 2);
}

void Player::SetTimer(bool on)
{
    timer_on_ = on;
    if (on)
    {
        timer_count_ = timer_backup_;
    }
    else
    {
        timer_backup_ = timer_count_;
    }
}

void Player::Advance(MixChannel& m, uint32_t samples)
{
    // A mixing routine steps through the points a sample of the mix at a time, keeping the position's fraction.
    const int32_t sum = int32_t(uint32_t(m.frac) + uint32_t(m.inc) * samples);
    m.pos += uint32_t(sum >> 16);
    m.frac = uint16_t(sum);

    // The ramp at a stop counts down its samples and frees the channel when they're done.
    if (m.mix == 7)
    {
        const uint32_t done = std::min<uint32_t>(m.sfx, samples);
        const uint16_t level = uint16_t(m.loop_from + (m.loop_from >> 16) * done);
        m.loop_from = (m.loop_from & 0xFFFF0000) | level;
        m.sfx = uint8_t(m.sfx - done);
        if (m.sfx == 0)
        {
            m.status = 0;
        }
    }
}

void Player::Mix(uint32_t amount)
{
    for (MixChannel& m : mixer_)
    {
        if (m.status != 1)
        {
            continue;
        }

        // A silent channel moves on by a guess at the points that its samples take, without mixing them.
        uint32_t todo = amount;
        if (m.volume == 0)
        {
            const uint32_t guess = uint32_t(m.inc >> 16) * todo + todo;
            if (m.loop == 0)
            {
                if (m.pos + guess >= m.end)
                {
                    m.status = 0;
                }
                else
                {
                    m.pos += guess;
                }
            }
            else if (m.loop == 2)
            {
                // Back and forth: each time the guess passes an end of the loop, it comes back by the loop's length,
                // and an odd count of times turns it round.
                uint32_t turns = 0;
                m.pos += guess;
                if (m.inc >= 0)
                {
                    for (; m.pos >= m.end && m.loop_length; turns++)
                    {
                        m.pos -= m.loop_length;
                    }
                    if (turns & 1)
                    {
                        m.pos = m.end - (m.pos - m.loop_from);
                        m.inc = -m.inc;
                    }
                }
                else
                {
                    for (; m.pos < m.loop_from && m.loop_length; turns++)
                    {
                        m.pos += m.loop_length;
                    }
                    if (turns & 1)
                    {
                        m.pos = m.loop_from + (m.end - m.pos);
                        m.inc = -m.inc;
                    }
                }
            }
            else
            {
                m.pos += guess;
                while (m.pos >= m.end && m.loop_length)
                {
                    m.pos -= m.loop_length;
                }
            }
            continue;
        }

        // A playing channel mixes until its sample or loop ends, then loops or stops, and goes on with what's left.
        for (;;)
        {
            bool fits = false;
            int32_t num = 0, den = 0;
            if (m.inc >= 0)
            {
                const uint32_t guess = uint32_t(m.inc >> 16) * todo + todo;
                fits = m.pos + guess < m.end;
                num = int32_t(((m.end - m.pos) << 14) - (uint32_t(m.frac) >> 2));
                den = m.inc >> 2;
            }
            else
            {
                const uint32_t guess = uint32_t(m.inc >> 16) * todo;
                fits = m.pos + guess > m.loop_from;
                num = int32_t(((m.pos - m.loop_from) << 14) + (uint32_t(m.frac) >> 2));
                den = int32_t(0u - uint32_t(m.inc)) >> 2;
            }
            if (fits)
            {
                Advance(m, todo);
                break;
            }

            const auto [quotient, remainder] = Div(num, den);
            int32_t fit = quotient + (remainder != 0 ? 1 : 0);
            if (fit > int32_t(todo))
            {
                Advance(m, todo);
                break;
            }
            if (fit & 3)
            {
                fit = (fit & ~3) + 4;
            }
            if (fit >= int32_t(todo))
            {
                Advance(m, uint32_t(fit));
                if (m.loop == 0)
                {
                    m.status = 0;
                }
                else if (m.loop == 2)
                {
                    m.inc = -m.inc;
                }
                else
                {
                    m.pos -= m.loop_length;
                }
                break;
            }

            if (fit != 0)
            {
                Advance(m, uint32_t(fit));
                todo -= uint32_t(fit);
            }
            if (m.loop == 0)
            {
                m.status = 0;
                break;
            }
            if (m.loop == 2)
            {
                m.inc = -m.inc;
            }
            else
            {
                m.pos -= m.loop_length;
            }
            if (todo == 0)
            {
                break;
            }
        }
    }

    elapsed_ += amount;
}

void Player::Frame()
{
    // The worker mixes up to each tick that falls in the frame, runs it, and mixes the rest.
    uint32_t amount = uint32_t(info_.frame_samples);
    while (amount)
    {
        if (timer_count_ > amount || !timer_on_)
        {
            Mix(amount);
            timer_count_ = uint16_t(timer_count_ - amount);
            break;
        }

        const uint32_t count = timer_count_;
        Mix(count);
        amount -= count;
        Timer();
        if (on_tick_)
        {
            on_tick_(elapsed_);
        }
        timer_count_ = timer_speed_;
    }
}

std::array<uint8_t, kPlayerRecordSize> Player::PlayerRecord() const
{
    std::array<uint8_t, kPlayerRecordSize> r = {};
    Put(r, 0x00, mode_, 4);
    Put(r, 0x04, status_, 4);
    Put(r, 0x08, valid_ ? module_.address : 0, 4);
    Put(r, 0x0C, tempo_, 2);
    r[0x0E] = speed_;
    r[0x0F] = tick_;
    r[0x10] = row_;
    r[0x11] = order_;
    Put(r, 0x14, data_, 4);
    r[0x18] = uint8_t(goto_order_);
    r[0x19] = uint8_t(goto_row_);
    r[0x1A] = uint8_t(loop_row_);
    r[0x1B] = uint8_t(loop_count_);
    r[0x1C] = uint8_t(loop_active_);
    r[0x1D] = uint8_t(volume_);
    r[0x1E] = volume_slide_;
    r[0x1F] = vibrato_offset_;
    r[0x20] = instrument_based_;
    r[0x21] = fast_slides_;
    Put(r, 0x2C, song_start_, 4);
    Put(r, 0x30, pattern_, 4);

    return r;
}

std::array<uint8_t, kChannelRecordSize> Player::ChannelRecord(const Channel& c)
{
    std::array<uint8_t, kChannelRecordSize> r = {};
    Put(r, 0x00, c.handle, 4);
    r[0x04] = uint8_t(c.volume);
    r[0x05] = uint8_t(c.channel_volume);
    r[0x06] = uint8_t(c.panning);
    r[0x07] = c.start_point;
    r[0x08] = c.effect;
    Put(r, 0x0A, uint16_t(c.note), 2);
    Put(r, 0x0C, uint16_t(c.period), 2);
    Put(r, 0x0E, uint16_t(c.played_period), 2);
    Put(r, 0x10, c.sample, 4);
    Put(r, 0x14, c.instrument, 4);
    r[0x18] = c.last_effect;
    r[0x19] = c.effect_op;
    Put(r, 0x1A, c.porta_step, 2);
    Put(r, 0x1C, uint16_t(c.porta_goal), 2);
    Put(r, 0x1E, uint16_t(c.gliss_note), 2);
    r[0x20] = c.vibrato_pos;
    r[0x21] = c.vibrato_speed;
    r[0x22] = c.vibrato_depth;
    r[0x23] = c.vibrato_reset;
    Put(r, 0x24, c.vibrato_wave, 4);
    r[0x28] = c.tremolo_pos;
    r[0x29] = c.tremolo_speed;
    r[0x2A] = c.tremolo_depth;
    r[0x2B] = c.tremolo_reset;
    Put(r, 0x2C, c.tremolo_wave, 4);
    r[0x30] = c.panbrello_pos;
    r[0x31] = c.panbrello_speed;
    r[0x32] = c.panbrello_depth;
    r[0x33] = c.panbrello_reset;
    Put(r, 0x34, c.panbrello_wave, 4);
    const uint8_t bytes[] = {c.glissando,
                             c.tremor_count,
                             c.tremor_on,
                             c.tremor_cycle,
                             c.volume_slide,
                             c.fine_volume_slide,
                             c.channel_volume_slide,
                             c.volume_slide_once,
                             c.pan_slide,
                             c.pan_slide_once,
                             c.arpeggio,
                             c.arpeggio_step,
                             c.porta,
                             c.fine_porta,
                             c.extra_fine_porta,
                             c.retrig,
                             c.restore_note,
                             c.play_note,
                             c.in_row,
                             c.envelopes_on,
                             c.volume_env_state};
    std::copy(std::begin(bytes), std::end(bytes), r.begin() + 0x38);
    r[0x4D] = uint8_t(c.envelope_volume);
    Put(r, 0x4E, c.volume_env_level, 2);
    r[0x50] = c.volume_env_point;
    r[0x51] = c.volume_env_tick;
    r[0x52] = c.volume_env_target;
    r[0x53] = c.fading;
    Put(r, 0x54, uint16_t(c.fade), 2);
    r[0x56] = c.pan_env_state;
    r[0x57] = uint8_t(c.envelope_pan);
    Put(r, 0x58, c.pan_env_level, 2);
    r[0x5A] = c.pan_env_point;
    r[0x5B] = c.pan_env_tick;
    r[0x5C] = c.pan_env_target;
    r[0x5D] = c.column_effect;
    r[0x5E] = c.instrument_vibrato_pos;

    return r;
}

std::array<uint8_t, kMixRecordSize> Player::MixRecord(const MixChannel& m)
{
    std::array<uint8_t, kMixRecordSize> r = {};
    r[0x00] = m.volume;
    r[0x01] = uint8_t(m.pan);
    r[0x02] = m.status;
    r[0x03] = m.loop;
    Put(r, 0x04, m.start, 4);
    Put(r, 0x08, m.pos, 4);
    Put(r, 0x0C, m.end, 4);
    Put(r, 0x10, m.loop_length, 4);
    Put(r, 0x14, uint32_t(m.inc), 4);
    Put(r, 0x18, m.handle, 4);
    Put(r, 0x1C, m.frac, 2);
    r[0x1E] = m.left;
    r[0x1F] = m.right;
    r[0x20] = m.hq;
    r[0x21] = m.mix;
    r[0x22] = m.hq_sample;
    r[0x24] = m.sfx;
    Put(r, 0x28, m.loop_from, 4);

    return r;
}

std::array<uint8_t, 16> Player::Globals() const
{
    std::array<uint8_t, 16> r = {};
    r[0] = master_[0];
    r[1] = master_[1];
    r[2] = hq_mode_;
    r[3] = hq_ramp_;
    Put(r, 4, timer_speed_, 2);
    Put(r, 6, timer_count_, 2);
    Put(r, 8, uint32_t(music_volume_), 4);
    Put(r, 12, plays_, 4);

    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// The player

void Player::Timer()
{
    tick_++;
    if (tick_ >= speed_)
    {
        tick_ = 0;
        PlayRow();
    }
    else
    {
        PlayTickBetweenRows();
    }

    if (instrument_based_)
    {
        RunEnvelopes();
    }
}

void Player::FindRow()
{
    // The order's pattern, from the module's table of patterns.
    pattern_ = rom_.U32(module_.address + kModulePatterns + 4 * uint32_t(module_.orders[order_]));
    data_ = pattern_ + kPatternData;
    if (!rom_.Contains(pattern_, kPatternData))
    {
        Warn("order " + std::to_string(order_) + " names a pattern outside the ROM, so the module stops there");
        Stop();
        return;
    }
    if (row_ == 0)
    {
        return;
    }

    // The index gives every 4th row's data, and the rows between are skipped.
    data_ += rom_.U16(pattern_ + 2 * ((uint32_t(row_) >> 2) & 0xF));
    for (int left = row_ & 3; left > 0;)
    {
        const uint8_t follow = rom_.U8(data_++);
        if (!follow)
        {
            left--;
            continue;
        }

        data_ += (follow & 0x20 ? 2 : 0) + (follow & 0x40 ? 1 : 0) + (follow & 0x80 ? 2 : 0);
    }
}

void Player::NextRow()
{
    // A jump goes to its order's first row, through the order's end.
    if (goto_order_ >= 0)
    {
        order_ = uint8_t(goto_order_ - 1);
        row_ = 0xFE;
        goto_order_ = -1;
    }

    row_++;
    if (row_ < rom_.U8(pattern_ + kPatternRows) && goto_row_ < 0)
    {
        return;
    }

    // The next order, past the markers between songs (+++).
    order_++;
    bool marker = false;
    while (module_.orders[order_] == kOrderSkip && order_ < module_.order_count)
    {
        order_++;
        marker = true;
    }

    // Past the last order, or at a marker in song mode, the module goes back to its restart or its song's start.
    if (order_ >= module_.order_count || (marker && (mode_ & kModeSong)))
    {
        order_ = uint8_t(song_start_);
        if (!(mode_ & kModeLoop))
        {
            Stop();
            return;
        }
    }
    row_ = 0;
    FindRow();

    // A break goes to its row of the next order, or the first row if the pattern is shorter.
    if (goto_row_ >= 0)
    {
        row_ = goto_row_ >= rom_.U8(pattern_ + kPatternRows) ? uint8_t(0) : uint8_t(goto_row_);
        FindRow();
        goto_row_ = -1;
    }
}

void Player::PlayRow()
{
    row_order_ = order_;
    row_number_ = row_;
    row_loop_state_ =
        uint32_t(uint8_t(loop_row_)) | (uint32_t(uint8_t(loop_count_)) << 8) | (uint32_t(uint8_t(loop_active_)) << 16);

    while (status_ != 0)
    {
        const uint8_t follow = rom_.U8(data_++);
        if (!follow)
        {
            break;
        }

        // The channel's note and instrument, volume and effect, whichever the row gives.
        uint8_t note = 0, volume = 0, effect = 0, effect_op = 0;
        uint16_t instrument = 0;
        if (follow & 0x20)
        {
            const uint8_t b0 = rom_.U8(data_++);
            instrument = uint16_t(rom_.U8(data_++) | ((b0 & 1) << 8));
            note = uint8_t(b0 >> 1);
        }
        if (follow & 0x40)
        {
            volume = rom_.U8(data_++);
        }
        if (follow & 0x80)
        {
            effect = rom_.U8(data_++);
            effect_op = rom_.U8(data_++);
        }
        if ((follow & 0x1F) >= kPlayerChannels)
        {
            Warn("a pattern names channel " + std::to_string(follow & 0x1F) + ", past the player's channels");
            continue;
        }

        Channel& c = channels_[follow & 0x1F];
        c.in_row = 1;
        c.last_effect = c.effect;
        c.effect = effect;

        // An instrument sets the sample and its volume, and in a module with instruments, its pan too.
        bool set_volume = false, sample_set = false;
        if (instrument)
        {
            const uint32_t index = uint16_t(instrument - 1);
            if (!instrument_based_)
            {
                c.sample = rom_.U32(info_.samples + 4 * index);
                c.sample_index = int(index);
                c.volume = int8_t(rom_.U8(c.sample + 0xE));
                set_volume = true;
            }
            else if (note)
            {
                c.instrument = rom_.U32(info_.instruments + 4 * index);
                const uint32_t sample_index = rom_.U16(c.instrument + 2 * uint32_t(note) - 2);
                c.sample = rom_.U32(info_.samples + 4 * sample_index);
                c.sample_index = int(sample_index);
                c.panning = int8_t(rom_.U8(c.sample + 0xF));
                c.volume = int8_t(rom_.U8(c.sample + 0xE));
                set_volume = true;
                sample_set = true;
            }
        }

        // The volume column: a volume, or from 0x61 on, an effect.
        if (volume)
        {
            if (volume <= 0x60)
            {
                c.volume = int8_t(volume - 0x10);
                if (c.volume > 64)
                {
                    c.volume = 64;
                }
                c.column_effect = 0;
                set_volume = true;
            }
            else
            {
                c.column_effect = uint8_t(volume - 0x60);
                VolumeColumnEffect(c, true);
            }
        }

        // A note, unless the channel has no sample: a note off releases the note, a note with a slide to it sets the
        // slide's goal, and any other starts a note.
        c.play_note = 0;
        if (note && c.sample)
        {
            uint8_t n = uint8_t(note - 1);
            if (n == 0x7E)
            {
                if (c.volume_env_state)
                {
                    c.fading = 1;
                    c.volume_env_state = c.volume_env_state == 2 ? uint8_t(3) : uint8_t(4);
                    if (c.pan_env_state)
                    {
                        c.pan_env_state = c.pan_env_state == 2 ? uint8_t(3) : uint8_t(4);
                    }
                }
                else
                {
                    StopChannel(c);
                }
            }
            else
            {
                n = uint8_t(n + rom_.U8(c.sample + 0xD));
                if (effect == kPortaNote || effect == kVolumeSlidePorta)
                {
                    c.porta_goal = int16_t(PeriodOf(n, c.sample));
                }
                else
                {
                    if (instrument_based_)
                    {
                        if (!sample_set)
                        {
                            const uint32_t sample_index = rom_.U16(c.instrument + 2 * uint32_t(n));
                            c.sample = rom_.U32(info_.samples + 4 * sample_index);
                            c.sample_index = int(sample_index);
                        }

                        // The instrument's envelopes start again.
                        c.envelopes_on = 0;
                        c.instrument_vibrato_pos = 0;
                        if (rom_.U8(c.instrument + 0xF3) & 1)
                        {
                            c.volume_env_state = 3;
                            c.volume_env_tick = c.volume_env_point = 0xFF;
                            c.volume_env_target = 0;
                            c.fade = 0x7FFF;
                            c.fading = 0;
                            c.envelopes_on = 1;
                        }
                        else
                        {
                            c.envelope_volume = 64;
                            c.volume_env_state = 0;
                        }
                        if (rom_.U8(c.instrument + 0x127) & 1)
                        {
                            c.pan_env_state = 3;
                            c.pan_env_tick = c.pan_env_point = 0xFF;
                            c.pan_env_target = 0;
                            c.envelopes_on = 1;
                        }
                        else
                        {
                            c.envelope_pan = 0;
                            c.pan_env_state = 0;
                        }
                    }

                    c.gliss_note = c.note = n;
                    c.period = c.played_period = c.porta_goal = int16_t(PeriodOf(n, c.sample));
                    c.tremor_count = 0;
                    c.start_point = 0;
                    c.play_note = 1;
                }
            }
        }

        // The effect's part for the row, then the note or the volume.
        c.effect_op = effect_op;
        if (effect)
        {
            Effect(c, effect, true, false);
        }
        if (c.play_note)
        {
            PlayNote(c);
        }
        else if (set_volume)
        {
            SetChannelVolume(c);
        }
    }

    // The channels that the row leaves out lose their effects, and a note that an effect changed goes back to its
    // pitch, volume and pan when the effect changes.
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!c.in_row)
        {
            c.effect_op = 0;
            c.last_effect = c.effect;
            c.effect = 0;
            c.column_effect = 0;
        }
        else
        {
            c.in_row = 0;
        }

        if (c.restore_note && c.effect != c.last_effect)
        {
            SetChannelFreq(c);
            SetChannelVolume(c);
            SetChannelPan(c);
            c.restore_note = 0;
        }
    }

    if (status_ != 0)
    {
        NextRow();
    }
}

void Player::PlayTickBetweenRows()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (c.column_effect && kColumnBetween[c.column_effect >> 4])
        {
            VolumeColumnEffect(c, false);
        }

        if (!c.effect)
        {
            continue;
        }

        int effect = c.effect;
        if (effect < kEffectCount && EntryOf(effect).between)
        {
            Effect(c, effect, false, false);
            effect = c.effect;
        }
        if (effect < kEffectCount && EntryOf(effect).second)
        {
            Effect(c, effect, false, true);
        }
    }
}

void Player::RunEnvelopes()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!c.envelopes_on)
        {
            continue;
        }

        // The volume envelope and the fade after the release.
        const uint32_t ins = c.instrument;
        if (c.volume_env_state)
        {
            bool set_volume = false;
            if (c.fading)
            {
                set_volume = true;
                c.fade = int16_t(c.fade - rom_.U16(ins + 0x128));
                if (c.fade <= 0)
                {
                    StopChannel(c);
                    continue;
                }
            }

            if (c.volume_env_state > 2)
            {
                set_volume = true;
                const uint32_t env = ins + 0xC0;
                c.volume_env_level =
                    uint16_t(c.volume_env_level + rom_.U16(env + 4 * uint32_t(c.volume_env_point) + 2));
                c.volume_env_tick++;
                if (c.volume_env_tick == c.volume_env_target)
                {
                    // The next point: a sustain point holds until the release, and the last one loops or stops.
                    c.volume_env_point++;
                    if ((rom_.U8(env + 0x33) & 2) && c.volume_env_point == rom_.U8(env + 0x31) &&
                        c.volume_env_state != 4)
                    {
                        c.volume_env_state = 2;
                    }
                    if (c.volume_env_point == rom_.U8(env + 0x30))
                    {
                        if (rom_.U8(env + 0x33) & 4)
                        {
                            c.volume_env_point = rom_.U8(env + 0x32);
                            c.volume_env_tick = uint8_t(rom_.U16(env + 4 * uint32_t(c.volume_env_point)));
                        }
                        else
                        {
                            c.volume_env_state = 1;
                            if ((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) == 0)
                            {
                                StopChannel(c);
                                continue;
                            }
                        }
                    }
                    c.volume_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) << 8);
                    c.volume_env_target = uint8_t(rom_.U8(env + 4 * uint32_t(c.volume_env_point) + 4) + 1);
                }
            }

            if (set_volume)
            {
                c.envelope_volume = int8_t((int32_t(c.volume_env_level) * c.fade) >> 23);
                if (!SetVol(c.handle, uint32_t((c.volume * c.channel_volume * c.envelope_volume) >> 12)))
                {
                    StopChannel(c);
                    continue;
                }
            }
        }

        // The pan envelope.
        if (c.pan_env_state > 2)
        {
            const uint32_t env = ins + 0xF4;
            c.pan_env_level = uint16_t(c.pan_env_level + rom_.U16(env + 4 * uint32_t(c.pan_env_point) + 2));
            c.pan_env_tick++;
            if (c.pan_env_tick == c.pan_env_target)
            {
                c.pan_env_point++;
                if ((rom_.U8(env + 0x33) & 2) && c.pan_env_point == rom_.U8(env + 0x31) && c.pan_env_state != 4)
                {
                    c.pan_env_state = 2;
                }
                if (c.pan_env_point == rom_.U8(env + 0x30))
                {
                    if (rom_.U8(env + 0x33) & 4)
                    {
                        c.pan_env_point = rom_.U8(env + 0x32);
                        c.pan_env_tick = uint8_t(rom_.U8(env + 4 * uint32_t(c.pan_env_point)) + 1);
                    }
                    else
                    {
                        c.pan_env_state = 1;
                    }
                }
                c.pan_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.pan_env_point)) >> 9) << 8);
                c.pan_env_target = rom_.U8(env + 4 * uint32_t(c.pan_env_point) + 4);
            }

            c.envelope_pan = int8_t(((c.pan_env_level >> 8) - 32) << 1);
            int pan = int8_t(uint8_t(c.panning + c.envelope_pan));
            if (!SetPan(c.handle, std::clamp(pan, -64, 64)))
            {
                StopChannel(c);
                continue;
            }
        }

        // The instrument's vibrato, on the pitch that the effects leave.
        const uint8_t rate = rom_.U8(ins + 0x12D);
        if (rate)
        {
            const int32_t wave = Wave(c.vibrato_wave, uint32_t(c.instrument_vibrato_pos) >> 2);
            const uint16_t period = uint16_t(uint16_t(c.played_period) + ((wave * rom_.U8(ins + 0x12C)) >> 8));
            SetFreq(c.handle, FrequencyOf(period));
            c.instrument_vibrato_pos = uint8_t(c.instrument_vibrato_pos + rate);
        }
    }
}

void Player::PlayNote(Channel& c)
{
    c.restore_note = 0;
    if (!c.sample)
    {
        Warn("a note plays on a channel that has no sample yet, so it's left out");
        return;
    }

    const uint32_t freq = FrequencyOf(c.period);
    c.handle =
        Play(c.sample, c.sample_index, 0, c.handle, freq, uint32_t((c.volume * c.channel_volume) >> 6), c.panning);
    if (c.start_point)
    {
        SetPos(c.handle, uint32_t(c.start_point) << 8);
    }
}

void Player::Stop()
{
    if (status_ == 0)
    {
        return;
    }

    status_ = 0;
    SetTimer(false);
    for (int i = 0; i < module_.channels; i++)
    {
        StopChannel(channels_[size_t(i)]);
    }
}

void Player::SetGlobalVolume()
{
    SetMusicVolume((int32_t(module_.global_volume) * volume_ * music_volume_) >> 13);
}

void Player::SetChannelVolume(Channel& c)
{
    SetVol(c.handle, uint32_t((c.channel_volume * c.volume * c.envelope_volume) >> 12));
}

void Player::SetChannelPan(Channel& c)
{
    const int pan = int8_t(uint8_t(c.panning + c.envelope_pan));
    SetPan(c.handle, std::clamp(pan, -64, 64));
}

void Player::SetChannelFreq(Channel& c)
{
    c.played_period = c.period;
    SetFreq(c.handle, FrequencyOf(c.period));
}

void Player::StopChannel(Channel& c)
{
    c.envelopes_on = 0;
    Stop(c.handle);
}

uint16_t Player::PeriodOf(uint32_t note, uint32_t sample) const
{
    const int fine_tune = rom_.S8(sample + 0xC);
    if (module_.linear)
    {
        return uint16_t((fine_tune >> 1) + int(note << 6) + 0x600);
    }

    const uint32_t factor = rom_.U16(info_.fine_tunes + 2 * uint32_t(32 + (fine_tune >> 2)));
    return uint16_t((rom_.U16(info_.periods + 2 * note) * factor) >> 15);
}

uint32_t Player::FrequencyOf(int32_t period) const
{
    if (module_.linear)
    {
        return LinearFreq(uint32_t(period));
    }

    return uint32_t(Div(kAmigaClock, period).first);
}

uint32_t Player::LinearFreq(uint32_t period) const
{
    // The octave above the table's, and the step within it, of 768 to the octave.
    const uint32_t octave = ((period >> 6) * 0xAAAB) >> 19;
    const uint32_t step = period - octave * 768;
    return ShiftRight(rom_.U32(info_.linear + 4 * step), 23 - octave);
}

int16_t Player::Wave(uint32_t table, uint32_t index) const
{
    return int16_t(rom_.U16(table + 2 * index));
}

// ---------------------------------------------------------------------------------------------------------------------
// The effects

void Player::Effect(Channel& c, int effect, bool on_row, bool second)
{
    const uint8_t op = c.effect_op;
    switch (second ? (effect == kVolumeSlideVibrato || effect == kVolumeSlideVibratoXm ? kVibrato : kPortaNote)
                   : effect)
    {
    case kSpeed:
        if (op)
        {
            speed_ = op;
        }
        break;

    case kBpm:
    case kSpeedBpm:
        if (op > 0x1F)
        {
            tempo_ = op;
            SetTimerBpm(op * 24);
        }
        else if (effect == kSpeedBpm)
        {
            speed_ = op;
        }
        break;

    case kPatternJump:
        goto_order_ = int8_t(op);
        break;

    case kPatternBreak:
        goto_row_ = int8_t(uint8_t((op >> 4) * 10 + (op & 0xF)));
        break;

    case kVolumeSlideS3m:
    case kVolumeSlideVibrato:
    case kVolumeSlidePorta:
        VolumeSlideS3m(c, on_row);
        break;

    case kVolumeSlideXm:
    case kVolumeSlideVibratoXm:
    case kVolumeSlidePortaXm:
        VolumeSlideXm(c, on_row);
        break;

    case kVolumeSlideDownFine:
    case kVolumeSlideUpFine:
        if (op)
        {
            c.fine_volume_slide = op;
        }
        if (effect == kVolumeSlideDownFine)
        {
            VolumeDown(c, c.fine_volume_slide);
        }
        else
        {
            VolumeUp(c, c.fine_volume_slide);
        }
        break;

    case kPortaDownXm:
    case kPortaUpXm:
        if (on_row)
        {
            if (op)
            {
                c.porta = op;
            }
            break;
        }

        // Down subtracts, and a period that reaches 0 stops the note.
        if (effect == kPortaUpXm)
        {
            c.period = int16_t(c.period + (c.porta << 2));
            SetChannelFreq(c);
            break;
        }
        c.period = int16_t(c.period - (c.porta << 2));
        if (c.period > 0)
        {
            SetChannelFreq(c);
        }
        else
        {
            StopChannel(c);
        }
        break;

    case kPortaDownS3m:
    case kPortaUpS3m:
        PortaS3m(c, on_row, effect == kPortaUpS3m);
        break;

    case kPortaDownFine:
    case kPortaDownExtraFine:
    case kPortaUpFine:
    case kPortaUpExtraFine:
        {
            const bool fine = effect == kPortaDownFine || effect == kPortaUpFine;
            uint8_t& step = fine ? c.fine_porta : c.extra_fine_porta;
            if (op)
            {
                step = op;
            }
            const int by = fine ? step << 2 : step;
            if (effect == kPortaUpFine || effect == kPortaUpExtraFine)
            {
                c.period = int16_t(c.period + by);
                SetChannelFreq(c);
                break;
            }
            c.period = int16_t(c.period - by);
            if (c.period > 0)
            {
                SetChannelFreq(c);
            }
            else
            {
                StopChannel(c);
            }
            break;
        }

    case kVolume:
        c.volume = int8_t(op);
        SetChannelVolume(c);
        break;

    case kPortaNote:
        PortaNote(c, on_row);
        break;

    case kVibrato:
        Vibrato(c, on_row);
        break;

    case kTremor:
        if (on_row && op)
        {
            c.tremor_on = uint8_t((op >> 4) + 1);
            c.tremor_cycle = uint8_t(c.tremor_on + (op & 0xF) + 1);
        }
        if (c.tremor_count == c.tremor_on)
        {
            SetVol(c.handle, 0);
            c.restore_note = 1;
        }
        else if (c.tremor_count == c.tremor_cycle)
        {
            c.tremor_count = 0;
            SetChannelVolume(c);
            c.restore_note = 0;
        }
        c.tremor_count++;
        break;

    case kArpeggio:
        Arpeggio(c, on_row);
        break;

    case kChannelVolume:
        c.channel_volume = int8_t(op);
        if (c.channel_volume > 64)
        {
            c.channel_volume = 64;
        }
        SetChannelVolume(c);
        break;

    case kChannelVolumeSlide:
        if (on_row)
        {
            if (op)
            {
                c.channel_volume_slide = op;
            }
        }
        else if (c.channel_volume_slide & 0xF)
        {
            c.channel_volume = int8_t(c.channel_volume - (c.channel_volume_slide & 0xF));
            c.channel_volume = std::max<int8_t>(c.channel_volume, 0);
            SetChannelVolume(c);
        }
        else if (c.channel_volume_slide >> 4)
        {
            c.channel_volume = int8_t(c.channel_volume + (c.channel_volume_slide >> 4));
            c.channel_volume = std::min<int8_t>(c.channel_volume, 64);
            SetChannelVolume(c);
        }
        break;

    case kOffset:
        if (op)
        {
            if (c.play_note)
            {
                c.start_point = op;
            }
            else
            {
                SetPos(c.handle, uint32_t(op) << 8);
            }
        }
        break;

    case kPanSlide:
        PanSlide(c, on_row);
        break;

    case kRetrig:
        Retrig(c, on_row);
        break;

    case kTremolo:
        if (on_row)
        {
            if (c.play_note && c.tremolo_reset)
            {
                c.tremolo_pos = 0;
            }
            if (op & 0xF)
            {
                c.tremolo_depth = uint8_t(op & 0xF);
            }
            if (op & 0xF0)
            {
                c.tremolo_speed = uint8_t(op >> 4);
            }
        }
        else
        {
            // This build shifts the volume down by 6 bits rather than 12, so the mixer gets the bottom byte of a volume
            // 64 times too loud.
            const int delta = int8_t((Wave(c.tremolo_wave, c.tremolo_pos) * c.tremolo_depth) >> 6);
            const int volume = std::clamp<int>(int8_t(uint8_t(delta + c.volume)), 0, 64);
            SetVol(c.handle, uint32_t(volume * c.channel_volume * c.envelope_volume) >> 6);
            c.tremolo_pos = uint8_t((c.tremolo_pos + c.tremolo_speed) & 0x3F);
            c.restore_note = 1;
        }
        break;

    case kFineVibrato:
        Vibrato(c, on_row);
        if (on_row && (op & 0xF))
        {
            c.vibrato_depth >>= 2;
        }
        break;

    case kGlobalVolume:
        volume_ = int8_t(std::min<uint8_t>(op, 64));
        SetGlobalVolume();
        break;

    case kGlobalVolumeSlide:
        if (on_row)
        {
            if (op)
            {
                volume_slide_ = op;
            }
        }
        else if (volume_slide_ & 0xF)
        {
            volume_ = std::max<int8_t>(int8_t(volume_ - (volume_slide_ & 0xF)), 0);
            SetGlobalVolume();
        }
        else if (volume_slide_ >> 4)
        {
            volume_ = std::min<int8_t>(int8_t(volume_ + (volume_slide_ >> 4)), 64);
            SetGlobalVolume();
        }
        break;

    case kPan:
        c.panning = op > 0x80 ? int8_t(0) : int8_t(op - 0x40);
        SetChannelPan(c);
        break;

    case kPanbrello:
        if (on_row)
        {
            if (c.play_note && c.panbrello_reset)
            {
                c.panbrello_pos = 0;
            }
            if (op & 0xF)
            {
                c.panbrello_depth = uint8_t(op & 0xF);
            }
            if (op & 0xF0)
            {
                c.panbrello_speed = uint8_t(op >> 4);
            }
        }
        else
        {
            const int delta = int8_t((Wave(c.panbrello_wave, c.panbrello_pos >> 2) * c.panbrello_depth) >> 6);
            const int panning = int8_t(uint8_t(delta + c.panning));
            c.panbrello_pos = uint8_t(c.panbrello_pos + c.panbrello_speed);
            SetPan(c.handle, std::clamp(panning + c.envelope_pan, -64, 64));
            c.restore_note = 1;
        }
        break;

    case kGlissando:
        c.glissando = op != 0;
        break;

    case kVibratoWave:
    case kTremoloWave:
    case kPanbrelloWave:
        {
            const uint32_t tables[4] = {info_.sine, info_.ramp, info_.square, info_.random};
            uint32_t& table = effect == kVibratoWave   ? c.vibrato_wave
                              : effect == kTremoloWave ? c.tremolo_wave
                                                       : c.panbrello_wave;
            uint8_t& retrig = effect == kVibratoWave   ? c.vibrato_reset
                              : effect == kTremoloWave ? c.tremolo_reset
                                                       : c.panbrello_reset;
            table = tables[op & 3];
            retrig = op <= 3;
            break;
        }

    case kPatternLoop:
        PatternLoop(c);
        break;

    case kNoteCut:
        if (on_row)
        {
            if (op == 0)
            {
                c.play_note = 0;
            }
        }
        else if (op == tick_)
        {
            StopChannel(c);
        }
        break;

    case kNoteDelay:
        if (on_row)
        {
            if (op)
            {
                c.play_note = 0;
            }
        }
        else if (op == tick_)
        {
            PlayNote(c);
        }
        break;

    case kMark:
    default:
        if (effect >= kEffectCount)
        {
            Warn("a pattern has effect " + std::to_string(effect) + ", which the player doesn't have");
        }
        break;
    }
}

void Player::VolumeColumnEffect(Channel& c, bool on_row)
{
    const int x = c.column_effect & 0xF;
    switch (c.column_effect >> 4)
    {
    case 0:
        if (!on_row)
        {
            VolumeDown(c, x);
        }
        break;

    case 1:
        if (!on_row)
        {
            VolumeUp(c, x);
        }
        break;

    case 2:
        VolumeDown(c, x);
        break;

    case 3:
        VolumeUp(c, x);
        break;

    case 4:
        if (c.column_effect)
        {
            c.vibrato_speed = uint8_t(x);
        }
        break;

    case 5:
        if (!on_row)
        {
            VibratoTick(c);
        }
        else if (c.column_effect)
        {
            c.vibrato_depth = uint8_t(x << 2);
        }
        break;

    case 6:
        c.panning = c.column_effect == 7 ? int8_t(0) : int8_t(((x * 0x88900) >> 16) - 64);
        SetChannelPan(c);
        break;

    case 7:
        if (!on_row)
        {
            PanLeft(c, x);
        }
        break;

    case 8:
        if (!on_row)
        {
            PanRight(c, x);
        }
        break;

    default:
        // The slide to a note sets its step on the row; between rows, this build runs the effect's part for the row
        // rather than the slide.
        if (on_row)
        {
            if (c.effect_op)
            {
                c.porta_step = uint16_t(x << 4);
            }
        }
        else
        {
            PortaNote(c, true);
        }
        break;
    }
}

void Player::VolumeDown(Channel& c, int by)
{
    c.volume = std::max<int8_t>(int8_t(c.volume - by), 0);
    SetChannelVolume(c);
}

void Player::VolumeUp(Channel& c, int by)
{
    c.volume = std::min<int8_t>(int8_t(c.volume + by), 64);
    SetChannelVolume(c);
}

void Player::PanLeft(Channel& c, int by)
{
    c.panning = std::max<int8_t>(int8_t(c.panning - by), -64);
    SetChannelPan(c);
}

void Player::PanRight(Channel& c, int by)
{
    c.panning = std::min<int8_t>(int8_t(c.panning + by), 64);
    SetChannelPan(c);
}

void Player::VolumeSlideS3m(Channel& c, bool on_row)
{
    // On the row, a fine slide (xF or Fx) slides once, and a normal one, in a module with fast slides, too.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.volume_slide = c.effect_op;
        }

        const int up = c.volume_slide >> 4, down = c.volume_slide & 0xF;
        c.volume_slide_once = 0;
        if (up && down == 0xF)
        {
            c.volume_slide_once = 1;
            VolumeUp(c, up);
        }
        else if (down && up == 0xF)
        {
            c.volume_slide_once = 1;
            VolumeDown(c, down);
        }

        if (c.effect == kVolumeSlideVibrato && c.restore_note && c.last_effect == kVibrato)
        {
            c.restore_note = 0;
        }
        if (!fast_slides_)
        {
            return;
        }
    }

    if (c.volume_slide_once)
    {
        return;
    }

    if (c.volume_slide & 0xF)
    {
        VolumeDown(c, c.volume_slide & 0xF);
    }
    else if (c.volume_slide >> 4)
    {
        VolumeUp(c, c.volume_slide >> 4);
    }
}

void Player::VolumeSlideXm(Channel& c, bool on_row)
{
    if (on_row)
    {
        if (c.effect_op)
        {
            c.volume_slide = c.effect_op;
        }
        if (c.effect == kVolumeSlideVibratoXm && c.restore_note && c.last_effect == kVibrato)
        {
            c.restore_note = 0;
        }
        return;
    }

    if (c.volume_slide & 0xF)
    {
        VolumeDown(c, c.volume_slide & 0xF);
    }
    else if (c.volume_slide >> 4)
    {
        VolumeUp(c, c.volume_slide >> 4);
    }
}

void Player::PortaS3m(Channel& c, bool on_row, bool up)
{
    // On the row, Ex and Fx steps slide once, extra fine and fine; between rows, other steps slide by 4 times
    // themselves. Up lowers the period, and a period that reaches 0 stops the note.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.porta = c.effect_op;
        }
        if (c.porta < 0xE0)
        {
            return;
        }
        if ((c.porta & 0xF0) == 0xF0)
        {
            c.period = int16_t(c.period + (up ? -1 : 1) * ((c.porta & 0xF) << 2));
        }
        if ((c.porta & 0xF0) == 0xE0)
        {
            c.period = int16_t(c.period + (up ? -1 : 1) * (c.porta & 0xF));
        }
        if (up && c.period <= 0)
        {
            StopChannel(c);
        }
        SetChannelFreq(c);
        return;
    }

    if (uint8_t(c.porta - 1) > 0xDE)
    {
        return;
    }

    const int by = c.porta << 2;
    if (!up)
    {
        c.period = int16_t(c.period + by);
        SetChannelFreq(c);
        return;
    }
    c.period = int16_t(c.period - by);
    if (c.period <= 0)
    {
        StopChannel(c);
        return;
    }
    SetChannelFreq(c);
}

void Player::PortaNote(Channel& c, bool on_row)
{
    if (on_row)
    {
        if (c.effect_op)
        {
            c.porta_step = c.effect_op;
        }
        return;
    }

    const auto period_of = [&](int note)
    {
        return int(rom_.U16(info_.periods + 2 * uint32_t(int16_t(note))));
    };

    // The period slides towards the goal and stops there. With glissando, the note plays the nearest note of the table
    // on the slide's side.
    const int16_t step = int16_t(c.porta_step << 2);
    if (c.period > c.porta_goal)
    {
        c.period = int16_t(c.period - step);
        if (c.period < c.porta_goal)
        {
            c.period = c.porta_goal;
        }
        if (c.glissando)
        {
            if (c.period < period_of(c.gliss_note))
            {
                do
                {
                    c.gliss_note++;
                } while (c.period < period_of(c.gliss_note));
            }
            if (c.period > period_of(c.gliss_note))
            {
                c.gliss_note--;
            }
        }
    }
    else if (c.period < c.porta_goal)
    {
        c.period = int16_t(c.period + step);
        if (c.period > c.porta_goal)
        {
            c.period = c.porta_goal;
        }
        if (c.glissando)
        {
            if (c.period > period_of(c.gliss_note))
            {
                do
                {
                    c.gliss_note--;
                } while (c.period > period_of(c.gliss_note));
            }
            if (c.period < period_of(c.gliss_note))
            {
                c.gliss_note++;
            }
        }
    }
    else
    {
        return;
    }

    if (!c.glissando)
    {
        SetChannelFreq(c);
        return;
    }
    const uint16_t period = rom_.U16(info_.periods + 2 * uint32_t(c.gliss_note));
    c.played_period = int16_t(period);
    SetFreq(c.handle, FrequencyOf(period));
}

void Player::Vibrato(Channel& c, bool on_row)
{
    if (!on_row)
    {
        VibratoTick(c);
        return;
    }

    if (c.play_note && c.vibrato_reset)
    {
        c.vibrato_pos = vibrato_offset_;
    }
    if (c.effect_op & 0xF)
    {
        c.vibrato_depth = uint8_t((c.effect_op & 0xF) << 2);
    }
    if (c.effect_op & 0xF0)
    {
        c.vibrato_speed = uint8_t(c.effect_op >> 4);
    }
    if (c.restore_note && c.last_effect == kVolumeSlideVibrato)
    {
        c.restore_note = 0;
    }
}

void Player::VibratoTick(Channel& c)
{
    const uint16_t period =
        uint16_t(uint16_t(c.period) + ((Wave(c.vibrato_wave, c.vibrato_pos) * c.vibrato_depth) >> 7));
    c.played_period = int16_t(period);
    SetFreq(c.handle, FrequencyOf(period));
    c.vibrato_pos = uint8_t((c.vibrato_pos + c.vibrato_speed) & 0x3F);
    c.restore_note = 1;
}

void Player::PanSlide(Channel& c, bool on_row)
{
    // On the row, a fine slide (xF or Fx) slides once by twice its step. The fine slide right marks the volume slide as
    // fine rather than the pan slide, as this build does.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.pan_slide = c.effect_op;
        }

        const int left = c.pan_slide >> 4, right = c.pan_slide & 0xF;
        c.pan_slide_once = 0;
        if (left && right == 0xF)
        {
            c.pan_slide_once = 1;
            PanLeft(c, left << 1);
        }
        else if (right && left == 0xF)
        {
            c.volume_slide_once = 1;
            PanRight(c, right << 1);
        }
        return;
    }

    if (c.pan_slide_once)
    {
        return;
    }

    if (c.pan_slide & 0xF)
    {
        PanRight(c, (c.pan_slide & 0xF) << 1);
    }
    else if (c.pan_slide >> 4)
    {
        PanLeft(c, (c.pan_slide >> 4) << 1);
    }
}

void Player::Retrig(Channel& c, bool on_row)
{
    // On the row, a note that the row doesn't start starts again; between rows, the note starts again every so many
    // ticks, with its volume changed as the high digit says.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.retrig = c.effect_op;
        }
        if (!c.play_note && c.retrig)
        {
            SetPos(c.handle, 0);
        }
        return;
    }

    if (!c.retrig || Div(tick_, c.retrig).second != 0)
    {
        return;
    }

    SetPos(c.handle, 0);

    const int x = c.retrig >> 4;
    if (x >= 1 && x <= 5)
    {
        VolumeDown(c, 1 << (x - 1));
    }
    else if (x >= 9 && x <= 13)
    {
        VolumeUp(c, 1 << (x - 9));
    }
    else if (x == 6 || x == 7 || x == 14 || x == 15)
    {
        // Two thirds, a half, three halves and twice the volume.
        const int volume = c.volume;
        const int changed = x == 6    ? (volume * 342) >> 9
                            : x == 7  ? volume >> 1
                            : x == 14 ? (volume * 3) >> 1
                                      : volume << 1;
        c.volume = int8_t(changed);
        if ((x == 6 ? changed : int(c.volume)) > 64)
        {
            c.volume = 64;
        }
        SetChannelVolume(c);
    }
}

void Player::Arpeggio(Channel& c, bool on_row)
{
    if (on_row)
    {
        if (c.effect_op)
        {
            c.arpeggio = c.effect_op;
        }
        if (!c.play_note)
        {
            SetChannelFreq(c);
        }
        c.arpeggio_step = 0;
        return;
    }

    // Each tick plays the note, the note plus the high digit and the note plus the low digit in turn, with linear
    // frequencies in every module.
    if (!c.sample)
    {
        return;
    }

    c.arpeggio_step++;
    if (c.arpeggio_step > 2)
    {
        c.arpeggio_step = 0;
    }
    int16_t note = c.note;
    if (c.arpeggio_step == 1)
    {
        note = int16_t(uint16_t(c.note) + (c.arpeggio >> 4));
    }
    else if (c.arpeggio_step == 2)
    {
        note = int16_t(uint16_t(c.note) + (c.arpeggio & 0xF));
    }

    const uint32_t capped = std::min<uint32_t>(uint16_t(note), 0x77);
    const uint16_t period = uint16_t((rom_.S8(c.sample + 0xC) >> 1) + int(capped << 6) + 0x600);
    c.played_period = int16_t(period);
    SetFreq(c.handle, LinearFreq(uint32_t(int32_t(int16_t(period)))));
    c.restore_note = 1;
}

void Player::PatternLoop(Channel& c)
{
    // An operand of 0 marks the row that the loop goes back to, and another goes back there that many times.
    if (!c.effect_op)
    {
        loop_row_ = int8_t(row_);
        return;
    }

    if (!loop_active_)
    {
        loop_count_ = int8_t(c.effect_op);
        loop_active_ = 1;
    }
    loop_count_--;
    if (loop_count_ == -1)
    {
        loop_active_ = 0;
    }
    else
    {
        goto_row_ = loop_row_;
    }
}

} // namespace supergbamidi::krawall
