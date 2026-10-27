// SPDX-License-Identifier: MIT

#include "krawall/unpack.h"

namespace agbsf2::krawall
{
namespace
{

// The stream's header, after the 3-byte size.
constexpr uint32_t kEscape = 3;     // the first escape code
constexpr uint32_t kEscapeBits = 4; // the bits of an escape code, and of a literal's top bits
constexpr uint32_t kMaxGamma = 5;   // the most bits of a gamma code's value
constexpr uint32_t kShortRun = 6;   // a run's gamma code below this is its length
constexpr uint32_t kOffsetBits = 7; // the bits that follow a long copy's first gamma code
constexpr uint32_t kRankCount = 8;  // the count of the frequent bytes after it
constexpr uint32_t kMaxRanks = 32;  // the decoder's stack has room for 32

// Reads the stream's bits, the top bit of each byte first.
class BitReader
{
public:
    BitReader(const Rom& rom, uint32_t at) : rom_(rom), at_(at)
    {
    }

    uint32_t Bit()
    {
        if (left_ == 0)
        {
            ok_ = ok_ && rom_.Contains(at_);
            byte_ = rom_.U8(at_++);
            left_ = 8;
        }
        left_--;
        return (byte_ >> left_) & 1;
    }

    // Returns `value` with `n` more bits read below it.
    uint32_t Bits(uint32_t value, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
        {
            value = value * 2 + Bit();
        }

        return value;
    }

    // Reads a gamma code: up to `max` - 1 bits of 1, a 0 unless there are that many, then as many bits as there were
    // 1s below a leading 1.
    uint32_t Gamma(uint32_t max)
    {
        uint32_t ones = 0;
        while (ones + 1 < max && Bit())
        {
            ones++;
        }

        return Bits(1, ones);
    }

    // Returns false once a read has passed the ROM's end.
    bool Ok() const
    {
        return ok_;
    }

private:
    const Rom& rom_;
    uint32_t at_;
    uint8_t byte_ = 0;
    int left_ = 0;
    bool ok_ = true;
};

} // namespace

bool Unpack(const Rom& rom, uint32_t at, std::vector<uint8_t>& out)
{
    out.clear();
    const uint32_t size = rom.U8(at) | (rom.U8(at + 1) << 8) | (rom.U8(at + 2) << 16);
    uint32_t escape = rom.U8(at + kEscape);
    const uint32_t escape_bits = rom.U8(at + kEscapeBits);
    const uint32_t max_gamma = rom.U8(at + kMaxGamma);
    const uint32_t short_run = rom.U8(at + kShortRun);
    const uint32_t offset_bits = rom.U8(at + kOffsetBits);
    const uint32_t rank_count = rom.U8(at + kRankCount);
    const uint32_t end_code = (2 * short_run - 1) & 0xFF;

    // The game's decoder loops on without end for the values outside these ranges.
    if (!rom.Contains(at, kRankCount + 1 + rank_count) || escape_bits > 8 || max_gamma < 2 || max_gamma > 8 ||
        offset_bits > 8 || rank_count == 0 || rank_count > kMaxRanks)
    {
        return false;
    }

    std::vector<uint8_t> ranks(rank_count);
    for (uint32_t i = 0; i < rank_count; i++)
    {
        ranks[i] = rom.U8(at + kRankCount + 1 + i);
    }

    BitReader bits(rom, at + kRankCount + 1 + rank_count);

    // Copies `count` bytes from `distance` bytes back. The copy may overlap what it makes.
    const auto copy = [&](uint32_t distance, uint32_t count)
    {
        if (distance > out.size())
        {
            return false;
        }
        for (uint32_t i = 0; i < count && out.size() <= size; i++)
        {
            out.push_back(out[out.size() - distance]);
        }
        return true;
    };

    while (bits.Ok() && out.size() <= size)
    {
        const uint32_t code = bits.Bits(0, escape_bits);
        if (code != escape)
        {
            // A byte whose top bits aren't the escape code.
            out.push_back(uint8_t(bits.Bits(code, 8 - escape_bits)));
            continue;
        }

        const uint32_t length = bits.Gamma(max_gamma);
        if (length >= 2)
        {
            // A copy of 3 or more bytes, or the end.
            const uint32_t high = bits.Gamma(max_gamma);
            if (high == end_code)
            {
                return bits.Ok();
            }

            const uint32_t offset_high = bits.Bits(high - 1, offset_bits) & 0xFF;
            const uint32_t offset_low = bits.Bits(0, 8);
            if (!copy((offset_high << 8) + 256 - offset_low, (length & 0xFF) + 1))
            {
                return false;
            }
        }
        else if (!bits.Bit())
        {
            // A copy of 2 bytes from up to 256 back.
            if (!copy(256 - bits.Bits(0, 8), 2))
            {
                return false;
            }
        }
        else if (!bits.Bit())
        {
            // A byte whose top bits are the escape code, and a new escape code.
            const uint32_t next = bits.Bits(0, escape_bits);
            out.push_back(uint8_t(bits.Bits(escape, 8 - escape_bits)));
            escape = next;
        }
        else
        {
            // A run of one byte.
            uint32_t run = bits.Gamma(max_gamma);
            uint32_t run_high = 0;
            if (run >= short_run)
            {
                run = bits.Bits(run, 9 - max_gamma);
                run_high = bits.Gamma(max_gamma) - 1;
            }
            const uint32_t rank = bits.Gamma(max_gamma);
            if (rank < 32 && rank > rank_count)
            {
                return false;
            }

            const uint8_t value = rank < 32 ? ranks[rank - 1] : uint8_t(bits.Bits(rank, 3));
            const uint32_t count = (run & 0xFF) + 1 + (run_high << 8);
            for (uint32_t i = 0; i < count && out.size() <= size; i++)
            {
                out.push_back(value);
            }
        }
    }
    return false;
}

} // namespace agbsf2::krawall
