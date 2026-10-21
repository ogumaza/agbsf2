// SPDX-License-Identifier: MIT

// The warnings that the drivers' models and conversions collect.

#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace agbsf2
{

// Adds `message` to `warnings`, unless the list holds it already. A problem that comes back in every frame or note then
// gives one warning.
inline void WarnOnce(std::vector<std::string>& warnings, const std::string& message)
{
    if (std::find(warnings.begin(), warnings.end(), message) == warnings.end())
    {
        warnings.push_back(message);
    }
}

} // namespace agbsf2
