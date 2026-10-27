// SPDX-License-Identifier: MIT

// The parts of the model in which the builds of player.c, 1.2.2.10 to 1.4, differ from the build of 2003/09/01.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"

#include "krawall/build.h"
#include "krawall/driver.h"
#include "krawall/player.h"

namespace agbsf2::krawall
{

std::vector<uint8_t> Player::OldMixRecord(const MixChannel& m) const
{
    // The fields up to the sides are where the later builds keep them. The record has no ramp. The play's choice of
    // interpolation, the sound effects' flag and the mixing routine follow the sides.
    std::vector<uint8_t> r(0x28);
    r[0x00] = m.volume;
    r[0x01] = uint8_t(m.pan);
    r[0x02] = m.status;
    r[0x03] = m.loop;
    xsflib::StoreLe32(&r[0x04], m.start);
    xsflib::StoreLe32(&r[0x08], m.pos);
    xsflib::StoreLe32(&r[0x0C], m.end);
    xsflib::StoreLe32(&r[0x10], m.loop_length);
    xsflib::StoreLe32(&r[0x14], uint32_t(m.inc));
    xsflib::StoreLe32(&r[0x18], m.handle);
    xsflib::StoreLe16(&r[0x1C], m.frac);
    r[0x1E] = m.left;
    r[0x1F] = m.right;
    r[0x20] = m.hq;
    r[0x21] = m.hq_sample;
    r[0x22] = m.sfx;
    r[0x23] = m.mix;
    xsflib::StoreLe32(&r[0x24], m.loop_from);

    return r;
}

std::vector<uint8_t> Player::OldChannelRecord(const std::vector<uint8_t>& r)
{
    // The pan, the start point and the effect move down a byte, and the note and its periods two. The slides from the
    // volume slide's once flag to the envelope's volume move down a byte. Padding takes the rest.
    std::vector<uint8_t> old(r.size());
    std::copy(r.begin(), r.begin() + 0x05, old.begin());
    std::copy(r.begin() + 0x06, r.begin() + 0x09, old.begin() + 0x05);
    std::copy(r.begin() + 0x0A, r.begin() + 0x10, old.begin() + 0x08);
    std::copy(r.begin() + 0x10, r.begin() + 0x3E, old.begin() + 0x10);
    std::copy(r.begin() + 0x3F, r.begin() + 0x4E, old.begin() + 0x3E);
    std::copy(r.begin() + 0x4E, r.end(), old.begin() + 0x4E);

    return old;
}

void Player::OldFrame()
{
    // The worker mixes up to each tick that falls in the frame and runs it if the sound timer runs. The count starts
    // again either way.
    uint32_t amount = uint32_t(info_.frame_samples);
    while (amount)
    {
        if (timer_count_ > amount)
        {
            Mix(amount);
            timer_count_ = uint16_t(timer_count_ - amount);
            break;
        }

        const uint32_t count = timer_count_;
        Mix(count);
        amount -= count;
        if (timer_on_)
        {
            Timer();
            if (on_tick_)
            {
                on_tick_(elapsed_);
            }
        }
        timer_count_ = timer_speed_;
    }
}

void Player::NextRowPlayer()
{
    // A jump goes straight to its order's first row, or to the first order if it names one past the last.
    if (goto_order_ >= 0)
    {
        order_ = goto_order_ < module_.order_count ? uint8_t(goto_order_) : uint8_t(0);
        goto_order_ = -1;
        row_ = 0;
        FindRow();
        BreakRow();
        return;
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

    if ((order_ >= module_.order_count || (marker && (mode_ & kModeSong))) && !EndOfModule())
    {
        return;
    }

    row_ = 0;
    FindRow();
    BreakRow();
}

void Player::OldPatternJump(uint8_t op)
{
    // Without a break, the jump goes to its order's first row.
    if (goto_row_ < 0)
    {
        goto_row_ = 0;
    }
    goto_order_ = int8_t(op);
}

void Player::OldPatternBreak(uint8_t op)
{
    // Without a jump, the break goes to the next order. Its operand is binary rather than decimal, and a row past 63
    // counts as row 0.
    if (goto_order_ < 0)
    {
        goto_order_ = int8_t(uint8_t(order_ + 1));
    }
    goto_row_ = op > 0x3F ? 0 : op;
}

void Player::OldNextRow()
{
    // A jump goes to its order, or to the first order if it names one past the last. A break then goes to its row,
    // whatever the pattern's length. Neither looks for a marker or the module's end.
    bool moved = false;
    if (goto_order_ >= 0)
    {
        order_ = goto_order_ < module_.order_count ? uint8_t(goto_order_) : uint8_t(0);
        row_ = 0;
        FindRow();
        goto_order_ = -1;
        moved = true;
    }

    if (goto_row_ >= 0)
    {
        row_ = uint16_t(goto_row_);
        FindRow();
        goto_row_ = -1;
        moved = true;
    }

    if (moved)
    {
        return;
    }

    row_++;
    if (row_ < rom_.U8(pattern_ + kPatternRows))
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

    if ((order_ >= module_.order_count || (marker && (mode_ & kModeSong))) && !EndOfModule())
    {
        return;
    }

    row_ = 0;
    FindRow();
}

bool Player::OldRunVolumeEnvelope(Channel& c)
{
    // The fade after the release.
    const uint32_t ins = c.instrument;
    if (c.fading)
    {
        c.fade = int16_t(c.fade - rom_.U16(ins + kInstrumentFade));
        if (c.fade <= 0)
        {
            StopChannel(c);
            return false;
        }
    }

    if (c.volume_env_state > 2)
    {
        const uint32_t env = ins + kInstrumentVolumeEnvelope;
        if (c.volume_env_tick++ == c.volume_env_target)
        {
            // The next point: a sustain point holds until the release, and the last one loops or stops.
            c.volume_env_point++;
            if ((rom_.U8(env + kEnvelopeFlags) & kEnvelopeSustains) &&
                c.volume_env_point == rom_.U8(env + kEnvelopeSustain) && c.volume_env_state != 4)
            {
                c.volume_env_state = 2;
            }
            if (c.volume_env_point == rom_.U8(env + kEnvelopeLast))
            {
                if (rom_.U8(env + kEnvelopeFlags) & kEnvelopeLoops)
                {
                    c.volume_env_point = rom_.U8(env + kEnvelopeLoopStart);
                    c.volume_env_tick = uint8_t(rom_.U16(env + 4 * uint32_t(c.volume_env_point)));
                }
                else
                {
                    c.volume_env_state = 1;
                    if ((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) == 0)
                    {
                        StopChannel(c);
                        return false;
                    }
                }
            }
            c.volume_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) << 8);
            c.volume_env_target = rom_.U8(env + 4 * uint32_t(c.volume_env_point) + 4);
        }

        c.envelope_volume = int8_t((int32_t(c.volume_env_level) * c.fade) >> 23);
        c.volume_env_level = uint16_t(c.volume_env_level + rom_.U16(env + 4 * uint32_t(c.volume_env_point) + 2));
    }
    else
    {
        c.envelope_volume = int8_t((int32_t(c.volume_env_level) * c.fade) >> 23);
    }

    if (!SetChannelVolume(c))
    {
        StopChannel(c);
        return false;
    }

    return true;
}

bool Player::OldRunPanEnvelope(Channel& c)
{
    const uint32_t env = c.instrument + kInstrumentPanEnvelope;
    if (c.pan_env_tick++ == c.pan_env_target)
    {
        c.pan_env_point++;
        if ((rom_.U8(env + kEnvelopeFlags) & kEnvelopeSustains) && c.pan_env_point == rom_.U8(env + kEnvelopeSustain) &&
            c.pan_env_state != 4)
        {
            c.pan_env_state = 2;
        }
        if (c.pan_env_point == rom_.U8(env + kEnvelopeLast))
        {
            if (rom_.U8(env + kEnvelopeFlags) & kEnvelopeLoops)
            {
                c.pan_env_point = rom_.U8(env + kEnvelopeLoopStart);
                c.pan_env_tick = uint8_t(rom_.U16(env + 4 * uint32_t(c.pan_env_point)));
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
    c.pan_env_level = uint16_t(c.pan_env_level + rom_.U16(env + 4 * uint32_t(c.pan_env_point) + 2));
    if (!SetChannelPan(c))
    {
        StopChannel(c);
        return false;
    }

    return true;
}

bool Player::EndOfModule()
{
    // The module goes back to its restart or its song's start. Build 1.4 stops a module that doesn't loop at once.
    // The builds before it stop it and still read the order's first row.
    order_ = uint8_t(song_start_);
    if (mode_ & kModeLoop)
    {
        return true;
    }

    StopModule();

    return info_.build < Build::kV1_4;
}

void Player::BreakRow()
{
    // A break goes to its row of the order, or the first row if the pattern is shorter.
    if (goto_row_ < 0)
    {
        return;
    }

    row_ = goto_row_ >= rom_.U8(pattern_ + kPatternRows) ? uint8_t(0) : uint8_t(goto_row_);
    FindRow();
    goto_row_ = -1;
}

} // namespace agbsf2::krawall
