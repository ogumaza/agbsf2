// SPDX-License-Identifier: MIT

// Minimal zlib/DEFLATE decoder (RFC 1950/1951), used to read GSF files without depending on an external zlib.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agbsf2
{

// Decompresses a zlib stream (2-byte header, DEFLATE data, Adler-32 trailer). Returns false and sets `error` on
// malformed input.
bool ZlibDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);

} // namespace agbsf2
