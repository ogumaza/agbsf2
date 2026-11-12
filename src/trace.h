// SPDX-License-Identifier: MIT

// Helpers for the Trace() functions of the Music classes.

#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

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

// Prints a frame's line for a record that has changed since the last one printed, as the traces of the module players'
// driver_emu.py scripts do. Before the first line, the record counts as zeros.
inline void PrintChanged(std::FILE* out, long long frame, const char* name, const std::vector<uint8_t>& record,
                         std::vector<uint8_t>& last)
{
    if (last.empty())
    {
        last.assign(record.size(), 0);
    }
    if (record == last)
    {
        return;
    }

    last = record;
    std::fprintf(out, "%lld %s ", frame, name);
    PrintHex(out, record);
    std::fprintf(out, "\n");
}

} // namespace agbsf2
