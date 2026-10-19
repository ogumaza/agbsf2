// SPDX-License-Identifier: MIT

// Ubisoft Milan's sequence format: the commands of a sequence, and a listing of a piece's sequences.

#pragma once

#include <string>

#include "rom.h"
#include "ubimilan/driver.h"

namespace agbsf2::ubimilan
{

// The status byte that ends a sequence. No countdown follows it.
constexpr int kEndCommand = 0xFC;

// Returns the bytes of arguments after the status byte `status`: 2 for a note-off's or a note-on's key and velocity (8c
// and 9c), 1 for a program change's program (Cc), and 0 for the command that does nothing (Ec) and for the end. Returns
// -1 for any other status. The game resets on those.
int ArgumentBytes(int status);

// Writes a text listing of each sequence of a song. Returns false and sets `error` if it can't.
bool DumpSong(const Rom& rom, const DriverInfo& info, int song, const std::string& path, std::string& error);

} // namespace agbsf2::ubimilan
