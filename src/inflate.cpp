// SPDX-License-Identifier: Zlib

// Minimal zlib decoder, adapted from puff, Mark Adler's reference inflate in the zlib sources (contrib/puff). This is
// an altered version: the DEFLATE decoder is rewritten as a C++ class, and ZlibDecompress adds the zlib header and
// Adler-32 checks. puff's copyright notice follows.

/*
  Copyright (C) 2002-2013 Mark Adler, all rights reserved
  version 2.3, 21 Jan 2013

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler    madler@alumni.caltech.edu
 */

#include "inflate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agbsf2
{
namespace
{

constexpr int kMaxBits = 15;

// A canonical Huffman code, stored as puff stores it: the number of codes of each length, and the symbols in code
// order.
struct Huffman
{
    uint16_t count[kMaxBits + 1] = {};
    uint16_t symbol[288] = {};
};

// Decodes a DEFLATE stream (RFC 1951) into `out`, one block at a time.
class Inflater
{
public:
    Inflater(const uint8_t* in, size_t size, std::vector<uint8_t>& out) : in_(in), size_(size), out_(out)
    {
    }

    // Decodes blocks up to the last one. Returns false and sets `error` if the stream is corrupt or cut short.
    bool Run(std::string& error)
    {
        int last;
        do
        {
            last = Bits(1);
            const int type = Bits(2);
            bool ok;
            switch (type)
            {
            case 0:
                ok = Stored();
                break;

            case 1:
                ok = Fixed();
                break;

            case 2:
                ok = Dynamic();
                break;

            default:
                error = "invalid DEFLATE block type";
                return false;
            }
            if (!ok || overrun_)
            {
                error = overrun_ ? "truncated DEFLATE stream" : "corrupt DEFLATE stream";
                return false;
            }
        } while (!last);

        return true;
    }

    // Returns the number of bytes of input the stream took up, once Run() has finished.
    size_t BytePosition() const
    {
        return pos_;
    }

private:
    // The codes RFC 1951 fixes for blocks of type 1: 8 or 9 bits a literal, 7 or 8 a length, and 5 a distance.
    struct FixedCodes
    {
        Huffman lencode, distcode;
    };

    // Reads the next `need` bits of input and returns them, the first bit lowest.
    int Bits(int need)
    {
        uint32_t val = bit_buf_;
        while (bit_count_ < need)
        {
            if (pos_ >= size_)
            {
                overrun_ = true;
                return 0;
            }

            val |= uint32_t(in_[pos_++]) << bit_count_;
            bit_count_ += 8;
        }

        bit_buf_ = val >> need;
        bit_count_ -= need;

        return int(val & ((1u << need) - 1));
    }

    // Decodes a stored block: its length, the length inverted, and that many bytes as they are.
    bool Stored()
    {
        bit_buf_ = 0;
        bit_count_ = 0;

        if (pos_ + 4 > size_)
        {
            overrun_ = true;
            return false;
        }

        const unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
        const unsigned nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;

        if (len != (~nlen & 0xFFFFu))
        {
            return false;
        }
        if (pos_ + len > size_)
        {
            overrun_ = true;
            return false;
        }

        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
        pos_ += len;

        return true;
    }

    // Decodes a symbol of code `h`, a bit at a time. Returns -1 for an invalid code or the end of the input.
    int Decode(const Huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; len++)
        {
            code |= Bits(1);
            if (overrun_)
            {
                return -1;
            }

            const int count = h.count[len];
            if (code - count < first)
            {
                return h.symbol[index + (code - first)];
            }

            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }

        return -1;
    }

    // Builds `h` from the code lengths of `n` symbols. Returns <0 for an over-subscribed set, 0 for complete, >0 for
    // incomplete.
    static int Construct(Huffman& h, const uint8_t* length, int n)
    {
        for (auto& c : h.count)
        {
            c = 0;
        }

        for (int s = 0; s < n; s++)
        {
            h.count[length[s]]++;
        }
        if (h.count[0] == n)
        {
            return 0;
        }

        int left = 1;
        for (int len = 1; len <= kMaxBits; len++)
        {
            left <<= 1;
            left -= h.count[len];
            if (left < 0)
            {
                return left;
            }
        }

        uint16_t offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; len++)
        {
            offs[len + 1] = uint16_t(offs[len] + h.count[len]);
        }

        for (int s = 0; s < n; s++)
        {
            if (length[s] != 0)
            {
                h.symbol[offs[length[s]]++] = uint16_t(s);
            }
        }

        return left;
    }

    // Decodes literals and length/distance pairs with the given codes, up to the end of the block.
    bool Codes(const Huffman& lencode, const Huffman& distcode)
    {
        static const uint16_t kLbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t kLext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t kDbase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                            33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t kDext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                          6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        for (;;)
        {
            int symbol = Decode(lencode);
            if (symbol < 0)
            {
                return false;
            }

            if (symbol < 256)
            {
                out_.push_back(uint8_t(symbol));
            }
            else if (symbol == 256)
            {
                return true;
            }
            else
            {
                symbol -= 257;
                if (symbol >= 29)
                {
                    return false;
                }

                const size_t len = kLbase[symbol] + Bits(kLext[symbol]);
                const int dsym = Decode(distcode);
                if (dsym < 0 || dsym >= 30)
                {
                    return false;
                }

                const size_t dist = kDbase[dsym] + Bits(kDext[dsym]);
                if (overrun_ || dist > out_.size())
                {
                    return false;
                }

                const size_t from = out_.size() - dist;
                for (size_t i = 0; i < len; i++)
                {
                    out_.push_back(out_[from + i]);
                }
            }
        }
    }

    // Returns the fixed codes, built from the code lengths that RFC 1951 gives them.
    static FixedCodes BuildFixedCodes()
    {
        FixedCodes codes;
        uint8_t lengths[288];
        int s = 0;
        for (; s < 144; s++)
        {
            lengths[s] = 8;
        }
        for (; s < 256; s++)
        {
            lengths[s] = 9;
        }
        for (; s < 280; s++)
        {
            lengths[s] = 7;
        }
        for (; s < 288; s++)
        {
            lengths[s] = 8;
        }
        Construct(codes.lencode, lengths, 288);

        for (s = 0; s < 30; s++)
        {
            lengths[s] = 5;
        }
        Construct(codes.distcode, lengths, 30);

        return codes;
    }

    // Decodes a block coded with the fixed codes of RFC 1951.
    bool Fixed()
    {
        // Built on first use. The initialisation of a local static is thread-safe.
        static const FixedCodes kFixed = BuildFixedCodes();
        return Codes(kFixed.lencode, kFixed.distcode);
    }

    // Decodes a block that carries its own codes, themselves Huffman coded.
    bool Dynamic()
    {
        static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        uint8_t lengths[320] = {};
        const int nlen = Bits(5) + 257;
        const int ndist = Bits(5) + 1;
        const int ncode = Bits(4) + 4;
        if (overrun_ || nlen > 286 || ndist > 30)
        {
            return false;
        }

        int index = 0;
        for (; index < ncode; index++)
        {
            lengths[kOrder[index]] = uint8_t(Bits(3));
        }
        for (; index < 19; index++)
        {
            lengths[kOrder[index]] = 0;
        }

        Huffman lencode, distcode;
        if (Construct(lencode, lengths, 19) != 0)
        {
            return false;
        }

        index = 0;
        while (index < nlen + ndist)
        {
            int symbol = Decode(lencode);
            if (symbol < 0)
            {
                return false;
            }

            if (symbol < 16)
            {
                lengths[index++] = uint8_t(symbol);
            }
            else
            {
                int len = 0;
                if (symbol == 16)
                {
                    if (index == 0)
                    {
                        return false;
                    }

                    len = lengths[index - 1];
                    symbol = 3 + Bits(2);
                }
                else if (symbol == 17)
                {
                    symbol = 3 + Bits(3);
                }
                else
                {
                    symbol = 11 + Bits(7);
                }
                if (index + symbol > nlen + ndist)
                {
                    return false;
                }

                while (symbol--)
                {
                    lengths[index++] = uint8_t(len);
                }
            }
        }

        if (lengths[256] == 0)
        {
            return false;
        }

        // Build the literal/length code and the distance code. Either one can only be incomplete if it's a single code
        // of one bit.
        int err = Construct(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen != lencode.count[0] + lencode.count[1]))
        {
            return false;
        }

        err = Construct(distcode, lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist != distcode.count[0] + distcode.count[1]))
        {
            return false;
        }

        return Codes(lencode, distcode);
    }

    const uint8_t* in_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    bool overrun_ = false;
    std::vector<uint8_t>& out_;
};

// Returns the zlib checksum of `data`. The sums are reduced every 5552 bytes, the most that can't overflow 32 bits.
uint32_t Adler32(const std::vector<uint8_t>& data)
{
    uint32_t a = 1, b = 0;
    size_t i = 0;
    while (i < data.size())
    {
        const size_t chunk = std::min<size_t>(data.size() - i, 5552);
        for (size_t k = 0; k < chunk; k++)
        {
            a += data[i + k];
            b += a;
        }

        a %= 65521;
        b %= 65521;
        i += chunk;
    }

    return (b << 16) | a;
}

} // namespace

bool ZlibDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error)
{
    out.clear();

    if (size < 6)
    {
        error = "zlib stream too short";
        return false;
    }

    const unsigned cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0)
    {
        error = "not a zlib stream";
        return false;
    }
    if (flg & 0x20)
    {
        error = "zlib preset dictionaries aren't supported";
        return false;
    }

    Inflater inflater(data + 2, size - 2, out);
    if (!inflater.Run(error))
    {
        return false;
    }

    const size_t end = 2 + inflater.BytePosition();
    if (end + 4 > size)
    {
        error = "zlib stream ends before its checksum";
        return false;
    }

    const uint32_t expected =
        (uint32_t(data[end]) << 24) | (uint32_t(data[end + 1]) << 16) | (uint32_t(data[end + 2]) << 8) | data[end + 3];
    if (expected != Adler32(out))
    {
        error = "zlib checksum mismatch";
        return false;
    }

    return true;
}

} // namespace agbsf2
