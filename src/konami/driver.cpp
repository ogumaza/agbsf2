// SPDX-License-Identifier: MIT

#include "konami/driver.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <utility>

#include "konami/seqformat.h"

namespace supergbamidi::konami
{
namespace
{

// Limits on the commands and table entries that a scan reads, so unrelated ROM data can't keep detection busy.
constexpr uint32_t kMaxScanCommands = 60000;
constexpr int kMaxSongs = 1024;

// A Thumb instruction to look for: its bits under `mask` have to equal `value`.
struct Pattern
{
    uint16_t value;
    uint16_t mask;
};

// Returns a pattern that matches one Thumb instruction exactly.
constexpr Pattern Exact(uint16_t v)
{
    return {v, 0xFFFF};
}

// Returns a pattern that matches an instruction by its high byte, leaving the immediate as a wildcard (an ldr literal
// offset or a branch).
constexpr Pattern HiByte(uint16_t v)
{
    return {v, 0xFF00};
}

// Finds every halfword-aligned match of `pat` in the ROM (Thumb code).
std::vector<uint32_t> FindThumb(const Rom& rom, const std::vector<Pattern>& pat)
{
    std::vector<uint32_t> hits;
    const size_t n = pat.size();
    if (rom.Size() < 2 * n)
    {
        return hits;
    }

    const uint8_t* d = rom.Ptr(kRomBase);
    const size_t last = rom.Size() - 2 * n;
    for (size_t i = 0; i <= last; i += 2)
    {
        size_t k = 0;
        for (; k < n; k++)
        {
            const uint16_t h = uint16_t(d[i + 2 * k] | (d[i + 2 * k + 1] << 8));
            if ((h & pat[k].mask) != pat[k].value)
            {
                break;
            }
        }
        if (k == n)
        {
            hits.push_back(kRomBase + uint32_t(i));
        }
    }

    return hits;
}

// Returns the address of the literal that the Thumb "ldr rX, [pc, #imm]" at `addr` loads.
uint32_t LiteralAddress(const Rom& rom, uint32_t addr)
{
    return ((addr + 4) & ~3u) + (uint32_t(rom.U16(addr) & 0xFF) << 2);
}

// Returns the value loaded by the Thumb "ldr rX, [pc, #imm]" at `addr`.
uint32_t ThumbLiteral(const Rom& rom, uint32_t addr)
{
    return rom.U32(LiteralAddress(rom, addr));
}

// Returns the literal loaded by the ldr at index `ldr_index` of the first match of `pat` whose literal is a ROM
// address, or 0 if there's none.
uint32_t FindLiteral(const Rom& rom, const std::vector<Pattern>& pat, int ldr_index)
{
    for (uint32_t hit : FindThumb(rom, pat))
    {
        const uint32_t v = ThumbLiteral(rom, hit + 2 * ldr_index);
        if (rom.Contains(v))
        {
            return v;
        }
    }

    return 0;
}

// Returns true if a song table entry points into the ROM: the song's base address and the start of each of its tracks.
bool HeaderInRom(const Rom& rom, const SongHeader& h)
{
    if (!rom.Contains(h.base))
    {
        return false;
    }

    for (uint16_t o : h.offsets)
    {
        if (!rom.Contains(h.base + o))
        {
            return false;
        }
    }

    return true;
}

// Returns the number of the song's tracks that parse cleanly to an end command, or -1 if the header doesn't point into
// the ROM.
int ScoreSong(const Rom& rom, const SongHeader& h, Revision revision)
{
    if (!HeaderInRom(rom, h))
    {
        return -1;
    }

    int good = 0;
    for (int t = 0; t < h.tracks; t++)
    {
        if (WalkTrack(rom, h, t, revision, [](const Command&, uint32_t) {}, kMaxScanCommands))
        {
            good++;
        }
    }

    return good;
}

// Returns true for an empty (all-zero) song table entry.
bool IsEmptyEntry(const SongHeader& h)
{
    if (h.base != 0)
    {
        return false;
    }

    for (uint16_t o : h.offsets)
    {
        if (o)
        {
            return false;
        }
    }

    return true;
}

// The size of a candidate song table, as CountSongs() finds it.
struct SongCount
{
    int entries = 0;     // table entries, including empty ones between songs
    int valid = 0;       // entries that hold a song
    int unreadable = -1; // the entry that ended the count, a song in the ROM whose tracks don't parse; -1 if none did
};

// Counts the entries of a candidate song table. An entry holds a song if at least half of its tracks parse cleanly to
// an end. Song data follows the table, so the table ends where the lowest song data begins.
SongCount CountSongs(const Rom& rom, uint32_t table, Revision revision)
{
    const uint32_t entry_size = SongEntrySize(revision);
    uint32_t data_start = 0xFFFFFFFF;
    int count = 0, valid = 0, unreadable = -1;
    for (int i = 0; i < kMaxSongs; i++)
    {
        const uint32_t e = table + uint32_t(i) * entry_size;
        if (uint64_t(e) + entry_size > data_start)
        {
            break;
        }

        SongHeader h;
        if (!ReadSongHeader(rom, table, i, revision, h))
        {
            break;
        }
        if (IsEmptyEntry(h))
        {
            count = i + 1;
            continue;
        }

        // A header that points into the ROM, before any song's data, is most likely a song whose tracks use commands
        // supergbamidi doesn't know.
        const int score = ScoreSong(rom, h, revision);
        if (score < h.tracks / 2)
        {
            unreadable = score >= 0 ? i : -1;
            break;
        }

        data_start = std::min(data_start, h.base);
        count = i + 1;
        valid++;
    }

    // Trailing empty entries aren't songs.
    while (count > 0)
    {
        SongHeader h;
        ReadSongHeader(rom, table, count - 1, revision, h);
        if (!IsEmptyEntry(h))
        {
            break;
        }

        count--;
    }

    return {count, valid, unreadable};
}

// Returns true if any song of the table plays notes.
bool HasNotes(const Rom& rom, uint32_t table, int count, Revision revision)
{
    for (int s = 0; s < count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        for (int t = 0; t < h.tracks; t++)
        {
            bool found = false;
            auto check_for_note = [&](const Command& c, uint32_t)
            {
                found |= c.op == Op::kNote || (c.op == Op::kPsgNote && (c.vol & 15));
            };
            WalkTrack(rom, h, t, revision, check_for_note, kMaxScanCommands);
            if (found)
            {
                return true;
            }
        }
    }

    return false;
}

// Checks the layout of the first `count` song table entries. Songs must follow the table, with only additional table
// entries allowed between them. Tracks must cover each song without gaps, starting at offset 0; shared track data is
// allowed. Songs must be contiguous and contain notes. Almost every byte is a valid command, so parsing alone cannot
// distinguish song data from unrelated bytes.
bool PlausibleLayout(const Rom& rom, uint32_t table, int count, Revision revision)
{
    const uint32_t entry_size = SongEntrySize(revision);
    std::vector<std::pair<uint32_t, uint32_t>> songs; // (base, end of its data)
    for (int s = 0; s < count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        std::map<uint16_t, uint32_t> region_end; // track offset -> end of the data starting there
        for (int t = 0; t < h.tracks; t++)
        {
            uint32_t end = 0;
            auto extend_end = [&](const Command& c, uint32_t)
            {
                end = std::max(end, c.addr + c.length);
            };
            if (!WalkTrack(rom, h, t, revision, extend_end, kMaxScanCommands))
            {
                return false;
            }

            uint32_t& e = region_end[h.offsets[t]];
            e = std::max(e, end);
        }

        if (region_end.begin()->first != 0)
        {
            return false;
        }

        for (auto it = region_end.begin(); std::next(it) != region_end.end(); ++it)
        {
            if (it->second != h.base + std::next(it)->first)
            {
                return false;
            }
        }

        songs.emplace_back(h.base, region_end.rbegin()->second);
    }

    if (songs.empty() || !HasNotes(rom, table, count, revision))
    {
        return false;
    }

    std::sort(songs.begin(), songs.end());
    const uint32_t gap = songs.front().first - table;
    if (songs.front().first < table || gap % entry_size || gap / entry_size < uint32_t(count))
    {
        return false;
    }

    // Between the entries counted and the songs, there can only be more of the table: empty entries, or ones that point
    // into the ROM, such as songs whose tracks don't parse. A table that starts partway through the real one has song
    // data there instead.
    for (int i = count; i < int(gap / entry_size); i++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, table, i, revision, h) || !(IsEmptyEntry(h) || HeaderInRom(rom, h)))
        {
            return false;
        }
    }

    for (size_t i = 0; i < songs.size(); i++)
    {
        size_t next = i + 1;
        while (next < songs.size() && songs[next].first == songs[i].first)
        {
            next++;
        }
        if (next < songs.size() && songs[i].second != songs[next].first)
        {
            return false;
        }
    }

    return true;
}

// Returns true if `p` could point at a sample header: a sane step, data that fits in the ROM, and a loop start inside
// the sample or -1.
bool PlausibleSampleHeader(const Rom& rom, uint32_t p)
{
    if (!rom.Contains(p, 12))
    {
        return false;
    }

    const uint32_t step = rom.U32(p);
    const int32_t len = rom.S32(p + 4);
    const int32_t loop = rom.S32(p + 8);

    if (step < 0x40 || step > 0x40000)
    {
        return false;
    }
    if (len <= 0 || len > 0x1000000)
    {
        return false;
    }
    if (!rom.Contains(p + 12, uint32_t(len)))
    {
        return false;
    }
    if (loop != -1 && (loop < 0 || loop >= len))
    {
        return false;
    }

    return true;
}

// Returns true if `entry` could be an entry of the Dungeon Dice Monsters revision's sample table: a pointer to the PCM,
// and a word with the length in blocks of 16 samples in bits 20-31 and the sample's timer period in bits 0-15, for a
// rate of 1 to 65 kHz.
bool PlausibleDungeonDiceSample(const Rom& rom, uint32_t entry)
{
    if (!rom.Contains(entry, 8))
    {
        return false;
    }

    const uint32_t word = rom.U32(entry + 4);
    const uint32_t length = (word >> 20) * 16;
    const uint32_t period = word & 0xFFFF;
    return length > 0 && period >= 0x100 && period <= 0x4000 && rom.Contains(rom.U32(entry), length);
}

// Returns true if entry `index` of a sample table of revision `revision` at `table` is a sample supergbamidi can read.
bool ReadableSample(const Rom& rom, uint32_t table, int index, Revision revision)
{
    if (revision == Revision::kDungeonDiceMonsters)
    {
        return PlausibleDungeonDiceSample(rom, table + 8 * uint32_t(index));
    }

    return PlausibleSampleHeader(rom, rom.U32(table + 4 * uint32_t(index)));
}

// Returns true if `used` is nonempty and every referenced sample has a plausible header at `table`.
bool SampleTableFits(const Rom& rom, uint32_t table, const std::vector<int>& used)
{
    for (int idx : used)
    {
        if (!ReadableSample(rom, table, idx, Revision::kUltimateMasters))
        {
            return false;
        }
    }

    return !used.empty();
}

// Returns the samples the songs play, the most played first. In the Dungeon Dice Monsters revision, a note gives an
// entry of the sample map at `sample_map`.
std::vector<int> UsedSamples(const Rom& rom, uint32_t song_table, int song_count, Revision revision,
                             uint32_t sample_map)
{
    std::map<int, int> freq;
    auto count_note = [&](const Command& c, uint32_t)
    {
        if (c.op != Op::kNote && c.op != Op::kNextNote)
        {
            return;
        }

        if (revision != Revision::kDungeonDiceMonsters)
        {
            freq[c.sample]++;
        }
        else if (rom.Contains(sample_map + uint32_t(c.sample)))
        {
            freq[rom.U8(sample_map + uint32_t(c.sample))]++;
        }
    };

    for (int s = 0; s < song_count; s++)
    {
        SongHeader h;
        if (!ReadSongHeader(rom, song_table, s, revision, h) || IsEmptyEntry(h))
        {
            continue;
        }

        for (int t = kPsgTracks; t < h.tracks; t++)
        {
            WalkTrack(rom, h, t, revision, count_note);
        }
    }

    std::vector<int> used;
    for (const auto& [index, count] : freq)
    {
        used.push_back(index);
    }

    // Check the most frequently used samples first to reject invalid candidates sooner.
    std::sort(used.begin(), used.end(), [&](int a, int b) { return freq.at(a) > freq.at(b); });

    return used;
}

// Returns the song tables the play-song routine could load: its entry is table + song * 36 (song * 9 * 4), or song * 28
// (song * 7 * 4) in the Rave Master revision, song * 24 (song * 3 * 8) in the Eternal Duelist revision, or song * 20
// (song * 5 * 4) in the Dungeon Dice Monsters revision. Sets `exact` if they come from the routine's code, rather than
// from code that only works out an address the same way.
std::set<uint32_t> SongTablesFromCode(const Rom& rom, Revision revision, bool& exact)
{
    // Ultimate Masters: lsls r0,r4,#3 / adds r0,r0,r4 / lsls r0,r0,#2 / ldr r1,=table / adds r2,r0,r1 / ldrh r1,[r2] /
    // ldrh r0,[r2,#2] / lsls r0,r0,#16 / orrs r1,r0. WCT 2004 has the song number in r5 and the entry's address in r3,
    // and Rave Master subtracts the song number where the others add it.
    std::vector<Pattern> code = {Exact(0x00E0), Exact(0x1900), Exact(0x0080), HiByte(0x4900), Exact(0x1842),
                                 Exact(0x8811), Exact(0x8850), Exact(0x0400), Exact(0x4301)};
    if (revision == Revision::kWct2004)
    {
        code = {Exact(0x00E8), Exact(0x1940), Exact(0x0080), HiByte(0x4900), Exact(0x1843),
                Exact(0x8819), Exact(0x8858), Exact(0x0400), Exact(0x4301)};
    }
    else if (revision == Revision::kRaveMaster)
    {
        code[1] = Exact(0x1B00);
    }
    else if (revision == Revision::kEternalDuelist)
    {
        code[0] = Exact(0x0060);
        code[2] = Exact(0x00C0);
    }
    else if (revision == Revision::kDungeonDiceMonsters)
    {
        // lsls r0,r4,#2 / adds r0,r0,r4 / lsls r0,r0,#2 / ldr r1,=table / adds r1,r0,r1 / ldrh r4,[r1,#2] /
        // lsls r0,r4,#16 / ldrh r5,[r1] / orrs r0,r5
        code = {Exact(0x00A0), Exact(0x1900), Exact(0x0080), HiByte(0x4900), Exact(0x1841),
                Exact(0x884C), Exact(0x0420), Exact(0x880D), Exact(0x4328)};
    }

    std::set<uint32_t> tables;
    for (uint32_t hit : FindThumb(rom, code))
    {
        tables.insert(ThumbLiteral(rom, hit + 6));
    }

    exact = !tables.empty();
    if (exact)
    {
        return tables;
    }

    // The same computation with any registers: a shift, an add (a subtract in Rave Master) and another shift.
    const bool subtract = revision == Revision::kRaveMaster;
    const int first_shift = revision == Revision::kEternalDuelist        ? 1
                            : revision == Revision::kDungeonDiceMonsters ? 2
                                                                         : 3;
    const int second_shift = revision == Revision::kEternalDuelist ? 3 : 2;
    const uint8_t* d = rom.Ptr(kRomBase);
    for (size_t i = 0; i + 10 <= rom.Size(); i += 2)
    {
        auto h = [&](int k)
        {
            return uint16_t(d[i + 2 * k] | (d[i + 2 * k + 1] << 8));
        };

        const uint16_t a = h(0), b = h(1), c = h(2), l = h(3);
        if ((a & 0xFFC0) != uint16_t(first_shift << 6)) // lsls rd, rm, #first_shift
        {
            continue;
        }

        // adds rd, rd, rm (either way round), or subs rd, rd, rm
        const int rd = a & 7, rm = (a >> 3) & 7;
        const int bd = b & 7, bn = (b >> 3) & 7, bm = (b >> 6) & 7;
        const bool adds = (b & 0xFE00) == 0x1800 && ((bn == rd && bm == rm) || (bn == rm && bm == rd));
        const bool subs = (b & 0xFE00) == 0x1A00 && bn == rd && bm == rm;
        if (bd != rd || !(subtract ? subs : adds))
        {
            continue;
        }
        if (c != uint16_t((second_shift << 6) | (rd << 3) | rd)) // lsls rd, rd, #second_shift
        {
            continue;
        }
        if ((l & 0xF800) != 0x4800) // ldr rt, =table
        {
            continue;
        }

        tables.insert(ThumbLiteral(rom, kRomBase + uint32_t(i) + 6));
    }

    return tables;
}

// Scans the data for a song table: one whose first songs parse, laid out like the driver's song data. Returns the first
// such table with two or more songs, or failing that the first with one, or 0 if there's none.
uint32_t ScanForSongTable(const Rom& rom, Revision revision)
{
    const uint32_t entry_size = SongEntrySize(revision);
    uint32_t best = 0;
    int best_valid = 0;
    for (uint32_t t = kRomBase; t + 2 * entry_size <= rom.End() && best_valid < 2; t += 4)
    {
        const uint32_t base = rom.U32(t);
        if (!rom.Contains(base) || base <= t || (base - t) % entry_size)
        {
            continue;
        }

        SongHeader h0, h1;
        ReadSongHeader(rom, t, 0, revision, h0);
        ReadSongHeader(rom, t, 1, revision, h1);
        if (ScoreSong(rom, h0, revision) < h0.tracks)
        {
            continue;
        }
        if (!IsEmptyEntry(h1) && ScoreSong(rom, h1, revision) < h1.tracks)
        {
            continue;
        }

        const SongCount songs = CountSongs(rom, t, revision);
        if (songs.valid > best_valid && PlausibleLayout(rom, t, songs.entries, revision))
        {
            best = t;
            best_valid = songs.valid;
        }
    }

    return best;
}

// Finds the song table from the play-song routine's code, or failing that in the data. Returns 0 and sets `error` if
// the code loads a table without songs, or returns 0 with `error` empty if there's no sign of a song table.
uint32_t FindSongTable(const Rom& rom, Revision revision, std::vector<std::string>& log, std::string& error)
{
    // Choose the candidate table with the most songs. The play-song routine's code gives the table's address, so its
    // table only needs one song that parses. Tables found through other code must also pass the data scan's layout
    // checks. Other revisions compute the same address, and some of their jingles can parse despite the different
    // command sets.
    bool exact = false;
    const std::set<uint32_t> candidates = SongTablesFromCode(rom, revision, exact);
    uint32_t loaded = 0; // the first of them in the ROM
    uint32_t best = 0;
    int best_valid = 0;
    for (uint32_t t : candidates)
    {
        if (!rom.Contains(t))
        {
            continue;
        }

        loaded = loaded ? loaded : t;
        const SongCount songs = CountSongs(rom, t, revision);
        if (songs.valid > best_valid && (exact || PlausibleLayout(rom, t, songs.entries, revision)))
        {
            best = t;
            best_valid = songs.valid;
        }
    }
    if (best)
    {
        log.push_back("song table " + Hex(best) + " (from play-song code)");
        return best;
    }

    // The play-song routine's code gives the table's address, so a table found in the data instead would be the wrong
    // one.
    if (exact && loaded)
    {
        error = "found the play-song code, but the song table it loads, at " + Hex(loaded) +
                ", holds no songs supergbamidi can read (--song-table and --song-count override detection)";
        return 0;
    }

    const char* why = candidates.empty() ? "play-song code not recognised"
                      : exact            ? "the song table that the play-song code loads isn't in the ROM"
                                         : "found code like the play-song routine, but no table it loads is laid out "
                                           "like the driver's song data";
    log.push_back(std::string(why) + ", scanning data for a song table");
    best = ScanForSongTable(rom, revision);
    if (best)
    {
        log.push_back("song table " + Hex(best) + " (from data scan)");
        return best;
    }

    // Nothing that holds the driver's songs: without the driver's command reader, the game has another driver.
    error.clear();

    return 0;
}

// Code that loads one of the driver's tables, and the index of its ldr of the table.
struct TableCode
{
    std::vector<Pattern> code;
    int ldr = 0;
};

// The code that loads each of a revision's tables, apart from the song table.
struct RevisionCode
{
    TableCode note_start; // code in the note start that loads the sample table
    TableCode volume;     // code that loads the volume table
    TableCode psg_freq;   // code that loads the PSG frequency table
    TableCode noise;      // code that loads the noise table
};

// Returns the code that loads revision `r`'s tables.
RevisionCode CodeFor(Revision r)
{
    RevisionCode c;
    if (r == Revision::kUltimateMasters)
    {
        // Note start: ldr r3,=0xFFF / ands r3,r7 / movs r0,#0xA0 / lsls r0,#8 / ands r0,r7 / cmp r0,#0 / bne /
        // ldr r1,=sample_table. DirectSound output: ldrb r2,[r2,#3] / lsls r1,r2,#6 / ldr r0,=volume_table /
        // adds r1,r1,r0. Square 1 output: ldr r0,=freq_table / movs r7,#0 / ldrsh r1,[r5,r7] / lsls r1,#1 / adds /
        // ldrh. Noise output: ldr r1,=noise_table / movs r4,#0x24 / ldrsh r0,[r3,r4] / lsls r0,#1 / adds / ldrh.
        c.note_start = {{HiByte(0x4B00), Exact(0x403B), Exact(0x20A0), Exact(0x0200), Exact(0x4038), Exact(0x2800),
                         HiByte(0xD100), HiByte(0x4900)},
                        7};
        c.volume = {{Exact(0x78D2), Exact(0x0191), HiByte(0x4800), Exact(0x1809)}, 2};
        c.psg_freq = {{HiByte(0x4800), Exact(0x2700), Exact(0x5FE9), Exact(0x0049), Exact(0x1809), Exact(0x880D)}, 0};
        c.noise = {{HiByte(0x4900), Exact(0x2424), Exact(0x5F18), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};
        return c;
    }

    // Dungeon Dice Monsters. Voice start: lsls r1,r2,#3 / ldr r0,=sample_table / adds r0,r1,r0 / ldr r5,[r0] /
    // ldr r1,[r0,#4]. Square 1 output: ldr r1,=freq_table / movs r4,#0 / ldrsh r0,[r6,r4] / lsls r0,#1 / adds / ldrh.
    // The frequency table holds its noise settings too, and it has no volume table.
    if (r == Revision::kDungeonDiceMonsters)
    {
        c.note_start = {{Exact(0x00D1), HiByte(0x4800), Exact(0x1808), Exact(0x6805), Exact(0x6841)}, 1};
        c.psg_freq = {{HiByte(0x4900), Exact(0x2400), Exact(0x5F30), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};
        return c;
    }

    // Eternal Duelist. Note start: ldr r1,=sample_table / lsls r0,r4,#2 / adds r0,r0,r1 / ldr r2,[r0] / lsls r0,r3,#1 /
    // ldr r1,=pitch_table / adds / ldrh r1,[r0] / ldr r0,[r2] / muls r0,r1. Square 1 output: ldr r0,=freq_table /
    // movs r3,#0 / ldrsh r1,[r7,r3] / lsls r1,#1 / adds / ldrh r5,[r1]. Noise output: ldr r1,=noise_table /
    // movs r3,#0x18 / ldrsh r0,[r7,r3] / lsls r0,#1 / adds / ldrh. Its voices have no pan, and it has no volume table.
    if (r == Revision::kEternalDuelist)
    {
        c.note_start = {{HiByte(0x4900), Exact(0x00A0), Exact(0x1840), Exact(0x6802), Exact(0x0058), HiByte(0x4900),
                         Exact(0x1840), Exact(0x8801), Exact(0x6810), Exact(0x4348)},
                        0};
        c.psg_freq = {{HiByte(0x4800), Exact(0x2300), Exact(0x5EF9), Exact(0x0049), Exact(0x1809), Exact(0x880D)}, 0};
        c.noise = {{HiByte(0x4900), Exact(0x2318), Exact(0x5EF8), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};
        return c;
    }

    // WCT 2004. Note start: ldr r1,=sample_table / ldr r0,=0xFFF / ands r0,r6 / lsls r0,#2 / adds r0,r0,r1 /
    // ldr r5,[r0]. DirectSound and PSG output: ldrb r0,[r7,#3] / lsls r0,#4 / ldr r1,=volume_table / adds r1,r0,r1 /
    // movs r0,#0xF / ands r0,r4. Square 1 output: ldr r0,=freq_table / movs r2,#0 / ldrsh r1,[r7,r2] / lsls r1,#1 /
    // adds / ldrh r4,[r1]. Noise output: as in Ultimate Masters, with ldrsh r0,[r7,r4]. The Rave Master revision has
    // other registers in some of them.
    const bool rave_master = r == Revision::kRaveMaster;
    c.note_start = {{HiByte(0x4900), HiByte(0x4800), Exact(rave_master ? 0x4028 : 0x4030), Exact(0x0080), Exact(0x1840),
                     Exact(rave_master ? 0x6802 : 0x6805)},
                    0};
    c.volume = {{Exact(0x78F8), Exact(0x0100), HiByte(0x4900), Exact(0x1841), Exact(0x200F),
                 Exact(rave_master ? 0x4028 : 0x4020)},
                2};
    c.psg_freq = {{HiByte(0x4800), Exact(0x2200), Exact(0x5EB9), Exact(0x0049), Exact(0x1809),
                   Exact(rave_master ? 0x880D : 0x880C)},
                  0};
    c.noise = {{HiByte(0x4900), Exact(0x2424), Exact(0x5F38), Exact(0x0040), Exact(0x1840), Exact(0x8800)}, 0};

    return c;
}

// Scans for a table containing the samples in `used`, which must be nonempty. Returns 0 if none is found. Adds a
// warning if the table could only be identified by a guess.
uint32_t ScanForSampleTable(const Rom& rom, const std::vector<int>& used, std::vector<std::string>& log,
                            std::vector<std::string>& warnings)
{
    // The sample headers follow the table directly, so entry 0 points just past the last entry, and the entries
    // increase. That pins down the start.
    const uint32_t max_used = uint32_t(*std::max_element(used.begin(), used.end()));
    for (uint32_t t = kRomBase; t + 4 <= rom.End(); t += 4)
    {
        const uint32_t first = rom.U32(t);
        if (first <= t || (first - t) % 4 || !rom.Contains(first))
        {
            continue;
        }

        const uint32_t n = (first - t) / 4;
        if (n <= max_used || n > 0x10000)
        {
            continue;
        }

        bool ok = true;
        for (uint32_t i = 0, prev = 0; i < n && ok; i++)
        {
            const uint32_t p = rom.U32(t + 4 * i);
            ok = p > prev && PlausibleSampleHeader(rom, p);
            prev = p;
        }
        if (ok)
        {
            log.push_back("sample table " + Hex(t) + " (from data scan)");
            return t;
        }
    }

    // Rips zero the entries of unused samples, which breaks the pattern above: accept the first table that serves every
    // sample the songs use.
    for (uint32_t t = kRomBase; t + 4 <= rom.End(); t += 4)
    {
        if (SampleTableFits(rom, t, used))
        {
            warnings.push_back("sample table " + Hex(t) + " guessed from the data; check the result");
            return t;
        }
    }

    return 0;
}

// Finds the sample table of revision `revision` from the note-start routine's code, or failing that in the data, which
// has to be in the newer revisions' layout. Returns 0 if no song plays a sample, and 0 with `error` set if there's no
// table it can use. Adds the samples it can't read, or a table that's only a guess, to `warnings`.
uint32_t FindSampleTable(const Rom& rom, const std::vector<int>& used, const TableCode& note_start, Revision revision,
                         std::vector<std::string>& log, std::vector<std::string>& warnings, std::string& error)
{
    if (used.empty())
    {
        log.push_back("no song plays a sample, so no sample table is needed");
        return 0;
    }

    // Of the tables the code loads, take the one with the fewest samples supergbamidi can't read.
    const uint32_t ldr_offset = 2 * uint32_t(note_start.ldr);
    const std::vector<uint32_t> hits = FindThumb(rom, note_start.code);
    uint32_t best = 0;
    std::vector<int> unreadable; // samples used by the songs that have no readable header in `best`
    for (uint32_t hit : hits)
    {
        const uint32_t t = ThumbLiteral(rom, hit + ldr_offset);
        if (!rom.Contains(t))
        {
            continue;
        }

        std::vector<int> missing;
        for (int idx : used)
        {
            if (!ReadableSample(rom, t, idx, revision))
            {
                missing.push_back(idx);
            }
        }
        if (!best || missing.size() < unreadable.size())
        {
            best = t;
            unreadable = missing;
        }
    }

    // The code gives the table's address, so a table found in the data instead would be the wrong one. The table only
    // has to have a header supergbamidi can read for at least half of the samples. The others may be in a mode
    // supergbamidi doesn't know, such as one with a negative length.
    if (best)
    {
        if (unreadable.size() * 2 > used.size())
        {
            error =
                "found the note-start code, but supergbamidi can't read most of the songs' samples in the sample "
                "table it loads, at " +
                Hex(best) + " (--sample-table overrides detection)";
            return 0;
        }

        log.push_back("sample table " + Hex(best) + " (from note-start code)");
        if (!unreadable.empty())
        {
            std::sort(unreadable.begin(), unreadable.end());
            std::string list;
            for (int idx : unreadable)
            {
                list += (list.empty() ? "" : ", ") + std::to_string(idx);
            }
            warnings.push_back("samples supergbamidi can't read, whose notes are skipped: " + list);
        }

        return best;
    }

    const char* why = hits.empty() ? "note-start code not recognised"
                                   : "the sample table that the note-start code loads isn't in the ROM";
    if (revision == Revision::kDungeonDiceMonsters)
    {
        error = std::string(why) +
                ", and supergbamidi can't look for this revision's sample table in the data (--sample-table overrides "
                "detection)";
        return 0;
    }

    log.push_back(std::string(why) + ", scanning data for the sample table");
    const uint32_t scanned = ScanForSampleTable(rom, used, log, warnings);
    if (!scanned)
    {
        error = "couldn't find the sample table (--sample-table overrides detection)";
    }

    return scanned;
}

// Returns the mixer's output rate in mixer mode 0, from the timer 0 setting in the driver's timer table, or 0 if that
// doesn't give a rate between 4000 and 65536 Hz.
double TimerRate(const Rom& rom, uint32_t timer_table)
{
    if (!timer_table)
    {
        return 0;
    }

    constexpr int kPrescale[4] = {1, 64, 256, 1024};
    const uint32_t v = rom.U32(timer_table);
    const double rate = kCpuHz / ((0x10000 - (v & 0xFFFF)) * kPrescale[(v >> 16) & 3]);
    return rate >= 4000 && rate <= 65536 ? rate : 0;
}

// Finds the driver's command reader, and works out the driver's revision from the opcodes it compares the command with:
// ldrb rX,[rY] / cmp rX,#0xFC / ble, then FF, and FD in the Ultimate Masters revision or FE in older ones. Of those,
// the WCT 2004 revision goes on with EF and FA, the Rave Master revision with EF and F5, the Eternal Duelist revision
// with EF and F3, and the Dungeon Dice Monsters revision with EF and FC. Sets `reader` to the address of the reader, or
// to 0 if there's none. Returns false and sets `error` for another older revision, which supergbamidi can't read. A ROM
// without a command reader that this recognises is taken to have the Ultimate Masters revision.
bool FindRevision(const Rom& rom, Revision& revision, uint32_t& reader, std::vector<std::string>& log,
                  std::string& error)
{
    revision = Revision::kUltimateMasters;
    reader = 0;

    const uint8_t* d = rom.Ptr(kRomBase);
    const size_t count = rom.Size() / 2;
    auto h = [&](size_t i)
    {
        return uint16_t(d[2 * i] | (d[2 * i + 1] << 8));
    };

    for (size_t i = 0; i + 3 <= count; i++)
    {
        const uint16_t load = h(i);
        if ((load & 0xFFC0) != 0x7800)
        {
            continue;
        }

        const uint16_t cmp = uint16_t(0x2800 | ((load & 7) << 8));
        if (h(i + 1) != (cmp | 0xFC) || (h(i + 2) & 0xFF00) != 0xDD00)
        {
            continue;
        }

        // The first five opcodes that the next 200 instructions compare the register with. Comparisons with 0 check a
        // command's argument.
        std::vector<int> opcodes;
        for (size_t k = i + 1; k < std::min(count, i + 200) && opcodes.size() < 5; k++)
        {
            if ((h(k) & 0xFF00) == cmp && (h(k) & 0xFF) != 0)
            {
                opcodes.push_back(h(k) & 0xFF);
            }
        }
        if (opcodes.size() < 5 || opcodes[1] != 0xFF || (opcodes[2] != 0xFD && opcodes[2] != 0xFE))
        {
            continue;
        }

        reader = kRomBase + uint32_t(2 * i);
        if (opcodes[2] == 0xFE && (opcodes[3] != 0xEF || (opcodes[4] != 0xFA && opcodes[4] != 0xF5 &&
                                                          opcodes[4] != 0xF3 && opcodes[4] != 0xFC)))
        {
            error =
                "this game has an older revision of Konami's driver, whose commands supergbamidi can't read (its "
                "command reader is at " +
                Hex(reader) + ")";
            return false;
        }

        revision = opcodes[2] == 0xFD   ? Revision::kUltimateMasters
                   : opcodes[4] == 0xFA ? Revision::kWct2004
                   : opcodes[4] == 0xF5 ? Revision::kRaveMaster
                   : opcodes[4] == 0xF3 ? Revision::kEternalDuelist
                                        : Revision::kDungeonDiceMonsters;
        log.push_back(std::string(RevisionName(revision)) + " revision of the driver (from the command reader at " +
                      Hex(reader) + ")");
        return true;
    }

    log.push_back("command reader not recognised, assuming the Ultimate Masters revision of the driver");

    return true;
}

// Finds the Dungeon Dice Monsters revision's tables from the code that loads them. Returns false and sets `error` if
// the sample map or the sample period table, which its notes read, isn't there.
bool FindDungeonDiceTables(const Rom& rom, DriverInfo& info, std::string& error)
{
    // Sample note: ldr r0,=sample_map / ldrb r5,[r5,#1] / adds r0,r5,r0 / ldrb r0,[r0] / strb r0,[r4,#2]. Sample bend:
    // ldr r0,=period_table / lsls r1,r2,#1 / adds r1,r1,r0 / ldrh r0,[r1] / strh r0,[r7] / ldrh r0,[r1] / strh r0,[r3].
    info.sample_map = FindLiteral(rom, {HiByte(0x4800), Exact(0x786D), Exact(0x1828), Exact(0x7800), Exact(0x70A0)}, 0);
    info.sample_period_table = FindLiteral(
        rom, {HiByte(0x4800), Exact(0x0051), Exact(0x1809), Exact(0x8808), Exact(0x8038), Exact(0x8808), Exact(0x8018)},
        0);
    if (!info.sample_map || !info.sample_period_table)
    {
        error =
            "found the command reader of the Dungeon Dice Monsters revision, but not the sample map or the sample "
            "period table that its notes read";
        return false;
    }

    // Wave command: lsls r0,r3,#4 / ldr r1,=wave_table / adds r0,r0,r1 / str r0,[r2]. Wave output:
    // ldr r1,=volume_table / ldrb r2,[r6,#0x1b] / lsls r0,r2,#1 / adds r0,r0,r1 / ldrb r3,[r6,#0x1a] / ldrh r0,[r0] /
    // orrs r3,r0.
    info.wave_table = FindLiteral(rom, {Exact(0x0118), HiByte(0x4900), Exact(0x1840), Exact(0x6010)}, 1);
    info.wave_volume_table = FindLiteral(
        rom, {HiByte(0x4900), Exact(0x7EF2), Exact(0x0050), Exact(0x1840), Exact(0x7EB3), Exact(0x8800), Exact(0x4303)},
        0);

    // Vibrato: asrs r3,r3,#2 / ldr r0,=vibrato_part / ldr r1,=freq_table / subs r0,r0,r1. The vibrato's part of the
    // frequency table starts at the entry the difference gives.
    const std::vector<uint32_t> vibrato =
        FindThumb(rom, {Exact(0x109B), HiByte(0x4800), HiByte(0x4900), Exact(0x1A40)});
    if (vibrato.empty())
    {
        info.warnings.push_back("vibrato code not recognised, so vibrato plays at the wrong pitches");
    }
    else
    {
        const uint32_t part = ThumbLiteral(rom, vibrato.front() + 2);
        const uint32_t table = ThumbLiteral(rom, vibrato.front() + 4);
        info.vibrato_entry = int32_t(part - table) / 2;
    }

    return true;
}

} // namespace

SampleInfo DriverInfo::Sample(const Rom& rom, int index) const
{
    SampleInfo s;
    if (!sample_table || index < 0)
    {
        return s;
    }

    // The Dungeon Dice Monsters revision's samples have no loop.
    if (revision == Revision::kDungeonDiceMonsters)
    {
        const uint32_t entry = sample_table + 8 * uint32_t(index);
        if (PlausibleDungeonDiceSample(rom, entry))
        {
            const uint32_t word = rom.U32(entry + 4);
            s.valid = true;
            s.period = word & 0xFFFF;
            s.length = int32_t((word >> 20) * 16);
            s.data = rom.U32(entry);
        }

        return s;
    }

    const uint32_t p = rom.U32(sample_table + 4 * uint32_t(index));
    if (!PlausibleSampleHeader(rom, p))
    {
        return s;
    }

    s.valid = true;
    s.step = rom.U32(p);
    s.length = rom.S32(p + 4);
    s.loop_start = rom.S32(p + 8);
    s.data = p + 12;

    return s;
}

int DriverInfo::TableLevel(const Rom& rom, int volume, int pan) const
{
    // In the older revisions, a row of the volume table has 16 pan levels.
    const int pan_levels = revision == Revision::kUltimateMasters ? 64 : 16;
    volume = std::clamp(volume, 0, 255);
    pan = std::clamp(pan, 0, pan_levels - 1);

    if (volume_table)
    {
        const uint32_t a = volume_table + uint32_t(volume * pan_levels + pan);
        if (rom.Contains(a))
        {
            return rom.U8(a);
        }
    }

    return volume * pan / (pan_levels - 1);
}

int DriverInfo::VoiceLevel(const Rom& rom, int volume, int pan) const
{
    // The Eternal Duelist and Dungeon Dice Monsters revisions' voices have no pan. Eternal Duelist's mixer plays a
    // voice at (volume + 1)/16 on both sides, and not at all at volume 0. Dungeon Dice Monsters mixes two voices into
    // each FIFO at (volume + 1)/32 each, and plays both FIFOs on both sides.
    if (revision == Revision::kEternalDuelist || revision == Revision::kDungeonDiceMonsters)
    {
        volume = std::clamp(volume, 0, 15);
        return volume ? (volume + 1) * (revision == Revision::kEternalDuelist ? 8 : 4) : 0;
    }

    // The WCT 2004 revision's mixer plays a level of n at amplitude n/16, and the Rave Master revision's at n/15.
    const int level = TableLevel(rom, volume, pan);
    switch (revision)
    {
    case Revision::kWct2004:
        return level * 8;
    case Revision::kRaveMaster:
        return (level * 128 + 7) / 15;
    default:
        return level;
    }
}

uint16_t DriverInfo::NoiseSetting(const Rom& rom, int note) const
{
    // The driver reads whatever lies at the note's place, before the table for a negative note.
    if (noise_table)
    {
        const uint32_t a = noise_table + 2 * uint32_t(note);
        if (rom.Contains(a, 2))
        {
            return rom.U16(a);
        }
    }

    // Fallback matching Yu-Gi-Oh! Ultimate Masters Edition's table: shift = note + 1.
    return uint16_t(0x8000 | ((std::clamp(note, 0, 12) + 1) << 4));
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    auto& log = info.log;

    // Check that table address overrides fall within the ROM.
    for (const auto& [what, address] :
         {std::pair("song", overrides.song_table), std::pair("sample", overrides.sample_table)})
    {
        if (address && !rom.Contains(address))
        {
            error = std::string("the ") + what + " table address " + Hex(address) + " is outside the ROM (" +
                    Hex(kRomBase) + "-" + Hex(rom.End() - 1) + ")";
            return false;
        }
    }

    uint32_t reader = 0;
    if (!FindRevision(rom, info.revision, reader, log, error))
    {
        return false;
    }

    const bool dungeon_dice = info.revision == Revision::kDungeonDiceMonsters;
    if (dungeon_dice && !FindDungeonDiceTables(rom, info, error))
    {
        return false;
    }

    if (overrides.song_table)
    {
        info.song_table = overrides.song_table;
        log.push_back("song table " + Hex(overrides.song_table) + " (user supplied)");
    }
    else
    {
        info.song_table = FindSongTable(rom, info.revision, log, error);
        if (!info.song_table)
        {
            // The command reader shows that the game has the driver, so a song table that can't be found is an error.
            if (reader && error.empty())
            {
                error = "found the command reader of the " + std::string(RevisionName(info.revision)) +
                        " revision of Konami's driver, at " + Hex(reader) +
                        ", but not its song table (--song-table and --song-count override detection)";
            }

            return false;
        }
    }

    if (overrides.song_count > 0)
    {
        info.song_count = overrides.song_count;
    }
    else
    {
        const SongCount songs = CountSongs(rom, info.song_table, info.revision);
        info.song_count = songs.entries;
        if (songs.unreadable >= 0)
        {
            info.warnings.push_back("song " + std::to_string(songs.unreadable) +
                                    "'s tracks don't read cleanly, so the song table is taken to end before it "
                                    "(--song-count overrides this)");
        }
    }
    if (info.song_count <= 0)
    {
        error = "the song table at " + Hex(info.song_table) +
                " holds no songs supergbamidi can read (--song-count overrides this)";
        return false;
    }

    const RevisionCode code = CodeFor(info.revision);
    const std::vector<int> used = UsedSamples(rom, info.song_table, info.song_count, info.revision, info.sample_map);
    if (overrides.sample_table)
    {
        info.sample_table = overrides.sample_table;
        log.push_back("sample table " + Hex(overrides.sample_table) + " (user supplied)");
    }
    else
    {
        info.sample_table = FindSampleTable(rom, used, code.note_start, info.revision, log, info.warnings, error);
        if (!info.sample_table && !used.empty())
        {
            return false;
        }
    }

    info.volume_table = code.volume.code.empty() ? 0 : FindLiteral(rom, code.volume.code, code.volume.ldr);
    info.psg_freq_table = FindLiteral(rom, code.psg_freq.code, code.psg_freq.ldr);
    info.noise_table = code.noise.code.empty() ? 0 : FindLiteral(rom, code.noise.code, code.noise.ldr);

    // The Dungeon Dice Monsters revision's noise settings are entries of the frequency table, and it has no mixer: each
    // FIFO plays at the rate of the samples on it.
    char buf[160];
    if (dungeon_dice)
    {
        constexpr uint32_t kNoiseEntry = 16 * kDungeonDiceNoiseNote;
        info.noise_table = info.psg_freq_table ? info.psg_freq_table + 2 * kNoiseEntry : 0;
        if (!info.psg_freq_table)
        {
            info.warnings.push_back(
                "PSG frequency table not found, so square notes are left out, wave notes play at "
                "the wrong pitch and noise notes use default settings");
        }

        std::snprintf(buf, sizeof buf, "%d song%s, samples at their own rates, PSG freq table %s, noise table %s",
                      info.song_count, info.song_count == 1 ? "" : "s",
                      info.psg_freq_table ? Hex(info.psg_freq_table).c_str() : "(not found)",
                      info.noise_table ? Hex(info.noise_table).c_str() : "(default)");
        log.push_back(buf);

        std::snprintf(buf, sizeof buf, "sample map %s, sample period table %s, wave table %s, wave volume table %s",
                      Hex(info.sample_map).c_str(), Hex(info.sample_period_table).c_str(),
                      info.wave_table ? Hex(info.wave_table).c_str() : "(default)",
                      info.wave_volume_table ? Hex(info.wave_volume_table).c_str() : "(default)");
        log.push_back(buf);

        info.bend_kept = false;
        return true;
    }

    // Wave RAM loader: push {r4,lr} / adds r4,r0,#0 / ldr r3,=wave_table / cmp r2,#0 / beq / movs r0,#0x80 /
    // ands r0,r1 / cmp r0,#0 / beq / ldr r3,=sfx_wave_table. A wave from 80 up comes from the second table, which sound
    // effects use. A GSF rip zeroes the code of the second table, which the songs don't run.
    const std::vector<uint32_t> wave_code = FindThumb(rom, {Exact(0xB510), Exact(0x1C04), HiByte(0x4B00), Exact(0x2A00),
                                                            HiByte(0xD000), Exact(0x2080), Exact(0x4008)});
    for (uint32_t hit : wave_code)
    {
        if (!rom.Contains(ThumbLiteral(rom, hit + 4)))
        {
            continue;
        }

        info.wave_table = ThumbLiteral(rom, hit + 4);
        const bool second = rom.U16(hit + 14) == 0x2800 && (rom.U16(hit + 16) & 0xFF00) == 0xD000 &&
                            (rom.U16(hit + 18) & 0xFF00) == 0x4B00 && rom.Contains(ThumbLiteral(rom, hit + 18));
        info.sfx_wave_table = second ? ThumbLiteral(rom, hit + 18) : 0;
        break;
    }

    // The Eternal Duelist revision's loader has no check for volume 0 and no second table: push {r4,lr} /
    // lsls r1,r1,#4 / adds r1,r1,r2 / lsls r1,r1,#4 / ldr r2,=wave_table / adds r1,r1,r2.
    if (!info.wave_table)
    {
        info.wave_table = FindLiteral(
            rom, {Exact(0xB510), Exact(0x0109), Exact(0x1889), Exact(0x0109), HiByte(0x4A00), Exact(0x1889)}, 4);
    }

    // Mixer rate: ldr r1,=timer_table / lsls r0,r3,#2 / adds / ldr r0,[r0] / movs r1,#0x80 / lsls r1,#16 / orrs / str.
    // Some builds load the setting into r3 instead: ldr r3,[r0] / movs r0,#0x80 / lsls r0,#16 / orrs r0,r3.
    info.timer_table = FindLiteral(rom,
                                   {HiByte(0x4900), Exact(0x0098), Exact(0x1840), Exact(0x6800), Exact(0x2180),
                                    Exact(0x0409), Exact(0x4308), Exact(0x6010)},
                                   0);
    if (!info.timer_table)
    {
        info.timer_table = FindLiteral(rom,
                                       {HiByte(0x4900), Exact(0x0098), Exact(0x1840), Exact(0x6803), Exact(0x2080),
                                        Exact(0x0400), Exact(0x4318), Exact(0x6010)},
                                       0);
    }

    // The Rave Master revision loads the setting itself: ldr r0,[r1,#8] / adds r1,#0x38 / ldr r0,=setting /
    // str r0,[r1]. Eternal Duelist has ldr r1,=0x04000100 / ldr r0,=setting / str r0,[r1]. The literal then stands for
    // the table.
    if (!info.timer_table)
    {
        const std::vector<uint32_t> hits =
            FindThumb(rom, {Exact(0x6888), Exact(0x3138), HiByte(0x4800), Exact(0x6008)});
        info.timer_table = hits.empty() ? 0 : LiteralAddress(rom, hits.front() + 4);
    }
    if (!info.timer_table)
    {
        for (uint32_t hit : FindThumb(rom, {HiByte(0x4900), HiByte(0x4800), Exact(0x6008)}))
        {
            if (ThumbLiteral(rom, hit) == 0x04000100)
            {
                info.timer_table = LiteralAddress(rom, hit + 2);
                break;
            }
        }
    }

    const double timer_rate = TimerRate(rom, info.timer_table);
    if (overrides.mix_rate > 0)
    {
        info.mix_rate = overrides.mix_rate;
    }
    else if (timer_rate > 0)
    {
        info.mix_rate = timer_rate;
    }
    else
    {
        info.mix_rate = kCpuHz / (0x10000 - 0xFCE2);
        info.warnings.push_back(info.timer_table
                                    ? "mixer rate in the timer table is outside 4000-65536 Hz, assuming 21024 Hz"
                                    : "mixer timer not found, assuming 21024 Hz");
    }

    std::snprintf(buf, sizeof buf, "%d song%s, mixer %.2f Hz, volume table %s, PSG freq table %s", info.song_count,
                  info.song_count == 1 ? "" : "s", info.mix_rate,
                  info.volume_table ? Hex(info.volume_table).c_str() : "(computed)",
                  info.psg_freq_table ? Hex(info.psg_freq_table).c_str() : "(default)");
    log.push_back(buf);

    std::snprintf(buf, sizeof buf, "noise table %s, wave table %s, timer table %s",
                  info.noise_table ? Hex(info.noise_table).c_str() : "(default)",
                  info.wave_table ? Hex(info.wave_table).c_str() : "(default)",
                  info.timer_table ? Hex(info.timer_table).c_str() : "(default)");
    log.push_back(buf);

    // The older revisions never keep a bend.
    if (info.revision != Revision::kUltimateMasters)
    {
        info.bend_kept = false;
        return true;
    }

    // Pitch bend: the bend command hands on the bent pitch with ldrh r0,[r6,#0x1c] / mov r7,r8 / strh r0,[r7,#6] /
    // ldrh r0,[r6] / adds r0,r0,r4 / strh r0,[r7] / ldrb r0,[r7,#5] / movs r1,#0x20 / orrs r0,r1 / strb r0,[r7,#5]. A
    // driver that keeps the bend for later notes stores it next, with strh r4,[r6,#0x1e].
    const std::vector<uint32_t> bend_code =
        FindThumb(rom, {Exact(0x8BB0), Exact(0x4647), Exact(0x80F8), Exact(0x8830), Exact(0x1900), Exact(0x8038),
                        Exact(0x7978), Exact(0x2120), Exact(0x4308), Exact(0x7178)});
    if (bend_code.empty())
    {
        log.push_back("pitch bend code not recognised, assuming a bend carries over to later notes");
    }
    else
    {
        info.bend_kept = rom.U16(bend_code.front() + 20) == 0x83F4;
        log.push_back(info.bend_kept ? "pitch bends carry over to later notes (from bend code)"
                                     : "pitch bends apply to the playing note only (from bend code)");
    }

    return true;
}

} // namespace supergbamidi::konami
