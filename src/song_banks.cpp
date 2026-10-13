// SPDX-License-Identifier: MIT

#include "song_banks.h"

namespace agbsf2
{

void SongBanks::Begin(int song)
{
    song_ = song;
    if (song >= 0 && song < kBanks)
    {
        given_.set(size_t(song));
    }
}

std::optional<BankProgram> SongBanks::Next()
{
    // The song's bank first, from program 0.
    if (song_ >= 0 && song_ < kBanks)
    {
        auto& programs = taken_[size_t(song_)];
        for (int p = 0; p < kPrograms; p++)
        {
            if (!programs.test(size_t(p)))
            {
                programs.set(size_t(p));
                return BankProgram{song_, p};
            }
        }
    }

    // Then the first free program of a bank that no song has, or whose song has been given its presets.
    for (int b = 0; b < kBanks; b++)
    {
        if (b < songs_ && !given_.test(size_t(b)))
        {
            continue;
        }

        auto& programs = taken_[size_t(b)];
        for (int p = 0; p < kPrograms; p++)
        {
            if (!programs.test(size_t(p)))
            {
                programs.set(size_t(p));
                return BankProgram{b, p};
            }
        }
    }

    return std::nullopt;
}

std::vector<BankProgram> SongBanks::Place(int song, int count)
{
    const SongBanks before = *this;
    Begin(song);
    std::vector<BankProgram> slots;
    for (int i = 0; i < count; i++)
    {
        const std::optional<BankProgram> slot = Next();
        if (!slot)
        {
            *this = before;
            return {};
        }

        slots.push_back(*slot);
    }

    return slots;
}

} // namespace agbsf2
