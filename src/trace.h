// SPDX-License-Identifier: MIT

// A helper for the Trace() functions of the Music classes.

#pragma once

#include <cstdint>
#include <cstdio>
#include <span>

namespace agbsf2
{

// Prints `bytes` in hex, two digits a byte, as the traces of the tools' driver_emu.py scripts do.
inline void PrintHex(std::FILE* out, std::span<const uint8_t> bytes)
{
    for (uint8_t b : bytes)
    {
        std::fprintf(out, "%02x", b);
    }
}

} // namespace agbsf2
