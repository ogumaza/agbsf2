// SPDX-License-Identifier: MIT

// Reading a game's ARM code for detection: matching instruction patterns and reading the words that ldr instructions
// load.

#pragma once

#include <cstdint>
#include <span>

#include "rom.h"

namespace agbsf2
{

// An ARM instruction to look for: its bits under `mask` have to equal `value`.
struct ArmPattern
{
    uint32_t value;
    uint32_t mask = 0xFFFFFFFF;
};

// Returns true if the words from `at` on match the instructions of `pattern`, one after another.
bool MatchesArm(const Rom& rom, uint32_t at, std::span<const ArmPattern> pattern);

// Returns the address of the first match of `pattern` from `from` up to `to`, in steps of a word, or 0 if there's none.
uint32_t FindArm(const Rom& rom, uint32_t from, uint32_t to, std::span<const ArmPattern> pattern);

// Returns the word that the ARM instruction `ldr rX, [pc, #imm12]` at `at` loads.
uint32_t ArmLiteral(const Rom& rom, uint32_t at);

} // namespace agbsf2
