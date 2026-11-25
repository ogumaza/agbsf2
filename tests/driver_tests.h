// SPDX-License-Identifier: MIT

// Each driver's tests, in konami_test.cpp, rare_test.cpp, quintet_test.cpp, mp2k_test.cpp, rd2_test.cpp,
// brownie_test.cpp, krawall_test.cpp, ubimilan_test.cpp, logikstate_test.cpp, webfoot_test.cpp,
// frontline_mod_test.cpp and frontline_xm_test.cpp, and what they share (test_main.cpp).

#pragma once

#include <string>
#include <vector>

#include "music.h"

namespace agbsf2
{

// What Music::Trace() wrote for a song, and what it returned.
struct TraceResult
{
    bool traced = false;
    std::string text;
    std::vector<std::string> warnings;
};

// Traces the first `frames` frames of `song` into a temporary file, and returns the file's text with the result.
TraceResult TraceText(const Music& music, int song, long long frames);

} // namespace agbsf2

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

namespace agbsf2::logikstate
{

void RunTests();

} // namespace agbsf2::logikstate

namespace agbsf2::webfoot
{

void RunTests();

} // namespace agbsf2::webfoot

namespace agbsf2::frontline_mod
{

void RunTests();

} // namespace agbsf2::frontline_mod

namespace agbsf2::frontline_xm
{

void RunTests();

} // namespace agbsf2::frontline_xm

namespace agbsf2::engine_software
{

void RunTests();

} // namespace agbsf2::engine_software
