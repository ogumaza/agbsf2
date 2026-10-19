// SPDX-License-Identifier: MIT

// Reading a game's Thumb code for detection: matching instruction patterns, following calls and literals, and running
// small routines that work out a table's address.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "rom.h"

namespace agbsf2
{

// A Thumb instruction to look for: its bits under `mask` have to equal `value`.
struct ThumbPattern
{
    uint16_t value;
    uint16_t mask;
};

// Parses a pattern written as Thumb halfwords in hex, separated by spaces, such as "2E53 DD00 4Dxx". Each x is a digit
// that can have any value.
std::vector<ThumbPattern> ParseThumbPattern(std::string_view text);

// Returns the address of every halfword-aligned match of `pattern` in the ROM.
std::vector<uint32_t> FindThumb(const Rom& rom, const std::vector<ThumbPattern>& pattern);

// Returns the address of the first match of the pattern that `text` gives in ParseThumbPattern()'s form, or 0 if the
// ROM has none.
uint32_t FindFirstThumb(const Rom& rom, std::string_view text);

// Returns the word that the Thumb instruction `ldr rX, [pc, #imm]` at `at` loads.
uint32_t ThumbLiteral(const Rom& rom, uint32_t at);

// Returns the target of the Thumb bl at `at`, or 0 if there's no bl there.
uint32_t BlTarget(const Rom& rom, uint32_t at);

// Returns the address of every Thumb bl in the ROM that calls `target`.
std::vector<uint32_t> FindCalls(const Rom& rom, uint32_t target);

// Runs the Thumb routine at `address` with r0-r3 set to `args` until it returns, and returns r0. The routine may do
// arithmetic, read the ROM, use the stack and call other such routines. Returns nothing if it does anything else, such
// as reading RAM or I/O, switching to ARM code or making a BIOS call, or if it runs for more than `max_steps`
// instructions.
std::optional<uint32_t> RunThumb(const Rom& rom, uint32_t address, const std::array<uint32_t, 4>& args,
                                 int max_steps = 256);

} // namespace agbsf2
