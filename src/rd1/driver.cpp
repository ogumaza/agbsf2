// SPDX-License-Identifier: MIT
// Locating Nintendo R&D1's MIDI-file sound driver and its tables in a ROM.

#include "rd1/driver.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "rom.h"
#include "thumb.h"

namespace agbsf2::rd1
{

namespace
{

// The routine that starts entry r0 of the song table: it loads the player table and the song table, takes the entry's
// record and player, and calls the start routine with the player's record and the song's record.
constexpr const char* kPlayRoutine =
    "B500 0400 4Bxx 49xx 0B40 1840 8882 0051 1889 0089 18C9 680A 6801 1C10 Fxxx Fxxx BC01 4700";
constexpr uint32_t kPlayPlayerTable = 4; // ldr r3, =player table
constexpr uint32_t kPlaySongTable = 6;   // ldr r1, =song table
constexpr uint32_t kPlayCall = 0x1C;     // bl start routine

// In the start routine: ldr r1, =bank table; ldrh r0, [r5, #4]; lsls r0, r0, #17; lsrs r0, r0, #22.
constexpr const char* kBankLookup = "49xx 88A8 0440 0D80";

// The driver's init: it sets the mixer up at the rate in its first literal, then each player from the setup table.
constexpr const char* kInitRoutine = "B5F0 4647 B480 B084 49xx 22C4 00D2 4Bxx 2080 9000";
constexpr uint32_t kInitRate = 8; // ldr r1, =mix rate
// The init's loop over the players: movs r5, #0; ldr r6, =setup table; mov r8, r6; movs r7, #0.
constexpr const char* kInitPlayers = "2500 4Exx 46B0 2700";
// The loop's end: adds r6, #20; adds r7, #20; adds r5, #1; cmp r5, #n; bls.
constexpr const char* kInitPlayersEnd = "3614 3714 3501 2Dxx D9xx";
// The init's call that sets the voices up: ldr r1, =voices; movs r0, #n; bl.
constexpr const char* kInitVoices = "49xx 20xx Fxxx Fxxx 2500";

// The routine that sets the echo: it stores r0-r3 as four bytes.
constexpr const char* kEchoRoutine = "B510 4Cxx 7020 7061 70A2 70E3 BC10 BC01 4700";

// The routine that runs a channel's LFO and works out its level. Its literal at +0x2A is the LFO's sine table.
constexpr const char* kLfoRoutine =
    "B570 1C06 1C0B 0159 69B0 1844 7D60 2800 D0xx 3801 2100 7560 8261 E0xx 8A20 8A61 1840 8260 6861 02C9 0E49 4Axx";
constexpr uint32_t kLfoSine = 0x2A;
// The same routine in Rhythm Tengoku's revision. It gives a group of tracks a level of its own, and its levels have 8
// bits.
constexpr const char* kGroupLfoRoutine =
    "B5F0 1C06 1C0F 0179 69B0 1844 7D60 2800 D0xx 3801 2100 7560 8261 E0xx 8A20 8A61 1840 8260 6861 02C9 0E49 4Axx";

// The per-frame routine.
constexpr const char* kFrameRoutine = "B5F0 464F 4646 B4C0 48xx 2600 5786 2701 57C7 2102 5641 4688";

// The noise's lookup: ldr r0, =table; subs r1, #0x15; adds r1, r1, r0; ldrb r5, [r1].
constexpr const char* kNoiseLookup = "48xx 3915 1809 780D";

// The wave channel's level lookup: ldr r0, =table; mov r2, ip; lsrs r1, r2, #2; lsls r1, r1, #1; adds r1, r1, r0.
constexpr const char* kWaveLevelLookup = "48xx 4662 0891 0049 1809";

// The pitch table's values for keys 60 to 71, in Hz. The table holds 128 of them, from 8 Hz for key 0 to 12544 Hz for
// key 127.
constexpr uint16_t kPitchOctave[12] = {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494};
constexpr uint32_t kPitchOctaveKey = 60;

constexpr uint32_t kEntrySize = 8;
constexpr uint32_t kRecordSize = 20;
constexpr uint32_t kSetupSize = 20;
constexpr int kInstruments = 128;

// Returns true if the Thumb code at `at` matches the pattern that `text` gives in ParseThumbPattern()'s form.
bool MatchesAt(const Rom& rom, uint32_t at, std::string_view text)
{
    const std::vector<ThumbPattern> pattern = ParseThumbPattern(text);
    for (size_t k = 0; k < pattern.size(); k++)
    {
        const uint32_t address = at + 2 * uint32_t(k);
        if (!rom.Contains(address, 2) || (rom.U16(address) & pattern[k].mask) != pattern[k].value)
        {
            return false;
        }
    }
    return true;
}

// Returns true if `at` is a song record whose first word points to a Standard MIDI File.
bool IsRecord(const Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, kRecordSize))
    {
        return false;
    }
    const uint32_t midi = rom.U32(at);
    return rom.Contains(midi, 14) && rom.U32(midi) == 0x6468544Du; // "MThd"
}

// Returns the immediate of the Thumb `movs rN, #imm` at `at`, or -1 if there's no such instruction or it sets another
// register.
int MovsImmediate(const Rom& rom, uint32_t at, int reg)
{
    const uint16_t op = rom.U16(at);
    if ((op >> 11) != 0x04 || ((op >> 8) & 7) != reg)
    {
        return -1;
    }
    return op & 0xFF;
}

// Finds the game's call of the echo routine that follows the call of the init. The four movs before it give the echo's
// settings. Returns false if there's no such call.
bool FindEchoCall(const Rom& rom, DriverInfo& info)
{
    for (uint32_t call : FindCalls(rom, info.echo_routine))
    {
        bool after_init = false;
        for (uint32_t back = 2; back <= 16 && !after_init; back += 2)
        {
            after_init = BlTarget(rom, call - back - 2) == info.init_routine;
        }
        if (!after_init)
        {
            continue;
        }
        std::array<int, 4> settings = {-1, -1, -1, -1};
        for (uint32_t back = 2; back <= 8; back += 2)
        {
            for (int reg = 0; reg < 4; reg++)
            {
                const int value = MovsImmediate(rom, call - back, reg);
                if (value >= 0 && settings[reg] < 0)
                {
                    settings[reg] = value;
                }
            }
        }
        if (settings[0] < 0 || settings[1] < 0 || settings[2] < 0 || settings[3] < 0)
        {
            continue;
        }
        info.echo = settings;
        info.echo_call = call;
        return true;
    }
    return false;
}

// Returns true if `at` holds an instrument record of a type that the note-on routine plays.
bool IsInstrument(const Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, 8))
    {
        return false;
    }
    switch (rom.U8(at))
    {
    case 0x41:
    case 0x46:
    case 0x50:
    case 0x51:
        return rom.Contains(at, 32);
    case 0x52:
    case 0x53:
        return true;
    default:
        return false;
    }
}

// Returns true if `at` is a bank: 128 words, each 0 or an instrument record, with at least one instrument.
bool IsBank(const Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, 4 * kInstruments))
    {
        return false;
    }
    int found = 0;
    for (int p = 0; p < kInstruments; p++)
    {
        const uint32_t instrument = rom.U32(at + 4 * p);
        if (instrument == 0)
        {
            continue;
        }
        if (!IsInstrument(rom, instrument))
        {
            return false;
        }
        found++;
    }
    return found > 0;
}

// Returns the pitch table: 128 u16 values on a 2-byte boundary that hold kPitchOctave at key 60, 8 at key 0 and 12544
// at key 127. Returns 0 if there's none.
uint32_t FindPitchTable(const Rom& rom)
{
    uint8_t octave[sizeof kPitchOctave];
    for (size_t k = 0; k < std::size(kPitchOctave); k++)
    {
        octave[2 * k] = uint8_t(kPitchOctave[k]);
        octave[2 * k + 1] = uint8_t(kPitchOctave[k] >> 8);
    }
    const uint8_t* data = rom.Ptr(kRomBase);
    const uint8_t* end = data + rom.Size();
    for (const uint8_t* at = data; (at = std::search(at, end, octave, octave + sizeof octave)) != end; at++)
    {
        const uint32_t address = kRomBase + uint32_t(at - data);
        if (address % 2 || address < kRomBase + 2 * kPitchOctaveKey)
        {
            continue;
        }
        const uint32_t table = address - 2 * kPitchOctaveKey;
        if (rom.U16(table) == 8 && rom.U16(table + 2 * 127) == 12544)
        {
            return table;
        }
    }
    return 0;
}

} // namespace

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    info.play_routine = FindFirstThumb(rom, kPlayRoutine);
    if (!info.play_routine)
    {
        return false;
    }
    info.player_table = ThumbLiteral(rom, info.play_routine + kPlayPlayerTable);
    info.song_table = ThumbLiteral(rom, info.play_routine + kPlaySongTable);
    info.start_routine = BlTarget(rom, info.play_routine + kPlayCall);
    info.frame_routine = FindFirstThumb(rom, kFrameRoutine);
    info.init_routine = FindFirstThumb(rom, kInitRoutine);
    info.echo_routine = FindFirstThumb(rom, kEchoRoutine);
    info.lfo_routine = FindFirstThumb(rom, kLfoRoutine);
    if (!info.lfo_routine)
    {
        info.lfo_routine = FindFirstThumb(rom, kGroupLfoRoutine);
        info.group_levels = info.lfo_routine != 0;
    }
    if (info.lfo_routine)
    {
        info.sine_table = ThumbLiteral(rom, info.lfo_routine + kLfoSine);
    }
    // MP2K's song start has the same code as the play routine. The per-frame routine and the echo routine show that
    // the game has this driver.
    if (!info.frame_routine || !info.echo_routine)
    {
        return false;
    }
    if (!info.start_routine || !info.init_routine || !rom.Contains(info.sine_table, 512))
    {
        error = "found the play routine of Nintendo R&D1's sound driver at " + Hex(info.play_routine) +
                ", but not its start, init or LFO routine";
        return false;
    }

    // The PSG's tables and the pitch table.
    const uint32_t noise_lookup = FindFirstThumb(rom, kNoiseLookup);
    const uint32_t wave_lookup = FindFirstThumb(rom, kWaveLevelLookup);
    info.noise_table = noise_lookup ? ThumbLiteral(rom, noise_lookup) : 0;
    info.wave_levels = wave_lookup ? ThumbLiteral(rom, wave_lookup) : 0;
    info.pitch_table = FindPitchTable(rom);
    if (!rom.Contains(info.noise_table, 0x50 - 0x15 + 1) || !rom.Contains(info.wave_levels, 8) || !info.pitch_table)
    {
        error = "found Nintendo R&D1's sound driver at " + Hex(info.play_routine) +
                ", but not its pitch table or its noise or wave tables";
        return false;
    }

    // The bank table: the start routine's lookup, within its first 0x80 bytes.
    for (uint32_t at = info.start_routine; at < info.start_routine + 0x80 && !info.bank_table; at += 2)
    {
        if (MatchesAt(rom, at, kBankLookup))
        {
            info.bank_table = ThumbLiteral(rom, at);
        }
    }
    // Rhythm Tengoku's table has empty entries. The table ends before the first entry that's neither empty nor a bank.
    for (int b = 0; info.bank_table && rom.Contains(info.bank_table + 4 * uint32_t(b), 4); b++)
    {
        const uint32_t bank = rom.U32(info.bank_table + 4 * uint32_t(b));
        if (bank && !IsBank(rom, bank))
        {
            break;
        }
        if (bank)
        {
            info.bank_count = b + 1;
        }
    }
    if (!info.bank_count)
    {
        error = "found Nintendo R&D1's sound driver, but not its instrument banks";
        return false;
    }

    // The mixer's rate and the players, from the init.
    info.mix_rate = int(ThumbLiteral(rom, info.init_routine + kInitRate));
    uint32_t players = 0, players_end = 0, voices = 0;
    for (uint32_t at = info.init_routine; at < info.init_routine + 0x100; at += 2)
    {
        if (!players && MatchesAt(rom, at, kInitPlayers))
        {
            players = at;
        }
        if (!players_end && MatchesAt(rom, at, kInitPlayersEnd))
        {
            players_end = at;
        }
        if (!voices && MatchesAt(rom, at, kInitVoices))
        {
            voices = at;
        }
    }
    if (!players || !players_end || !voices || info.mix_rate < 4000 || info.mix_rate > 65536)
    {
        error = "found Nintendo R&D1's sound driver, but not how its init sets the players and the mixer up";
        return false;
    }
    info.player_setup = ThumbLiteral(rom, players + 2);
    info.voices = rom.U8(voices + 2);
    const int player_count = rom.U8(players_end + 6) + 1;
    if (player_count > kMaxPlayers || info.voices < 1 || info.voices > kMaxVoices)
    {
        error = "Nintendo R&D1's sound driver has more players or voices than agbsf2 supports";
        return false;
    }
    for (int p = 0; p < player_count; p++)
    {
        const uint32_t setup = info.player_setup + kSetupSize * uint32_t(p);
        const uint16_t head = rom.U16(setup);
        PlayerSetup s;
        s.channels = (head >> 5) & 0x1F;
        s.effects = (head >> 10) & 1;
        info.players.push_back(s);
    }

    if (!FindEchoCall(rom, info))
    {
        info.echo = {0x23, 2, 2, 4};
        info.warnings.push_back("didn't find the game's call that sets the echo; took the settings of WarioWare, Inc.");
    }

    // The song table runs until an entry that's neither empty nor a record.
    for (;;)
    {
        const uint32_t entry = info.song_table + kEntrySize * uint32_t(info.song_count);
        if (!rom.Contains(entry, kEntrySize))
        {
            break;
        }
        const uint32_t record = rom.U32(entry);
        if (record != 0 && !IsRecord(rom, record))
        {
            break;
        }
        info.song_count++;
    }
    int songs = 0;
    for (int s = 0; s < info.song_count; s++)
    {
        songs += rom.U32(info.song_table + kEntrySize * uint32_t(s)) != 0;
    }
    if (!songs)
    {
        error = "the song table of Nintendo R&D1's sound driver at " + Hex(info.song_table) + " has no songs";
        return false;
    }

    info.log.push_back("play routine " + Hex(info.play_routine) + ", start routine " + Hex(info.start_routine) +
                       ", per-frame routine " + Hex(info.frame_routine));
    info.log.push_back("song table " + Hex(info.song_table) + ": " + std::to_string(info.song_count) + " entries, " +
                       std::to_string(songs) + " songs");
    info.log.push_back("player table " + Hex(info.player_table) + ", " + std::to_string(player_count) +
                       " players set up from " + Hex(info.player_setup));
    info.log.push_back("bank table " + Hex(info.bank_table) + ": " + std::to_string(info.bank_count) + " banks");
    info.log.push_back("mixer: " + std::to_string(info.mix_rate) + " Hz, " + std::to_string(info.voices) +
                       " sample voices");
    info.log.push_back("pitch table " + Hex(info.pitch_table) + ", sine table " + Hex(info.sine_table) +
                       ", noise table " + Hex(info.noise_table) + ", wave levels " + Hex(info.wave_levels));
    info.log.push_back("echo settings " + std::to_string(info.echo[0]) + ", " + std::to_string(info.echo[1]) + ", " +
                       std::to_string(info.echo[2]) + ", " + std::to_string(info.echo[3]) +
                       (info.echo_call ? " (the game's call at " + Hex(info.echo_call) + ")" : std::string()));
    return true;
}

bool ReadSongRecord(const Rom& rom, const DriverInfo& info, int song, SongRecord& record)
{
    if (song < 0 || song >= info.song_count)
    {
        return false;
    }
    const uint32_t entry = info.song_table + kEntrySize * uint32_t(song);
    const uint32_t at = rom.U32(entry);
    if (!at || !IsRecord(rom, at))
    {
        return false;
    }
    record.address = at;
    record.midi = rom.U32(at);
    record.player = rom.U16(entry + 4);
    const uint16_t settings = rom.U16(at + 4);
    record.bank = (settings >> 5) & 0x3FF;
    record.volume = int((rom.U32(at + 4) >> 15) & 0x7F);
    record.priority = rom.U16(at + 6) >> 6;
    record.name.clear();
    const uint32_t name = rom.U32(at + 12);
    for (uint32_t c = name; rom.Contains(c, 1) && rom.U8(c) && c < name + 64; c++)
    {
        record.name += char(rom.U8(c));
    }
    return record.player < int(info.players.size()) && record.bank < info.bank_count;
}

} // namespace agbsf2::rd1
