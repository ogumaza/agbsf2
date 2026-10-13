// SPDX-License-Identifier: MIT

// GBA cartridge image access.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/text.h"

namespace agbsf2
{

constexpr uint32_t kRomBase = 0x08000000;

// Returns `v` as 0x and 8 capital hexadecimal digits, for messages about addresses and values in the ROM.
inline std::string Hex(uint32_t v)
{
    return xsflib::Hex(v, 8, true);
}

// A cartridge image mapped at 0x08000000. All reads take GBA bus addresses and are bounds-checked: out-of-range reads
// return 0 rather than failing, so callers must validate addresses with Contains() where it matters.
class Rom
{
public:
    // Loads a raw .gba image, or a GSF rip (.gsf / .minigsf / .gsflib, following _lib references). Returns false and
    // sets `error` on failure.
    bool Load(const std::string& path, std::string& error);

    // Uses `image` (mapped at 0x08000000) as the cartridge, e.g. for tests.
    void Assign(std::vector<uint8_t> image)
    {
        data_ = std::move(image);
        from_gsf_ = false;
        library_.clear();
    }

    bool Contains(uint32_t addr, uint32_t len = 1) const
    {
        return addr >= kRomBase && uint64_t(addr) - kRomBase + len <= data_.size();
    }

    uint8_t U8(uint32_t addr) const
    {
        return Contains(addr) ? data_[addr - kRomBase] : 0;
    }

    int8_t S8(uint32_t addr) const
    {
        return int8_t(U8(addr));
    }

    uint16_t U16(uint32_t addr) const
    {
        return uint16_t(U8(addr) | (U8(addr + 1) << 8));
    }

    uint32_t U32(uint32_t addr) const
    {
        return U16(addr) | (uint32_t(U16(addr + 2)) << 16);
    }

    int32_t S32(uint32_t addr) const
    {
        return int32_t(U32(addr));
    }

    // Returns a pointer to the byte at `addr`, or nullptr if the address is outside the image.
    const uint8_t* Ptr(uint32_t addr) const
    {
        return Contains(addr) ? &data_[addr - kRomBase] : nullptr;
    }

    // Returns the address just past the image.
    uint32_t End() const
    {
        return kRomBase + uint32_t(data_.size());
    }

    size_t Size() const
    {
        return data_.size();
    }

    // Returns the game title in the header (0xA0, 12 characters).
    std::string Title() const;

    // Returns the game code in the header (0xAC, 4 characters).
    std::string GameCode() const;

    bool FromGsf() const
    {
        return from_gsf_;
    }

    // Returns the ROM library used by a mini-GSF, or an empty string for other inputs.
    const std::string& GsfLibrary() const
    {
        return library_;
    }

private:
    // Loads a GSF file, its own program and its libraries. `depth` counts the libraries that led to it.
    bool LoadGsf(const std::string& path, int depth, std::string& error);

    // Copies a GSF program section into the image, at the address it gives.
    bool ApplyGsfProgram(const std::vector<uint8_t>& program, std::string& error);

    std::vector<uint8_t> data_;
    bool from_gsf_ = false;
    std::string library_;
};

} // namespace agbsf2
