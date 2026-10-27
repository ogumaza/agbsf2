// SPDX-License-Identifier: MIT

// The compressed data of the music files in games whose modified Krawall loads each module into RAM before it plays
// it. docs/krawall.md describes the format.

#pragma once

#include <cstdint>
#include <vector>

#include "rom.h"

namespace agbsf2::krawall
{

// Unpacks the data at `at`: a 3-byte size, then the compressed stream. Returns false if the stream is broken, runs
// past the ROM's end or makes more bytes than the size gives.
bool Unpack(const Rom& rom, uint32_t at, std::vector<uint8_t>& out);

} // namespace agbsf2::krawall
