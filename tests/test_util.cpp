// SPDX-License-Identifier: MIT

#include "test_util.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/read_error.h"

#include "files.h"

namespace supergbamidi::test
{

std::filesystem::path TempPath(const std::string& name)
{
    return g_temp / PathFromUtf8(name);
}

std::FILE* OpenTempFile(const std::string& name)
{
    // MSVC warns that std::fopen is unsafe, so it gets fopen_s, which does the same here.
    const std::string path = TempPath(name).string();
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "wb") != 0)
    {
        return nullptr;
    }
#else
    f = std::fopen(path.c_str(), "wb");
#endif

    return f;
}

std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::vector<uint8_t> data;
    ReadFile(path, data);

    return data;
}

MidiEvents ReadMidi(const std::string& path)
{
    try
    {
        return seqlib::midi::Read(ReadAll(path));
    }
    catch (const seqlib::ReadError& e)
    {
        std::printf("can't read %s: %s\n", path.c_str(), e.what());
        return {};
    }
}

bool RepeatsTempoAtLoopStart(const MidiEvents& midi)
{
    if (midi.tracks.empty())
    {
        return false;
    }

    // Follow the tempo up to the marker, and then look for a tempo change after it at the same tick.
    std::vector<uint8_t> tempo;
    std::optional<uint64_t> marker;
    for (const auto& [tick, bytes] : midi.tracks[0].events)
    {
        if (marker && tick != *marker)
        {
            break;
        }

        const bool is_tempo = bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51;
        if (marker && is_tempo)
        {
            return bytes == tempo;
        }
        if (is_tempo)
        {
            tempo = bytes;
        }
        else if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0x06 &&
                 std::string(bytes.begin() + 3, bytes.end()) == "loopStart")
        {
            marker = tick;
        }
    }

    return false;
}

Sf2Records ReadSf2(const std::string& path)
{
    try
    {
        return seqlib::sf2::Read(ReadAll(path));
    }
    catch (const seqlib::ReadError& e)
    {
        std::printf("can't read %s: %s\n", path.c_str(), e.what());
        return {};
    }
}

} // namespace supergbamidi::test
