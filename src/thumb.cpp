// SPDX-License-Identifier: MIT

#include "thumb.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "xsflib/bytes.h"

namespace agbsf2
{
namespace
{

// The return address the routine gets in lr. It isn't in any memory, so reaching it means the routine has returned.
constexpr uint32_t kReturnAddress = 0xFFFFFFF0;

// The stack: a block of IWRAM below the address sp starts at, the only RAM the routine may use.
constexpr uint32_t kStackTop = 0x03007F00;
constexpr uint32_t kStackSize = 0x200;

// Returns `value`'s lowest `bits` bits, sign-extended.
int32_t SignExtend(uint32_t value, int bits)
{
    const uint32_t sign = 1u << (bits - 1);
    return int32_t((value & ((sign << 1) - 1)) ^ sign) - int32_t(sign);
}

// A Thumb CPU that runs a routine as RunThumb() describes, with the ROM and a small stack for its memory.
class Interpreter
{
public:
    Interpreter(const Rom& rom, const std::array<uint32_t, 4>& args) : rom_(rom)
    {
        for (size_t i = 0; i < args.size(); i++)
        {
            r_[i] = args[i];
        }

        r_[13] = kStackTop;
        r_[14] = kReturnAddress | 1;
    }

    // Runs the routine at `address`. Returns r0 once it returns, or nothing.
    std::optional<uint32_t> Run(uint32_t address, int max_steps)
    {
        pc_ = address & ~1u;
        for (int step = 0; step < max_steps; step++)
        {
            if (!Step())
            {
                return std::nullopt;
            }
            if (returned_)
            {
                return r_[0];
            }
        }

        return std::nullopt;
    }

private:
    // Runs one instruction. Returns false if it's one the interpreter doesn't run.
    bool Step()
    {
        if (!rom_.Contains(pc_, 2))
        {
            return false;
        }

        const uint16_t op = rom_.U16(pc_);
        next_ = pc_ + 2;
        bool ok = false;
        if ((op & 0xF800) == 0x1800)
        {
            ok = AddSubtract(op);
        }
        else if ((op & 0xE000) == 0x0000)
        {
            ok = ShiftImmediate(op);
        }
        else if ((op & 0xE000) == 0x2000)
        {
            ok = Immediate(op);
        }
        else if ((op & 0xFC00) == 0x4000)
        {
            ok = Alu(op);
        }
        else if ((op & 0xFC00) == 0x4400)
        {
            ok = HighRegister(op);
        }
        else if ((op & 0xF000) < 0xD000)
        {
            ok = LoadStore(op);
        }
        else
        {
            ok = Branch(op);
        }

        pc_ = next_;

        return ok;
    }

    // Runs a format 2 instruction, which adds or subtracts a register or a 3-bit immediate.
    bool AddSubtract(uint16_t op)
    {
        const uint32_t operand = op & 0x0400 ? (op >> 6) & 7 : r_[(op >> 6) & 7];
        const uint32_t rs = r_[(op >> 3) & 7];
        r_[op & 7] = op & 0x0200 ? Subtract(rs, operand) : Add(rs, operand, 0);

        return true;
    }

    // Runs a format 1 instruction, a shift by an immediate. An lsr or asr by 0 shifts by 32.
    bool ShiftImmediate(uint16_t op)
    {
        const int kind = (op >> 11) & 3;
        const uint32_t amount = (op >> 6) & 31;
        r_[op & 7] = Shift(kind, r_[(op >> 3) & 7], amount == 0 && kind != 0 ? 32 : amount);

        return true;
    }

    // Runs a format 3 instruction: mov, cmp, add or sub with an 8-bit immediate.
    bool Immediate(uint16_t op)
    {
        uint32_t& rd = r_[(op >> 8) & 7];
        const uint32_t value = op & 0xFF;
        switch ((op >> 11) & 3)
        {
        case 0:
            rd = value;
            SetNz(rd);
            break;

        case 1:
            Subtract(rd, value);
            break;

        case 2:
            rd = Add(rd, value, 0);
            break;

        default:
            rd = Subtract(rd, value);
            break;
        }

        return true;
    }

    // Runs a format 4 instruction, one of the ALU operations on low registers.
    bool Alu(uint16_t op)
    {
        uint32_t& rd = r_[op & 7];
        const uint32_t rs = r_[(op >> 3) & 7];
        switch ((op >> 6) & 15)
        {
        case 0x0:
            rd &= rs;
            SetNz(rd);
            break;

        case 0x1:
            rd ^= rs;
            SetNz(rd);
            break;

        case 0x2:
            rd = Shift(0, rd, rs & 0xFF);
            break;

        case 0x3:
            rd = Shift(1, rd, rs & 0xFF);
            break;

        case 0x4:
            rd = Shift(2, rd, rs & 0xFF);
            break;

        case 0x5:
            rd = Add(rd, rs, c_ ? 1 : 0);
            break;

        case 0x6:
            rd = Add(rd, ~rs, c_ ? 1 : 0);
            break;

        case 0x7:
            rd = Shift(3, rd, rs & 0xFF);
            break;

        case 0x8:
            SetNz(rd & rs);
            break;

        case 0x9:
            rd = Subtract(0, rs);
            break;

        case 0xA:
            Subtract(rd, rs);
            break;

        case 0xB:
            Add(rd, rs, 0);
            break;

        case 0xC:
            rd |= rs;
            SetNz(rd);
            break;

        case 0xD:
            rd *= rs;
            SetNz(rd);
            break;

        case 0xE:
            rd &= ~rs;
            SetNz(rd);
            break;

        default:
            rd = ~rs;
            SetNz(rd);
            break;
        }

        return true;
    }

    // Runs a format 5 instruction: add, cmp or mov with high registers, or bx.
    bool HighRegister(uint16_t op)
    {
        const int d = (op & 7) | (op & 0x80 ? 8 : 0);
        const int s = ((op >> 3) & 7) | (op & 0x40 ? 8 : 0);
        const uint32_t value = Register(s);
        switch ((op >> 8) & 3)
        {
        case 0:
            return SetRegister(d, Register(d) + value);

        case 1:
            Subtract(Register(d), value);
            return true;

        case 2:
            return SetRegister(d, value);

        default:
            return Jump(value, true);
        }
    }

    // Runs a format 6 to 15 instruction: loads, stores, address arithmetic and the stack.
    bool LoadStore(uint16_t op)
    {
        const int rd = op & 7;
        const int rb = (op >> 3) & 7;
        const uint32_t offset5 = (op >> 6) & 31;
        if ((op & 0xF800) == 0x4800)
        {
            return Read(((pc_ + 4) & ~3u) + (op & 0xFF) * 4, 4, false, r_[(op >> 8) & 7]);
        }
        if ((op & 0xF200) == 0x5000)
        {
            // Format 7: str, strb, ldr and ldrb with a register offset.
            const uint32_t address = r_[rb] + r_[(op >> 6) & 7];
            const int size = op & 0x0400 ? 1 : 4;
            return op & 0x0800 ? Read(address, size, false, r_[rd]) : Write(address, size, r_[rd]);
        }
        if ((op & 0xF200) == 0x5200)
        {
            // Format 8: strh, ldsb, ldrh and ldsh with a register offset.
            const uint32_t address = r_[rb] + r_[(op >> 6) & 7];
            switch ((op >> 10) & 3)
            {
            case 0:
                return Write(address, 2, r_[rd]);
            case 1:
                return Read(address, 1, true, r_[rd]);
            case 2:
                return Read(address, 2, false, r_[rd]);
            default:
                return Read(address, 2, true, r_[rd]);
            }
        }
        if ((op & 0xE000) == 0x6000)
        {
            // Format 9: str, ldr, strb and ldrb with an immediate offset.
            const int size = op & 0x1000 ? 1 : 4;
            const uint32_t address = r_[rb] + offset5 * uint32_t(size);
            return op & 0x0800 ? Read(address, size, false, r_[rd]) : Write(address, size, r_[rd]);
        }
        if ((op & 0xF000) == 0x8000)
        {
            const uint32_t address = r_[rb] + offset5 * 2;
            return op & 0x0800 ? Read(address, 2, false, r_[rd]) : Write(address, 2, r_[rd]);
        }
        if ((op & 0xF000) == 0x9000)
        {
            const uint32_t address = r_[13] + (op & 0xFF) * 4;
            uint32_t& reg = r_[(op >> 8) & 7];
            return op & 0x0800 ? Read(address, 4, false, reg) : Write(address, 4, reg);
        }
        if ((op & 0xF000) == 0xA000)
        {
            r_[(op >> 8) & 7] = (op & 0x0800 ? r_[13] : (pc_ + 4) & ~3u) + (op & 0xFF) * 4;
            return true;
        }
        if ((op & 0xFF00) == 0xB000)
        {
            const uint32_t amount = (op & 0x7F) * 4;
            r_[13] = op & 0x80 ? r_[13] - amount : r_[13] + amount;
            return true;
        }
        if ((op & 0xF600) == 0xB400)
        {
            return op & 0x0800 ? Pop(op) : Push(op);
        }
        if ((op & 0xF000) == 0xC000)
        {
            return Multiple(op);
        }

        return false;
    }

    // Runs a format 16 to 19 instruction: branches and bl. A BIOS call or an undefined instruction stops the routine.
    bool Branch(uint16_t op)
    {
        if ((op & 0xF000) == 0xD000)
        {
            const int condition = (op >> 8) & 15;
            if (condition >= 14)
            {
                return false;
            }
            if (Passes(condition))
            {
                next_ = pc_ + 4 + uint32_t(SignExtend(op, 8) * 2);
            }
            return true;
        }
        if ((op & 0xF800) == 0xE000)
        {
            next_ = pc_ + 4 + uint32_t(SignExtend(op, 11) * 2);
            return true;
        }
        if ((op & 0xF800) == 0xF000)
        {
            r_[14] = pc_ + 4 + uint32_t(SignExtend(op, 11) << 12);
            return true;
        }
        if ((op & 0xF800) == 0xF800)
        {
            const uint32_t target = r_[14] + (op & 0x7FF) * 2;
            r_[14] = (pc_ + 2) | 1;
            next_ = target;
            return true;
        }

        return false;
    }

    bool Push(uint16_t op)
    {
        uint32_t address = r_[13] - 4 * Count(op);
        r_[13] = address;
        for (int i = 0; i < 9; i++)
        {
            if (op & (1 << i))
            {
                if (!Write(address, 4, r_[i < 8 ? i : 14]))
                {
                    return false;
                }
                address += 4;
            }
        }

        return true;
    }

    bool Pop(uint16_t op)
    {
        uint32_t address = r_[13];
        uint32_t pc = 0;
        for (int i = 0; i < 9; i++)
        {
            if (op & (1 << i))
            {
                if (!Read(address, 4, false, i < 8 ? r_[i] : pc))
                {
                    return false;
                }
                address += 4;
            }
        }

        r_[13] = address;

        // Popping the pc stays in Thumb state on the GBA's ARMv4T, whatever its bit 0.
        return op & 0x100 ? Jump(pc | 1, false) : true;
    }

    // Runs a format 15 instruction, stmia or ldmia.
    bool Multiple(uint16_t op)
    {
        const int rb = (op >> 8) & 7;
        uint32_t address = r_[rb];
        for (int i = 0; i < 8; i++)
        {
            if (op & (1 << i))
            {
                const bool ok = op & 0x0800 ? Read(address, 4, false, r_[i]) : Write(address, 4, r_[i]);
                if (!ok)
                {
                    return false;
                }
                address += 4;
            }
        }

        if (!(op & 0x0800) || !(op & (1 << rb)))
        {
            r_[rb] = address;
        }

        return true;
    }

    // Returns the number of registers a push or pop moves.
    static uint32_t Count(uint16_t op)
    {
        uint32_t n = 0;
        for (int i = 0; i < 9; i++)
        {
            n += (op >> i) & 1;
        }

        return n;
    }

    // Returns register `n`, which for the pc is the instruction's address + 4.
    uint32_t Register(int n) const
    {
        return n == 15 ? pc_ + 4 : r_[size_t(n)];
    }

    // Sets register `n`. Setting the pc branches.
    bool SetRegister(int n, uint32_t value)
    {
        if (n == 15)
        {
            return Jump(value | 1, false);
        }

        r_[size_t(n)] = value;

        return true;
    }

    // Branches to `target`. With `exchange` (bx), bit 0 clear would switch to ARM code, which the interpreter doesn't
    // run.
    bool Jump(uint32_t target, bool exchange)
    {
        if ((target & ~1u) == kReturnAddress)
        {
            returned_ = true;
            return true;
        }
        if (exchange && !(target & 1))
        {
            return false;
        }

        next_ = target & ~1u;

        return true;
    }

    // Returns true if condition code `condition` (0-13) passes.
    bool Passes(int condition) const
    {
        switch (condition)
        {
        case 0:
            return z_;
        case 1:
            return !z_;
        case 2:
            return c_;
        case 3:
            return !c_;
        case 4:
            return n_;
        case 5:
            return !n_;
        case 6:
            return v_;
        case 7:
            return !v_;
        case 8:
            return c_ && !z_;
        case 9:
            return !c_ || z_;
        case 10:
            return n_ == v_;
        case 11:
            return n_ != v_;
        case 12:
            return !z_ && n_ == v_;
        default:
            return z_ || n_ != v_;
        }
    }

    // Reads `size` bytes at `address` from the ROM or the stack, sign-extending them with `sign`.
    bool Read(uint32_t address, int size, bool sign, uint32_t& value) const
    {
        if (address % uint32_t(size) != 0)
        {
            return false;
        }

        uint32_t raw = 0;
        if (rom_.Contains(address, uint32_t(size)))
        {
            raw = size == 1 ? rom_.U8(address) : size == 2 ? rom_.U16(address) : rom_.U32(address);
        }
        else if (OnStack(address, size))
        {
            for (int i = 0; i < size; i++)
            {
                raw |= uint32_t(stack_[address - (kStackTop - kStackSize) + uint32_t(i)]) << (8 * i);
            }
        }
        else
        {
            return false;
        }

        value = sign ? uint32_t(SignExtend(raw, 8 * size)) : raw;

        return true;
    }

    // Writes `size` bytes of `value` at `address`, which has to be on the stack.
    bool Write(uint32_t address, int size, uint32_t value)
    {
        if (address % uint32_t(size) != 0 || !OnStack(address, size))
        {
            return false;
        }

        for (int i = 0; i < size; i++)
        {
            stack_[address - (kStackTop - kStackSize) + uint32_t(i)] = uint8_t(value >> (8 * i));
        }

        return true;
    }

    static bool OnStack(uint32_t address, int size)
    {
        return address >= kStackTop - kStackSize && address <= kStackTop - uint32_t(size);
    }

    // Returns a + b + carry, setting the flags.
    uint32_t Add(uint32_t a, uint32_t b, uint32_t carry)
    {
        const uint64_t sum = uint64_t(a) + b + carry;
        const uint32_t result = uint32_t(sum);
        c_ = (sum >> 32) != 0;
        v_ = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
        SetNz(result);

        return result;
    }

    // Returns a - b, setting the flags: the carry is set when there's no borrow.
    uint32_t Subtract(uint32_t a, uint32_t b)
    {
        return Add(a, ~b, 1);
    }

    // Returns `value` shifted by `amount` with lsl (0), lsr (1), asr (2) or ror (3), setting the flags. An amount of 0
    // leaves the carry alone.
    uint32_t Shift(int kind, uint32_t value, uint32_t amount)
    {
        uint32_t result = value;
        if (amount != 0)
        {
            switch (kind)
            {
            case 0:
                c_ = amount <= 32 && ((value >> (32 - amount)) & 1);
                result = amount < 32 ? value << amount : 0;
                break;

            case 1:
                c_ = amount <= 32 && ((value >> (amount - 1)) & 1);
                result = amount < 32 ? value >> amount : 0;
                break;

            case 2:
                c_ = amount < 32 ? ((value >> (amount - 1)) & 1) != 0 : (value >> 31) != 0;
                result = amount < 32 ? uint32_t(int32_t(value) >> amount) : (value >> 31 ? 0xFFFFFFFF : 0);
                break;

            default:
                {
                    const uint32_t r = amount & 31;
                    result = r ? (value >> r) | (value << (32 - r)) : value;
                    c_ = (result >> 31) != 0;
                    break;
                }
            }
        }

        SetNz(result);

        return result;
    }

    void SetNz(uint32_t value)
    {
        n_ = (value >> 31) != 0;
        z_ = value == 0;
    }

    const Rom& rom_;
    std::array<uint32_t, 16> r_ = {};
    uint32_t pc_ = 0;
    uint32_t next_ = 0;
    bool n_ = false, z_ = false, c_ = false, v_ = false;
    bool returned_ = false;
    std::array<uint8_t, kStackSize> stack_ = {};
};

// Returns the value of hex digit `c`, or -1 if it isn't one.
int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }

    return -1;
}

} // namespace

std::vector<ThumbPattern> ParseThumbPattern(std::string_view text)
{
    std::vector<ThumbPattern> pattern;
    ThumbPattern p = {0, 0};
    int digits = 0;
    for (char c : text)
    {
        if (c == ' ')
        {
            continue;
        }

        const int d = HexDigit(c);
        p.value = uint16_t(p.value << 4 | (d < 0 ? 0 : d));
        p.mask = uint16_t(p.mask << 4 | (d < 0 ? 0 : 0xF));
        if (++digits == 4)
        {
            pattern.push_back(p);
            p = {0, 0};
            digits = 0;
        }
    }

    return pattern;
}

std::vector<uint32_t> FindThumb(const Rom& rom, const std::vector<ThumbPattern>& pattern)
{
    std::vector<uint32_t> hits;
    const size_t n = pattern.size();
    if (n == 0 || rom.Size() < 2 * n)
    {
        return hits;
    }

    const uint8_t* data = rom.Ptr(kRomBase);
    for (size_t i = 0; i + 2 * n <= rom.Size(); i += 2)
    {
        size_t k = 0;
        while (k < n && (xsflib::LoadLe16(data + i + 2 * k) & pattern[k].mask) == pattern[k].value)
        {
            k++;
        }
        if (k == n)
        {
            hits.push_back(kRomBase + uint32_t(i));
        }
    }

    return hits;
}

uint32_t FindFirstThumb(const Rom& rom, std::string_view text)
{
    const std::vector<uint32_t> hits = FindThumb(rom, ParseThumbPattern(text));
    return hits.empty() ? 0 : hits[0];
}

uint32_t ThumbLiteral(const Rom& rom, uint32_t at)
{
    return rom.U32(((at + 4) & ~3u) + (rom.U16(at) & 0xFFu) * 4);
}

uint32_t BlTarget(const Rom& rom, uint32_t at)
{
    const uint16_t high = rom.U16(at);
    const uint16_t low = rom.U16(at + 2);
    if ((high & 0xF800) != 0xF000 || (low & 0xF800) != 0xF800)
    {
        return 0;
    }

    return at + 4 + uint32_t(SignExtend(high, 11) << 12) + (low & 0x7FFu) * 2;
}

std::vector<uint32_t> FindCalls(const Rom& rom, uint32_t target)
{
    std::vector<uint32_t> calls;
    for (uint32_t at = kRomBase; at + 4 <= rom.End(); at += 2)
    {
        if (BlTarget(rom, at) == target)
        {
            calls.push_back(at);
        }
    }

    return calls;
}

std::optional<uint32_t> RunThumb(const Rom& rom, uint32_t address, const std::array<uint32_t, 4>& args, int max_steps)
{
    Interpreter interpreter(rom, args);
    return interpreter.Run(address, max_steps);
}

} // namespace agbsf2
