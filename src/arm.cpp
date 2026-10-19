// SPDX-License-Identifier: MIT

#include "arm.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace agbsf2
{

bool MatchesArm(const Rom& rom, uint32_t at, std::span<const ArmPattern> pattern)
{
    if (!rom.Contains(at, 4 * uint32_t(pattern.size())))
    {
        return false;
    }

    for (size_t i = 0; i < pattern.size(); i++)
    {
        if ((rom.U32(at + 4 * uint32_t(i)) & pattern[i].mask) != pattern[i].value)
        {
            return false;
        }
    }

    return true;
}

uint32_t FindArm(const Rom& rom, uint32_t from, uint32_t to, std::span<const ArmPattern> pattern)
{
    for (uint32_t at = from; at + 4 * uint32_t(pattern.size()) <= to; at += 4)
    {
        if (MatchesArm(rom, at, pattern))
        {
            return at;
        }
    }

    return 0;
}

uint32_t ArmLiteral(const Rom& rom, uint32_t at)
{
    const uint32_t insn = rom.U32(at);
    const uint32_t offset = insn & 0xFFF;
    return rom.U32((insn & 0x00800000) ? at + 8 + offset : at + 8 - offset);
}

} // namespace agbsf2
