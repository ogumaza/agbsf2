// SPDX-License-Identifier: MIT

// File access with UTF-8 paths, including on Windows. xsflib's files.h does the work.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "xsflib/files.h"

namespace agbsf2
{

using xsflib::PathFromUtf8;
using xsflib::Utf8;

// Makes a single filename component valid on Windows as well as macOS and Linux. Keeps UTF-8 characters intact.
// agbsf2's rules strip every trailing dot and space, put '_' before a device name and give "output" for an empty name.
inline std::string SafeFileName(std::string name)
{
    static const seqlib::FileNameRules kRules = {seqlib::TrailingRule::kRemoveAll, seqlib::DeviceMark::kBeforeName,
                                                 false, "output"};

    return seqlib::SafeFileName(std::move(name), kRules);
}

// Reads the whole file at `path` into `out`. Returns false if it can't be opened.
inline bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    return xsflib::ReadFile(PathFromUtf8(path), out);
}

// Writes `data` to the file at `path`, replacing it. Returns false and sets `error` on failure.
inline bool WriteFile(const std::string& path, const std::vector<uint8_t>& data, std::string& error)
{
    if (std::optional<std::string> message = xsflib::WriteFile(PathFromUtf8(path), data))
    {
        error = std::move(*message);
        return false;
    }

    return true;
}

// Writes `text` to the file at `path`, replacing it. The text goes out byte for byte. Its lines then end in LF on every
// platform. Returns false and sets `error` on failure.
inline bool WriteTextFile(const std::string& path, const std::string& text, std::string& error)
{
    return WriteFile(path, std::vector<uint8_t>(text.begin(), text.end()), error);
}

} // namespace agbsf2
