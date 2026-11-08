// SPDX-License-Identifier: MIT

// Nintendo R&D1's model of a player, from ntdlib.

#pragma once

#include "ntdlib/rd1/sequencer.h"

#include "rd1/driver.h"
#include "rd1/song.h"
#include "rom.h"

namespace agbsf2::rd1
{

using ntdlib::rd1::ChannelRecord;
using ntdlib::rd1::kChannelSize;
using ntdlib::rd1::Sequencer;
using ntdlib::rd1::TrackState;

} // namespace agbsf2::rd1
