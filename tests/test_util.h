// SPDX-License-Identifier: MIT

// Checks and helpers for the unit tests, which build small synthetic cartridges in memory, so no game ROM is needed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/testing/check.h"
#include "xsflib/bytes.h"

#include "gba.h"
#include "music.h"
#include "rom.h"

namespace agbsf2::test
{

// Output folder for this test run.
inline std::filesystem::path g_temp;

// Returns the path of `name` in this run's temp folder.
std::filesystem::path TempPath(const std::string& name);

// Returns the default settings for a conversion that writes its files to this run's temp folder. The files' names
// start with `base_name`.
ConvertSettings TempSettings(const std::string& base_name);

// Opens `name` in this run's temp folder for writing, in binary mode, or returns null if it can't be opened.
std::FILE* OpenTempFile(const std::string& name);

// Returns a file's bytes, or none if it can't be read.
std::vector<uint8_t> ReadAll(const std::string& path);

// Returns the number at `p`, in little-endian byte order (xsflib's bytes.h).
inline uint16_t Le16(const uint8_t* p)
{
    return xsflib::LoadLe16(p);
}

inline uint32_t Le32(const uint8_t* p)
{
    return xsflib::LoadLe32(p);
}

// A MIDI file's header and its tracks' events, and a SoundFont's chunks, as seqlib's readers give them.
using MidiEvents = seqlib::midi::File;
using Sf2Records = seqlib::sf2::Records;

// Returns the value of the last write to `address` in `writes`, or -1 if there was none.
long long LastWrite(const std::vector<RegisterWrite>& writes, uint32_t address);

// Reads a MIDI file with seqlib's reader. A file that seqlib can't read gives no tracks, after a message that says why.
MidiEvents ReadMidi(const std::string& path);

// Returns true if the first track has a loopStart marker and, at the marker's tick and after it, a tempo change to the
// tempo in effect there.
bool RepeatsTempoAtLoopStart(const MidiEvents& midi);

// Reads a SoundFont's chunks with seqlib's reader. A file that seqlib can't read gives no chunks, after a message that
// says why.
Sf2Records ReadSf2(const std::string& path);

// Returns true if the SoundFont has an instrument and every instrument starts with a global zone that holds the
// modulators of seqlib's kNoVelocityFilterFullBend. The pitch wheel then covers the whole range that RPN 0 sets.
bool FullBendRange(const Sf2Records& sf);

// A cartridge image that a test builds in memory. Its first byte is at kRomBase, where the GBA maps the ROM. Each
// driver's tests build their cartridges on it.
class CartImage
{
public:
    // Starts an image of `size` bytes, all 0.
    explicit CartImage(size_t size);

    // Put8(), Put16() and Put32() write `v` at address `a`, in little-endian byte order.
    void Put8(uint32_t a, uint8_t v);
    void Put16(uint32_t a, uint16_t v);
    void Put32(uint32_t a, uint32_t v);

    // Writes `bytes` from address `a` on.
    void PutBytes(uint32_t a, const std::vector<uint8_t>& bytes);

    // Writes `halfwords`, such as Thumb code, from address `a` on.
    void PutHalfwords(uint32_t a, const std::vector<uint16_t>& halfwords);

    // Writes the Thumb bl at address `a` that calls `target`.
    void PutBl(uint32_t a, uint32_t target);

    // Grows or shrinks the image to `size` bytes. New bytes are 0.
    void Resize(size_t size);

    // Get8() and Get16() return the byte or the halfword at address `a`.
    uint8_t Get8(uint32_t a) const;
    uint16_t Get16(uint32_t a) const;

    // Returns the image as a ROM.
    Rom ToRom() const;

private:
    std::vector<uint8_t> bytes_;
};

} // namespace agbsf2::test

// Each driver's tests, in konami_test.cpp, rare_test.cpp, quintet_test.cpp, mp2k_test.cpp, rd2_test.cpp,
// brownie_test.cpp, krawall_test.cpp and ubimilan_test.cpp.
namespace agbsf2::konami
{

void RunTests();

} // namespace agbsf2::konami

namespace agbsf2::rare
{

void RunTests();

} // namespace agbsf2::rare

namespace agbsf2::quintet
{

void RunTests();

} // namespace agbsf2::quintet

namespace agbsf2::mp2k
{

void RunTests();

} // namespace agbsf2::mp2k

namespace agbsf2::rd2
{

void RunTests();

} // namespace agbsf2::rd2

namespace agbsf2::brownie
{

void RunTests();

} // namespace agbsf2::brownie

namespace agbsf2::krawall
{

void RunTests();

} // namespace agbsf2::krawall

namespace agbsf2::ubimilan
{

void RunTests();

} // namespace agbsf2::ubimilan
