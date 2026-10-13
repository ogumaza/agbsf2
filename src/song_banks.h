// SPDX-License-Identifier: MIT

// The banks and programs of the presets in a SoundFont that a game's songs share.

#pragma once

#include <array>
#include <bitset>
#include <optional>
#include <vector>

namespace agbsf2
{

// A preset's bank and program.
struct BankProgram
{
    int bank = 0;
    int program = 0;
};

// Gives each song's presets their banks and programs. A song's presets go in the bank numbered after the song, from
// program 0. Each bank then holds the presets of one song. MIDI selects banks 0 to 127. So the presets of a song
// numbered 128 or more, and a song's presets past the 128th, take the first free programs of the other banks. A bank
// that a song has but that hasn't been given out yet stays free for that song. A SoundFont for one song numbers its
// presets from bank 0.
class SongBanks
{
public:
    // Starts with no program taken, for `songs` songs.
    explicit SongBanks(int songs) : songs_(songs)
    {
    }

    // Starts giving out `song`'s presets.
    void Begin(int song);

    // Returns the bank and program of the song's next preset, or nothing if no program is free.
    std::optional<BankProgram> Next();

    // Returns the banks and programs of `song`'s `count` presets, or an empty list, with nothing taken, if they don't
    // fit.
    std::vector<BankProgram> Place(int song, int count);

private:
    static constexpr int kBanks = 128;
    static constexpr int kPrograms = 128;

    int songs_;
    int song_ = 0;
    std::array<std::bitset<kPrograms>, kBanks> taken_;
    std::bitset<kBanks> given_; // the songs' banks that have been given out
};

} // namespace agbsf2
