// SPDX-License-Identifier: MIT

// Unit tests for Konami's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "files.h"
#include "konami/convert.h"
#include "konami/driver.h"
#include "konami/seqformat.h"
#include "konami/sequencer.h"
#include "konami/soundfont.h"
#include "midi.h"
#include "music.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"

namespace fs = std::filesystem;

namespace agbsf2::konami
{
namespace
{

using test::ReadAll;
using test::ReadSf2;
using test::TempPath;

// The size of a song table entry with 16 tracks, as the tests' songs have.
constexpr uint32_t kSongEntrySize = 36;

// A cartridge image being put together.
struct Image
{
    explicit Image(size_t size) : d(size, 0)
    {
    }

    void Put16(uint32_t a, uint16_t v)
    {
        d[a - kRomBase] = uint8_t(v);
        d[a - kRomBase + 1] = uint8_t(v >> 8);
    }

    void Put32(uint32_t a, uint32_t v)
    {
        Put16(a, uint16_t(v));
        Put16(a + 2, uint16_t(v >> 16));
    }

    void Bytes(uint32_t a, const std::vector<uint8_t>& b)
    {
        std::copy(b.begin(), b.end(), d.begin() + (a - kRomBase));
    }

    // Writes the halfwords `h`, such as Thumb code, from `a` on.
    void Halfwords(uint32_t a, const std::vector<uint16_t>& h)
    {
        for (size_t i = 0; i < h.size(); i++)
        {
            Put16(a + 2 * uint32_t(i), h[i]);
        }
    }

    // Writes entry `song` of the song table at `table`: its base address, and the offset of every track, which is
    // `rest` unless `offsets` gives another.
    void Song(uint32_t table, int song, uint32_t base, uint16_t rest, const std::vector<std::pair<int, int>>& offsets)
    {
        const uint32_t e = table + uint32_t(song) * kSongEntrySize;
        Put32(e, base);
        for (int t = 0; t < kTracks; t++)
        {
            Put16(e + 4 + 2 * uint32_t(t), rest);
        }
        for (const auto& [t, o] : offsets)
        {
            Put16(e + 4 + 2 * uint32_t(t), uint16_t(o));
        }
    }

    Rom ToRom() const
    {
        Rom r;
        r.Assign(d);

        return r;
    }

    std::vector<uint8_t> d;
};

// Songs and one sample at fixed places, for use with DriverOverrides.
constexpr uint32_t kSongTable = kRomBase + 0x100;
constexpr uint32_t kSongBase = kRomBase + 0x200;
constexpr uint32_t kSampleTable = kRomBase + 0x400;
constexpr uint32_t kSampleHeader = kRomBase + 0x500;

// The starts of song 0's tracks, relative to kSongBase: every track ends at once except 0 (PSG), 4 and 5 (samples).
constexpr uint16_t kRestOffset = 0x100;
const std::vector<std::pair<int, int>> kTrackOffsets = {{0, 0x40}, {4, 0x00}, {5, 0x80}};

// Returns a cartridge without driver code, holding two songs and a sample at the places above. Song 0 loops and plays
// notes on tracks 0, 4 and 5; song 1 plays none.
Image SyntheticImage()
{
    Image m(0x1000);

    // Song 0.
    m.Song(kSongTable, 0, kSongBase, kRestOffset, kTrackOffsets);
    m.Bytes(kSongBase + 0x100, {0x00, 0xFD});

    // Track 4: note (vol 16, sample 5), wait 10, rest, wait 5, loop the song.
    m.Bytes(kSongBase + 0x00, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xE0, 0x05, 0xFF, 0x01, 0x00});

    // Track 5: wait 5, loop point (+2 bytes), wait 2, note (vol 7, sample 5, +12), wait 3, rest, wait 1, end.
    m.Bytes(kSongBase + 0x80, {0x05, 0xF3, 0xAA, 0xBB, 0x02, 0xB7, 0x05, 0x0C, 0x03, 0xE0, 0x01, 0xFE});

    // Track 0: duty 25%, PSG note (vol 3, note 24), wait 4, rest, wait 0, end.
    m.Bytes(kSongBase + 0x40, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE});

    // Song 1: a placeholder that plays no notes. Every track: rest, wait 5, end.
    m.Song(kSongTable, 1, kSongBase + 0x180, 0, {});
    m.Bytes(kSongBase + 0x180, {0x00, 0xE0, 0x05, 0xFE});

    // Sample 5: 64-point sawtooth at the mixer rate, one-shot.
    m.Put32(kSampleTable + 4 * 5, kSampleHeader);
    m.Put32(kSampleHeader, 0x1000);
    m.Put32(kSampleHeader + 4, 64);
    m.Put32(kSampleHeader + 8, 0xFFFFFFFF);
    for (int i = 0; i < 64; i++)
    {
        m.d[kSampleHeader - kRomBase + 12 + i] = uint8_t(int8_t(i * 4 - 128));
    }

    return m;
}

Rom SyntheticRom()
{
    return SyntheticImage().ToRom();
}

// Returns the settings of a driver whose song table is at `song_table`, for a Sequencer to play. `bend_kept` is
// DriverInfo::bend_kept.
DriverInfo SongsAt(uint32_t song_table, bool bend_kept = true)
{
    DriverInfo info;
    info.song_table = song_table;
    info.bend_kept = bend_kept;

    return info;
}

// The addresses ScanRom puts the song and sample tables at.
constexpr uint32_t kScanSongTable = kRomBase + 0x100;
constexpr uint32_t kScanSampleTable = kRomBase + 0x168;

// Returns a cartridge with no driver code, but with song and sample tables laid out the way the driver's data is, for
// the detection scans to find: two songs right after the song table, the tracks of each song tiling its data, and
// sample headers right after the sample table. `gap` puts that many bytes between the two songs.
Image ScanImage(uint32_t gap = 0)
{
    Image m(0x400);
    constexpr uint32_t kSong0 = kScanSongTable + 2 * kSongEntrySize;

    // Song 0: track 4 at +0 (note, sample 0), track 0 at +8 (PSG note), the other tracks share +17.
    m.Song(kScanSongTable, 0, kSong0, 17, {{4, 0}, {0, 8}});
    m.Bytes(kSong0 + 0, {0x00, 0xA8, 0x10, 0x00, 0x0A, 0xE0, 0x05, 0xFE});
    m.Bytes(kSong0 + 8, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE});
    m.Bytes(kSong0 + 17, {0x00, 0xFD});

    // Song 1, from the end of song 0's data: track 5 at +0 (note, sample 1, then stop the song), the other tracks share
    // +8.
    const uint32_t song1 = kSong0 + 19 + gap;
    m.Song(kScanSongTable, 1, song1, 8, {{5, 0}});
    m.Bytes(song1 + 0, {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00});
    m.Bytes(song1 + 8, {0x00, 0xFD});

    // Samples 0 (one-shot) and 1 (looped), with their headers right after the table.
    constexpr uint32_t kHeader0 = kScanSampleTable + 8, kHeader1 = kHeader0 + 12 + 32;
    m.Put32(kScanSampleTable, kHeader0);
    m.Put32(kScanSampleTable + 4, kHeader1);

    m.Put32(kHeader0, 0x1000);
    m.Put32(kHeader0 + 4, 32);
    m.Put32(kHeader0 + 8, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 32; i++)
    {
        m.d[kHeader0 - kRomBase + 12 + i] = uint8_t(i * 8);
    }

    m.Put32(kHeader1, 0xC2D);
    m.Put32(kHeader1 + 4, 16);
    m.Put32(kHeader1 + 8, 4);
    for (uint32_t i = 0; i < 16; i++)
    {
        m.d[kHeader1 - kRomBase + 12 + i] = uint8_t(i * 16);
    }

    return m;
}

Rom ScanRom(uint32_t gap = 0)
{
    return ScanImage(gap).ToRom();
}

// The play-song routine's code. When it starts at a multiple of 4, its ldr at +6 loads the song table's address from
// +0x18.
const std::vector<uint16_t> kPlaySongCode = {0x00E0, 0x1900, 0x0080, 0x4904, 0x1842, 0x8811, 0x8850, 0x0400, 0x4301};

// The note-start routine's code. When it starts at a multiple of 4, its ldr at +14 loads the sample table's address
// from +0x14.
const std::vector<uint16_t> kNoteStartCode = {0x4B00, 0x403B, 0x20A0, 0x0200, 0x4038, 0x2800, 0xD100, 0x4901};

void TestDecoding()
{
    std::vector<uint8_t> d(0x40, 0);
    const std::vector<uint8_t> kStream = {0x05, 0xE3, 0x10, 0xF0, 0x34, 0x12, 0xF3, 0xAA, 0xBB, 0xB8, 0x20,
                                          0x07, 0xFE, 0xA3, 0x09, 0xC9, 0x0C, 0xD2, 0x30, 0x83, 0x06, 0xFF};
    std::copy(kStream.begin(), kStream.end(), d.begin());
    Rom rom;
    rom.Assign(d);
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(rom, kRomBase + 0, Revision::kUltimateMasters, delay), 1);
    SEQLIB_CHECK_EQ(delay, 5);
    SEQLIB_CHECK_EQ(ReadDelay(rom, kRomBase + 1, Revision::kUltimateMasters, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x310);
    SEQLIB_CHECK_EQ(ReadDelay(rom, kRomBase + 3, Revision::kUltimateMasters, delay), 3);
    SEQLIB_CHECK_EQ(delay, 0x1234);

    // F3 carries two extra bytes on sample tracks only.
    SEQLIB_CHECK_EQ(DecodeCommand(rom, kRomBase + 6, 0, Revision::kUltimateMasters).length, 1);
    SEQLIB_CHECK(DecodeCommand(rom, kRomBase + 6, 4, Revision::kUltimateMasters).op == Op::kLoopPoint);
    SEQLIB_CHECK_EQ(DecodeCommand(rom, kRomBase + 6, 4, Revision::kUltimateMasters).length, 3);

    Command c =
        DecodeCommand(rom, kRomBase + 9, 4, Revision::kUltimateMasters); // B8 20 07 FE: vol byte, sample, semitone

    SEQLIB_CHECK(c.op == Op::kNote);
    SEQLIB_CHECK_EQ(c.vol, 0x20);
    SEQLIB_CHECK_EQ(c.sample, 7);
    SEQLIB_CHECK_EQ(c.semitone, -2);
    SEQLIB_CHECK_EQ(c.length, 4);

    c = DecodeCommand(rom, kRomBase + 13, 4, Revision::kUltimateMasters); // A3 09: vol 3, sample 9
    SEQLIB_CHECK(c.op == Op::kNote && c.vol == 3 && c.sample == 9 && c.semitone == 0 && c.length == 2);

    c = DecodeCommand(rom, kRomBase + 15, 0, Revision::kUltimateMasters); // C9 0C: volume byte
    SEQLIB_CHECK(c.op == Op::kVolume && c.vol == 12 && c.length == 2);

    c = DecodeCommand(rom, kRomBase + 17, 0, Revision::kUltimateMasters); // D2 30: PSG note, vol 2
    SEQLIB_CHECK(c.op == Op::kPsgNote && c.vol == 2 && c.note == 0x30 && c.length == 2);

    c = DecodeCommand(rom, kRomBase + 19, 0, Revision::kUltimateMasters); // 83: duty 75%
    SEQLIB_CHECK(c.op == Op::kDuty && c.value == 0xC0);

    c = DecodeCommand(rom, kRomBase + 20, 2, Revision::kUltimateMasters); // 06: wave 2
    SEQLIB_CHECK(c.op == Op::kDuty && c.value == 2);

    c = DecodeCommand(rom, kRomBase + 21, 4, Revision::kUltimateMasters); // FF 00: stop
    SEQLIB_CHECK(c.op == Op::kJump && c.value == 0 && c.length == 2);

    // F2 bends by its byte less 0x40. A byte above 7F takes the next one too: F2 xh ll bends by 0xhll - 0x400.
    Rom bends;
    bends.Assign({0xF2, 0x50, 0xF2, 0x84, 0x61, 0xF2, 0x90, 0x00});

    c = DecodeCommand(bends, kRomBase + 0, 4, Revision::kUltimateMasters);
    SEQLIB_CHECK(c.op == Op::kPitchBend && c.value == 0x10 && c.length == 2);

    c = DecodeCommand(bends, kRomBase + 2, 4, Revision::kUltimateMasters);
    SEQLIB_CHECK(c.op == Op::kPitchBend && c.value == 0x61 && c.length == 3);

    c = DecodeCommand(bends, kRomBase + 5, 0, Revision::kUltimateMasters);
    SEQLIB_CHECK(c.op == Op::kPitchBend && c.value == -0x400 && c.length == 3);
}

void TestSequencer()
{
    const Rom rom = SyntheticRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);
    SEQLIB_CHECK(seq.Valid());

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 20; f++)
    {
        frames.push_back(seq.Step());

        if (f == 15)
        {
            SEQLIB_CHECK(seq.LoopedLastFrame());
            SEQLIB_CHECK_EQ(seq.LoopsDone(), 1);
            SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 0); // track 4 loops the song, and has no loop point
        }
        else
        {
            SEQLIB_CHECK(!seq.LoopedLastFrame());
        }
    }

    // Track 4: note on at 0, rest at 10, note again at 15 after the song loops.
    SEQLIB_CHECK(frames[0][4].flags & kOutNoteOn);
    SEQLIB_CHECK_EQ(frames[0][4].key, 5);
    SEQLIB_CHECK_EQ(frames[0][4].vol, 16);
    for (int f = 1; f < 10; f++)
    {
        SEQLIB_CHECK(!frames[size_t(f)][4].Any());
    }
    SEQLIB_CHECK(frames[10][4].flags & kOutStop);
    SEQLIB_CHECK(frames[15][4].flags & kOutNoteOn);

    // Track 5: loop point at 5 (skipping its two extra bytes), note at 7 an octave up; the loop restarts it from the
    // loop point: note again at 17.
    SEQLIB_CHECK(frames[7][5].flags & kOutNoteOn);
    SEQLIB_CHECK_EQ(frames[7][5].note_pitch, 12 * 32);
    SEQLIB_CHECK_EQ(frames[7][5].vol, 7);
    SEQLIB_CHECK(frames[10][5].flags & kOutStop);
    SEQLIB_CHECK(frames[17][5].flags & kOutNoteOn);
    for (int f = 11; f < 17; f++)
    {
        SEQLIB_CHECK(!(frames[size_t(f)][5].flags & kOutNoteOn));
    }

    // Track 0: 25% duty note 24 at frame 0, then the rest silences it at 4.
    SEQLIB_CHECK(frames[0][0].trig && (frames[0][0].flags & kOutPsgNote));
    SEQLIB_CHECK_EQ(frames[0][0].b2 >> 6, 1);
    SEQLIB_CHECK_EQ(frames[0][0].pitch, 24 * 32);
    SEQLIB_CHECK_EQ(frames[0][0].vol, 3);
    SEQLIB_CHECK(frames[4][0].trig && frames[4][0].vol == 0);

    // Tracks ending with FD are gone for good, and don't come back on the loop.
    SEQLIB_CHECK(!frames[1][7].active);
    SEQLIB_CHECK(!frames[16][7].active);
}

void TestLoopStart()
{
    // Track 4 ends where it looped the song, and track 5 loops the song where it ended, at frame 11. It passed its loop
    // point at frame 5.
    Image m = SyntheticImage();
    m.Bytes(kSongBase + 0x07, {0xFE});
    m.Bytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    int looped_at = -1;
    for (int f = 0; f < 20 && looped_at < 0; f++)
    {
        seq.Step();
        looped_at = seq.LoopedLastFrame() ? f : -1;
    }

    // The loop goes back to the frame where track 5 passed its loop point.
    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 5);
}

void TestLoopStartEarliest()
{
    // As in TestLoopStart, but track 0 also passes a loop point, at frame 4: duty 25%, PSG note (vol 3, note 24), wait
    // 4, loop point, rest, end. The loop goes back to frame 4, the earlier of the two.
    Image m = SyntheticImage();
    m.Bytes(kSongBase + 0x07, {0xFE});
    m.Bytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    m.Bytes(kSongBase + 0x40, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xF3, 0x00, 0xE0, 0x00, 0xFE});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    int looped_at = -1;
    for (int f = 0; f < 20 && looped_at < 0; f++)
    {
        seq.Step();
        looped_at = seq.LoopedLastFrame() ? f : -1;
    }

    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 4);
}

void TestLoopStartLast()
{
    // As in TestLoopStart, but track 5 passes a second loop point, at frame 8: wait 5, loop point, wait 3, loop point,
    // wait 2, note, wait 3, rest, wait 1, loop the song. Each loop takes the track back to the second loop point, so
    // the song loops at frame 14 and every 6 frames after it.
    Image m = SyntheticImage();
    m.Bytes(kSongBase + 0x07, {0xFE});
    m.Bytes(kSongBase + 0x80, {0x05, 0xF3, 0xAA, 0xBB, 0x03, 0xF3, 0xAA, 0xBB, 0x02, 0xB7, 0x05, 0x0C, 0x03, 0xE0, 0x01,
                               0xFF, 0x01, 0x00});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    std::vector<int> looped_at;
    for (int f = 0; f < 30; f++)
    {
        seq.Step();
        if (seq.LoopedLastFrame())
        {
            looped_at.push_back(f);

            // The loop goes back to the frame where track 5 passed the second loop point, and stays there.
            SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 8);
        }
    }

    SEQLIB_CHECK(looped_at == (std::vector<int>{14, 20, 26}));
}

// Track 5 plays a note of sample F0. Sample numbers above EF select a mode that agbsf2 leaves out. The track stops with
// a warning that names the sample.
void TestSampleAboveEf()
{
    Image m = SyntheticImage();
    m.Bytes(kSongBase + 0x80, {0x00, 0xA0, 0xF0, 0x03, 0xFE});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    for (int f = 0; f < 4; f++)
    {
        seq.Step();
    }

    const std::vector<std::string>& warnings = seq.Warnings();
    SEQLIB_CHECK(std::any_of(warnings.begin(), warnings.end(),
                             [](const std::string& w) { return w.find("note of sample F0") != std::string::npos; }));
}

// Runs an Ultimate Masters song until it first loops, and returns the frame it loops in.
int FirstLoop(UltimateMastersSequencer& seq)
{
    for (int f = 0; f < 20; f++)
    {
        seq.Step();
        if (seq.LoopedLastFrame())
        {
            return f;
        }
    }

    return -1;
}

void TestLoopStartFarEarlier()
{
    // As in TestLoopStart, but track 0 passes a loop point at its first command, in frame 0. Such a loop point means
    // the same as none: loop point, duty 25%, PSG note (vol 3, note 24), wait 12, rest, end. It comes more than
    // kLoopPointWindow frames before track 5's loop point, at frame 5. So the loop starts at frame 5.
    Image m = SyntheticImage();
    m.Bytes(kSongBase + 0x07, {0xFE});
    m.Bytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    m.Bytes(kSongBase + 0x40, {0x00, 0xF3, 0x00, 0x81, 0x00, 0xD3, 0x18, 0x0C, 0xE0, 0x00, 0xFE});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    const int looped_at = FirstLoop(seq);

    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 5);
}

void TestLoopStartEndedTrack()
{
    // As in TestLoopStartEarliest, but track 0 ends with FD. No loop restarts a track after FD. Track 0's loop point,
    // at frame 4, no longer counts, and the loop starts at track 5's loop point, at frame 5.
    Image m = SyntheticImage();
    m.Bytes(kSongBase + 0x07, {0xFE});
    m.Bytes(kSongBase + 0x8B, {0xFF, 0x01, 0x00});
    m.Bytes(kSongBase + 0x40, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xF3, 0x00, 0xE0, 0x00, 0xFD});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kSongTable), 0);

    const int looped_at = FirstLoop(seq);

    SEQLIB_CHECK_EQ(looped_at, 11);
    SEQLIB_CHECK_EQ(seq.LoopStartFrame(), 5);
}

void TestBendKept()
{
    // Track 4: note, wait 1, bend by +16/32, wait 1, note, wait 1, vibrato of depth 16, wait 2, end. The other tracks
    // end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x20, {{4, 0}});
    m.Bytes(kBase, {0x00, 0xA8, 0x10, 0x05, 0x01, 0xF2, 0x50, 0x01, 0xA8, 0x10, 0x05, 0x01, 0xF1, 0x20, 0x02, 0xFD});
    m.Bytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    for (const bool kept : {true, false})
    {
        UltimateMastersSequencer seq(rom, SongsAt(kTable, kept), 0);

        std::vector<TrackOutput> track4;
        for (int f = 0; f < 4; f++)
        {
            track4.push_back(seq.Step()[4]);
        }

        // The bend raises the note that's playing. A driver that keeps it raises the next note and the vibrato by it
        // too. The vibrato's first step adds (sin[0x18] * 16) >> 12 = 8.
        const int later = kept ? 16 : 0;
        SEQLIB_CHECK_EQ(track4[1].pitch, 16);
        SEQLIB_CHECK(track4[2].flags & kOutNoteOn);
        SEQLIB_CHECK_EQ(track4[2].pitch, later);
        SEQLIB_CHECK_EQ(track4[3].pitch, later + 8);
    }
}

// A channel event of a MIDI file, the name of the track it's on, and the frame it's on.
struct MidiEvent
{
    std::string track;
    uint32_t frame;
    uint8_t status, data1, data2;
};

// Returns every channel event of every track of the MIDI file at `path`, at the frame its tick stands for under the
// file's tempo map.
std::vector<MidiEvent> ReadEvents(const std::string& path)
{
    const test::MidiEvents midi = test::ReadMidi(path);
    const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
    std::vector<MidiEvent> events;
    std::vector<uint64_t> ticks;
    std::vector<std::pair<uint64_t, uint32_t>> tempos = {{0, 500000}}; // tick, microseconds a quarter note
    for (size_t t = 0; t < midi.tracks.size(); t++)
    {
        for (const auto& [tick, bytes] : midi.tracks[t].events)
        {
            if (bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51)
            {
                tempos.emplace_back(tick, uint32_t(bytes[3] << 16 | bytes[4] << 8 | bytes[5]));
            }
            if (bytes[0] < 0xF0)
            {
                events.push_back({names[t], 0, bytes[0], bytes[1], uint8_t(bytes.size() > 2 ? bytes[2] : 0)});
                ticks.push_back(tick);
            }
        }
    }

    // Each event's tick in seconds, then in frames.
    const double division = midi.division;
    for (size_t e = 0; e < events.size(); e++)
    {
        double seconds = 0;
        uint64_t at = 0;
        uint32_t tempo = 500000;
        for (const auto& [when, value] : tempos)
        {
            if (when > ticks[e])
            {
                break;
            }

            seconds += double(when - at) * tempo / division / 1e6;
            at = when;
            tempo = value;
        }
        seconds += double(ticks[e] - at) * tempo / division / 1e6;
        events[e].frame = uint32_t(std::lround(seconds / kFrameSeconds));
    }

    return events;
}

// A preset header of a SoundFont.
struct PresetHeader
{
    std::string name;
    int bank, program;
};

// Returns the preset headers of the SoundFont at `path`, in the file's order.
std::vector<PresetHeader> ReadPresets(const std::string& path)
{
    const std::vector<uint8_t> phdr = ReadSf2(path).chunks["phdr"];
    std::vector<PresetHeader> presets;
    for (size_t h = 0; h + 2 * 38 <= phdr.size(); h += 38) // the last header only ends the list
    {
        presets.push_back({reinterpret_cast<const char*>(&phdr[h]), phdr[h + 22] | (phdr[h + 23] << 8),
                           phdr[h + 20] | (phdr[h + 21] << 8)});
    }

    return presets;
}

// Returns the key range and root key, {lo, hi, root}, of each instrument zone of the SoundFont at `path`, in the file's
// order. Each zone starts with its key range.
std::vector<std::array<int, 3>> ReadZoneKeys(const std::string& path)
{
    const std::vector<uint8_t> igen = ReadSf2(path).chunks["igen"];
    std::vector<std::array<int, 3>> zones;
    for (size_t g = 0; g + 4 <= igen.size(); g += 4)
    {
        const int op = igen[g] | (igen[g + 1] << 8);
        if (op == sf2::kKeyRange)
        {
            zones.push_back({igen[g + 2], igen[g + 3], -1});
        }
        else if (op == sf2::kOverridingRootKey && !zones.empty())
        {
            zones.back()[2] = int16_t(igen[g + 2] | (igen[g + 3] << 8));
        }
    }

    return zones;
}

void TestConversion()
{
    const Rom rom = SyntheticRom();
    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 1;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));
    SEQLIB_CHECK_EQ(info.song_count, 1);

    const fs::path dir = TempPath("out");
    fs::create_directories(dir);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.loops = 1;
    opt.out_dir = Utf8(dir);
    opt.base_name = "synthetic";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(sum.loop_start_frame, 0);
    SEQLIB_CHECK_EQ(sum.loop_end_frame, 15);
    SEQLIB_CHECK_EQ(sum.frames, 15);
    SEQLIB_CHECK_EQ(sum.tracks, 3);
    SEQLIB_CHECK(fs::exists(PathFromUtf8(sum.sf2_path)));

    const std::vector<MidiEvent> events = ReadEvents(sum.midi_path);

    auto has = [&](const std::string& track, uint32_t frame, uint8_t type)
    {
        for (const MidiEvent& e : events)
        {
            if (e.track == track && e.frame == frame && (e.status & 0xF0) == type)
            {
                return true;
            }
        }

        return false;
    };

    SEQLIB_CHECK(has("Voice 0", 0, 0x90));
    SEQLIB_CHECK(has("Voice 0", 10, 0x80));
    SEQLIB_CHECK(has("Voice 1", 7, 0x90));
    SEQLIB_CHECK(has("Voice 1", 10, 0x80));
    SEQLIB_CHECK(has("Square 1", 0, 0x90));
    SEQLIB_CHECK(has("Square 1", 4, 0x80));

    for (const MidiEvent& e : events)
    {
        if (e.track == "Square 1" && (e.status & 0xF0) == 0x90)
        {
            SEQLIB_CHECK_EQ(e.data1, 60); // PSG note 24 = C4
        }
    }

    opt.base_name = "\xE9\x9F\xB3\xE6\xA5\xBD:demo. ";

    const SongSummary renamed = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(renamed.ok);
    SEQLIB_CHECK(PathFromUtf8(renamed.midi_path) == dir / PathFromUtf8("\xE9\x9F\xB3\xE6\xA5\xBD_demo_00.mid"));
    SEQLIB_CHECK(ReadAll(renamed.midi_path) == ReadAll(sum.midi_path));
    SEQLIB_CHECK(ReadAll(renamed.sf2_path) == ReadAll(sum.sf2_path));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestSilentSongs()
{
    const Rom rom = SyntheticRom();
    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 2;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    std::error_code ec;
    const fs::path dir = TempPath("silent");
    fs::create_directories(dir, ec);
    SEQLIB_CHECK(!ec);
    if (ec)
    {
        return;
    }

    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    opt.base_name = "silent";

    // Song 1 plays no notes, so nothing is written for it.
    SongSummary sum = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.silent);
    SEQLIB_CHECK_EQ(sum.tracks, 0);
    SEQLIB_CHECK(sum.midi_path.empty() && sum.sf2_path.empty());

    // Song 0 has notes, but none on track 15.
    opt.track_mask = 1u << 15;

    sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.silent);
    SEQLIB_CHECK(fs::is_empty(dir));
    fs::remove_all(dir, ec);
}

void TestSharedSoundfont()
{
    // A shared SoundFont puts each song's presets in the bank numbered after the song. MIDI can't select a bank above
    // 127. Song 128's presets therefore take the free programs after the presets of song 0 in bank 0. This table has
    // the synthetic song as both song 0 and song 128.
    Image m = SyntheticImage();
    m.d.resize(0x3000);
    constexpr uint32_t kTable = kRomBase + 0x1000;
    m.Song(kTable, 0, kSongBase, kRestOffset, kTrackOffsets);
    m.Song(kTable, 128, kSongBase, kRestOffset, kTrackOffsets);
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kTable;
    ov.song_count = 129;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    std::error_code ec;
    const fs::path dir = TempPath("shared");
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    SoundfontBuilder shared(rom, info, 129);

    const SongSummary first = ConvertSong(rom, info, 0, opt, &shared);
    const size_t first_count = shared.File().presets.size();
    const SongSummary last = ConvertSong(rom, info, 128, opt, &shared);

    const std::vector<sf2::Preset>& presets = shared.File().presets;
    SEQLIB_CHECK(first.ok && last.ok && first.sf2_path.empty());
    SEQLIB_CHECK(first_count > 0 && presets.size() == 2 * first_count);
    for (size_t i = 0; i < presets.size(); i++)
    {
        SEQLIB_CHECK(presets[i].bank == 0 && presets[i].program == int(i));
    }
    fs::remove_all(dir, ec);
}

void TestEchoRouting()
{
    // Song 2 routes voice 0 to bus 0, then to bus 1 as well, then off both, then to bus 0 again, 5 frames apart. The
    // mixer takes a voice routed to buses 0 and 1 as routed to bus 1 alone, so the echo is only sent while the voice is
    // on bus 0 alone.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.Bytes(kBase, {0x00, 0xF7, 0x7F, 0x00, 0xF9, 0x01, 0x00, 0xA8, 0x10, 0x05, 0x05,
                    0xF9, 0x02, 0x05, 0xF9, 0x00, 0x05, 0xF9, 0x01, 0x05, 0xFD});
    m.Bytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 3;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    std::error_code ec;
    const fs::path dir = TempPath("echo");
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 2, opt, nullptr);

    std::vector<std::pair<uint32_t, int>> sends;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Voice 0" && (e.status & 0xF0) == 0xB0 && e.data1 == cc::kReverb)
        {
            sends.emplace_back(e.frame, e.data2);
        }
    }

    const std::vector<std::pair<uint32_t, int>> kExpected = {{0, 0}, {0, 127}, {5, 0}, {15, 127}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(sends == kExpected);
    fs::remove_all(dir, ec);
}

// Song 2's sample track plays a note at volume 16, and then from its loop point a note at 16 and one at 8. The first
// time through, the loop's first note has the level and program that the note before it left, but a player that jumps
// back to the loop's start comes from the note at volume 8, so they're written again there, and so is the tempo.
void TestLoopSettingsAgain()
{
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.Bytes(kBase, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xF3, 0x00, 0x00, 0x00, 0xA8,
                    0x10, 0x05, 0x0A, 0xA8, 0x08, 0x05, 0x0A, 0xFF, 0x01, 0x00});
    m.Bytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 3;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    std::error_code ec;
    const fs::path dir = TempPath("loop_again");
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 2, opt, nullptr);

    std::vector<std::pair<uint32_t, int>> levels, programs;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Voice 0" && (e.status & 0xF0) == 0xB0 && e.data1 == cc::kExpression)
        {
            levels.emplace_back(e.frame, e.data2);
        }
        if (e.track == "Voice 0" && (e.status & 0xF0) == 0xC0)
        {
            programs.emplace_back(e.frame, e.data1);
        }
    }

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(sum.loop_start_frame, 10);
    SEQLIB_CHECK(test::RepeatsTempoAtLoopStart(test::ReadMidi(sum.midi_path)));
    SEQLIB_REQUIRE_EQ(levels.size(), 6);
    SEQLIB_REQUIRE_EQ(programs.size(), 2);
    SEQLIB_CHECK(levels[2] == std::make_pair(10u, levels[1].second));
    SEQLIB_CHECK(levels[3].second != levels[2].second && levels[4] == std::make_pair(30u, levels[1].second));
    SEQLIB_CHECK(programs[1] == std::make_pair(10u, programs[0].second));
    fs::remove_all(dir, ec);
}

// Song 2's sample track pans to the left before its loop point, plays a note and then pans to the right, which the
// driver keeps when every track goes back to its loop point. The first pass's note plays on the left and the later
// passes' on the right, so the marked loop starts at the second pass, and the song plays the first pass before it.
void TestLoopStartsAtSecondPass()
{
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.Bytes(kBase, {0x00, 0xF0, 0x00, 0x00, 0xF3, 0x00, 0x00, 0x00, 0xA8, 0x10, 0x05, 0x0A, 0xF0, 0x7F, 0x0A, 0xFF,
                    0x01, 0x00});
    m.Bytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 3;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    std::error_code ec;
    const fs::path dir = TempPath("loop_second");
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 2, opt, nullptr);

    // The first pass runs from frame 0 to 20, so the loop runs from 20 to 40, and the song ends after its second
    // pass through it.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(sum.loop_start_frame, 20);
    SEQLIB_CHECK_EQ(sum.loop_end_frame, 40);
    SEQLIB_CHECK_EQ(sum.frames, 60u);
    fs::remove_all(dir, ec);
}

void TestNoiseKeys()
{
    // A noise track that plays notes 0 to 129, more than one preset has keys for. Notes 0 to 91 go on keys 36 to 127
    // and 92 to 127 on the keys below 36, and 128 and 129 need a second preset. Each note gets a key of its own.
    Image m = SyntheticImage();
    m.d.resize(0x2000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    m.Song(kSongTable, 3, kBase, 0x400, {{3, 0}});
    std::vector<uint8_t> track = {0x00};
    for (int n = 0; n < 130; n++)
    {
        track.insert(track.end(), {0xD8, 0x0F, uint8_t(n), 0x01}); // note n at volume 15, then wait a frame
    }
    track.insert(track.end(), {0xE0, 0x00, 0xFD});
    m.Bytes(kBase, track);
    m.Bytes(kBase + 0x400, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 4;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    const fs::path dir = TempPath("noise");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 3, opt, nullptr);

    std::vector<std::pair<int, int>> sounds; // (program, key) of each note
    int program = -1;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if (e.track == "Noise" && (e.status & 0xF0) == 0xC0)
        {
            program = e.data1;
        }
        if (e.track == "Noise" && (e.status & 0xF0) == 0x90)
        {
            sounds.emplace_back(program, e.data1);
        }
    }

    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(sounds.size(), 130);
    SEQLIB_CHECK(sounds.front() == std::make_pair(sounds.front().first, 36));
    SEQLIB_CHECK(sounds[92] == std::make_pair(sounds.front().first, 35));
    SEQLIB_CHECK(sounds[128].first != sounds.front().first && sounds[128].second == 127);

    std::sort(sounds.begin(), sounds.end());
    SEQLIB_CHECK(std::unique(sounds.begin(), sounds.end()) == sounds.end());
    fs::remove_all(dir, ec);
}

void TestPsgPastTable()
{
    // Square 1 plays note 9, note FF, a rest and note FF again, 5 frames apart. The code at kFreqCode loads the address
    // of the PSG frequency table, which has an entry for note 9. Note FF reads the word at 0x3FC0 past its start.
    Image m = SyntheticImage();
    m.d.resize(0x6000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    constexpr uint32_t kFreqCode = kRomBase + 0x1100;
    constexpr uint32_t kFreqTable = kRomBase + 0x1200;
    m.Song(kSongTable, 2, kBase, 0x80, {{0, 0}});
    m.Bytes(kBase, {0x00, 0x82, 0x00, 0xD5, 0x09, 0x05, 0xD5, 0xFF, 0x05, 0xE0, 0x05, 0xD5, 0xFF, 0x05, 0xFD});
    m.Bytes(kBase + 0x80, {0x00, 0xFD});
    m.Halfwords(kFreqCode, {0x4803, 0x2700, 0x5FE9, 0x0049, 0x1809, 0x880D});
    m.Put32(kFreqCode + 0x10, kFreqTable);
    m.Put16(kFreqTable, 0x802C);
    m.Put16(kFreqTable + 2 * 9 * 32, 0x8358);

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 3;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    const fs::path dir = TempPath("past_table");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    // The note-ons (frame, key) and pitch bends (frame, value) of Square 1.
    struct Square1
    {
        std::vector<std::pair<uint32_t, int>> notes, bends;
    };

    // Returns the events of Square 1 when the word past the table is `word`.
    auto convert = [&](uint16_t word)
    {
        m.Put16(kFreqTable + 0x3FC0, word);
        const Rom rom = m.ToRom();
        DriverInfo info;
        std::string err;
        SEQLIB_CHECK(DetectDriver(rom, ov, info, err) && info.psg_freq_table == kFreqTable);

        const SongSummary sum = ConvertSong(rom, info, 2, opt, nullptr);
        SEQLIB_CHECK(sum.ok);

        Square1 square;
        for (const MidiEvent& e : ReadEvents(sum.midi_path))
        {
            if (e.track == "Square 1" && (e.status & 0xF0) == 0x90 && e.data2 > 0)
            {
                square.notes.emplace_back(e.frame, e.data1);
            }
            if (e.track == "Square 1" && (e.status & 0xF0) == 0xE0 && e.frame > 0)
            {
                square.bends.emplace_back(e.frame, e.data1 | (e.data2 << 7));
            }
        }

        return square;
    };

    const Square1 plain = convert(0x0000);

    // Register 0 without the restart bit: note 9 goes on at 64 Hz, 12/32 semitone below note 0, and the second note FF,
    // after the rest, plays nothing. The bend range is 10 semitones, enough for the 300/32 between them.
    SEQLIB_REQUIRE_EQ(plain.notes.size(), 1);
    SEQLIB_CHECK(plain.bends == (std::vector<std::pair<uint32_t, int>>{{5, 8192 - 7680}}));

    const Square1 restarting = convert(0x8000);

    // With the restart bit, each note FF starts a note of its own, on the key of note 0, bent down by 12/32 semitone of
    // the 2-semitone bend range. The bend is already there for the second one.
    SEQLIB_REQUIRE_EQ(restarting.notes.size(), 3);
    SEQLIB_CHECK(restarting.bends == (std::vector<std::pair<uint32_t, int>>{{5, 8192 - 1536}}));
    const int key0 = plain.notes[0].second - 9;
    SEQLIB_CHECK(restarting.notes[1] == std::make_pair(uint32_t(5), key0));
    SEQLIB_CHECK(restarting.notes[2] == std::make_pair(uint32_t(15), key0));

    fs::remove_all(dir, ec);
}

void TestSampleKeys()
{
    // Voice 0 plays samples 5 and 6 (the same data) at two pitches each, so each gets a zone that spans them. Voices 1
    // and 2 play sample 5 at a single pitch, 50 semitones up and 128 down, and each of those gets a key of its own. The
    // sample has no clear pitch, so it's laid out as if its own pitch were C4.
    Image m = SyntheticImage();
    m.Put32(kSampleTable + 4 * 6, kSampleHeader);
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0x00}, {5, 0x20}, {6, 0x30}});
    m.Bytes(kBase, {0x00, 0xB7, 0x05, 0x00, 0x02, 0xB7, 0x05, 0x02, 0x02, 0xB7,
                    0x06, 0x0A, 0x02, 0xB7, 0x06, 0x0C, 0x02, 0xE0, 0x00, 0xFD});
    m.Bytes(kBase + 0x20, {0x00, 0xB7, 0x05, 0x32, 0x02, 0xE0, 0x00, 0xFD});
    m.Bytes(kBase + 0x30, {0x00, 0xB7, 0x05, 0x80, 0x02, 0xE0, 0x00, 0xFD});
    m.Bytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 3;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    const fs::path dir = TempPath("keys");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 2, opt, nullptr);

    // Voice 0's zones start out on keys 60-62 and 70-72. The keys between them are split halfway, and the outer ends
    // grow by an octave. Voice 1's key is one whose root key, 50 below it, is a MIDI key too. No key has a root key 128
    // above it, so voice 2 gets the nearest: key 0, with root key 127.
    const std::vector<std::array<int, 3>> kExpected = {{48, 66, 60}, {67, 84, 60}, {50, 50, 0}, {0, 0, 127}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(ReadZoneKeys(sum.sf2_path) == kExpected);
    fs::remove_all(dir, ec);
}

void TestLoopWithoutDelay()
{
    // Song 2 loops before a frame has passed, and song 3 after a note of 20 frames. Neither waits a frame after its
    // loop point, which would hang the driver. Each song stops there, with a warning that says why, however many times
    // the loop is to be played.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x40, {{4, 0}});
    m.Song(kSongTable, 3, kBase, 0x40, {{4, 0x20}});
    m.Bytes(kBase, {0x00, 0xA7, 0x05, 0x00, 0xFF, 0x01});
    m.Bytes(kBase + 0x20, {0x00, 0xA7, 0x05, 0x14, 0xF3, 0x00, 0x00, 0x00, 0xFF, 0x01});
    m.Bytes(kBase + 0x40, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 4;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    const fs::path dir = TempPath("hang");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    ConvertOptions looped = opt;
    looped.loops = 10;

    const SongSummary at_start = ConvertSong(rom, info, 2, opt, nullptr);
    const SongSummary at_start_looped = ConvertSong(rom, info, 2, looped, nullptr);
    const SongSummary later = ConvertSong(rom, info, 3, looped, nullptr);

    auto hangs = [](const SongSummary& s)
    {
        return s.warnings.size() == 1 && s.warnings[0].find("song loops without waiting a frame") != std::string::npos;
    };

    // Song 2 plays nothing, so it's skipped. Song 3 plays its note for 20 frames and stops on the next, without a loop.
    SEQLIB_CHECK(at_start.ok && at_start.silent && hangs(at_start));
    SEQLIB_CHECK(at_start_looped.ok && at_start_looped.silent && hangs(at_start_looped));
    SEQLIB_CHECK(later.ok && !later.silent && hangs(later));
    SEQLIB_CHECK_EQ(later.frames, 21);
    SEQLIB_CHECK_EQ(later.loop_end_frame, -1);
    fs::remove_all(dir, ec);
}

void TestFrameLimit()
{
    // Song 2 plays a note and loops every 2048 frames, and song 3 plays a note and waits 65535 frames twice before it
    // ends. Neither finishes within the frame limit, and song 2 only gets to play its loop 52 of the 100 times asked
    // for.
    Image m = SyntheticImage();
    constexpr uint32_t kBase = kRomBase + 0x600;
    m.Song(kSongTable, 2, kBase, 0x20, {{4, 0}});
    m.Song(kSongTable, 3, kBase, 0x20, {{4, 0x10}});
    m.Bytes(kBase, {0x00, 0xA7, 0x05, 0xF0, 0x00, 0x08, 0xFF, 0x01});
    m.Bytes(kBase + 0x10, {0x00, 0xA7, 0x05, 0xF0, 0xFF, 0xFF, 0xE0, 0xF0, 0xFF, 0xFF, 0xFE});
    m.Bytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 4;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    const fs::path dir = TempPath("limit");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);
    opt.loops = 100;

    const SongSummary looped = ConvertSong(rom, info, 2, opt, nullptr);
    const SongSummary long_song = ConvertSong(rom, info, 3, opt, nullptr);

    // Both are cut off at the limit, and the warnings say how far each got.
    SEQLIB_CHECK(looped.ok && looped.frames == int(kMaxFrames));
    SEQLIB_CHECK(looped.warnings ==
                 std::vector<std::string>{
                     "song reached the frame limit after playing its loop 52 of 100 times; output truncated"});
    SEQLIB_CHECK(long_song.ok && long_song.frames == int(kMaxFrames));
    SEQLIB_CHECK(long_song.warnings ==
                 std::vector<std::string>{"song didn't end or loop within the frame limit; output truncated"});
    fs::remove_all(dir, ec);
}

void TestTrackPastEnd()
{
    // Track 0 plays a note and waits 2 frames, and that delay is the ROM's last byte. The other tracks end at once.
    Image m(0x12A);
    constexpr uint32_t kTable = kRomBase + 0x100;
    m.Song(kTable, 0, kTable + kSongEntrySize, 0, {{0, 2}});
    m.Bytes(kTable + kSongEntrySize, {0x00, 0xFD, 0x00, 0xD7, 0x30, 0x02});
    const Rom rom = m.ToRom();
    UltimateMastersSequencer seq(rom, SongsAt(kTable), 0);

    for (int f = 0; f < 4; f++)
    {
        seq.Step();
    }

    // The track stops where its data runs out, and the warning says so.
    const std::vector<std::string>& warnings = seq.Warnings();
    SEQLIB_CHECK(warnings.size() == 1 && warnings[0].find("runs past the end of the ROM") != std::string::npos);
}

void TestManyPresets()
{
    // A song that needs 149 presets. The wave track plays 9 waves at 15 volumes, which makes 135 wave presets, and
    // every other track plays a note, so the song uses all 16 MIDI channels, channel 10 included.
    Image m = SyntheticImage();
    m.d.resize(0x2000);
    constexpr uint32_t kBase = kRomBase + 0x1000;
    std::vector<uint8_t> waves = {0x00};
    for (int w = 0; w < 9; w++)
    {
        waves.insert(waves.end(), {uint8_t(0x04 + w), 0x00}); // wave w
        for (int v = 1; v <= 15; v++)
        {
            waves.insert(waves.end(), {0xD8, uint8_t(v), 0x18, 0x01}); // note 24 at volume v, then wait a frame
        }
    }
    waves.insert(waves.end(), {0xE0, 0x00, 0xFD});
    m.Bytes(kBase, waves);
    m.Bytes(kBase + 0x300, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFD}); // the squares
    m.Bytes(kBase + 0x310, {0x00, 0xD3, 0x05, 0x04, 0xE0, 0x00, 0xFD});             // noise
    m.Bytes(kBase + 0x320, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xE0, 0x05, 0xFD});       // every sample voice
    m.Song(kSongTable, 3, kBase, 0x320, {{0, 0x300}, {1, 0x300}, {2, 0x000}, {3, 0x310}});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 4;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    const fs::path dir = TempPath("presets");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    // In a SoundFont of its own, the presets go on into bank 1. Only the preset of the track on channel 10, the last
    // one, gets a copy in the drum bank, so no two presets have the same bank and program.
    const SongSummary own = ConvertSong(rom, info, 3, opt, nullptr);

    const std::vector<PresetHeader> presets = ReadPresets(own.sf2_path);
    std::set<std::pair<int, int>> ids;
    for (const PresetHeader& p : presets)
    {
        ids.insert({p.bank, p.program});
    }

    SEQLIB_CHECK(own.ok);
    SEQLIB_REQUIRE_EQ(presets.size(), 150);
    SEQLIB_REQUIRE_EQ(ids.size(), presets.size());
    SEQLIB_CHECK(presets[128].bank == 1 && presets[128].program == 0);
    SEQLIB_CHECK(presets.back().bank == 128 && presets.back().name == "Voice 11");

    // In a shared SoundFont, the song's bank takes the first 128 presets, and the other 21 the free programs after song
    // 0's in bank 0. Banks 1 and 2 stay free for songs 1 and 2.
    SoundfontBuilder shared(rom, info, 4);
    SEQLIB_CHECK(ConvertSong(rom, info, 0, opt, &shared).ok);
    const int song0 = int(shared.File().presets.size());

    const SongSummary in_shared = ConvertSong(rom, info, 3, opt, &shared);

    const std::vector<sf2::Preset>& shared_presets = shared.File().presets;
    std::set<std::pair<int, int>> shared_ids;
    for (const sf2::Preset& p : shared_presets)
    {
        shared_ids.insert({p.bank, p.program});
    }
    SEQLIB_CHECK(in_shared.ok);
    SEQLIB_REQUIRE_EQ(shared_presets.size(), size_t(song0) + 149);
    SEQLIB_REQUIRE_EQ(shared_ids.size(), shared_presets.size());
    const sf2::Preset& first = shared_presets[size_t(song0)];
    const sf2::Preset& spilled = shared_presets[size_t(song0) + 128];
    SEQLIB_CHECK(first.bank == 3 && first.program == 0);
    SEQLIB_CHECK(spilled.bank == 0 && spilled.program == song0);
    fs::remove_all(dir, ec);
}

void TestDetectionScan()
{
    // Without the driver's code to go by, the tables are found in the data.
    DriverInfo info;
    std::string err;

    SEQLIB_CHECK(DetectDriver(ScanRom(), DriverOverrides(), info, err));

    SEQLIB_CHECK_EQ(info.song_table, kScanSongTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK_EQ(info.sample_table, kScanSampleTable);
    SEQLIB_CHECK(info.warnings == std::vector<std::string>{"mixer timer not found, assuming 21024 Hz"});

    int scanned = 0;
    for (const std::string& line : info.log)
    {
        scanned += line.find("(from data scan)") != std::string::npos;
    }
    SEQLIB_CHECK_EQ(scanned, 2);

    // Data that parses but isn't laid out like the driver's isn't taken for a song table: here, one byte between the
    // songs.
    SEQLIB_CHECK(!DetectDriver(ScanRom(1), DriverOverrides(), info, err));
    SEQLIB_CHECK(err.empty());

    // A table whose first song doesn't parse isn't taken, and neither is the rest of it after that song, which looks
    // like a table of its own. Song 0 uses F4 and runs up to song 1, and songs 1 and 2 are like ScanRom's.
    Image partial(0x400);
    constexpr uint32_t kTable = kRomBase + 0x100;
    partial.Song(kTable, 0, kRomBase + 0x16C, 0, {});
    partial.Bytes(kRomBase + 0x16C, {0x00, 0xF4, 0x00, 0xFD});
    partial.Song(kTable, 1, kRomBase + 0x190, 8, {{4, 0}});
    partial.Bytes(kRomBase + 0x190, {0x00, 0xA8, 0x10, 0x00, 0x0A, 0xE0, 0x05, 0xFE, 0x00, 0xFD});
    partial.Song(kTable, 2, kRomBase + 0x19A, 8, {{5, 0}});
    partial.Bytes(kRomBase + 0x19A, {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00, 0x00, 0xFD});

    SEQLIB_CHECK(!DetectDriver(partial.ToRom(), DriverOverrides(), info, err));
    SEQLIB_CHECK(err.empty());

    // But a table whose third song doesn't parse is taken, with its first two songs and a warning. Songs 0 and 1 play a
    // PSG note, and song 2 uses F4.
    Image later(0x400);
    later.Song(kTable, 0, kRomBase + 0x16C, 9, {{0, 0}});
    later.Bytes(kRomBase + 0x16C, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE, 0x00, 0xFD});
    later.Song(kTable, 1, kRomBase + 0x177, 9, {{0, 0}});
    later.Bytes(kRomBase + 0x177, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE, 0x00, 0xFD});
    later.Song(kTable, 2, kRomBase + 0x182, 0, {});
    later.Bytes(kRomBase + 0x182, {0x00, 0xF4, 0x00, 0xFD});

    DriverOverrides rate_only; // so that the mixer rate, which it has no code for, isn't a warning
    rate_only.mix_rate = 16000;

    const bool found = DetectDriver(later.ToRom(), rate_only, info, err);

    const std::vector<std::string> kWarnings = {
        "song 2's tracks don't read cleanly, so the song table is taken to end before it (--song-count overrides "
        "this)"};
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.song_table, kTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK(info.warnings == kWarnings);

    // The driver's code doesn't stop the scans when the tables it loads aren't in the ROM, and the log says why they
    // ran.
    Image outside = ScanImage();
    outside.Halfwords(kRomBase + 0x300, kPlaySongCode);
    outside.Put32(kRomBase + 0x318, 0x02000000);
    outside.Halfwords(kRomBase + 0x340, kNoteStartCode);
    outside.Put32(kRomBase + 0x354, 0x02000000);

    SEQLIB_CHECK(DetectDriver(outside.ToRom(), DriverOverrides(), info, err));

    auto logged = [&](const char* line)
    {
        return std::count(info.log.begin(), info.log.end(), line) == 1;
    };

    SEQLIB_CHECK_EQ(info.song_table, kScanSongTable);
    SEQLIB_CHECK_EQ(info.sample_table, kScanSampleTable);
    SEQLIB_CHECK(
        logged("the song table that the play-song code loads isn't in the ROM, scanning data for a song table"));
    SEQLIB_CHECK(
        logged("the sample table that the note-start code loads isn't in the ROM, scanning data for the sample table"));

    // A sample table whose entries aren't in order is only a guess, which is a warning.
    Image swapped = ScanImage();
    swapped.Put32(kScanSampleTable, kScanSampleTable + 52);
    swapped.Put32(kScanSampleTable + 4, kScanSampleTable + 8);

    SEQLIB_CHECK(DetectDriver(swapped.ToRom(), DriverOverrides(), info, err));
    SEQLIB_CHECK_EQ(info.sample_table, kScanSampleTable);
    SEQLIB_CHECK(std::count(info.warnings.begin(), info.warnings.end(),
                            "sample table 0x08000168 guessed from the data; check the result") == 1);

    // Noise isn't taken for the driver's data.
    Image noise(0x40000);
    uint32_t x = 1;
    for (uint8_t& b : noise.d)
    {
        b = uint8_t((x = x * 1103515245u + 12345u) >> 16);
    }

    SEQLIB_CHECK(!DetectDriver(noise.ToRom(), DriverOverrides(), info, err));
}

void TestSongTableFromOtherCode()
{
    // Code that works out song * 36 + table like the play-song routine, with other registers: lsls r0,r2,#3 /
    // adds r0,r0,r2 / lsls r0,r0,#2 / ldr r1,=table. Its ldr loads the address of ScanRom's song table from +0x10.
    constexpr uint32_t kCode = kRomBase + 0x300;
    const std::vector<uint16_t> kCodeHalfwords = {0x00D0, 0x1880, 0x0080, 0x4902};
    Image m = ScanImage();
    m.Halfwords(kCode, kCodeHalfwords);
    m.Put32(kCode + 0x10, kScanSongTable);
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    // The table is laid out like the driver's song data, so it's taken.
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.song_table, kScanSongTable);
    SEQLIB_CHECK(std::count(info.log.begin(), info.log.end(), "song table 0x08000100 (from play-song code)") == 1);

    // With a byte between its two songs, it isn't, although both songs parse and play notes. The data scan doesn't take
    // it either.
    Image gap = ScanImage(1);
    gap.Halfwords(kCode, kCodeHalfwords);
    gap.Put32(kCode + 0x10, kScanSongTable);

    const bool found_gap = DetectDriver(gap.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(!found_gap);
    SEQLIB_CHECK(err.empty());
}

void TestMixerRate()
{
    // The timer setup code, whose ldr at +0 loads the timer table's address from +0x14, and a timer table that sets
    // timer 0 to 0xFC00 without a prescaler, for 16777216 / 1024 Hz.
    Image m = ScanImage();
    constexpr uint32_t kTimerCode = kRomBase + 0x380;
    constexpr uint32_t kTimerTable = kRomBase + 0x3A0;
    m.Halfwords(kTimerCode, {0x4904, 0x0098, 0x1840, 0x6800, 0x2180, 0x0409, 0x4308, 0x6010});
    m.Put32(kTimerCode + 0x14, kTimerTable);
    m.Put32(kTimerTable, 0xFC00);
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    // The rate comes from the table, without a warning.
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.timer_table, kTimerTable);
    SEQLIB_CHECK(info.mix_rate == 16384.0);
    SEQLIB_CHECK(info.warnings.empty());

    // The rate is also detected when the code loads the setting into r3.
    m.Halfwords(kTimerCode + 6, {0x6803, 0x2080, 0x0400, 0x4318});

    const bool found_r3 = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_r3);
    SEQLIB_CHECK_EQ(info.timer_table, kTimerTable);
    SEQLIB_CHECK(info.mix_rate == 16384.0);
    SEQLIB_CHECK(info.warnings.empty());

    // A setting whose rate is out of range (256 Hz) isn't used, and the driver's usual rate, 16777216 / 798 Hz, is
    // assumed, with a warning.
    m.Put32(kTimerTable, 0);

    const bool found_slow = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_slow);
    SEQLIB_CHECK_EQ(info.timer_table, kTimerTable);
    SEQLIB_CHECK(info.mix_rate == 16777216.0 / 798);
    SEQLIB_CHECK(info.warnings ==
                 std::vector<std::string>{"mixer rate in the timer table is outside 4000-65536 Hz, assuming 21024 Hz"});
}

void TestBendDetection()
{
    // The end of the bend command's code. A driver that keeps the bend for later notes stores it next, with
    // strh r4,[r6,#0x1e].
    const std::vector<uint16_t> kBendCode = {0x8BB0, 0x4647, 0x80F8, 0x8830, 0x1900,
                                             0x8038, 0x7978, 0x2120, 0x4308, 0x7178};
    constexpr uint32_t kCode = kRomBase + 0x300;
    Image m = ScanImage();
    m.Halfwords(kCode, kBendCode);
    m.Put16(kCode + 20, 0x83F4);
    DriverInfo info;
    std::string err;

    const bool found_kept = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_kept && info.bend_kept);
    SEQLIB_CHECK(info.log.back() == "pitch bends carry over to later notes (from bend code)");

    // A driver that branches away instead only bends the note that's playing.
    m.Put16(kCode + 20, 0xE137);

    const bool found_once = DetectDriver(m.ToRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_once && !info.bend_kept);
    SEQLIB_CHECK(info.log.back() == "pitch bends apply to the playing note only (from bend code)");

    // Without the code, the bend is taken to carry over, and the log says that's an assumption.
    const bool found_without = DetectDriver(ScanRom(), DriverOverrides(), info, err);

    SEQLIB_CHECK(found_without && info.bend_kept);
    SEQLIB_CHECK(info.log.back() == "pitch bend code not recognised, assuming a bend carries over to later notes");
}

// Returns the start of a command reader in Thumb code: ldrb r5,[r3] / cmp r5,#0xFC / ble / cmp r5,#0xFF / bne, then
// `third` (a cmp) / bne / cmp r5,#0xEF / bgt, then `fifth` (a cmp) / bne.
std::vector<uint16_t> CommandReader(uint16_t third, uint16_t fifth)
{
    return {0x781D, 0x2DFC, 0xDD1E, 0x2DFF, 0xD107, third, 0xD100, 0x2DEF, 0xDC00, fifth, 0xD110};
}

void TestRevisionDetection()
{
    // SyntheticImage() with a command reader at kCode, and its tables given by hand.
    constexpr uint32_t kCode = kRomBase + 0x800;
    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 2;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;

    // The result of a detection: its return value, the driver info it filled in, and its error.
    struct Result
    {
        bool found;
        DriverInfo info;
        std::string error;
    };

    // Returns the result of detection with the reader that compares `third` and `fifth`, or with none if they're 0.
    auto detect = [&](uint16_t third, uint16_t fifth)
    {
        Image m = SyntheticImage();
        if (third)
        {
            m.Halfwords(kCode, CommandReader(third, fifth));
        }

        Result r;
        r.found = DetectDriver(m.ToRom(), ov, r.info, r.error);

        return r;
    };

    const Result wct2004 = detect(0x2DFE, 0x2DFA);
    const Result rave_master = detect(0x2DFE, 0x2DF5);
    const Result ultimate_masters = detect(0x2DFD, 0x2DFB);
    const Result eternal_duelist = detect(0x2DFE, 0x2DF3);
    const Result dungeon_dice = detect(0x2DFE, 0x2DFC);
    const Result older = detect(0x2DFE, 0x2DF9);
    const Result none = detect(0, 0);

    // A reader that compares FF and then FE is from an older revision than the Ultimate Masters revision's, which
    // compares FD. Of those, the WCT 2004 revision goes on with EF and FA, the Rave Master revision with EF and F5, the
    // Eternal Duelist revision with EF and F3, and the Dungeon Dice Monsters revision with EF and FC. The synthetic
    // image has none of the tables that the Dungeon Dice Monsters revision's notes read.
    SEQLIB_CHECK(wct2004.found && wct2004.info.revision == Revision::kWct2004);
    SEQLIB_CHECK(wct2004.info.log[0] == "WCT 2004 revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(rave_master.found && rave_master.info.revision == Revision::kRaveMaster);
    SEQLIB_CHECK(rave_master.info.log[0] ==
                 "Rave Master revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(eternal_duelist.found && eternal_duelist.info.revision == Revision::kEternalDuelist);
    SEQLIB_CHECK(!dungeon_dice.found && dungeon_dice.info.revision == Revision::kDungeonDiceMonsters);
    SEQLIB_CHECK(dungeon_dice.error ==
                 "found the command reader of the Dungeon Dice Monsters revision, but not the sample map or the "
                 "sample period table that its notes read");
    SEQLIB_CHECK(ultimate_masters.found && ultimate_masters.info.revision == Revision::kUltimateMasters);
    SEQLIB_CHECK(ultimate_masters.info.log[0] ==
                 "Ultimate Masters revision of the driver (from the command reader at 0x08000800)");
    SEQLIB_CHECK(!older.found);
    SEQLIB_CHECK(older.error ==
                 "this game has an older revision of Konami's driver with commands that agbsf2 can't read "
                 "(its command reader is at 0x08000800)");
    SEQLIB_CHECK(none.found && none.info.revision == Revision::kUltimateMasters);
    SEQLIB_CHECK(none.info.log[0] ==
                 "command reader not recognised, assuming the Ultimate Masters revision of the driver");

    // Without its tables given, a cartridge that holds only the command reader has no song table to find. The reader
    // still shows that the game has the driver, so detection fails with an error that names it, where a cartridge
    // without the reader shows no sign of the driver.
    Image reader_only(0x1000);
    reader_only.Halfwords(kCode, CommandReader(0x2DFE, 0x2DFA));
    DriverInfo info;
    std::string reader_error, blank_error;

    const bool found_reader_only = DetectDriver(reader_only.ToRom(), DriverOverrides(), info, reader_error);
    const bool found_blank = DetectDriver(Image(0x1000).ToRom(), DriverOverrides(), info, blank_error);

    SEQLIB_CHECK(!found_reader_only);
    SEQLIB_CHECK(reader_error ==
                 "found the command reader of the WCT 2004 revision of Konami's driver, at 0x08000800, but not "
                 "its song table (--song-table and --song-count override detection)");
    SEQLIB_CHECK(!found_blank && blank_error.empty());
}

void TestWct2004Decoding()
{
    // Delays: a byte up to DF, or two bytes that give 13 bits from E0 up.
    Rom delays;
    delays.Assign({0xDF, 0xE1, 0x23, 0xFF, 0xFF});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kWct2004, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xDF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kWct2004, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 3, Revision::kWct2004, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x1FFF);

    // FD, FE, FF / F0 8F / F2 90 / F3 / F4 20 / FB / D5 30 / C9 / A7 09 / B3 0A FE / 93 34 12 02 / 02 / 1E
    Rom rom;
    rom.Assign({0xFD, 0xFE, 0xFF, 0xF0, 0x8F, 0xF2, 0x90, 0xF3, 0xF4, 0x20, 0xFB, 0xD5, 0x30,
                0xC9, 0xA7, 0x09, 0xB3, 0x0A, 0xFE, 0x93, 0x34, 0x12, 0x02, 0x02, 0x1E});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kWct2004);
    };

    // The end commands take no argument: FD ends the track, FE loops the song and FF stops it.
    SEQLIB_CHECK(at(0, 4).op == Op::kEndTrack && at(0, 4).length == 1);
    SEQLIB_CHECK(at(1, 4).op == Op::kJump && at(1, 4).value == 1 && at(1, 4).length == 1);
    SEQLIB_CHECK(at(2, 4).op == Op::kJump && at(2, 4).value == 0 && at(2, 4).length == 1);

    // F0 holds two pan levels, a bend takes one byte whatever it is, F3 carries two more bytes on sample tracks, F4
    // takes one, and FB none.
    SEQLIB_CHECK(at(3, 4).op == Op::kPanLevels && at(3, 4).value == 0x8F && at(3, 4).length == 2);
    SEQLIB_CHECK(at(5, 4).op == Op::kPitchBend && at(5, 4).value == 0x50 && at(5, 4).length == 2);
    SEQLIB_CHECK(at(7, 0).length == 1 && at(7, 4).op == Op::kLoopPoint && at(7, 4).length == 3);
    SEQLIB_CHECK(at(8, 0).op == Op::kAttack && at(8, 0).value == 0x20 && at(8, 0).length == 2);
    SEQLIB_CHECK(at(10, 0).op == Op::kNop && at(10, 0).length == 1);

    // Notes and volumes take their volume from the opcode's low nibble.
    Command c = at(11, 0);
    SEQLIB_CHECK(c.op == Op::kPsgNote && c.vol == 5 && c.note == 0x30 && c.length == 2);
    c = at(13, 0);
    SEQLIB_CHECK(c.op == Op::kVolume && c.vol == 9 && c.length == 1);
    c = at(14, 4);
    SEQLIB_CHECK(c.op == Op::kNote && c.vol == 7 && c.sample == 9 && c.semitone == 0 && c.length == 2);
    c = at(16, 4);
    SEQLIB_CHECK(c.op == Op::kNote && c.vol == 3 && c.sample == 10 && c.semitone == -2 && c.length == 3);

    // 9x lo hi nn calls nn commands at hilo, and 00-8F set the duty from a low nibble up to 3, or the wave above it.
    c = at(19, 4);
    SEQLIB_CHECK(c.op == Op::kCall && c.value == 0x1234 && c.count == 2 && c.length == 4);
    c = at(23, 0);
    SEQLIB_CHECK(c.op == Op::kDuty && c.value == 0x80 && c.length == 1);
    c = at(24, 2);
    SEQLIB_CHECK(c.op == Op::kWave && c.value == 10 && c.length == 1);

    // The Rave Master revision's F3 takes no extra bytes, F6-FA and 00-7F do nothing, and 80-8F store the duty as it
    // is.
    Rom rave;
    rave.Assign({0xF3, 0xF7, 0x07, 0x82, 0x85, 0xF4, 0x20});
    auto rave_at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rave, kRomBase + offset, track, Revision::kRaveMaster);
    };

    SEQLIB_CHECK(rave_at(0, 4).op == Op::kLoopPoint && rave_at(0, 4).length == 1);
    SEQLIB_CHECK(rave_at(1, 4).op == Op::kNop && rave_at(1, 4).length == 1);
    SEQLIB_CHECK(rave_at(2, 0).op == Op::kNop && rave_at(2, 0).length == 1);
    SEQLIB_CHECK(rave_at(3, 0).op == Op::kDuty && rave_at(3, 0).value == 2);
    SEQLIB_CHECK(rave_at(4, 2).op == Op::kWave && rave_at(4, 2).value == 1);
    SEQLIB_CHECK(rave_at(5, 0).op == Op::kAttack && rave_at(5, 0).value == 0x20 && rave_at(5, 0).length == 2);
}

void TestRaveMasterSequencer()
{
    // A song of 12 tracks, with 28-byte song table entries. Track 4: pan left 8 right 15, note (vol 7, sample 7), wait
    // 32, loop the song. The other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 28;
    m.Put32(kTable, kBase);
    for (int t = 0; t < 12; t++)
    {
        m.Put16(kTable + 4 + 2 * uint32_t(t), t == 4 ? 0 : 0x10);
    }
    m.Bytes(kBase, {0x00, 0xF0, 0x8F, 0x00, 0xA7, 0x07, 0xE0, 0x20, 0xFE});
    m.Bytes(kBase + 0x10, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kRaveMaster;
    Wct2004Sequencer seq(rom, info, 0);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    for (int f = 0; f < 34; f++)
    {
        frames.push_back(seq.Step());
    }

    // Only the song's 12 tracks run. A loop centres the pan again, where WCT 2004 keeps it.
    SEQLIB_CHECK(seq.Valid());
    SEQLIB_CHECK(frames[0][11].active && !frames[0][12].active);
    SEQLIB_CHECK(frames[1][4].pan == 0x8F);
    SEQLIB_CHECK(seq.LoopsDone() == 1);
    SEQLIB_CHECK(frames[32][4].pan_start == 0xFF && frames[32][4].pan == 0x8F);
}

void TestWct2004Sequencer()
{
    // Song 0. Track 0: attack rate 0x40, PSG note (vol 8, note 0x30), wait 4, end. Track 4: a call of 1 command at
    // +0x0A, whose opcode also sets 75% duty, wait 5, note (vol 9, sample 7), wait 3, end, and then the fragment the
    // call plays: wait 2, note (vol 5, sample 11), wait 6. Track 5: pan left 8 right 15, note (vol 7, sample 7), wait
    // 32, loop the song. The other tracks end at once. Song 1: track 4 plays a note, waits 3 and stops the song.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 2 * kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x40, {{0, 0x00}, {4, 0x10}, {5, 0x30}});
    m.Bytes(kBase + 0x00, {0x00, 0xF4, 0x40, 0x00, 0xD8, 0x30, 0x04, 0xFD});
    m.Bytes(kBase + 0x10, {0x00, 0x93, 0x0A, 0x00, 0x01, 0x05, 0xA9, 0x07, 0x03, 0xFD, 0x02, 0xA5, 0x0B, 0x06});
    m.Bytes(kBase + 0x30, {0x00, 0xF0, 0x8F, 0x00, 0xA7, 0x07, 0xE0, 0x20, 0xFE});
    m.Bytes(kBase + 0x40, {0x00, 0xFD});
    m.Song(kTable, 1, kBase + 0x50, 0x10, {{4, 0x00}});
    m.Bytes(kBase + 0x50, {0x00, 0xA7, 0x07, 0x03, 0xFF});
    m.Bytes(kBase + 0x60, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kWct2004;
    Wct2004Sequencer seq(rom, info, 0);
    Wct2004Sequencer stopping(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    int looped_at = -1;
    for (int f = 0; f < 40; f++)
    {
        frames.push_back(seq.Step());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    // The attack takes the note's volume up from 2 to 8 over four frames, restarting the channel at each level.
    SEQLIB_CHECK(frames[0][0].flags == kOutPsgNote && frames[0][0].pitch == 0x600);
    for (int f = 0; f < 4; f++)
    {
        SEQLIB_CHECK_EQ(frames[f][0].vol, 2 * (f + 1));
        SEQLIB_CHECK_EQ(frames[f][0].trig, 1);
    }
    SEQLIB_CHECK_EQ(frames[4][0].flags, kOutStop);

    // The call sets the duty byte, waits 2, plays the fragment's note, and then waits the 5 after the call instead of
    // the 6 in the fragment.
    SEQLIB_CHECK_EQ(frames[0][4].b2, 0xC0);
    SEQLIB_CHECK(frames[2][4].flags == kOutNoteOn && frames[2][4].key == 11 && frames[2][4].vol == 5);
    SEQLIB_CHECK(frames[7][4].flags == kOutNoteOn && frames[7][4].key == 7 && frames[7][4].vol == 9);
    SEQLIB_CHECK_EQ(frames[10][4].flags, kOutStop);

    // The record holds the pan byte, and the byte as the frame started. A loop keeps the pan, and the attack starts
    // again.
    SEQLIB_CHECK(frames[0][5].pan == 0x8F && frames[0][5].pan_start == 0xFF);
    SEQLIB_CHECK(frames[1][5].pan == 0x8F && frames[1][5].pan_start == 0x8F);
    SEQLIB_CHECK_EQ(looped_at, 32);
    SEQLIB_CHECK(frames[32][5].pan == 0x8F && frames[32][5].pan_start == 0x8F);
    SEQLIB_CHECK_EQ(frames[32][0].vol, 2);

    for (int f = 0; f < 4; f++)
    {
        stopping.Step();
    }

    // FF stops the song.
    SEQLIB_CHECK(stopping.Stopped());
}

// --dump lists the commands that a call plays after the call's line, and counts their delays in the frames after the
// call. The call's duty reads like the duty of 80-83. Rave Master stores it as it is. 93 therefore gives 12.5% there.
void TestCallDump()
{
    // Square 1: a call of 1 command at +0x0A that also sets the duty, wait 5, PSG note (vol 9, note 0x30), wait 3, end,
    // and then the fragment: wait 2, PSG note (vol 5, note 0x30), wait 6. The other tracks end at once.
    Image m(0x200);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kTable + 2 * kSongEntrySize;
    m.Song(kTable, 0, kBase, 0x20, {{0, 0x00}});
    m.Bytes(kBase, {0x00, 0x93, 0x0A, 0x00, 0x01, 0x05, 0xD9, 0x30, 0x03, 0xFD, 0x02, 0xD5, 0x30, 0x06});
    m.Bytes(kBase + 0x20, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    std::string error;

    std::string listings[2];
    for (const Revision revision : {Revision::kWct2004, Revision::kRaveMaster})
    {
        info.revision = revision;
        const std::string path = Utf8(TempPath("call_dump.txt"));
        SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));
        const std::vector<uint8_t> text = ReadAll(path);
        listings[revision == Revision::kRaveMaster].assign(text.begin(), text.end());
    }

    const auto line_at = [](const std::string& s, uint32_t addr)
    {
        char key[16];
        std::snprintf(key, sizeof key, "0x%08X", addr);
        const size_t at = s.find(key);
        return at == std::string::npos ? std::string() : s.substr(at, s.find('\n', at) - at);
    };
    for (const std::string& s : listings)
    {
        SEQLIB_CHECK(line_at(s, kBase + 0x01).find("  0  93 0A 00 01 ") != std::string::npos);
        SEQLIB_CHECK(line_at(s, kBase + 0x01).ends_with(" wait 2"));
        SEQLIB_CHECK(line_at(s, kBase + 0x0B).find("  2  D5 30 ") != std::string::npos);
        SEQLIB_CHECK(line_at(s, kBase + 0x0B).ends_with(" wait 5"));
        SEQLIB_CHECK(line_at(s, kBase + 0x06).find("  7  D9 30 ") != std::string::npos);
        SEQLIB_CHECK(line_at(s, kBase + 0x09).find(" 10  FD ") != std::string::npos);
    }
    SEQLIB_CHECK(listings[0].find("call       1 commands at +0x000A, duty 75%") != std::string::npos);
    SEQLIB_CHECK(listings[1].find("call       1 commands at +0x000A, duty 12.5%") != std::string::npos);
}

void TestWct2004Panning()
{
    // Song 0: square 1 pans left 8 right 15, plays PSG note 0x30 at volume 12, waits 8 and ends. Square 2 plays note
    // 0x20 at volume 5. A WCT 2004 command reader is at kCode, and the tables are given by hand.
    Image m = SyntheticImage();
    constexpr uint32_t kCode = kRomBase + 0x800;
    m.Halfwords(kCode, CommandReader(0x2DFE, 0x2DFA));
    m.Song(kSongTable, 0, kSongBase, 0x60, {{0, 0x00}, {1, 0x20}});
    m.Bytes(kSongBase + 0x00, {0x00, 0xF0, 0x8F, 0x00, 0xDC, 0x30, 0x08, 0xFD});
    m.Bytes(kSongBase + 0x20, {0x00, 0xD5, 0x20, 0x10, 0xFD});
    m.Bytes(kSongBase + 0x60, {0x00, 0xFD});
    const Rom rom = m.ToRom();

    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 1;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err) && info.revision == Revision::kWct2004);

    const fs::path dir = TempPath("wct2004_pan");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each square's note-ons (key), and the pan (CC10) each starts with, the default of 64 if its track has none yet.
    std::map<std::string, std::vector<std::pair<int, int>>> notes;
    std::map<std::string, int> pan;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        if ((e.status & 0xF0) == 0xB0 && e.data1 == cc::kPan)
        {
            pan[e.track] = e.data2;
        }
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
        {
            notes[e.track].emplace_back(e.data1, pan.count(e.track) ? pan[e.track] : 64);
        }
    }

    // Square 1 plays its right side, and square 2 its left side, a copy of square 1's note in place of its own.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(notes["Square 1"].size(), 1);
    SEQLIB_REQUIRE_EQ(notes["Square 2"].size(), 1);
    SEQLIB_CHECK_EQ(notes["Square 1"][0].second, 127);
    SEQLIB_CHECK_EQ(notes["Square 2"][0].second, 0);
    SEQLIB_CHECK_EQ(notes["Square 2"][0].first, notes["Square 1"][0].first);

    fs::remove_all(dir, ec);
}

// Writes entry `song` of a song table at `table` for the Eternal Duelist revision, whose songs have 10 tracks: the
// song's base address, and the offset of every track, which is `rest` unless `offsets` gives another.
void EternalDuelistSong(Image& m, uint32_t table, int song, uint32_t base, uint16_t rest,
                        const std::vector<std::pair<int, int>>& offsets)
{
    const uint32_t e = table + uint32_t(song) * SongEntrySize(Revision::kEternalDuelist);
    m.Put32(e, base);
    for (int t = 0; t < TrackCount(Revision::kEternalDuelist); t++)
    {
        m.Put16(e + 4 + 2 * uint32_t(t), rest);
    }
    for (const auto& [t, o] : offsets)
    {
        m.Put16(e + 4 + 2 * uint32_t(t), uint16_t(o));
    }
}

void TestEternalDuelistDecoding()
{
    // Delays: a byte up to EF, or two bytes that give 12 bits from F0 up.
    Rom delays;
    delays.Assign({0xEF, 0xF1, 0x23, 0xFF, 0xFF});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kEternalDuelist, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xEF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kEternalDuelist, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 3, Revision::kEternalDuelist, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0xFFF);

    // F0 5A / F4 20 / FC 06 / F3 / 07 / 82
    Rom rom;
    rom.Assign({0xF0, 0x5A, 0xF4, 0x20, 0xFC, 0x06, 0xF3, 0x07, 0x82});
    auto at = [&](uint32_t offset, int track)
    {
        return DecodeCommand(rom, kRomBase + offset, track, Revision::kEternalDuelist);
    };

    // F0 sets a PSG track's NR51 bits, or a sample track's volume and the next one's. F4-FC are vibrato, as F1 is.
    SEQLIB_CHECK(at(0, 3).op == Op::kPsgPan && at(0, 3).value == 0x5A && at(0, 3).length == 2);
    SEQLIB_CHECK(at(0, 4).op == Op::kPairVolumes && at(0, 4).value == 0x5A && at(0, 4).length == 2);
    SEQLIB_CHECK(at(2, 4).op == Op::kVibrato && at(2, 4).value == 0x20 && at(2, 4).length == 2);
    SEQLIB_CHECK(at(4, 0).op == Op::kVibrato && at(4, 0).value == 0x06 && at(4, 0).length == 2);

    // As in the Rave Master revision, F3 takes no extra bytes, 00-7F do nothing, and 80-8F store the duty as it is.
    SEQLIB_CHECK(at(6, 4).op == Op::kLoopPoint && at(6, 4).length == 1);
    SEQLIB_CHECK(at(7, 0).op == Op::kNop && at(7, 0).length == 1);
    SEQLIB_CHECK(at(8, 0).op == Op::kDuty && at(8, 0).value == 2);
}

void TestEternalDuelistSequencer()
{
    // Song 0. Square 1: NR51 bits 10, loop point, PSG note (vol 8, note 0x30), wait 4, note 0x24 at the same volume,
    // wait 4, note 0x24 at volume 10, wait 4, end. Voice 1: note (vol 12, sample 7), wait 4, volume 10 and 5 for the
    // next voice, wait 4, end. Voice 2: note (vol 8, sample 7), wait 4, vibrato, wait 4, note (vol 9, sample 11), wait
    // 4, end. Voice 3: rest, wait 32, loop the song.
    Image m(0x800);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kRomBase + 0x200;
    EternalDuelistSong(m, kTable, 0, kBase, 0x60, {{0, 0x00}, {5, 0x20}, {6, 0x30}, {7, 0x50}});
    m.Bytes(kBase + 0x00,
            {0x00, 0xF0, 0x10, 0x00, 0xF3, 0x00, 0xD8, 0x30, 0x04, 0xD8, 0x24, 0x04, 0xDA, 0x24, 0x04, 0xFD});
    m.Bytes(kBase + 0x20, {0x00, 0xAC, 0x07, 0x04, 0xF0, 0x5A, 0x04, 0xFD});
    m.Bytes(kBase + 0x30, {0x00, 0xA8, 0x07, 0x04, 0xF1, 0x20, 0x04, 0xA9, 0x0B, 0x04, 0xFD});
    m.Bytes(kBase + 0x50, {0x00, 0xE0, 0x20, 0xFE});
    m.Bytes(kBase + 0x60, {0x00, 0xFD});

    // Song 1. Voice 0: a call of 1 command from the fragment after the end, wait 2, note (vol 9, sample 7), wait 1,
    // then 260 commands that do nothing, each followed by a wait of 1, and the end. The fragment: wait 3, note (vol 5,
    // sample 11), wait 6.
    constexpr uint32_t kBase1 = kRomBase + 0x280;
    std::vector<uint8_t> calls = {0x00, 0x9F, 0x00, 0x00, 0x01, 0x02, 0xA9, 0x07, 0x01};
    for (int i = 0; i < 260; i++)
    {
        calls.insert(calls.end(), {0x07, 0x01});
    }
    calls.push_back(0xFD);
    calls[2] = uint8_t(calls.size());
    calls[3] = uint8_t(calls.size() >> 8);
    calls.insert(calls.end(), {0x03, 0xA5, 0x0B, 0x06});
    EternalDuelistSong(m, kTable, 1, kBase1, 0x220, {{4, 0x00}});
    m.Bytes(kBase1, calls);
    m.Bytes(kBase1 + 0x220, {0x00, 0xFD});
    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kEternalDuelist;
    Wct2004Sequencer seq(rom, info, 0);
    Wct2004Sequencer calling(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    int looped_at = -1;
    for (int f = 0; f < 34; f++)
    {
        frames.push_back(seq.Step());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    std::vector<int> starts;
    for (int f = 0; f < 270; f++)
    {
        if (calling.Step()[4].flags == kOutNoteOn)
        {
            starts.push_back(f);
        }
    }

    // Square 1's F0 goes into its record at once. A note at the volume that's playing only changes the pitch, and a new
    // volume retriggers the channel.
    SEQLIB_CHECK(seq.Valid() && frames[0][9].active);
    SEQLIB_CHECK(frames[0][0].pan == 0x10 && frames[0][0].trig == 1);
    SEQLIB_CHECK(frames[4][0].flags == kOutPsgNote && frames[4][0].trig == 0 && frames[4][0].pitch == 0x480);
    SEQLIB_CHECK(frames[8][0].trig == 1 && frames[8][0].vol == 10);

    // F0 on voice 1 sets its own volume, and voice 2's through the next record.
    SEQLIB_CHECK(frames[4][5].vol == 10 && frames[4][5].trig == 1);
    SEQLIB_CHECK(frames[4][6].vol == 5 && frames[4][6].trig == 1);

    // Vibrato replaces the flags of voice 2's second note, so the note doesn't start.
    SEQLIB_CHECK(frames[8][6].flags == kOutPsgNote && frames[8][6].key == 11);

    // The loop sets square 1's pan back to the default, and the track goes on from its loop point, after its F0.
    SEQLIB_CHECK_EQ(looped_at, 32);
    SEQLIB_CHECK_EQ(frames[32][0].pan, 0x11);

    // The driver keeps the call's return position, so the 256th command after the call takes the track back there.
    SEQLIB_CHECK(starts == std::vector<int>({3, 5, 262}));
}

void TestEternalDuelistConversion()
{
    // Song 0 of the Eternal Duelist revision. Square 1: PSG note (vol 8, note 0x30), wait 4, notes 0x10 and 0x50 at the
    // same volume, 4 frames each, rest, wait 4, end. Wave: wave 2, note (vol 8, note 0x20), wait 4, note 0x24 at the
    // same volume, wait 4, rest, wait 4, end. Voice 0: note (vol 8, sample 5), wait 8, note (vol 8, sample 5, +7), a
    // bend of 0 in the same frame, wait 4, rest, wait 4, end. Voice 1 does the same, but its second note plays sample
    // 6, at semitone 0. An Eternal Duelist command reader is at kCode, and the tables are given by hand.
    Image m = SyntheticImage();
    constexpr uint32_t kCode = kRomBase + 0x800;
    constexpr uint32_t kWaveTable = kRomBase + 0x900;
    m.Halfwords(kCode, CommandReader(0x2DFE, 0x2DF3));
    EternalDuelistSong(m, kSongTable, 0, kSongBase, 0x60, {{0, 0x00}, {2, 0x20}, {4, 0x40}, {5, 0x70}});
    m.Bytes(kSongBase + 0x00, {0x00, 0xD8, 0x30, 0x04, 0xD8, 0x10, 0x04, 0xD8, 0x50, 0x04, 0xE0, 0x04, 0xFD});
    m.Bytes(kSongBase + 0x20, {0x00, 0x86, 0x00, 0xD8, 0x20, 0x04, 0xD8, 0x24, 0x04, 0xE0, 0x04, 0xFD});
    m.Bytes(kSongBase + 0x40, {0x00, 0xA8, 0x05, 0x08, 0xB8, 0x05, 0x07, 0x00, 0xF2, 0x40, 0x04, 0xE0, 0x04, 0xFD});
    m.Bytes(kSongBase + 0x60, {0x00, 0xFD});
    m.Bytes(kSongBase + 0x70, {0x00, 0xA8, 0x05, 0x08, 0xA8, 0x06, 0x00, 0xF2, 0x40, 0x04, 0xE0, 0x04, 0xFD});

    // Sample 6: sample 5's data, at twice its step.
    constexpr uint32_t kHeader6 = kRomBase + 0x600;
    m.Put32(kSampleTable + 4 * 6, kHeader6);
    m.Put32(kHeader6, 0x2000);
    m.Put32(kHeader6 + 4, 64);
    m.Put32(kHeader6 + 8, 0xFFFFFFFF);
    std::copy_n(m.d.begin() + (kSampleHeader + 12 - kRomBase), 64, m.d.begin() + (kHeader6 + 12 - kRomBase));

    // Waves 0 and 2 at volume 8: a square and a sawtooth.
    for (int i = 0; i < 16; i++)
    {
        m.d[kWaveTable - kRomBase + 8 * 16 + uint32_t(i)] = i < 8 ? 0x88 : 0x00;
        m.d[kWaveTable - kRomBase + 40 * 16 + uint32_t(i)] = uint8_t(i * 0x11);
    }

    const Rom rom = m.ToRom();
    DriverOverrides ov;
    ov.song_table = kSongTable;
    ov.song_count = 1;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;
    SEQLIB_CHECK(DetectDriver(rom, ov, info, err) && info.revision == Revision::kEternalDuelist);
    info.wave_table = kWaveTable;

    const fs::path dir = TempPath("eternal_duelist");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each track's note-ons (frame, program), bends (frame, value) and bend range.
    std::map<std::string, std::vector<std::pair<uint32_t, int>>> notes, bends;
    std::map<std::string, int> program, range;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        const int kind = e.status & 0xF0;
        if (kind == 0xC0)
        {
            program[e.track] = e.data1;
        }
        if (kind == 0x90 && e.data2 > 0)
        {
            notes[e.track].emplace_back(e.frame, program[e.track]);
        }
        if (kind == 0xE0 && e.frame > 0)
        {
            bends[e.track].emplace_back(e.frame, e.data1 | (e.data2 << 7));
        }
        if (kind == 0xB0 && e.data1 == cc::kDataEntry)
        {
            range[e.track] = e.data2;
        }
    }

    // Square 1's legato notes bend its first note by 32 semitones in either direction. The bend range must cover both.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(notes["Square 1"].size(), 1);
    SEQLIB_CHECK_EQ(range["Square 1"], 32);
    SEQLIB_CHECK(bends["Square 1"] == (std::vector<std::pair<uint32_t, int>>{{4, 0}, {8, 16383}}));

    // The wave's legato note reloads wave 0, which MIDI plays as a new note.
    SEQLIB_REQUIRE_EQ(notes["Wave"].size(), 2);
    SEQLIB_CHECK_EQ(notes["Wave"][1].first, 4);
    SEQLIB_CHECK(notes["Wave"][1].second != notes["Wave"][0].second);

    // Voice 0's second note doesn't start, and its first plays on at the second's pitch, 7 semitones up.
    SEQLIB_REQUIRE_EQ(notes["Voice 0"].size(), 1);
    SEQLIB_CHECK_EQ(range["Voice 0"], 7);
    SEQLIB_CHECK(bends["Voice 0"] == (std::vector<std::pair<uint32_t, int>>{{8, 16383}}));

    // Voice 1's first note plays on as well, an octave up, at the step of sample 6 at semitone 0.
    SEQLIB_REQUIRE_EQ(notes["Voice 1"].size(), 1);
    SEQLIB_CHECK_EQ(range["Voice 1"], 12);
    SEQLIB_CHECK(bends["Voice 1"] == (std::vector<std::pair<uint32_t, int>>{{8, 16383}}));

    fs::remove_all(dir, ec);
}

// Writes entry `song` of a song table at `table` for the Dungeon Dice Monsters revision, whose songs have 8 tracks: the
// song's base address, and the offset of every track, which is `rest` unless `offsets` gives another.
void DungeonDiceSong(Image& m, uint32_t table, int song, uint32_t base, uint16_t rest,
                     const std::vector<std::pair<int, int>>& offsets)
{
    const uint32_t e = table + uint32_t(song) * SongEntrySize(Revision::kDungeonDiceMonsters);
    m.Put32(e, base);
    for (int t = 0; t < TrackCount(Revision::kDungeonDiceMonsters); t++)
    {
        m.Put16(e + 4 + 2 * uint32_t(t), rest);
    }
    for (const auto& [t, o] : offsets)
    {
        m.Put16(e + 4 + 2 * uint32_t(t), uint16_t(o));
    }
}

void TestDungeonDiceDecoding()
{
    // Delays: a byte up to EF, or two bytes that give 12 bits from F0 up, as in the Eternal Duelist revision.
    Rom delays;
    delays.Assign({0xEF, 0xF1, 0x23});
    uint32_t delay = 0;

    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 0, Revision::kDungeonDiceMonsters, delay), 1);
    SEQLIB_CHECK_EQ(delay, 0xEF);
    SEQLIB_CHECK_EQ(ReadDelay(delays, kRomBase + 1, Revision::kDungeonDiceMonsters, delay), 2);
    SEQLIB_CHECK_EQ(delay, 0x123);

    // E5 30 / B9 0C / A2 3C / 9A 18 / C7 / D6 / 84 / 71 / 7D / 45 / F0 53 / F1 30 / F2 10 / F3 30 / F4 34 12 05 /
    // F5 34 12 05 / F7 / F9 / FA / FB / FC / FD / FE / FF
    Rom rom;
    rom.Assign({0xE5, 0x30, 0xB9, 0x0C, 0xA2, 0x3C, 0x9A, 0x18, 0xC7, 0xD6, 0x84, 0x71, 0x7D,
                0x45, 0xF0, 0x53, 0xF1, 0x30, 0xF2, 0x10, 0xF3, 0x30, 0xF4, 0x34, 0x12, 0x05,
                0xF5, 0x34, 0x12, 0x05, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF});
    auto at = [&](uint32_t offset)
    {
        return DecodeCommand(rom, kRomBase + offset, 4, Revision::kDungeonDiceMonsters);
    };

    // The notes: Ex nn on the PSG, Bx nn from the sample map, Ax nn the same on the next track, and 9x nn the track's
    // sample at note nn, each at volume x. Cx restarts the note at volume x, and Dx only sets the volume.
    SEQLIB_CHECK(at(0).op == Op::kPsgNote && at(0).vol == 5 && at(0).note == 0x30 && at(0).length == 2);
    SEQLIB_CHECK(at(2).op == Op::kNote && at(2).vol == 9 && at(2).sample == 0x0C && at(2).length == 2);
    SEQLIB_CHECK(at(4).op == Op::kNextNote && at(4).vol == 2 && at(4).sample == 0x3C);
    SEQLIB_CHECK(at(6).op == Op::kSampleAtNote && at(6).vol == 10 && at(6).note == 0x18);
    SEQLIB_CHECK(at(8).op == Op::kVolume && at(8).vol == 7 && at(8).length == 1);
    SEQLIB_CHECK(at(9).op == Op::kSetVolume && at(9).vol == 6);

    // 8x is vibrato at depth x, 70-7B load a wave, 7C-7F set a duty, and 00-6F set the duty byte itself.
    SEQLIB_CHECK(at(10).op == Op::kVibrato && at(10).value == 4);
    SEQLIB_CHECK(at(11).op == Op::kWave && at(11).value == 1);
    SEQLIB_CHECK(at(12).op == Op::kDuty && at(12).value == 0x40);
    SEQLIB_CHECK(at(13).op == Op::kDuty && at(13).value == 0x45);

    // F0 sets NR50, F1 and F2 bend a sample note, of the next track and this one, and F3 bends a PSG note, all in
    // sixteenths from the note.
    SEQLIB_CHECK(at(14).op == Op::kPsgVolume && at(14).value == 0x53 && at(14).length == 2);
    SEQLIB_CHECK(at(16).op == Op::kSampleBend && at(16).opcode == 0xF1 && at(16).value == 16);
    SEQLIB_CHECK(at(18).op == Op::kSampleBend && at(18).opcode == 0xF2 && at(18).value == -16);
    SEQLIB_CHECK(at(20).op == Op::kPitchBend && at(20).value == 16 && at(20).length == 2);

    // F4 and F5 call commands of the track's data. F5's fifth byte is read after the call.
    SEQLIB_CHECK(at(22).op == Op::kCall && at(22).value == 0x1234 && at(22).count == 5 && at(22).length == 4);
    SEQLIB_CHECK(at(26).op == Op::kCall && at(26).opcode == 0xF5 && at(26).length == 4);

    // F7 sets a flag that nothing reads, F9 fades the next track out, FA is a loop point with two bytes after it, FB
    // releases the note, FC rests, FD ends the track, FE loops the song and FF stops it.
    SEQLIB_CHECK(at(30).op == Op::kNop);
    SEQLIB_CHECK(at(31).op == Op::kFade && at(31).opcode == 0xF9);
    SEQLIB_CHECK(at(32).op == Op::kLoopPoint && at(32).length == 3);
    SEQLIB_CHECK(at(33).op == Op::kRelease);
    SEQLIB_CHECK(at(34).op == Op::kRest);
    SEQLIB_CHECK(at(35).op == Op::kEndTrack);
    SEQLIB_CHECK(at(36).op == Op::kJump && at(36).value == 1);
    SEQLIB_CHECK(at(37).op == Op::kJump && at(37).value == 0);
}

void TestDungeonDiceSequencer()
{
    // Song 0. Square 1: NR50 53, note (vol 8, note 0x20), wait 4, vibrato at depth 2 and note 0x21, wait 12, fade out,
    // wait 6, end. Noise: note (vol 4, note 0x48), wait 1, end. Voice 0: note (vol 12, map entry 1) and a note on voice
    // 1 (vol 6, map entry 2), wait 4, fade voice 1 out, wait 8, a bend of +16 from sample 5's rate, wait 4, note 0x1C
    // of the track's sample (vol 9) and a bend of +2 in the same frame, wait 4, end. Square 2: wait 2, loop point with
    // a wait of 0 and duty 75% for the loop, wait 2, note (vol 7, note 0x10), wait 18, loop the song.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kBase = kRomBase + 0x200;
    constexpr uint32_t kMap = kRomBase + 0x600;
    constexpr uint32_t kPeriods = kRomBase + 0x800;
    DungeonDiceSong(m, kTable, 0, kBase, 0x60, {{0, 0x00}, {1, 0x40}, {3, 0x20}, {4, 0x28}});
    m.Bytes(kBase + 0x00, {0x00, 0xF0, 0x53, 0x00, 0xE8, 0x20, 0x04, 0x82, 0x00, 0xE8, 0x21, 0x0C, 0xF8, 0x06, 0xFD});
    m.Bytes(kBase + 0x20, {0x00, 0xE4, 0x48, 0x01, 0xFD});
    m.Bytes(kBase + 0x28, {0x00, 0xBC, 0x01, 0x00, 0xA6, 0x02, 0x04, 0xF9, 0x08, 0xF2, 0x30, 0x04, 0x99, 0x1C, 0x00,
                           0xF2, 0x22, 0x04, 0xFD});
    m.Bytes(kBase + 0x40, {0x02, 0xFA, 0x00, 0x7F, 0x02, 0xE7, 0x10, 0x12, 0xFE});
    m.Bytes(kBase + 0x60, {0x00, 0xFD});
    m.Bytes(kMap, {0x00, 0x05, 0x07});
    for (uint32_t p = 0; p < 0x200; p++)
    {
        m.Put16(kPeriods + 2 * p, uint16_t(0x4000 - p));
    }

    // Song 1. Square 1: a call of 1 command from the fragment after the end, with duty byte 10 first, which is also the
    // wait after the call, note (vol 9, note 0x20), then 260 duty bytes of 07, each followed by a wait of 1, and a
    // stop. The fragment: wait 3, note (vol 5, note 0x24), wait 6.
    constexpr uint32_t kBase1 = kRomBase + 0x280;
    std::vector<uint8_t> calls = {0x00, 0xF5, 0x00, 0x00, 0x01, 0x10, 0xE9, 0x20, 0x01};
    for (int i = 0; i < 260; i++)
    {
        calls.insert(calls.end(), {0x07, 0x01});
    }
    calls.push_back(0xFF);
    calls[2] = uint8_t(calls.size());
    calls[3] = uint8_t(calls.size() >> 8);
    calls.insert(calls.end(), {0x03, 0xE5, 0x24, 0x06});
    DungeonDiceSong(m, kTable, 1, kBase1, 0x260, {{0, 0x00}});
    m.Bytes(kBase1, calls);
    m.Bytes(kBase1 + 0x260, {0x00, 0xFD});

    const Rom rom = m.ToRom();
    DriverInfo info = SongsAt(kTable);
    info.revision = Revision::kDungeonDiceMonsters;
    info.sample_map = kMap;
    info.sample_period_table = kPeriods;
    info.vibrato_entry = 1000;
    DungeonDiceSequencer seq(rom, info, 0);
    DungeonDiceSequencer calling(rom, info, 1);

    std::vector<std::array<TrackOutput, kTracks>> frames;
    std::vector<int> psg_volume;
    int looped_at = -1;
    for (int f = 0; f < 26; f++)
    {
        frames.push_back(seq.Step());
        psg_volume.push_back(seq.PsgVolume());
        looped_at = looped_at < 0 && seq.LoopedLastFrame() ? f : looped_at;
    }

    std::vector<std::pair<int, int>> starts; // (frame, duty byte) of each of square 1's notes
    int stopped_at = -1;
    for (int f = 0; f < 300 && stopped_at < 0; f++)
    {
        const TrackOutput& o = calling.Step()[0];
        if (o.trig && o.flags == 1 && o.vol)
        {
            starts.emplace_back(f, o.b2);
        }
        stopped_at = calling.Stopped() ? f : -1;
    }

    // Pitches go up 16 entries a note until note 71, and one entry a note from note 72 on. F0 sets NR50.
    SEQLIB_CHECK(seq.Valid() && frames[0][7].active);
    SEQLIB_CHECK(frames[0][0].pitch == 0x200 && frames[0][0].flags == 1 && frames[0][0].vol == 8);
    SEQLIB_CHECK_EQ(frames[0][3].pitch, 0x480);
    SEQLIB_CHECK_EQ(psg_volume[0], 0x53);

    // Every 4th frame, vibrato outputs a pitch from the table's vibrato section, alternating with 2 entries below it.
    SEQLIB_CHECK(frames[6][0].flags == 0 && frames[7][0].flags == 1 && frames[7][0].trig == 0);
    SEQLIB_CHECK_EQ(frames[7][0].pitch, 1000 + 0x84 - 2);
    SEQLIB_CHECK_EQ(frames[11][0].pitch, 1000 + 0x84);

    // The fade takes the volume down a quarter a frame, and ends by handing the channel a duty byte and volume of 0.
    SEQLIB_CHECK(frames[16][0].vol == 6 && frames[17][0].vol == 4 && frames[18][0].vol == 2);
    SEQLIB_CHECK(frames[19][0].vol == 0 && frames[19][0].trig == 1 && frames[19][0].b2 == 0);

    // Voice 0's note plays sample 5, and its note on voice 1 goes into voice 1's record. F9 fades voice 1 out from the
    // next frame on, since voice 1's track has had its turn.
    SEQLIB_CHECK(frames[0][4].b2 == 5 && frames[0][4].pitch == 0 && frames[0][4].flags == 1);
    SEQLIB_CHECK(frames[0][5].b2 == 7 && frames[0][5].vol == 6 && frames[0][5].trig == 1);
    SEQLIB_CHECK(frames[4][5].vol == 6 && frames[5][5].vol == 4 && frames[7][5].vol == 1);
    SEQLIB_CHECK(frames[8][5].vol == 0 && frames[8][5].trig == 1);

    // A bend without a new note outputs the period for pitch 24 * 16 + 1 + 16 with flag 40. A bend in the same frame as
    // a new note starts that note at the bent pitch.
    SEQLIB_CHECK(frames[12][4].flags == 0x40 && frames[12][4].pitch == 0x4000 - 0x191);
    SEQLIB_CHECK(frames[16][4].flags == 1 && frames[16][4].pitch == 0x1C1 + 2 && frames[16][4].vol == 9);

    // Square 2 skips its loop point's wait and duty until the song loops, and then reads them.
    SEQLIB_CHECK_EQ(looped_at, 22);
    SEQLIB_CHECK(frames[4][1].b2 == 0x80 && frames[4][1].trig == 1);
    SEQLIB_CHECK(frames[24][1].b2 == 0xC0 && frames[24][1].vol == 7);

    // F5's duty byte starts the call, whose note has it, and the note after the call comes 16 frames after the call
    // returns. The driver keeps the call's return position, so the 256th command after the call takes the track back
    // there, and the track never reaches its stop.
    SEQLIB_CHECK(starts == (std::vector<std::pair<int, int>>{{3, 0x10}, {19, 0x10}, {290, 0x07}}));
    SEQLIB_CHECK_EQ(stopped_at, -1);
}

// Writes Thumb code `code` at `at`, a multiple of 4, and after it the literals that its ldr instructions load: for each
// (index, value) of `literals`, the instruction at that index loads the value.
void CodeWithLiterals(Image& m, uint32_t at, std::vector<uint16_t> code,
                      const std::vector<std::pair<int, uint32_t>>& literals)
{
    uint32_t literal = (at + 2 * uint32_t(code.size()) + 3) & ~3u;
    for (const auto& [index, value] : literals)
    {
        const uint32_t pc = (at + 2 * uint32_t(index) + 4) & ~3u;
        code[size_t(index)] = uint16_t((code[size_t(index)] & 0xFF00) | ((literal - pc) / 4));
        m.Put32(literal, value);
        literal += 4;
    }

    m.Halfwords(at, code);
}

// The tables of DungeonDiceImage().
constexpr uint32_t kDdmTable = kRomBase + 0x100;
constexpr uint32_t kDdmBase = kRomBase + 0x200;
constexpr uint32_t kDdmSamples = kRomBase + 0x2000;
constexpr uint32_t kDdmMap = kRomBase + 0x2200;
constexpr uint32_t kDdmPeriods = kRomBase + 0x2400;
constexpr uint32_t kDdmFreq = kRomBase + 0x2C00;
constexpr uint32_t kDdmWaves = kRomBase + 0x3800;
constexpr uint32_t kDdmWaveVolumes = kRomBase + 0x3900;

// Returns a cartridge with the code of the Dungeon Dice Monsters revision that detection looks for, its tables, and a
// song. Square 1: NR50 53, vibrato at depth 2, note (vol 8, note 0x20), wait 16, vibrato off, rest, end. Wave: wave 0,
// note (vol 9, note 0x20), wait 8, wave 1, wait 8, rest, end. Noise: note (vol 8, note 0x4A), wait 4, rest, end. Voice
// 0: note (vol 8, map entry 5, sample 3), wait 8, duty byte 3 and note 0x1C of the track's sample (vol 10), wait 8, a
// bend of +16, wait 8, rest, end.
Image DungeonDiceImage()
{
    Image m(0x4000);
    m.Halfwords(kRomBase + 0x800, CommandReader(0x2DFE, 0x2DFC));
    CodeWithLiterals(m, kRomBase + 0x1000, {0x00A0, 0x1900, 0x0080, 0x4900, 0x1841, 0x884C, 0x0420, 0x880D, 0x4328},
                     {{3, kDdmTable}});
    CodeWithLiterals(m, kRomBase + 0x1040, {0x00D1, 0x4800, 0x1808, 0x6805, 0x6841}, {{1, kDdmSamples}});
    CodeWithLiterals(m, kRomBase + 0x1080, {0x4900, 0x2400, 0x5F30, 0x0040, 0x1840, 0x8800}, {{0, kDdmFreq}});
    CodeWithLiterals(m, kRomBase + 0x10C0, {0x4800, 0x786D, 0x1828, 0x7800, 0x70A0}, {{0, kDdmMap}});
    CodeWithLiterals(m, kRomBase + 0x1100, {0x4800, 0x0051, 0x1809, 0x8808, 0x8038, 0x8808, 0x8018},
                     {{0, kDdmPeriods}});
    CodeWithLiterals(m, kRomBase + 0x1140, {0x0118, 0x4900, 0x1840, 0x6010}, {{1, kDdmWaves}});
    CodeWithLiterals(m, kRomBase + 0x1180, {0x4900, 0x7EF2, 0x0050, 0x1840, 0x7EB3, 0x8800, 0x4303},
                     {{0, kDdmWaveVolumes}});
    CodeWithLiterals(m, kRomBase + 0x11C0, {0x109B, 0x4800, 0x4900, 0x1A40}, {{1, kDdmFreq + 2 * 1164}, {2, kDdmFreq}});

    DungeonDiceSong(m, kDdmTable, 0, kDdmBase, 0x60, {{0, 0x00}, {2, 0x10}, {3, 0x20}, {4, 0x30}});
    m.Bytes(kDdmBase + 0x00, {0x00, 0xF0, 0x53, 0x00, 0x82, 0x00, 0xE8, 0x20, 0x10, 0x80, 0x00, 0xFC, 0x00, 0xFD});
    m.Bytes(kDdmBase + 0x10, {0x00, 0x70, 0x00, 0xE9, 0x20, 0x08, 0x71, 0x08, 0xFC, 0x00, 0xFD});
    m.Bytes(kDdmBase + 0x20, {0x00, 0xE8, 0x4A, 0x04, 0xFC, 0x00, 0xFD});
    m.Bytes(kDdmBase + 0x30,
            {0x00, 0xB8, 0x05, 0x08, 0x03, 0x00, 0x9A, 0x1C, 0x08, 0xF2, 0x30, 0x08, 0xFC, 0x00, 0xFD});
    m.Bytes(kDdmBase + 0x60, {0x00, 0xFD});

    // Sample 3: 64 points of a sawtooth at a period of 1677, with map entry 5 naming it. Pitch 24 * 16 + 1 has the
    // sample's period, and the pitches of notes 28 and 29 the periods 4 and 5 semitones up.
    m.Put32(kDdmSamples + 8 * 3, kDdmSamples + 0x100);
    m.Put32(kDdmSamples + 8 * 3 + 4, (4u << 20) | 1677);
    for (uint32_t i = 0; i < 64; i++)
    {
        m.d[kDdmSamples + 0x100 - kRomBase + i] = uint8_t(i * 4);
    }
    m.d[kDdmMap + 5 - kRomBase] = 3;
    m.Put16(kDdmPeriods + 2 * (24 * 16 + 1), 1677);
    m.Put16(kDdmPeriods + 2 * (28 * 16 + 1), 1331);
    m.Put16(kDdmPeriods + 2 * (29 * 16 + 1), 1256);

    // The frequency table: a register value for each 1/16 semitone, restarting the channel, then 12 noise settings, and
    // the vibrato part, 4 entries for each note.
    for (uint32_t p = 0; p < 1152; p++)
    {
        m.Put16(kDdmFreq + 2 * p, uint16_t(0x8000 | std::min<uint32_t>(44 + p, 2047)));
    }
    for (uint32_t i = 0; i < 12; i++)
    {
        m.Put16(kDdmFreq + 2 * (1152 + i), uint16_t(0x8010 + 0x10 * i));
    }
    for (uint32_t i = 0; i < 288; i++)
    {
        m.Put16(kDdmFreq + 2 * (1164 + i), uint16_t(0x8000 | std::min<uint32_t>(44 + 4 * i, 2047)));
    }

    // Waves 0 and 1, a square and a sawtooth, and the volume codes: 25%, 50%, 75% and 100% from volumes 1, 3, 6 and 9.
    for (uint32_t i = 0; i < 16; i++)
    {
        m.d[kDdmWaves - kRomBase + i] = i < 8 ? 0xFF : 0x00;
        m.d[kDdmWaves - kRomBase + 16 + i] = uint8_t(i * 0x11);
    }
    constexpr uint16_t kCodes[16] = {0x0000, 0x6000, 0x6000, 0x4000, 0x4000, 0x4000, 0x8000, 0x8000,
                                     0x8000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000};
    for (uint32_t v = 0; v < 16; v++)
    {
        m.Put16(kDdmWaveVolumes + 2 * v, kCodes[v]);
    }

    return m;
}

void TestDungeonDiceLoopDelay()
{
    // Square 2: wait 2, a loop point with a wait for the loop and duty 75%, wait 2, note (vol 7, note 0x10), wait 18,
    // loop the song. The first pass skips the wait. With a wait of 3, the later passes play the track 3 frames later,
    // and the conversion marks the second pass as the loop. A wait of 0 moves nothing.
    for (const uint8_t wait : {uint8_t(0), uint8_t(3)})
    {
        Image m(0x1000);
        constexpr uint32_t kTable = kRomBase + 0x100;
        constexpr uint32_t kBase = kRomBase + 0x200;
        constexpr uint32_t kMap = kRomBase + 0x600;
        constexpr uint32_t kPeriods = kRomBase + 0x800;
        DungeonDiceSong(m, kTable, 0, kBase, 0x60, {{1, 0x40}});
        m.Bytes(kBase + 0x40, {0x02, 0xFA, wait, 0x7F, 0x02, 0xE7, 0x10, 0x12, 0xFE});
        m.Bytes(kBase + 0x60, {0x00, 0xFD});
        m.Bytes(kMap, {0x00, 0x05, 0x07});
        for (uint32_t p = 0; p < 0x200; p++)
        {
            m.Put16(kPeriods + 2 * p, uint16_t(0x4000 - p));
        }
        const Rom rom = m.ToRom();
        DriverInfo info = SongsAt(kTable);
        info.revision = Revision::kDungeonDiceMonsters;
        info.sample_map = kMap;
        info.sample_period_table = kPeriods;
        DungeonDiceSequencer seq(rom, info, 0);

        bool looped = false;
        for (int f = 0; f < 40 && !looped; f++)
        {
            seq.Step();
            looped = seq.LoopedLastFrame();
        }

        SEQLIB_CHECK(looped);
        SEQLIB_CHECK_EQ(seq.LoopPointDelayed(), wait > 0);
    }
}

void TestDungeonDiceDetection()
{
    const Rom rom = DungeonDiceImage().ToRom();
    DriverInfo info;
    std::string error;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);

    // Every table comes from the code that loads it. The noise settings are the frequency table's entries from note
    // 72's on, and there's no mixer.
    SEQLIB_CHECK(found && info.revision == Revision::kDungeonDiceMonsters);
    SEQLIB_CHECK(info.song_table == kDdmTable && info.song_count == 1);
    SEQLIB_CHECK_EQ(info.sample_table, kDdmSamples);
    SEQLIB_CHECK(info.sample_map == kDdmMap && info.sample_period_table == kDdmPeriods);
    SEQLIB_CHECK(info.psg_freq_table == kDdmFreq && info.noise_table == kDdmFreq + 2 * 1152);
    SEQLIB_CHECK(info.wave_table == kDdmWaves && info.wave_volume_table == kDdmWaveVolumes);
    SEQLIB_CHECK_EQ(info.vibrato_entry, 1164);
    SEQLIB_CHECK(info.mix_rate == 0 && info.warnings.empty());

    // A sample table entry gives the data, the length in blocks of 16 samples, and the sample's rate.
    const SampleInfo s = info.Sample(rom, 3);
    SEQLIB_CHECK(s.valid && s.data == kDdmSamples + 0x100 && s.length == 64 && !s.Looped());
    SEQLIB_CHECK(std::abs(s.Rate(0) - 16777216.0 / 1677) < 1e-6);
}

// Without the code that loads the PSG frequency table, detection still finds the driver, and warns that the square,
// wave and noise notes can't play as they should.
void TestDungeonDiceNoFrequencyTable()
{
    Image m = DungeonDiceImage();
    m.Halfwords(kRomBase + 0x1080, {0, 0, 0, 0, 0, 0});
    const Rom rom = m.ToRom();
    DriverInfo info;
    std::string error;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);

    SEQLIB_CHECK(found && info.psg_freq_table == 0);
    SEQLIB_CHECK(info.warnings.size() == 1 &&
                 info.warnings[0] ==
                     "PSG frequency table not found, so square notes are left out, wave notes "
                     "play at the wrong pitch and noise notes use default settings");
}

// Detection recognises code that works the song table's address out as the play-song routine does, in other registers.
// This table isn't laid out the way the data scan's checks expect, so detection then goes on to the scan.
void TestDungeonDiceSongTableFallback()
{
    Image m = DungeonDiceImage();
    CodeWithLiterals(m, kRomBase + 0x1000, {0x00AA, 0x1952, 0x0092, 0x4900, 0x1889, 0x884C, 0x0420, 0x880D, 0x4328},
                     {{3, kDdmTable}});
    const Rom rom = m.ToRom();
    DriverInfo info;
    std::string error;

    DetectDriver(rom, DriverOverrides(), info, error);

    const auto recognised = [](const std::string& line)
    {
        return line.find("found code like the play-song routine") == 0;
    };
    SEQLIB_CHECK(std::any_of(info.log.begin(), info.log.end(), recognised));
}

void TestDungeonDiceConversion()
{
    const Rom rom = DungeonDiceImage().ToRom();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    const fs::path dir = TempPath("dungeon_dice");
    std::error_code ec;
    fs::create_directories(dir, ec);
    ConvertOptions opt;
    opt.frame_timing = true;
    opt.out_dir = Utf8(dir);

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each track's note-ons (frame, key, program), bends (frame, value) and pans after the first, which centres it.
    std::map<std::string, std::vector<std::array<int, 3>>> notes;
    std::map<std::string, std::vector<std::pair<uint32_t, int>>> bends;
    std::map<std::string, int> program;
    std::map<std::string, std::vector<int>> pans;
    std::set<std::string> centred;
    for (const MidiEvent& e : ReadEvents(sum.midi_path))
    {
        const int kind = e.status & 0xF0;
        if (kind == 0xC0)
        {
            program[e.track] = e.data1;
        }
        if (kind == 0x90 && e.data2 > 0)
        {
            notes[e.track].push_back({int(e.frame), e.data1, program[e.track]});
        }
        if (kind == 0xE0 && e.frame > 0)
        {
            bends[e.track].emplace_back(e.frame, e.data1 | (e.data2 << 7));
        }
        if (kind == 0xB0 && e.data1 == cc::kPan && !centred.insert(e.track).second)
        {
            pans[e.track].push_back(e.data2);
        }
    }

    // Square 1's vibrato steps restart the channel, but bend its one note, and NR50 plays it louder on the left.
    SEQLIB_CHECK(sum.ok);
    SEQLIB_REQUIRE_EQ(notes["Square 1"].size(), 1);
    SEQLIB_REQUIRE(bends["Square 1"].size() >= 2);
    SEQLIB_CHECK(pans["Square 1"].size() == 1 && pans["Square 1"][0] < 64);

    // Loading a wave while a note plays restarts the channel with the new wave, a new note on another program.
    SEQLIB_REQUIRE_EQ(notes["Wave"].size(), 2);
    SEQLIB_CHECK_EQ(notes["Wave"][1][0], 8);
    SEQLIB_CHECK(notes["Wave"][1][2] != notes["Wave"][0][2]);

    // Noise note 0x4A is the third noise setting, on key 38.
    SEQLIB_CHECK(notes["Noise"].size() == 1 && notes["Noise"][0][1] == 38);

    // Voice 0's note at pitch 28 * 16 + 1 plays 4 semitones above its note at the sample's rate, and F2 bends it up a
    // semitone, the most the range of 2 semitones needs.
    SEQLIB_REQUIRE_EQ(notes["Voice 0"].size(), 2);
    SEQLIB_CHECK_EQ(notes["Voice 0"][1][1] - notes["Voice 0"][0][1], 4);
    SEQLIB_CHECK(bends["Voice 0"] == (std::vector<std::pair<uint32_t, int>>{{16, 12288}}));

    fs::remove_all(dir, ec);
}

void TestDetectionFromCode()
{
    // A cartridge with the play-song and note-start code, and three songs. Every track of song 0 plays samples 0, 1 and
    // 2. Sample 1's negative length selects a mode that agbsf2 doesn't support. The tracks of song 1 use F4. agbsf2
    // doesn't support that command either. Song 2 rests.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kSamples = kRomBase + 0x400;
    constexpr uint32_t kPlaySong = kRomBase + 0x800;
    constexpr uint32_t kNoteStart = kRomBase + 0x840;

    m.Halfwords(kPlaySong, kPlaySongCode);
    m.Put32(kPlaySong + 0x18, kTable);
    m.Halfwords(kNoteStart, kNoteStartCode);
    m.Put32(kNoteStart + 0x14, kSamples);

    m.Song(kTable, 0, kRomBase + 0x16C, 0, {});
    m.Bytes(kRomBase + 0x16C, {0x00, 0xA7, 0x00, 0x01, 0xA7, 0x01, 0x01, 0xA7, 0x02, 0x01, 0xFE});
    m.Song(kTable, 1, kRomBase + 0x180, 0, {});
    m.Bytes(kRomBase + 0x180, {0x00, 0xF4, 0x00, 0xFD});
    m.Song(kTable, 2, kRomBase + 0x190, 0, {});
    m.Bytes(kRomBase + 0x190, {0x00, 0xE0, 0x00, 0xFD});

    for (uint32_t s = 0; s < 3; s++)
    {
        const uint32_t header = kRomBase + 0x500 + 0x40 * s;
        m.Put32(kSamples + 4 * s, header);
        m.Put32(header, 0x1000);
        m.Put32(header + 4, s == 1 ? uint32_t(-32) : 32u);
        m.Put32(header + 8, 0xFFFFFFFF);
    }

    DriverOverrides rate_only; // so that the mixer rate, which it has no code for, isn't a warning
    rate_only.mix_rate = 16000;
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), rate_only, info, err);

    // Both tables come from the code. The count of songs ends before song 1, with a warning that says why. The sample
    // table holds sample 1 too, whose notes are skipped.
    const std::vector<std::string> kWarnings = {
        "song 1's tracks don't read cleanly, so the song table is taken to end before it (--song-count overrides this)",
        "agbsf2 can't read these samples and skips their notes: 1"};
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.song_table, kTable);
    SEQLIB_CHECK_EQ(info.song_count, 1);
    SEQLIB_CHECK_EQ(info.sample_table, kSamples);
    SEQLIB_CHECK(info.warnings == kWarnings);

    // When samples 0 and 2 have negative lengths too, the table the code loads doesn't fit. The error says so, and a
    // table at 0xB00 that the data scan would find, with its headers right after it, isn't taken instead.
    m.Put32(kRomBase + 0x504, uint32_t(-32));
    m.Put32(kRomBase + 0x584, uint32_t(-32));
    for (uint32_t s = 0; s < 3; s++)
    {
        const uint32_t header = kRomBase + 0xB0C + 44 * s;
        m.Put32(kRomBase + 0xB00 + 4 * s, header);
        m.Put32(header, 0x1000);
        m.Put32(header + 4, 32);
        m.Put32(header + 8, 0xFFFFFFFF);
    }

    SEQLIB_CHECK(!DetectDriver(m.ToRom(), rate_only, info, err));
    SEQLIB_CHECK(err ==
                 "found the note-start code, but agbsf2 can't read most of the songs' samples in the sample table "
                 "it loads, at 0x08000400 (--sample-table overrides detection)");

    // When song 0 only rests, the table the code loads is still taken, although none of the songs counted plays a note.
    // With no samples played, no sample table is needed.
    m.Bytes(kRomBase + 0x16C, {0x00, 0xE0, 0x00, 0xFD});

    const bool found_resting = DetectDriver(m.ToRom(), rate_only, info, err);

    SEQLIB_CHECK(found_resting);
    SEQLIB_CHECK_EQ(info.song_table, kTable);
    SEQLIB_CHECK_EQ(info.song_count, 1);
    SEQLIB_CHECK(info.warnings == std::vector<std::string>{kWarnings[0]});

    // When song 0 uses F4 too, there's no song to go by. The error says the code was found, and a table at 0xA00 that
    // the data scan would find, laid out like ScanRom's, isn't taken instead.
    m.Bytes(kRomBase + 0x16C, {0x00, 0xF4, 0x00, 0xFD});
    m.Song(kRomBase + 0xA00, 0, kRomBase + 0xA48, 17, {{4, 0}, {0, 8}});
    m.Bytes(kRomBase + 0xA48, {0x00, 0xA8, 0x10, 0x00, 0x0A, 0xE0, 0x05, 0xFE, 0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0,
                               0x00, 0xFE, 0x00, 0xFD});
    m.Song(kRomBase + 0xA00, 1, kRomBase + 0xA5B, 8, {{5, 0}});
    m.Bytes(kRomBase + 0xA5B, {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00, 0x00, 0xFD});

    SEQLIB_CHECK(!DetectDriver(m.ToRom(), rate_only, info, err));
    SEQLIB_CHECK(err ==
                 "found the play-song code, but the song table it loads, at 0x08000100, holds no songs agbsf2 can "
                 "read (--song-table and --song-count override detection)");
}

void TestSampleTableChoice()
{
    // Two copies of the note-start code load two sample tables. In the first, sample 1 has a negative length; in the
    // second, all three samples the song plays read.
    Image m(0x1000);
    constexpr uint32_t kTable = kRomBase + 0x100;
    m.Song(kTable, 0, kRomBase + 0x200, 0, {});
    m.Bytes(kRomBase + 0x200, {0x00, 0xA7, 0x00, 0x01, 0xA7, 0x01, 0x01, 0xA7, 0x02, 0x01, 0xFE});

    for (uint32_t copy = 0; copy < 2; copy++)
    {
        const uint32_t code = kRomBase + 0x800 + 0x40 * copy;
        const uint32_t samples = kRomBase + 0x400 + 0x200 * copy;
        m.Halfwords(code, kNoteStartCode);
        m.Put32(code + 0x14, samples);

        for (uint32_t s = 0; s < 3; s++)
        {
            const uint32_t header = samples + 0x100 + 0x40 * s;
            m.Put32(samples + 4 * s, header);
            m.Put32(header, 0x1000);
            m.Put32(header + 4, copy == 0 && s == 1 ? uint32_t(-32) : 32u);
            m.Put32(header + 8, 0xFFFFFFFF);
        }
    }

    DriverOverrides ov;
    ov.song_table = kTable;
    ov.song_count = 1;
    ov.mix_rate = 16000;
    DriverInfo info;
    std::string err;

    const bool found = DetectDriver(m.ToRom(), ov, info, err);

    // The table in which every sample reads is taken, rather than the first one found.
    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.sample_table, kRomBase + 0x600);
    SEQLIB_CHECK(info.warnings.empty());

    // Without the second copy of the code, and with a song that only plays samples 0 and 1, half of the samples in the
    // first table read, which is enough.
    m.Halfwords(kRomBase + 0x840, std::vector<uint16_t>(kNoteStartCode.size(), 0));
    m.Bytes(kRomBase + 0x200, {0x00, 0xA7, 0x00, 0x01, 0xA7, 0x01, 0x01, 0xFE});

    const bool found_half = DetectDriver(m.ToRom(), ov, info, err);

    SEQLIB_CHECK(found_half);
    SEQLIB_CHECK_EQ(info.sample_table, kRomBase + 0x400);
    SEQLIB_CHECK(info.warnings == std::vector<std::string>{"agbsf2 can't read these samples and skips their notes: 1"});
}

void TestOverrides()
{
    // A table address given by hand that isn't in the ROM, above it or below it, is reported as such.
    const Rom rom = SyntheticRom();
    DriverInfo info;
    std::string err;
    DriverOverrides ov;
    ov.song_table = kRomBase + 0x100000;

    SEQLIB_CHECK(!DetectDriver(rom, ov, info, err));
    SEQLIB_CHECK(err == "the song table address 0x08100000 is outside the ROM (0x08000000-0x08000FFF)");

    ov.song_table = kSongTable;
    ov.sample_table = 0x02000000;

    SEQLIB_CHECK(!DetectDriver(rom, ov, info, err));
    SEQLIB_CHECK(err == "the sample table address 0x02000000 is outside the ROM (0x08000000-0x08000FFF)");

    ov.sample_table = kSampleTable;

    SEQLIB_CHECK(DetectDriver(rom, ov, info, err));

    // The song table ends quietly at an entry that doesn't point into the ROM, which is no song at all.
    Image m = SyntheticImage();
    m.Put32(kSongTable + 2 * kSongEntrySize, 0x12345678);
    ov.mix_rate = 16000;

    SEQLIB_CHECK(DetectDriver(m.ToRom(), ov, info, err));
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK(info.warnings.empty());
}

// OpenMusic() reads a game with Konami's driver through the Music interface, which leaves out the empty entries of the
// song table and gives --info the address of each song's data and the length that converting it would give.
void TestMusic()
{
    // The scan image's tables are found from its data, and SyntheticRom()'s are given, with an empty third entry. Its
    // first song plays a loop of 15 frames from the start.
    const Rom scan = ScanRom();
    const Rom synthetic = SyntheticRom();
    Overrides given;
    given.song_table = kSongTable;
    given.song_count = 3;
    given.sample_table = kSampleTable;
    given.mix_rate = 16000;
    ConvertSettings three_loops;
    three_loops.loops = 3;
    Overrides as_rare;
    as_rare.driver = Driver::kRare;
    std::string found_error, given_error, rare_error;
    std::vector<std::string> trace_warnings;

    const std::unique_ptr<Music> found = agbsf2::OpenMusic(scan, Overrides(), found_error);
    const std::unique_ptr<Music> by_hand = agbsf2::OpenMusic(synthetic, given, given_error);
    const std::unique_ptr<Music> rare = agbsf2::OpenMusic(scan, as_rare, rare_error);

    SEQLIB_CHECK(found && found->Log().front() == "Konami's sound driver");
    SEQLIB_CHECK(found && found->SongCount() == 2);
    SEQLIB_CHECK(found && found->InspectSong(0, ConvertSettings()).address == kScanSongTable + 2 * kSongEntrySize);
    SEQLIB_CHECK(by_hand && by_hand->SongCount() == 3);
    SEQLIB_CHECK(by_hand && by_hand->HasSong(0) && by_hand->HasSong(1) && !by_hand->HasSong(2));
    SEQLIB_CHECK(by_hand && by_hand->InspectSong(0, ConvertSettings()).tracks == 3);
    SEQLIB_CHECK(by_hand && by_hand->InspectSong(0, three_loops).seconds == 45 * kFrameSeconds);
    SEQLIB_CHECK(by_hand && by_hand->InspectSong(0, three_loops).loop_end == 15 * kFrameSeconds);
    SEQLIB_CHECK(!rare && rare_error == "no Rare sound driver found (try --song-table)");

    // Only the song table's entries can be traced: song 2 is past the scan image's two, and so is song 1 << 30, whose
    // entry's address would wrap around to song 0's. Neither writes anything.
    SEQLIB_CHECK(found && !found->Trace(2, 1, stdout, trace_warnings));
    SEQLIB_CHECK(found && !found->Trace(1 << 30, 1, stdout, trace_warnings));
}

} // namespace

void RunTests()
{
    TestDecoding();
    TestSequencer();
    TestLoopStart();
    TestLoopStartEarliest();
    TestLoopStartLast();
    TestLoopStartFarEarlier();
    TestSampleAboveEf();
    TestLoopStartEndedTrack();
    TestBendKept();
    TestConversion();
    TestSilentSongs();
    TestSharedSoundfont();
    TestEchoRouting();
    TestLoopSettingsAgain();
    TestLoopStartsAtSecondPass();
    TestNoiseKeys();
    TestPsgPastTable();
    TestSampleKeys();
    TestLoopWithoutDelay();
    TestFrameLimit();
    TestTrackPastEnd();
    TestManyPresets();
    TestDetectionScan();
    TestSongTableFromOtherCode();
    TestMixerRate();
    TestBendDetection();
    TestRevisionDetection();
    TestWct2004Decoding();
    TestRaveMasterSequencer();
    TestWct2004Sequencer();
    TestCallDump();
    TestWct2004Panning();
    TestEternalDuelistDecoding();
    TestEternalDuelistSequencer();
    TestEternalDuelistConversion();
    TestDungeonDiceDecoding();
    TestDungeonDiceSequencer();
    TestDungeonDiceLoopDelay();
    TestDungeonDiceDetection();
    TestDungeonDiceNoFrequencyTable();
    TestDungeonDiceSongTableFallback();
    TestDungeonDiceConversion();
    TestDetectionFromCode();
    TestSampleTableChoice();
    TestOverrides();
    TestMusic();
}

} // namespace agbsf2::konami
