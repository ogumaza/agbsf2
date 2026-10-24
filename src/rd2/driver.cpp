// SPDX-License-Identifier: MIT

#include "rd2/driver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/text.h"

#include "rd2/sequencer.h"
#include "rom.h"
#include "thumb.h"

namespace agbsf2::rd2
{
namespace
{

// The most sequences detection reads, so that unrelated ROM data can't keep it busy.
constexpr int kMaxSequences = 1024;

// The most sound effects that detection reads. Like kMaxSequences, it keeps unrelated ROM data from keeping detection
// busy.
constexpr size_t kMaxEffects = 4096;

// The start of the driver's init routine, which stores the game's settings and sets up the sound hardware.
constexpr const char* kInitPattern = "B530 49xx 6008 49xx 2000 7008 2080 7008 3904 4Axx 1C10 8008 3102 200D 7008";

// The code that loads each of the driver's tables, and the ldr's index among its halfwords.
struct TablePattern
{
    const char* pattern;
    int index;
};

// A revision's code that loads the tables whose code differs between the revisions. The games' compilers built it with
// different prologues, registers and literal pools. The routine that works out a voice's pitch from its note loads the
// pitch table and then the frequency table. The routine that looks up a noise voice's NR43 loads the noise table. The
// LFO's step, in the routine that works out a voice's pitch for a frame, loads the LFO table. The note on loads the
// table of the voices that can play each type of region.
struct RevisionPatterns
{
    TablePattern pitch_table;
    TablePattern frequency_table;
    TablePattern noise_table;
    TablePattern lfo_table;
    TablePattern voice_classes;
};

// The routine that works out a voice's pitch from its note, in each revision.
constexpr const char* kLinkToThePastPitch =
    "B500 0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D107 48xx";
constexpr const char* kSuperMarioAdvance2Pitch =
    "0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 D108 48xx";

constexpr RevisionPatterns kLinkToThePastPatterns = {
    {kLinkToThePastPitch, 20},
    {kLinkToThePastPitch, 30},
    {"B500 0400 0C01 2977 D900 2177 48xx 1808 7800", 6},
    {"68A0 6883 2B00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809", 7},
    {"49xx 7830 1840 7800 1C29 3152 7809", 0},
};

constexpr RevisionPatterns kSuperMarioAdvance2Patterns = {
    {kSuperMarioAdvance2Pitch, 19},
    {kSuperMarioAdvance2Pitch, 30},
    {"0400 0C01 2977 D900 2177 48xx 1808 7800", 5},
    {"68A0 6882 2A00 D0xx 6860 2800 D1xx 48xx 6A29 0849 1809 7809", 7},
    {"49xx 7830 1840 7800 1C29 3152 780A 1C29", 0},
};

// The start of the routine that works out a sample voice's level in the Super Mario Advance 2 revision, which reads
// the velocity from the voice's byte 9 and shifts it left by 8: ldrb r4, [r5, #9]; lsls r4, r4, #8. A Link to the
// Past's reads byte 10 and shifts it by 7.
constexpr const char* kSuperMarioAdvance2Level = "B530 1C05 7868 2801 D1xx 7A6C 0224";

// The end of that routine in the Super Mario Advance 3 revision: a released voice's level, then the volume, with
// lsrs r4, r4, #8. A Link to the Past's routine multiplies the level by 3 and shifts it right by 9 instead. The Super
// Mario Advance 2 revision's routine ends the same way. Detection looks for this end only when the start above is
// missing.
constexpr const char* kSuperMarioAdvance3Level = "30E6 6969 4348 0A40 6168 1C04 0A24 1C20";

// The wave voice's fade after its release.
constexpr TablePattern kWaveVolumes = {"2904 D900 2104 0609 0E09 4Axx 48xx 1809 7808 7010", 6};

// The instrument lookup, for instruments with a sample for each key.
constexpr TablePattern kKeyEnvelope = {"8869 1859 4Axx 0070 1840 8800 8050 6022 48xx 6060", 8};

// The lookup of the music's entry in Super Mario Advance 3's play-music routine. The game's table holds a halfword for
// each music number, with the sequence in its low byte. The code is ldr r1, =table; lsls r0, r4, #1; adds r0, r0, r1;
// ldrh r1, [r0]; ldrb r5, [r0]; movs r0, #0x80; lsls r0, r0, #3; ands r0, r1; cmp r0, #0; beq. The movs, lsls and ands
// pick bit 10 of the entry. With the bit set, movs r0, #voices and a bl follow. The bl calls the routine that requests
// that many sample voices.
constexpr const char* kPlayMusicPattern = "49xx 0060 1840 8801 7805 2080 00C0 4008 2800 D0xx 20xx Fxxx Fxxx";

// The lookup of the music's sequence in Super Mario Advance 2's play-music routine, and the request that follows it.
// The code is ldr r0, =table; lsls r1, r5, #1; adds r1, r1, r0; ldrb r4, [r1]; movs r0, #player; adds r1, r4, #0; bl
// (the request for the sequence); adds r0, r5, #0; bl (which keeps the music number). For one sequence, the routine
// then mutes tracks of the player: cmp r4, #sequence; bne; movs r1, #mask; lsls r1, r1, #shift; cmp r5, #music; bne;
// movs r1, #mask; lsls r1, r1, #shift; movs r0, #player; movs r2, #1; bl (kMuteRequestPattern). The music number of the
// cmp mutes the tracks of the second mask, and every other music number those of the first.
constexpr const char* kMutingPlayMusicPattern =
    "48xx 0069 1809 780C 20xx 1C21 Fxxx Fxxx 1C28 Fxxx Fxxx 2Cxx D1xx 21xx 0xx9 2Dxx D1xx 21xx 0xx9 20xx 2201 Fxxx "
    "Fxxx";

// The driver's routine that makes request 0x0100. That request sets the mute of a player's tracks: r0 = the player,
// r1 = a mask of its tracks and r2 = the mute. The request holds its type, then (player << 16) | mute, then the mask.
constexpr const char* kMuteRequestPattern = "B510 0612 0E12 4Bxx 681C 2380 005B 8023 0400 4310 6060 60A1";

// Super Mario Advance 2's routine that mutes tracks of the music by the game's state: push {r4, lr}; lsls r0, r0, #24;
// lsrs r4, r0, #24; bl (the music number that plays, or -1); lsls r0, r0, #16; asrs r0, r0, #16; cmp r0, #0; blt;
// ldr r1, =table; lsls r0, r0, #1; adds r0, r0, r1; ldrh r1, [r0]; movs r0, #value; lsls r0, r0, #shift; ands r0, r1;
// cmp r0, #0; beq; movs r2, #0; cmp r4, #0; bne; movs r2, #1; movs r0, #player; movs r1, #mask; bl
// (kMuteRequestPattern). Where the music's entry has the bit, an argument of 0 mutes the tracks of the mask, and 1 lets
// them play. The game's callers pass the flag that the player rides Yoshi.
constexpr const char* kYoshiMutePattern =
    "B510 0600 0E04 Fxxx Fxxx 0400 1400 2800 DBxx 49xx 0040 1840 8801 20xx 00xx "
    "4008 2800 D0xx 2200 2C00 D1xx 2201 20xx 21xx Fxxx Fxxx";

// A mask of every track of a player.
constexpr uint32_t kTracks = (1u << kPlayerTracks) - 1;

// The most music numbers that detection reads from the game's table.
constexpr uint32_t kMaxMusic = 256;

// What each revision's code does differently from A Link to the Past's revision.
constexpr RevisionTraits kLinkToThePastTraits = {};
constexpr RevisionTraits kSuperMarioAdvance2Traits = {
    .byte_tempo = true,
    .player_volume = false,
    .short_notes = true,
    .keeps_note = false,
    .scaled_bend = true,
    .wrapped_step = true,
    .older_levels = true,
    .volume_scale = 1,
    .volume_shift = 8,
};
constexpr RevisionTraits kSuperMarioAdvance3Traits = {.volume_scale = 1, .volume_shift = 8};

// The part of the ROM that detection searches for the driver's code: from the driver's init up to the next copy's init.
// A cartridge can hold a second copy of the driver in the multiboot image that the game sends to other GBAs. The two
// copies can be different revisions. In Super Mario Advance 3, the image of Mario Bros. has the Super Mario Advance 2
// revision, and the game has a later one.
struct DriverCopy
{
    uint32_t begin;
    uint32_t end;
};

// Returns the address of the first match of a pattern in the copy of the driver, or 0 if there's no match.
uint32_t FindInCopy(const Rom& rom, const DriverCopy& copy, const char* pattern)
{
    for (uint32_t hit : FindThumb(rom, ParseThumbPattern(pattern)))
    {
        if (hit >= copy.begin && hit < copy.end)
        {
            return hit;
        }
    }

    return 0;
}

// Returns the literal that the ldr of the first match of a table's pattern in the copy of the driver loads, or 0 if
// there's no match.
uint32_t FindTable(const Rom& rom, const DriverCopy& copy, const TablePattern& table)
{
    const uint32_t hit = FindInCopy(rom, copy, table.pattern);
    return hit ? ThumbLiteral(rom, hit + 2 * uint32_t(table.index)) : 0;
}

// Returns the game's settings from its call to the driver's init: the ldr r0 just before the bl.
uint32_t FindSettings(const Rom& rom, uint32_t init)
{
    for (uint32_t call : FindCalls(rom, init))
    {
        if ((rom.U16(call - 2) & 0xFF00) == 0x4800)
        {
            const uint32_t settings = ThumbLiteral(rom, call - 2);
            if (rom.Contains(settings, kSettingsSize))
            {
                return settings;
            }
        }
    }

    return 0;
}

// Returns the address of entry `index` of a table of offsets from its own start, or 0 if it's outside the ROM.
uint32_t Entry(const Rom& rom, uint32_t table, uint32_t index)
{
    const uint32_t at = table + 4 * index;
    if (!rom.Contains(at, 4))
    {
        return 0;
    }

    const uint32_t address = table + rom.U32(at);
    return rom.Contains(address) ? address : 0;
}

// Returns the number of entries in a table of offsets, from where its first entry points: just past the table.
uint32_t EntryCount(const Rom& rom, uint32_t table)
{
    const uint32_t first = rom.U32(table);
    return first % 4 == 0 && rom.Contains(table, first) ? first / 4 : 0;
}

// Returns true if the sequence at `at` has a header the driver can read: 1 to 10 tracks, which start inside the ROM.
bool PlausibleSequence(const Rom& rom, uint32_t at)
{
    const int tracks = rom.S8(at);
    if (tracks < 1 || tracks > kPlayerTracks || !rom.Contains(at, 2 + 2 * uint32_t(tracks)))
    {
        return false;
    }

    for (int t = 0; t < tracks; t++)
    {
        const uint32_t offset = rom.U16(at + 2 + 2 * uint32_t(t));
        if (offset != 0 && (offset < 2 + 2 * uint32_t(tracks) || !rom.Contains(at + offset)))
        {
            return false;
        }
    }

    return true;
}

// What the code of the game's play-music routine gives: its table of music and the requests that it makes along with
// the request for the sequence. Super Mario Advance 3's form picks the sample voices by a bit of the entry, `bit`:
// `with_bit` voices with it and `without_bit` without. `volumes` holds each music number that sets the player's second
// volume, and the volume it sets. Super Mario Advance 2's form mutes tracks of one sequence, `muted_sequence`. Every
// music number that plays it mutes the tracks in `mask`, except `music`. That one mutes those in `music_mask`. The
// requests go to `player`, through the driver's routine at `mute_request` (kMuteRequestPattern). Unless the player
// rides Yoshi, that game also mutes the tracks in `yoshi_mask` of each music whose entry has `yoshi_bit` set
// (kYoshiMutePattern).
struct PlayMusicRoutine
{
    uint32_t table = 0;
    uint32_t bit = 0;
    int with_bit = kSampleVoices;
    int without_bit = kSampleVoices;
    std::vector<std::pair<uint32_t, int>> volumes;
    int muted_sequence = -1;
    uint32_t music = 0;
    uint16_t mask = 0;
    uint16_t music_mask = 0;
    uint32_t player = 0;
    uint32_t mute_request = 0;
    uint32_t yoshi_bit = 0;
    uint16_t yoshi_mask = 0;
};

// Reads the play-music routine where kPlayMusicPattern matches it, at `at`. With the entry's bit clear, the beq goes to
// movs r0, #voices and a bl to the same routine. Both ways go on to adds r1, r5, #0, movs r0, #player and a bl to the
// request for the sequence on the player. A run of cmp r4, #music; bne; movs r0, #player; movs r1, #volume; and a bl to
// the request for the player's second volume follows. Returns nothing if the code doesn't take that form.
std::optional<PlayMusicRoutine> ParsePlayMusic(const Rom& rom, uint32_t at)
{
    PlayMusicRoutine routine;
    routine.table = ThumbLiteral(rom, at);
    routine.bit = uint32_t(rom.U16(at + 10) & 0xFF) << ((rom.U16(at + 12) >> 6) & 0x1F);
    routine.with_bit = rom.U16(at + 20) & 0xFF;
    const uint32_t set_voices = BlTarget(rom, at + 22);
    const uint32_t clear = uint32_t(int64_t(at) + 22 + 2 * int8_t(rom.U16(at + 18) & 0xFF));
    routine.without_bit = rom.U16(clear) & 0xFF;
    const int player = rom.U16(clear + 8) & 0xFF;
    if ((rom.U16(clear) & 0xFF00) != 0x2000 || !set_voices || BlTarget(rom, clear + 2) != set_voices ||
        rom.U16(clear + 6) != 0x1C29 || (rom.U16(clear + 8) & 0xFF00) != 0x2000 || !BlTarget(rom, clear + 10) ||
        routine.with_bit < 1 || routine.with_bit > kSampleVoices || routine.without_bit < 1 ||
        routine.without_bit > kSampleVoices)
    {
        return std::nullopt;
    }

    // The music numbers that set the player's second volume, and the volume that each sets.
    uint32_t set_volume = 0;
    for (uint32_t p = clear + 14; (rom.U16(p) & 0xFF00) == 0x2C00; p += 12)
    {
        const uint32_t target = BlTarget(rom, p + 8);
        if ((rom.U16(p + 2) & 0xFF00) != 0xD100 || rom.U16(p + 4) != (0x2000 | player) ||
            (rom.U16(p + 6) & 0xFF00) != 0x2100 || !target || (set_volume && target != set_volume))
        {
            break;
        }

        set_volume = target;
        routine.volumes.push_back({rom.U16(p) & 0xFFu, rom.U16(p + 6) & 0xFF});
    }

    return routine;
}

// Returns the value that movs r1, #value at `at` and lsls r1, r1, #shift after it give, or nothing if the lsls shifts
// another register.
std::optional<uint32_t> ShiftedValue(const Rom& rom, uint32_t at)
{
    const uint16_t shift = rom.U16(at + 2);
    if ((shift & 0xF83F) != 0x0009)
    {
        return std::nullopt;
    }

    return uint32_t(rom.U16(at) & 0xFF) << ((shift >> 6) & 0x1F);
}

// Reads Super Mario Advance 2's play-music routine where kMutingPlayMusicPattern matches it, at `at`. The form requires
// both requests to go to the same player and the masks to hold only the player's tracks. The last bl has to call the
// request that mutes tracks (kMuteRequestPattern). Returns nothing if the code doesn't take that form.
std::optional<PlayMusicRoutine> ParseMutingPlayMusic(const Rom& rom, uint32_t at)
{
    const std::optional<uint32_t> mask = ShiftedValue(rom, at + 26);
    const std::optional<uint32_t> music_mask = ShiftedValue(rom, at + 34);
    const std::vector<uint32_t> requests = FindThumb(rom, ParseThumbPattern(kMuteRequestPattern));
    const uint32_t request = BlTarget(rom, at + 42);
    if (!mask || !music_mask || (*mask & ~kTracks) || (*music_mask & ~kTracks) || rom.U16(at + 8) != rom.U16(at + 38) ||
        std::find(requests.begin(), requests.end(), request) == requests.end())
    {
        return std::nullopt;
    }

    PlayMusicRoutine routine;
    routine.table = ThumbLiteral(rom, at);
    routine.muted_sequence = rom.U16(at + 22) & 0xFF;
    routine.music = rom.U16(at + 30) & 0xFFu;
    routine.mask = uint16_t(*mask);
    routine.music_mask = uint16_t(*music_mask);
    routine.player = rom.U16(at + 8) & 0xFFu;
    routine.mute_request = request;

    return routine;
}

// Reads into `routine` Super Mario Advance 2's routine that mutes tracks unless the player rides Yoshi.
// kYoshiMutePattern matches the routine at `at`. The routine has to read the play-music routine's table and request the
// mute on its player. The mask has to hold only the player's tracks. The last bl has to call the routine that the
// play-music routine calls for the mute. Leaves `routine` as it is if the code doesn't take that form.
void ParseYoshiMute(const Rom& rom, uint32_t at, PlayMusicRoutine& routine)
{
    const uint16_t shift = rom.U16(at + 28);
    const uint32_t mask = rom.U16(at + 46) & 0xFFu;
    if ((shift & 0xF83F) != 0 || ThumbLiteral(rom, at + 18) != routine.table ||
        (rom.U16(at + 44) & 0xFFu) != routine.player || (mask & ~kTracks) ||
        BlTarget(rom, at + 48) != routine.mute_request)
    {
        return;
    }

    routine.yoshi_bit = uint32_t(rom.U16(at + 26) & 0xFF) << ((shift >> 6) & 0x1F);
    routine.yoshi_mask = uint16_t(mask);
}

// Returns the settings that the play-music routine makes for music number `music` from its entry in the table, `entry`.
MusicSettings SettingsOf(const PlayMusicRoutine& routine, uint32_t music, uint16_t entry)
{
    MusicSettings s;
    s.music = int(music);
    s.sample_voices = (entry & routine.bit) ? routine.with_bit : routine.without_bit;
    for (const auto& [number, volume] : routine.volumes)
    {
        if (number == music)
        {
            s.volume2 = volume;
        }
    }
    if ((entry & 0xFF) == routine.muted_sequence)
    {
        s.muted_tracks = music == routine.music ? routine.music_mask : routine.mask;
    }

    return s;
}

// Gives each sequence the settings of the first music number that plays it, and a version for each later music number
// that mutes other tracks. The game's table of music doesn't give its length. Detection reads it up to the first entry
// whose sequence the game doesn't have. The game has `game_count` sequences. An override of the count doesn't change
// that. An entry for a sequence that isn't converted is skipped.
void ApplyMusicTable(const Rom& rom, const PlayMusicRoutine& routine, uint32_t game_count, DriverInfo& info)
{
    for (uint32_t music = 0; music < kMaxMusic && rom.Contains(routine.table + 2 * music, 2); music++)
    {
        const uint16_t entry = rom.U16(routine.table + 2 * music);
        const size_t sequence = entry & 0xFF;
        if (sequence >= game_count)
        {
            break;
        }
        if (sequence >= info.music_settings.size())
        {
            continue;
        }

        // Unless the player rides Yoshi, Super Mario Advance 2 mutes the tracks in yoshi_mask of the music whose entry
        // has yoshi_bit set. A new game starts without Yoshi. The version with those tracks muted comes first.
        MusicSettings s = SettingsOf(routine, music, entry);
        std::vector<MusicSettings> made = {s};
        if ((entry & routine.yoshi_bit) && routine.yoshi_mask)
        {
            made.front().muted_tracks |= routine.yoshi_mask;
            s.riding_yoshi = true;
            made.push_back(s);
        }

        std::vector<MusicSettings>& versions = info.music_settings[sequence];
        for (const MusicSettings& m : made)
        {
            const auto same_mutes = [&m](const MusicSettings& v)
            {
                return v.muted_tracks == m.muted_tracks;
            };
            if (versions.front().music < 0)
            {
                versions.front() = m;
            }
            else if (std::none_of(versions.begin(), versions.end(), same_mutes))
            {
                versions.push_back(m);
            }
        }
    }
}

// Returns a list in words: "a", "a and b", or "a, b and c".
std::string ListOf(const std::vector<std::string>& items)
{
    std::string text;
    for (size_t i = 0; i < items.size(); i++)
    {
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    }

    return text;
}

// Returns the tracks of a mask in words for --info: "track 8" or "tracks 8 and 9".
std::string TrackList(uint16_t mask)
{
    std::vector<std::string> tracks;
    for (int t = 0; t < kPlayerTracks; t++)
    {
        if ((mask >> t) & 1)
        {
            tracks.push_back(std::to_string(t));
        }
    }

    return (tracks.size() == 1 ? "track " : "tracks ") + ListOf(tracks);
}

// Returns the line for --info: the game's table of music, and the sequences that play with other settings than the
// driver's defaults. The form of the routine in Super Mario Advance 3 gives the sample voices and the second volume.
// The form in Super Mario Advance 2 gives the tracks that each version of a sequence mutes. That game's tracks that
// play only while the player rides Yoshi come last.
std::string MusicSettingsLine(const PlayMusicRoutine& routine, const DriverInfo& info)
{
    std::string voices, volume2;
    std::vector<std::string> mutes, yoshi;
    for (size_t sequence = 0; sequence < info.music_settings.size(); sequence++)
    {
        const std::vector<MusicSettings>& versions = info.music_settings[sequence];
        const MusicSettings& s = versions.front();
        if (s.sample_voices != kSampleVoices)
        {
            voices += (voices.empty() ? "" : ", ") + std::to_string(sequence);
        }
        if (s.volume2 != MusicSettings().volume2)
        {
            volume2 += (volume2.empty() ? "" : ", ") + std::to_string(sequence) + " (" +
                       xsflib::Hex(uint32_t(s.volume2), 2, true) + ")";
        }

        // The tracks that each version of the sequence mutes, without those that play while the player rides Yoshi.
        const bool riding =
            std::any_of(versions.begin(), versions.end(), [](const MusicSettings& v) { return v.riding_yoshi; });
        std::vector<std::string> muted;
        for (const MusicSettings& v : versions)
        {
            const uint16_t mask = riding ? v.muted_tracks & ~routine.yoshi_mask : v.muted_tracks;
            if (mask && !v.riding_yoshi)
            {
                muted.push_back(TrackList(mask) + " as music " + std::to_string(v.music));
            }
        }
        if (!muted.empty())
        {
            mutes.push_back("in sequence " + std::to_string(sequence) + ", " + ListOf(muted));
        }

        if (riding)
        {
            yoshi.push_back(std::to_string(sequence));
        }
    }

    std::string line = "the game's music table: " + Hex(routine.table);
    if (routine.bit)
    {
        line += "; sequences with " + std::to_string(routine.with_bit) +
                " sample voices: " + (voices.empty() ? "none" : voices) +
                "; second volume: " + (volume2.empty() ? "none" : volume2);
    }

    if (routine.muted_sequence >= 0)
    {
        std::string list;
        for (const std::string& m : mutes)
        {
            list += (list.empty() ? "" : "; ") + m;
        }
        line += "; muted tracks: " + (list.empty() ? "none" : list);
    }

    if (routine.yoshi_mask)
    {
        line += "; muted unless riding Yoshi: " +
                (yoshi.empty() ? "none"
                               : TrackList(routine.yoshi_mask) +
                                     (yoshi.size() == 1 ? " of sequence " : " of sequences ") + ListOf(yoshi));
    }

    return line;
}

// Finds the game's play-music routine in one of the forms that detection reads: the form in Super Mario Advance 3
// (kPlayMusicPattern) or the form in Super Mario Advance 2 (kMutingPlayMusicPattern). Returns nothing otherwise.
std::optional<PlayMusicRoutine> FindPlayMusic(const Rom& rom)
{
    const std::vector<uint32_t> voices = FindThumb(rom, ParseThumbPattern(kPlayMusicPattern));
    if (voices.size() == 1)
    {
        return ParsePlayMusic(rom, voices[0]);
    }

    const std::vector<uint32_t> mutes = FindThumb(rom, ParseThumbPattern(kMutingPlayMusicPattern));
    if (mutes.size() == 1)
    {
        std::optional<PlayMusicRoutine> routine = ParseMutingPlayMusic(rom, mutes[0]);
        const std::vector<uint32_t> yoshi = FindThumb(rom, ParseThumbPattern(kYoshiMutePattern));
        if (routine && yoshi.size() == 1)
        {
            ParseYoshiMute(rom, yoshi[0], *routine);
        }

        return routine;
    }

    return std::nullopt;
}

// Reads the settings that the game's play-music routine makes along with each sequence, and the versions of the
// sequences that it plays with other tracks muted. Returns a line for --info, or an empty one if the game has no such
// routine.
std::string ReadMusicSettings(const Rom& rom, uint32_t game_count, DriverInfo& info)
{
    info.music_settings.assign(info.sequence_addresses.size(), {MusicSettings()});
    const std::optional<PlayMusicRoutine> routine = FindPlayMusic(rom);
    if (!routine)
    {
        return "";
    }

    ApplyMusicTable(rom, *routine, game_count, info);

    return MusicSettingsLine(*routine, info);
}

// Returns the revision of the copy of the driver, from the code that works out a sample voice's level.
Revision DetectRevision(const Rom& rom, const DriverCopy& copy)
{
    if (FindInCopy(rom, copy, kSuperMarioAdvance2Level))
    {
        return Revision::kSuperMarioAdvance2;
    }

    return FindInCopy(rom, copy, kSuperMarioAdvance3Level) ? Revision::kSuperMarioAdvance3 : Revision::kLinkToThePast;
}

// Finds the driver's tables in the copy of the driver, from the revision's code that reads them. Returns false and sets
// `error` if a table isn't in the ROM.
bool FindTables(const Rom& rom, const DriverCopy& copy, DriverInfo& info, std::string& error)
{
    const RevisionPatterns& patterns =
        info.revision == Revision::kSuperMarioAdvance2 ? kSuperMarioAdvance2Patterns : kLinkToThePastPatterns;
    info.pitch_table = FindTable(rom, copy, patterns.pitch_table);
    info.frequency_table = FindTable(rom, copy, patterns.frequency_table);
    info.noise_table = FindTable(rom, copy, patterns.noise_table);
    info.lfo_table = FindTable(rom, copy, patterns.lfo_table);
    info.wave_volumes = FindTable(rom, copy, kWaveVolumes);
    info.voice_classes = FindTable(rom, copy, patterns.voice_classes);
    info.key_envelope = FindTable(rom, copy, kKeyEnvelope);

    const RomTable tables[] = {
        {info.pitch_table, 480, "pitch table"},
        {info.frequency_table, 240, "frequency table"},
        {info.noise_table, 120, "noise table"},
        {info.lfo_table, 256, "LFO table"},
        {info.wave_volumes, 5, "wave volume table"},
        {info.voice_classes, 5, "voice class table"},
        {info.key_envelope, 4, "envelope for instruments with a sample for each key"},
    };
    if (const char* missing = MissingTable(rom, tables))
    {
        error = std::string("found Nintendo R&D2's sound driver, but not its ") + missing;
        return false;
    }

    return true;
}

// Reads the address of each sequence and of its list of banks, up to the game's count of sequences or the count that
// `overrides` gives. The table of sequences ends where its first sequence starts, and so does the table of bank lists.
// The shorter gives the game's count. Returns the game's count.
uint32_t ReadSequenceTable(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info)
{
    uint32_t count = std::min(EntryCount(rom, info.sequences), EntryCount(rom, info.bank_lists));
    count = std::min<uint32_t>(count, kMaxSequences);
    const uint32_t game_count = count;
    if (overrides.sequence_count > 0)
    {
        count = std::min<uint32_t>(uint32_t(overrides.sequence_count), kMaxSequences);
    }

    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t sequence = Entry(rom, info.sequences, i);
        const uint32_t banks = Entry(rom, info.bank_lists, i);
        if (!sequence || !banks)
        {
            break;
        }

        info.sequence_addresses.push_back(sequence);
        info.bank_list_addresses.push_back(banks);
    }

    return game_count;
}

// Reads the sound effects' sequences, their lists of banks and their tracks. The table of the sequences ends where its
// first sequence starts. So does the table of their bank lists. A sequence's table of tracks ends where its first track
// starts. A track whose offset is 0, or points out of the ROM, gives no sound effect.
void ReadEffects(const Rom& rom, DriverInfo& info)
{
    if (!rom.Contains(info.effects, 4) || !rom.Contains(info.effect_bank_lists, 4))
    {
        return;
    }

    const uint32_t count =
        std::min({EntryCount(rom, info.effects), EntryCount(rom, info.effect_bank_lists), uint32_t(kMaxSequences)});
    for (uint32_t s = 0; s < count && info.sound_effects.size() < kMaxEffects; s++)
    {
        const uint32_t sequence = Entry(rom, info.effects, s);
        const uint32_t banks = Entry(rom, info.effect_bank_lists, s);
        if (!sequence || !banks)
        {
            break;
        }

        info.effect_sequence_addresses.push_back(sequence);
        info.effect_bank_list_addresses.push_back(banks);
        uint32_t end = 0x10000;
        for (uint32_t t = 0; 2 * t < end && rom.Contains(sequence + 2 * t, 2); t++)
        {
            const uint32_t offset = rom.U16(sequence + 2 * t);
            if (offset != 0)
            {
                end = std::min(end, offset);
            }
        }
        for (uint32_t t = 0;
             2 * t < end && rom.Contains(sequence + 2 * t, 2) && info.sound_effects.size() < kMaxEffects; t++)
        {
            const uint32_t offset = rom.U16(sequence + 2 * t);
            Effect e;
            e.sequence = int(s);
            e.track = int(t);
            e.address = offset != 0 && rom.Contains(sequence + offset) ? sequence + offset : 0;
            info.sound_effects.push_back(e);
        }
    }
}

// Adds the lines that --info prints: the revision, the game's settings and tables, and the driver's tables. `music` is
// the line for the settings that the game's play-music routine makes. An empty line adds nothing.
void LogDetection(const Rom& rom, const std::string& music, DriverInfo& info)
{
    info.log.push_back(std::string(RevisionName(info.revision)) +
                       " revision of the driver, init routine: " + Hex(info.init));
    info.log.push_back("the game's settings: " + Hex(info.settings));
    info.log.push_back("sequence table: " + Hex(info.sequences) + " (" +
                       std::to_string(info.sequence_addresses.size()) + " sequences)");
    info.log.push_back("bank lists: " + Hex(info.bank_lists) + ", banks: " + Hex(info.banks) + " (" +
                       std::to_string(EntryCount(rom, info.banks)) + ")");
    info.log.push_back("sample sets: " + Hex(info.sample_sets) + " (" +
                       std::to_string(EntryCount(rom, info.sample_sets)) +
                       "), each bank's set: " + Hex(info.bank_sample_sets));
    if (!info.sound_effects.empty())
    {
        const size_t first = info.sequence_addresses.size();
        info.log.push_back("sound effects: " + Hex(info.effects) + " (" +
                           std::to_string(info.effect_sequence_addresses.size()) + " sequences, " +
                           std::to_string(info.sound_effects.size()) + " effects: songs " + std::to_string(first) +
                           "-" + std::to_string(first + info.sound_effects.size() - 1) +
                           "), bank lists: " + Hex(info.effect_bank_lists));
    }
    info.log.push_back("pitch table: " + Hex(info.pitch_table) + ", frequency table: " + Hex(info.frequency_table) +
                       ", noise table: " + Hex(info.noise_table) + ", LFO table: " + Hex(info.lfo_table));
    if (!music.empty())
    {
        info.log.push_back(music);
    }
}

} // namespace

const char* RevisionName(Revision r)
{
    switch (r)
    {
    case Revision::kSuperMarioAdvance2:
        return "Super Mario Advance 2";
    case Revision::kSuperMarioAdvance3:
        return "Super Mario Advance 3";
    default:
        return "A Link to the Past";
    }
}

const RevisionTraits& TraitsOf(Revision r)
{
    switch (r)
    {
    case Revision::kSuperMarioAdvance2:
        return kSuperMarioAdvance2Traits;
    case Revision::kSuperMarioAdvance3:
        return kSuperMarioAdvance3Traits;
    default:
        return kLinkToThePastTraits;
    }
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    error.clear();
    const std::vector<uint32_t> inits = FindThumb(rom, ParseThumbPattern(kInitPattern));
    info.init = inits.empty() ? 0 : inits[0];
    if (!info.init && !overrides.settings)
    {
        return false;
    }

    // The game passes the driver its settings when it starts the sound.
    info.settings = overrides.settings ? overrides.settings : FindSettings(rom, info.init);
    if (!info.settings || !rom.Contains(info.settings, kSettingsSize))
    {
        error = "found Nintendo R&D2's sound driver, but not the settings the game gives it";
        return false;
    }

    const uint32_t s = info.settings;
    info.sample_sets = rom.U32(s);
    info.banks = rom.U32(s + 4);
    info.sequences = rom.U32(s + 8);
    info.effects = rom.U32(s + 12);
    info.bank_sample_sets = rom.U32(s + 16);
    info.bank_lists = rom.U32(s + 20);
    info.effect_bank_lists = rom.U32(s + 24);
    for (uint32_t table : {info.sample_sets, info.banks, info.sequences, info.bank_sample_sets, info.bank_lists})
    {
        if (!rom.Contains(table, 4))
        {
            error = "the driver's settings at " + Hex(s) + " don't point to its tables";
            return false;
        }
    }

    // The driver's tables come from the code that reads them. The revisions' compilers built that code differently.
    // With no init found, detection searches the whole ROM.
    const DriverCopy copy = {info.init, inits.size() > 1 ? inits[1] : rom.End()};
    info.revision = DetectRevision(rom, copy);
    if (!FindTables(rom, copy, info, error))
    {
        return false;
    }

    const uint32_t game_count = ReadSequenceTable(rom, overrides, info);
    if (info.sequence_addresses.empty())
    {
        error = "found Nintendo R&D2's sound driver, but its sequence table at " + Hex(info.sequences) + " is empty";
        return false;
    }

    const std::string music = ReadMusicSettings(rom, game_count, info);
    ReadEffects(rom, info);

    int unreadable = 0;
    for (uint32_t sequence : info.sequence_addresses)
    {
        unreadable += PlausibleSequence(rom, sequence) ? 0 : 1;
    }
    if (unreadable > 0)
    {
        info.warnings.push_back(std::to_string(unreadable) + " of the " +
                                std::to_string(info.sequence_addresses.size()) +
                                " sequences have no tracks the driver can read");
    }

    LogDetection(rom, music, info);

    return true;
}

int SongCount(const DriverInfo& info)
{
    return int(info.sequence_addresses.size() + info.sound_effects.size());
}

const Effect* SongEffect(const DriverInfo& info, int song)
{
    const int sequences = int(info.sequence_addresses.size());
    return song >= sequences && song < SongCount(info) ? &info.sound_effects[size_t(song - sequences)] : nullptr;
}

MusicSettings VersionSettings(const DriverInfo& info, int song, int version)
{
    if (song < 0 || size_t(song) >= info.music_settings.size() || info.music_settings[size_t(song)].empty())
    {
        return MusicSettings();
    }

    const std::vector<MusicSettings>& versions = info.music_settings[size_t(song)];
    return versions[size_t(std::clamp(version, 0, int(versions.size()) - 1))];
}

} // namespace agbsf2::rd2
