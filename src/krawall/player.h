// SPDX-License-Identifier: MIT

// Krawall's model of the player, from ntdlib.

#pragma once

#include "ntdlib/krawall/player.h"

#include "krawall/driver.h"
#include "rom.h"

namespace agbsf2::krawall
{

using ntdlib::krawall::AmigaPeriod;
using ntdlib::krawall::Channel;
using ntdlib::krawall::kAmigaClock;
using ntdlib::krawall::kMixChannels;
using ntdlib::krawall::kModifiedMixChannels;
using ntdlib::krawall::kOldMixChannels;
using ntdlib::krawall::kPlayerChannels;
using ntdlib::krawall::LinearPeriod;
using ntdlib::krawall::MixChannel;
using ntdlib::krawall::Player;

} // namespace agbsf2::krawall
