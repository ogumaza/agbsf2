// SPDX-License-Identifier: MIT

#include "test_util.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/read_error.h"
#include "seqlib/sf2/soundfont.h"

#include "files.h"

namespace agbsf2::test
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

bool FullBendRange(const Sf2Records& sf)
{
    const size_t records = sf.Count("inst", 22);
    if (records < 2)
    {
        return false;
    }

    // The last instrument record only ends the list.
    for (size_t i = 0; i + 1 < records; i++)
    {
        const uint16_t bag = Le16(sf.Record("inst", 22, i) + 20);
        if (bag >= Le16(sf.Record("inst", 22, i + 1) + 20) || sf.ZoneGens(bag).count(seqlib::sf2::kSampleId))
        {
            return false;
        }

        const uint16_t first = Le16(sf.Record("ibag", 4, bag) + 2);
        const uint16_t end = Le16(sf.Record("ibag", 4, bag + 1) + 2);
        for (const seqlib::sf2::Modulator& wanted : seqlib::sf2::kNoVelocityFilterFullBend)
        {
            bool found = false;
            for (uint16_t m = first; m < end; m++)
            {
                const uint8_t* mod = sf.Record("imod", 10, m);
                found = found || (Le16(mod) == wanted.source && Le16(mod + 2) == wanted.dest &&
                                  int16_t(Le16(mod + 4)) == wanted.amount && Le16(mod + 6) == wanted.amount_source &&
                                  Le16(mod + 8) == wanted.transform);
            }
            if (!found)
            {
                return false;
            }
        }
    }

    return true;
}

} // namespace agbsf2::test
