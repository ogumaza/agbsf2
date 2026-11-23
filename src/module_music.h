// SPDX-License-Identifier: MIT

// What the Music classes of the module players share: the ROM, what detection found, the shared SoundFont and the
// opening of the music after detection.

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "agblib/sequence_export/agb/sf2.h"

#include "music.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2
{

// The Music of a module player. Every entry of the song table is a song, the shared SoundFont collects the instruments
// in an `Instruments`, and no listing of the songs exists. `Info` is the player's DriverInfo, with its `log` and its
// `warnings`.
template <typename Info, typename Instruments>
class ModuleMusic : public Music
{
public:
    bool HasSong(int) const override
    {
        return true;
    }

    // The shared SoundFont numbers the instruments of every song together.
    void ShareSoundfont(const ConvertSettings&) override
    {
        shared_ = std::make_unique<Instruments>();
    }

    bool WriteSharedSoundfont(const std::string& path, std::string& error) override
    {
        agblib::sequence_export::agb::Sf2File f = shared_->Build(rom_);
        return WriteSoundfont(f, rom_, path, error);
    }

    bool DumpSong(int, const std::string&, std::string& error) const override
    {
        error = "no listing of this driver's songs";
        return false;
    }

protected:
    // Keeps a reference to `rom`. The ROM has to outlive this. `name` is the driver's name.
    ModuleMusic(const std::string& name, const Rom& rom, Info info)
        : Music(name, info.log, info.warnings), rom_(rom), info_(std::move(info))
    {
    }

    const Rom& GameRom() const
    {
        return rom_;
    }

    const Info& Detected() const
    {
        return info_;
    }

    // Returns the shared SoundFont's instruments, or null before ShareSoundfont().
    Instruments* Shared() const
    {
        return shared_.get();
    }

private:
    const Rom& rom_;
    Info info_;
    std::unique_ptr<Instruments> shared_;
};

// Detects the player in `rom` with `detect`, and returns its Music from `make`. Returns null and sets `error` where
// detection fails.
template <typename Info>
std::unique_ptr<Music> OpenDetected(const Rom& rom, bool (*detect)(const Rom&, Info&, std::string&),
                                    std::unique_ptr<Music> (*make)(const Rom&, Info), std::string& error)
{
    Info info;
    if (!detect(rom, info, error))
    {
        return nullptr;
    }

    return make(rom, std::move(info));
}

} // namespace agbsf2
