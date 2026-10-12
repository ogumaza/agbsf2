// SPDX-License-Identifier: MIT

#include "rom.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"

#include "files.h"
#include "inflate.h"

namespace supergbamidi
{
namespace
{

// A GSF file's PSF version.
constexpr uint8_t kGsfVersion = 0x22;

// Returns true if `file` starts with the signature of a ZIP, 7-Zip, RAR or gzip archive.
bool IsArchive(const std::vector<uint8_t>& file)
{
    static constexpr std::string_view kSignatures[] = {
        {"PK\x03\x04", 4}, {"7z\xBC\xAF\x27\x1C", 6}, {"Rar!\x1A\x07", 6}, {"\x1F\x8B", 2}};
    for (std::string_view signature : kSignatures)
    {
        if (file.size() >= signature.size() && std::memcmp(file.data(), signature.data(), signature.size()) == 0)
        {
            return true;
        }
    }

    return false;
}

// Decompresses a GSF file's program with puff. supergbamidi doesn't check the header's CRC-32. puff checks the zlib
// stream's Adler-32 instead.
std::optional<std::string> Decompress(const uint8_t* data, size_t size, uint32_t, std::vector<uint8_t>& program)
{
    std::string error;
    if (!ZlibDecompress(data, size, program, error))
    {
        return error;
    }

    return std::nullopt;
}

} // namespace

bool Rom::Load(const std::string& path, std::string& error)
{
    data_.clear();
    from_gsf_ = false;
    library_.clear();

    // Folders get dropped on the program too; some systems open them as empty files.
    std::error_code ec;
    if (std::filesystem::is_directory(PathFromUtf8(path), ec))
    {
        error = "it's a folder, not a ROM or GSF file";
        return false;
    }

    std::vector<uint8_t> file;
    if (!ReadFile(path, file))
    {
        error = "it can't be read";
        return false;
    }

    // An archive would otherwise be read as a ROM that has no driver supergbamidi knows.
    if (IsArchive(file))
    {
        error = "it's an archive: extract the ROM or GSF files from it first";
        return false;
    }

    if (file.size() >= 4 && std::memcmp(file.data(), "PSF", 3) == 0)
    {
        if (!LoadGsf(path, 0, error))
        {
            return false;
        }

        from_gsf_ = true;
    }
    else
    {
        if (file.size() < 0xC0)
        {
            error = "it's too small to be a GBA ROM";
            return false;
        }

        if (file.size() > 0x2000000)
        {
            file.resize(0x2000000);
        }

        data_ = std::move(file);
    }

    if (data_.empty())
    {
        error = "it contains no ROM data";
        return false;
    }

    return true;
}

bool Rom::LoadGsf(const std::string& path, int depth, std::string& error)
{
    if (depth > xsflib::psf::kMaxLibDepth)
    {
        error = "GSF library chain is too deep (circular _lib reference?)";
        return false;
    }

    // Messages about the file itself follow its name, and those about a library name it.
    const std::string who = depth == 0 ? "it" : "the library " + Utf8(PathFromUtf8(path).filename());
    const std::string whose = depth == 0 ? "its" : who + "'s";
    std::vector<uint8_t> file;
    if (!ReadFile(path, file))
    {
        error = who + " can't be read";
        return false;
    }

    // The header and the tags come first. The program is read after the _lib that it goes on top of.
    xsflib::psf::ParseOptions options;
    options.with_program = false;
    options.version = kGsfVersion;
    options.not_psf = who + " isn't a PSF file";
    options.wrong_version = who + " isn't a GSF (PSF version 0x22) file";
    options.truncated = who + " is truncated";
    options.decompress = Decompress;
    xsflib::psf::PsfFile psf;
    if (const std::optional<std::string> header_error = xsflib::psf::ParsePsf(file, psf, options))
    {
        error = *header_error;
        return false;
    }

    auto tag_value = [&psf](const std::string& key)
    {
        const auto it = psf.tags.find(key);
        return it != psf.tags.end() ? it->second : std::string();
    };

    // A _lib tag names a file beside this one. Where the folder has no file of the tag's name, one whose name differs
    // only in case takes its place. Windows and macOS find that file anyway.
    auto lib_path = [&](const std::string& name)
    {
        const std::filesystem::path exact = PathFromUtf8(path).parent_path() / PathFromUtf8(name);
        std::error_code ec;
        if (std::filesystem::exists(exact, ec))
        {
            return Utf8(exact);
        }

        const std::filesystem::path folder =
            exact.parent_path().empty() ? std::filesystem::path(".") : exact.parent_path();
        const std::string wanted = Utf8(exact.filename());
        const auto same = [](char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string found = Utf8(it->path().filename());
            if (found.size() == wanted.size() && std::equal(found.begin(), found.end(), wanted.begin(), same))
            {
                return Utf8(it->path());
            }
        }

        return Utf8(exact);
    };

    // As the PSF format specifies, _lib is loaded first, then the file's program on top of it, then _lib2, _lib3 and so
    // on up to the first one missing, each on top of the last.
    if (const std::string lib = tag_value("_lib"); !lib.empty())
    {
        if (depth == 0)
        {
            library_ = lib_path(lib);
        }
        if (!LoadGsf(lib_path(lib), depth + 1, error))
        {
            return false;
        }
    }

    if (psf.compressed_size > 0)
    {
        options.with_program = true;
        if (const std::optional<std::string> program_error = xsflib::psf::ParsePsf(file, psf, options))
        {
            error = whose + " program: " + *program_error;
            return false;
        }
        if (!ApplyGsfProgram(psf.program, error))
        {
            error = whose + " program: " + error;
            return false;
        }
    }

    for (int n = 2;; n++)
    {
        const std::string lib = tag_value("_lib" + std::to_string(n));
        if (lib.empty())
        {
            break;
        }

        if (!LoadGsf(lib_path(lib), depth + 1, error))
        {
            return false;
        }
    }

    return true;
}

bool Rom::ApplyGsfProgram(const std::vector<uint8_t>& program, std::string& error)
{
    if (program.size() < 12)
    {
        error = "GSF program section is too short";
        return false;
    }

    const uint32_t offset = xsflib::LoadLe32(&program[4]);
    uint32_t size = xsflib::LoadLe32(&program[8]);
    if (size > program.size() - 12)
    {
        size = uint32_t(program.size() - 12);
    }

    if ((offset >> 24) != 0x08 && (offset >> 24) != 0x09)
    {
        error = "multiboot GSF sets (loaded to EWRAM) aren't supported";
        return false;
    }

    const uint32_t rom_offset = offset & 0x01FFFFFF;
    if (uint64_t(rom_offset) + size > 0x2000000)
    {
        error = "GSF program doesn't fit in the cartridge address space";
        return false;
    }

    if (data_.size() < rom_offset + size)
    {
        data_.resize(rom_offset + size, 0);
    }

    std::copy(program.begin() + 12, program.begin() + 12 + size, data_.begin() + rom_offset);

    return true;
}

std::string Rom::Title() const
{
    std::string s;
    for (uint32_t i = 0; i < 12; i++)
    {
        const uint8_t c = U8(kRomBase + 0xA0 + i);
        if (c == 0)
        {
            break;
        }

        s += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }

    return s;
}

std::string Rom::GameCode() const
{
    std::string s;
    for (uint32_t i = 0; i < 4; i++)
    {
        const uint8_t c = U8(kRomBase + 0xAC + i);
        s += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }

    return s;
}

} // namespace supergbamidi
