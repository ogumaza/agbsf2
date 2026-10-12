// SPDX-License-Identifier: MIT

// Checks and helpers for the unit tests, which build small synthetic cartridges in memory, so no game ROM is needed.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/testing/check.h"
#include "xsflib/bytes.h"

namespace supergbamidi::test
{

// Output folder for this test run.
inline std::filesystem::path g_temp;

// Returns the path of `name` in this run's temp folder.
std::filesystem::path TempPath(const std::string& name);

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

// Reads a MIDI file with seqlib's reader. A file that seqlib can't read gives no tracks, after a message that says why.
MidiEvents ReadMidi(const std::string& path);

// Returns true if the first track has a loopStart marker and, at the marker's tick and after it, a tempo change to the
// tempo in effect there.
bool RepeatsTempoAtLoopStart(const MidiEvents& midi);

// Reads a SoundFont's chunks with seqlib's reader. A file that seqlib can't read gives no chunks, after a message that
// says why.
Sf2Records ReadSf2(const std::string& path);

} // namespace supergbamidi::test

// Each driver's tests, in konami_test.cpp, rare_test.cpp, quintet_test.cpp, mp2k_test.cpp, rd2_test.cpp,
// brownie_test.cpp, krawall_test.cpp and ubimilan_test.cpp.
namespace supergbamidi::konami
{

void RunTests();

} // namespace supergbamidi::konami

namespace supergbamidi::rare
{

void RunTests();

} // namespace supergbamidi::rare

namespace supergbamidi::quintet
{

void RunTests();

} // namespace supergbamidi::quintet

namespace supergbamidi::mp2k
{

void RunTests();

} // namespace supergbamidi::mp2k

namespace supergbamidi::rd2
{

void RunTests();

} // namespace supergbamidi::rd2

namespace supergbamidi::brownie
{

void RunTests();

} // namespace supergbamidi::brownie

namespace supergbamidi::krawall
{

void RunTests();

} // namespace supergbamidi::krawall

namespace supergbamidi::ubimilan
{

void RunTests();

} // namespace supergbamidi::ubimilan
