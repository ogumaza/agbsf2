// SPDX-License-Identifier: MIT

// Unit tests for Ubisoft Milan's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "gba.h"
#include "music.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"
#include "ubimilan/convert.h"
#include "ubimilan/driver.h"
#include "ubimilan/sequencer.h"
#include "ubimilan/soundfont.h"

namespace agbsf2::ubimilan
{
namespace
{

using test::HasGlobalZoneModulators;
using test::Le16;
using test::Le32;
using test::ReadMidi;
using test::ReadSf2;
using test::TempPath;

// Addresses in a test cartridge: the code that detection reads, the PSG's tables and the sound bank.
constexpr uint32_t kStep = kRomBase + 0x100;
constexpr uint32_t kTakeBank = kRomBase + 0x180;
constexpr uint32_t kWrapper = kRomBase + 0x1C0;
constexpr uint32_t kFileLookup = kRomBase + 0x200;
constexpr uint32_t kStart = kRomBase + 0x240;
constexpr uint32_t kInstrumentCode = kRomBase + 0x300;
constexpr uint32_t kNoiseCode = kRomBase + 0x380;
constexpr uint32_t kFrequencyCode = kRomBase + 0x3C0;
constexpr uint32_t kWaveCode = kRomBase + 0x440;
constexpr uint32_t kSoundInit = kRomBase + 0x480;
constexpr uint32_t kFineCode = kRomBase + 0x580;
constexpr uint32_t kSquare1Table = kRomBase + 0x600;
constexpr uint32_t kSquare2Table = kRomBase + 0x7E0;
constexpr uint32_t kWaveInstruments = kRomBase + 0x9C0;
constexpr uint32_t kFrequencyTable = kRomBase + 0xC00;
constexpr uint32_t kNoiseTable = kRomBase + 0xD00;
constexpr uint32_t kWaveTable = kRomBase + 0xD80;
constexpr uint32_t kBank = kRomBase + 0x1000;
constexpr uint32_t kFineTable = kRomBase + 0xA000;
constexpr uint32_t kPsgSequenceAt = kRomBase + 0xE000; // a sequence that a test puts in place of the third piece's

// The keys that the fine table covers, from key 36.
constexpr int kFineKeys = 48;

// The PSG's registers.
constexpr uint32_t kNr13 = 0x04000064;
constexpr uint32_t kNr21 = 0x04000068;
constexpr uint32_t kNr23 = 0x0400006C;
constexpr uint32_t kNr30 = 0x04000070;
constexpr uint32_t kNr31 = 0x04000072;
constexpr uint32_t kNr33 = 0x04000074;
constexpr uint32_t kNr41 = 0x04000078;
constexpr uint32_t kNr43 = 0x0400007C;
constexpr uint32_t kWaveRam = 0x04000090;

// The sound bank's resources.
enum : int
{
    kKitId,
    kShortId,   // 64 points, no loop: channel 9's key 40
    kLoopId,    // 100 points, looping from 20 to 90: key 41
    kLongId,    // 4000 points, no loop: key 42
    kOnceSeqId, // the first piece's sequence
    kOnceId,    // the first piece: plays its sequence once
    kLoopSeqId,
    kLoopsId, // the second piece: loops its sequence, with a voice of its own for the kit
    kMixSeqId,
    kMixId, // the third piece: overlapping notes, the PSG and a key outside the kit
    kResourceCount,

    // Resources in Tomb Raider's numbers, in a cart that has them.
    kLoopSampleId = 103,   // 4096 points, looping from the first to the last: the first sample that plays as music
    kInsideSampleId = 104, // 4112 points, looping from 16 to the last
    kOnceSampleId = 107,   // 4000 points, no loop
    kLastSampleId = 108,
};

// The pieces' sequences, as their commands and countdowns.
const std::vector<uint8_t> kOnceSequence = {
    0x00, 0x00, 0x99, 40,   64,   0x09, 0x00, // key 40 at velocity 64 on frame 0, then 10 frames
    0x89, 40,   0,    0x00, 0x00,             // its note-off on frame 10
    0x99, 41,   32,   0x13, 0x00,             // key 41 at 32, then 20 frames
    0x89, 41,   0,    0x00, 0x00,             // its note-off on frame 30
    0xFC,
};
const std::vector<uint8_t> kLoopSequence = {
    0x00, 0x00, 0x99, 42,   63,   0x04, 0x00, // key 42 on frame 0
    0x89, 42,   0,    0x09, 0x00,             // off on frame 5
    0x99, 40,   63,   0x04, 0x00,             // key 40 on frame 15
    0x89, 40,   0,    0x09, 0x00,             // off on frame 20, then the end on frame 30
    0xFC,
};
const std::vector<uint8_t> kMixSequence = {
    0x00, 0x00, 0xC0, 1,    0x00, 0x00, // square 1's instrument 1 on frame 0
    0x90, 60,   0x70, 0x00, 0x00,       // square 1's key 60, at volume 7
    0x99, 42,   63,   0x04, 0x00,       // key 42's long sample
    0x99, 41,   32,   0x04, 0x00,       // key 41's loop on frame 5, on another voice
    0x89, 42,   0,    0x09, 0x00,       // a note-off on frame 10, which stops the last note's voice
    0x80, 60,   0,    0x00, 0x00,       // square 1's note-off on frame 20
    0x90, 62,   0x00, 0x04, 0x00,       // a note at volume 0, which plays nothing
    0x80, 62,   0,    0x00, 0x00,       // on frame 25
    0x99, 50,   63,   0x00, 0x00,       // a key outside the kit
    0x99, 50,   63,   0x00, 0x00,       // and again, which doesn't warn again
    0xFC,
};

// The PSG's modulation, glide, legato, wave program and noise, in a sequence that a test puts in place of the third
// piece's.
const std::vector<uint8_t> kPsgSequence = {
    0x00, 0x00, 0xC0, 2,    0x00, 0x00, // square 1's instrument 2, with the modulation, on frame 0
    0x90, 60,   0x70, 0x00, 0x00,       // square 1's key 60 at volume 7
    0xC1, 1,    0x00, 0x00,             // square 2's instrument 1, with the pitch mode
    0x91, 60,   0x50, 0x00, 0x00,       // square 2's key 60 at volume 5
    0xC2, 1,    0x00, 0x00,             // the wave's program 1, which loads its wave
    0x92, 48,   0x20, 0x00, 0x00,       // the wave's key 48 at full volume
    0x99, 0,    0x00, 0x04, 0x00,       // noise key 0, then frame 5
    0x91, 64,   0x50, 0x0E, 0x00,       // square 2's key 64, which glides from key 60, then frame 20
    0x80, 60,   0,    0x00, 0x00,       // square 1's note-off
    0x81, 64,   0,    0x00, 0x00,       // square 2's note-off
    0xC1, 2,    0x00, 0x00,             // square 2's instrument 2, with legato
    0x91, 60,   0x50, 0x04, 0x00,       // key 60, then frame 25
    0x91, 62,   0x50, 0x04, 0x00,       // key 62, which goes on from key 60, then frame 30
    0x81, 62,   0,    0x00, 0x00,       // square 2's note-off
    0x82, 48,   0,    0x00, 0x00,       // the wave's note-off
    0xFC,
};

// Returns the frequency setting of a square `index` keys above key 36, equal-tempered: the frequency table's entry
// `index`, or with a fraction, the fine table's entry 100 times `index`.
uint16_t Setting(double index)
{
    const double hz = 440.0 * std::pow(2.0, (index + 36 - 69) / 12.0);
    return uint16_t(std::max(0L, std::lround(2048 - 131072 / hz)));
}

// Returns the cents from frequency setting `from` to `to` on a square.
double CentsBetween(uint16_t from, uint16_t to)
{
    return 1200 * std::log2(double(2048 - from) / double(2048 - to));
}

class Cart : public test::CartImage
{
public:
    // With `tomb_raider`, the cart has Tomb Raider's game code and resources up to its samples that play as music.
    explicit Cart(bool tomb_raider = false) : CartImage(0x10000), tomb_raider_(tomb_raider)
    {
        WriteCode();
        WriteTables();
        WriteBank();
    }

    // Returns the address of resource `id`.
    uint32_t Resource(int id) const
    {
        return resources_[size_t(id)];
    }

    // Gives the third piece the sequence of `commands` in place of its sequence.
    void ReplaceMixSequence(const std::vector<uint8_t>& commands)
    {
        PutBytes(kPsgSequenceAt, Sequence(commands));
        Put32(kBank + 0x10 + 4 * uint32_t(kMixSeqId), kPsgSequenceAt - kBank);
    }

private:
    // The driver's code that detection reads: the sequencer's step, the routine that takes the sound bank, the game's
    // routine that calls it, the archive's lookup, which returns the bank, the call of the two with file 2, the PSG's
    // lookups of its tables, the sound init with Timer 0's float, and the PSG's update, which reads the fine table.
    void WriteCode()
    {
        PutHalfwords(kStep, {0xB530, 0x1C04, 0x68E0, 0x2800, 0xD03D, 0x8820, 0x1C25, 0x3514, 0x2800,
                             0xD133, 0x6920, 0x2800, 0xD017, 0x2000, 0x6120, 0x68A2, 0x2A00, 0xD008,
                             0x6062, 0x7811, 0x7850, 0x0200, 0x4301, 0x1C90, 0x6060, 0x8021});
        PutHalfwords(kTakeBank, {0x4A07, 0x6110, 0x8801, 0x8011, 0x8841, 0x8051, 0x6841, 0x1841, 0x6051, 0x6881, 0x1841,
                                 0x6091, 0x68C1, 0x1840, 0x60D0, 0x4770});
        Put32(kTakeBank + 0x20, 0x02003B00);
        Put16(kWrapper, 0xB500);
        PutBl(kWrapper + 2, kTakeBank);
        Put16(kWrapper + 6, 0xBD00);
        PutHalfwords(kFileLookup, {0x4801, 0x4770});
        Put32(kFileLookup + 8, kBank);
        Put16(kStart, 0x2002);
        PutBl(kStart + 2, kFileLookup);
        PutBl(kStart + 6, kWrapper);

        PutHalfwords(kInstrumentCode,
                     {0xB500, 0x0600, 0x0E00, 0x2901, 0xD00E, 0x2901, 0xDC02, 0x2900, 0xD003, 0xE015, 0x2902, 0xD00D,
                      0xE012, 0x0100, 0x4901, 0x1840, 0xE00F, 0x0000, 0x0000, 0x0000, 0x0100, 0x4901, 0x1840, 0xE008,
                      0x0000, 0x0000, 0x0100, 0x4901, 0x1840, 0xE002, 0x0000, 0x0000, 0x2000, 0xBC02, 0x4708});
        Put32(kInstrumentCode + 0x24, kSquare1Table);
        Put32(kInstrumentCode + 0x30, kSquare2Table);
        Put32(kInstrumentCode + 0x3C, kWaveInstruments);
        PutHalfwords(kNoiseCode,
                     {0x00B1, 0x480A, 0x1809, 0x780A, 0x3101, 0x7808, 0x3101, 0x4338, 0x0200, 0x4310, 0x4A07, 0x8010});
        Put32(kNoiseCode + 0x2C, kNoiseTable);
        PutHalfwords(kFrequencyCode, {0x4914, 0x4642, 0x0050, 0x1840, 0x8807, 0x6BE0, 0x0040, 0x1840, 0x8800});
        Put32(kFrequencyCode + 0x54, kFrequencyTable);
        PutHalfwords(kWaveCode, {0x0168, 0x4906, 0x1840});
        Put32(kWaveCode + 0x1C, kWaveTable);
        PutHalfwords(kSoundInit,
                     {0x4929, 0x2001, 0x6008, 0x4929, 0x200E, 0x7008, 0x3101, 0x20A9, 0x7008, 0x3101, 0x2080, 0x7008,
                      0x4A25, 0x8810, 0x2380, 0x01DB, 0x1C19, 0x4308, 0x8010, 0x4C23, 0x4823, 0x4924, 0x6809, 0xF001,
                      0xFD9D, 0x6020, 0x4922, 0x2009, 0xF7FE, 0xFEFC, 0x4921, 0x200A, 0xF7FE, 0xFEF8, 0x4820});
        Put32(kSoundInit + 0xC8, 0x44800570);
        // The PSG's update and the start of its modulation's branches, as at 0x08004CA2 in The Mummy. ntdlib's
        // detection reads the code from the load of the fine table on.
        PutHalfwords(kFineCode, {0x1962, 0x2C00, 0xD106, 0x4640, 0x303C, 0x4448, 0x6801, 0x2064, 0x4348, 0x1812,
                                 0x4904, 0x0050, 0x1840, 0x8801, 0x2B01, 0xD000, 0x2B01, 0xDC00, 0x2B00, 0xD000});
        Put32(kFineCode + 0x28, kFineTable);
    }

    // Square 1, instrument 1: a 50% duty and an envelope that the velocity sets. Instrument 2 adds a modulation: a
    // delay of 2 frames, then a triangle of 20 cents either way, 5 cents a frame. Square 2, instrument 1: a 50% duty
    // and the pitch mode, with glides over 4 frames. Instrument 2: a 25% duty and legato. Program 1 of the wave channel
    // has another wave. The frequency table and the fine table are equal-tempered from key 36. Noise key 0 plays for 34
    // steps of the length counter at volume 15.
    void WriteTables()
    {
        Put8(kSquare1Table + 16 + 1, 0x80);
        Put8(kSquare1Table + 16 + 3, 0x80);
        const uint32_t modulated = kSquare1Table + 32;
        Put8(modulated + 1, 0x80);
        Put8(modulated + 3, 0x80);
        Put16(modulated + 4, 2);
        Put16(modulated + 6, 20);
        Put16(modulated + 8, 2);
        Put16(modulated + 12, 1);
        const uint32_t gliding = kSquare2Table + 16;
        Put8(gliding, 0x80);
        Put8(gliding + 2, 0x80);
        Put16(gliding + 10, 4);
        Put16(gliding + 12, 2);
        const uint32_t legato = kSquare2Table + 32;
        Put8(legato, 0x40);
        Put8(legato + 2, 0x80);
        Put16(legato + 12, 4);
        Put8(kWaveInstruments + 16 + 1, 0x80);

        for (int i = 0; i < 128; i++)
        {
            Put16(kFrequencyTable + 2 * uint32_t(i), Setting(i));
        }
        for (int c = 0; c < 100 * kFineKeys; c++)
        {
            Put16(kFineTable + 2 * uint32_t(c), Setting(c / 100.0));
        }
        Put32(kNoiseTable, 0xC000F01E);
        for (uint32_t i = 0; i < 32; i++)
        {
            Put8(kWaveTable + i, uint8_t(i * 8));
            Put8(kWaveTable + 32 + i, uint8_t(i < 16 ? 0xFF : 0x00));
        }
    }

    // The sound bank: its counts and the offsets of its tables, the table of resources, the kit table, and the
    // resources, each at a word's boundary.
    void WriteBank()
    {
        const int count = tomb_raider_ ? kLastSampleId + 1 : kResourceCount;
        const uint32_t kit_table = tomb_raider_ ? 0x10 + 4 * uint32_t(count) : 0x60;
        uint32_t next = kBank + kit_table + 0x20;
        const auto add = [&](int id, const std::vector<uint8_t>& bytes)
        {
            resources_[size_t(id)] = next;
            for (size_t i = 0; i < bytes.size(); i++)
            {
                Put8(next + uint32_t(i), bytes[i]);
            }
            Put32(kBank + 0x10 + 4 * uint32_t(id), next - kBank);
            next = (next + uint32_t(bytes.size()) + 3) & ~3u;
        };

        Put16(kBank + 2, uint16_t(count));
        Put32(kBank + 4, 0x10);
        Put32(kBank + 8, 0x10);
        Put32(kBank + 12, kit_table);
        Put16(kBank + kit_table, kKitId);

        std::vector<uint8_t> kit = {kKit, 1, 0xFF, 0xFF};
        for (int key = 0; key < 128; key++)
        {
            const int id = key == 40 ? kShortId : key == 41 ? kLoopId : key == 42 ? kLongId : kNoResource;
            kit.push_back(uint8_t(id));
            kit.push_back(uint8_t(id >> 8));
        }
        add(kKitId, kit);
        add(kShortId, Sample(64, false, 0, 0));
        add(kLoopId, Sample(100, true, 20, 90));
        add(kLongId, Sample(4000, false, 0, 0));
        add(kOnceSeqId, Sequence(kOnceSequence));
        add(kOnceId, Music(0, kOnceSeqId));
        add(kLoopSeqId, Sequence(kLoopSequence));
        add(kLoopsId, Music(9, kLoopSeqId));
        add(kMixSeqId, Sequence(kMixSequence));
        add(kMixId, Music(0, kMixSeqId));

        // Resources 103 to 108: the samples that play as music, and short samples up to them.
        if (tomb_raider_)
        {
            PutBytes(kRomBase + 0xAC, {'A', 'L', '9', 'P'});
            for (int id = kResourceCount; id <= kLastSampleId; id++)
            {
                add(id, id == kLoopSampleId     ? Sample(4096, true, 0, 4096)
                        : id == kInsideSampleId ? Sample(4112, true, 16, 4112)
                        : id == kOnceSampleId   ? Sample(4000, false, 0, 0)
                                                : Sample(16, false, 0, 0));
            }
        }
    }

    static std::vector<uint8_t> Sample(uint32_t length, bool loop, uint32_t start, uint32_t end)
    {
        std::vector<uint8_t> b = {kSample, uint8_t(loop ? 1 : 0), 0x40, 0xFF};
        for (uint32_t v : {length, loop ? start : 0x42424242u, loop ? end : 0x42424242u})
        {
            for (int i = 0; i < 4; i++)
            {
                b.push_back(uint8_t(v >> (8 * i)));
            }
        }
        for (uint32_t i = 0; i < length; i++)
        {
            b.push_back(uint8_t(int8_t((i * 7) % 200 - 100)));
        }

        return b;
    }

    static std::vector<uint8_t> Sequence(const std::vector<uint8_t>& commands)
    {
        std::vector<uint8_t> b = {kSequence, 0xFF, 0xFF, 0xFF};
        b.insert(b.end(), commands.begin(), commands.end());

        return b;
    }

    static std::vector<uint8_t> Music(int flags, int sequence)
    {
        return {kMusic, 0xFF, 0xFF, 0xFF, 1, 0, uint8_t(flags), 0, uint8_t(sequence), 0};
    }

    bool tomb_raider_ = false;
    std::array<uint32_t, kLastSampleId + 1> resources_ = {};
};

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, DriverOverrides(), info, error);

    return info;
}

// Detection finds the sequencer's step by its code, the sound bank through the archive's lookup that the game calls
// with file 2, the pieces of music among the resources, channel 9's kit, the PSG's tables, the fine table through the
// PSG's update, and Timer 0's reload from the sound init's float. A cartridge without the PSG's update gets a warning.
// A cartridge without the code shows no sign of the driver.
void TestDetection()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    DriverInfo blank_info;
    std::string error, blank_error;
    Rom blank;
    blank.Assign(std::vector<uint8_t>(0x10000, 0));

    DriverInfo info;
    const bool found = DetectDriver(rom, DriverOverrides(), info, error);
    const bool found_blank = DetectDriver(blank, DriverOverrides(), blank_info, blank_error);

    SEQLIB_CHECK(found && error.empty());
    SEQLIB_CHECK_EQ(info.step_routine, kStep);
    SEQLIB_CHECK_EQ(info.bank, kBank);
    SEQLIB_CHECK_EQ(info.resource_count, kResourceCount);
    SEQLIB_CHECK(info.songs == std::vector<int>({kOnceId, kLoopsId, kMixId}));
    SEQLIB_CHECK_EQ(info.kit, cart.Resource(kKitId) + 4);
    SEQLIB_CHECK_EQ(info.instrument_tables[0], kSquare1Table);
    SEQLIB_CHECK_EQ(info.instrument_tables[1], kSquare2Table);
    SEQLIB_CHECK_EQ(info.instrument_tables[2], kWaveInstruments);
    SEQLIB_CHECK_EQ(info.noise_table, kNoiseTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.wave_table, kWaveTable);
    SEQLIB_CHECK_EQ(info.fine_table, kFineTable);
    SEQLIB_CHECK_EQ(info.timer_reload, 0xFBFF);
    SEQLIB_CHECK(info.warnings.empty());
    SEQLIB_CHECK(!found_blank && blank_error.empty());

    // Without the load of the fine table, detection doesn't find the table.
    Cart no_fine;
    no_fine.Put16(kFineCode + 0x14, 0);
    const DriverInfo no_fine_info = Detect(no_fine.ToRom());
    SEQLIB_CHECK_EQ(no_fine_info.fine_table, 0);
    SEQLIB_CHECK(no_fine_info.warnings ==
                 std::vector<std::string>(
                     {"the PSG's fine frequency table wasn't found, so the PSG's modulation and glides are left out"}));

    // Detection starts from nothing. A game without the driver keeps none of an earlier game's tables.
    SEQLIB_CHECK(!DetectDriver(blank, DriverOverrides(), info, error));
    SEQLIB_CHECK(info.step_routine == 0 && info.songs.empty());
}

// The Sum of All Fears and Rogue Spear have an older build of the driver. The bank routine of that build copies two
// tables, not three. Detection finds the bank in the same way as for the newer build. The error says that the music is
// recorded samples. A bank of the older build with a sequence gets another error.
void TestOlderBuild()
{
    test::CartImage cart(0x10000);
    cart.PutHalfwords(kTakeBank, {0x4A06, 0x60D0, 0x8801, 0x8011, 0x8841, 0x8051, 0x6841, 0x1841, 0x6051, 0x6881,
                                  0x1840, 0x6090, 0x4770});
    cart.Put32(kTakeBank + 0x1C, 0x02003B00);
    cart.Put16(kWrapper, 0xB500);
    cart.PutBl(kWrapper + 2, kTakeBank);
    cart.Put16(kWrapper + 6, 0xBD00);
    cart.PutHalfwords(kFileLookup, {0x4801, 0x4770});
    cart.Put32(kFileLookup + 8, kBank);
    cart.Put16(kStart, 0x2002);
    cart.PutBl(kStart + 2, kFileLookup);
    cart.PutBl(kStart + 6, kWrapper);
    cart.Put16(kBank + 2, 2);
    cart.Put32(kBank + 4, 0x10);
    cart.Put32(kBank + 8, 0x10);
    cart.Put32(kBank + 0x10, 0x20);
    cart.Put32(kBank + 0x14, 0x30);
    cart.Put8(kBank + 0x20, kSample);
    cart.Put8(kBank + 0x30, kSample);

    const Rom rom = cart.ToRom();
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(!DetectDriver(rom, DriverOverrides(), info, error));
    SEQLIB_CHECK_EQ(info.older_bank, kBank);
    SEQLIB_CHECK(error ==
                 "Found an older build of Ubisoft Milan's sound driver. The music is recorded samples, with no "
                 "sequences to convert.");
    std::string open_error;
    SEQLIB_CHECK(!agbsf2::OpenMusic(rom, Overrides(), open_error));
    SEQLIB_CHECK(open_error == error);

    cart.Put8(kBank + 0x30, kSequence);
    SEQLIB_CHECK(!DetectDriver(cart.ToRom(), DriverOverrides(), info, error));
    SEQLIB_CHECK(error == "Found an older build of Ubisoft Milan's sound driver. This build isn't supported.");
}

// Returns the notes that the model plays in the first `frames` frames of `song`, and its warnings.
std::vector<Note> Play(const Rom& rom, const DriverInfo& info, int song, int frames, std::vector<std::string>* warnings)
{
    Sequencer seq(rom, info, song);
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
    }
    if (warnings)
    {
        *warnings = seq.Warnings();
    }

    return seq.Notes();
}

// A countdown of n puts the next command n + 1 frames later. A sample without a loop stops once the mixer reaches its
// end, which for 64 points is in the frame its note starts, and the voice is free again. A sample that loops plays
// until its note-off.
void TestTiming()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const std::vector<Note> notes = Play(rom, info, 0, 40, nullptr);

    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    SEQLIB_CHECK(notes[0].key == 40 && notes[0].velocity == 64 && notes[0].on == 0);
    SEQLIB_CHECK(notes[0].ended && notes[0].off == 1);
    SEQLIB_CHECK_EQ(notes[0].sample, cart.Resource(kShortId));
    SEQLIB_CHECK(notes[1].key == 41 && notes[1].velocity == 32 && notes[1].on == 10);
    SEQLIB_CHECK(notes[1].ended && notes[1].off == 30);
}

// A note takes the last free voice, so a second note while the first plays takes voice 2. A note-off stops the voice of
// channel 9's last note, whatever its key, so key 42's note-off ends key 41's note, and key 42 plays on until the mixer
// reaches the end of its 4000 points, in frame 29. The PSG's note at volume 7 plays until its note-off, the one at
// volume 0 too, though it's silent, and a key outside the kit plays nothing, with one warning for both its notes.
void TestVoices()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    std::vector<std::string> warnings;

    const std::vector<Note> notes = Play(rom, info, 2, 40, &warnings);

    SEQLIB_REQUIRE_EQ(notes.size(), 4);
    SEQLIB_CHECK(notes[0].channel == 0 && notes[0].key == 60 && notes[0].program == 1);
    SEQLIB_CHECK(notes[0].envelope == 0x70 && notes[0].duty == 0x80 && notes[0].off == 20);
    SEQLIB_CHECK(notes[1].key == 42 && notes[1].on == 0 && notes[1].off == 30);
    SEQLIB_CHECK(notes[2].key == 41 && notes[2].on == 5 && notes[2].off == 10);
    SEQLIB_CHECK(notes[3].channel == 0 && notes[3].key == 62 && notes[3].envelope == 0);
    SEQLIB_CHECK(notes[3].on == 20 && notes[3].off == 25);
    SEQLIB_CHECK(warnings ==
                 std::vector<std::string>({"channel 9's key 50 plays no sample, which the game doesn't check"}));
}

// With the game's switch of the kit off, channel 9's kit plays no notes and no note-offs, and no voice starts. The
// PSG's notes play as before. The key outside the kit doesn't reach the kit, so it doesn't warn.
void TestKitSwitch()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 2, false, false);

    for (int f = 0; f < 40; f++)
    {
        seq.Step();
    }

    SEQLIB_REQUIRE_EQ(seq.Notes().size(), 2);
    SEQLIB_CHECK(seq.Notes()[0].channel == 0 && seq.Notes()[0].key == 60 && seq.Notes()[0].off == 20);
    SEQLIB_CHECK(seq.Notes()[1].channel == 0 && seq.Notes()[1].key == 62 && seq.Notes()[1].off == 25);
    SEQLIB_CHECK(seq.Warnings().empty());
    for (int v = 0; v < kVoiceCount; v++)
    {
        SEQLIB_CHECK(!seq.VoicePlays(v));
    }
}

// A song that the game starts without the kit has a second version, with the kit, in files with the suffix _kit. The
// first version leaves the kit's notes out. Another song has one version.
void TestKitVersions()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    DriverInfo info = Detect(rom);
    info.kit_off_songs = {2};
    ConvertSettings opt;
    opt.out_dir = TempPath("").string();
    opt.base_name = "ubimilan_kit";

    const std::vector<SongVersion> versions = SongVersions(info, 2);
    const SongReport first = ConvertSong(rom, info, 2, opt, nullptr);
    opt.version = 1;
    const SongReport second = ConvertSong(rom, info, 2, opt, nullptr);

    SEQLIB_CHECK(!KitPlays(info, 2, 0) && KitPlays(info, 2, 1) && KitPlays(info, 0, 0));
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].name.empty() && versions[0].suffix.empty());
    SEQLIB_CHECK(versions[1].name == "with the kit" && versions[1].suffix == "_kit");
    SEQLIB_CHECK_EQ(SongVersions(info, 0).size(), 1);
    SEQLIB_CHECK(first.ok && first.tracks == 1 && first.midi_path.ends_with("ubimilan_kit_02.mid"));
    SEQLIB_CHECK(ReadMidi(first.midi_path).Find(0x9A).empty());
    SEQLIB_CHECK(second.ok && second.tracks == 2 && second.midi_path.ends_with("ubimilan_kit_02_kit.mid"));
    SEQLIB_CHECK_EQ(ReadMidi(second.midi_path).Find(0x9A).size(), 2);
}

// The piece that loops plays its sequence again after its end, with the kit on the voice it took at the start, and the
// track's state and the voices come out as driver_emu.py's trace prints them.
void TestLoopingPiece()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 1);

    for (int f = 0; f < 31; f++)
    {
        seq.Step();
    }
    const std::array<uint32_t, 5> track = seq.TrackState();

    SEQLIB_CHECK(seq.SequenceStarts() == std::vector<uint32_t>({0, 30}));
    SEQLIB_CHECK(seq.VoicePlays(3) && !seq.VoicePlays(2));
    SEQLIB_CHECK_EQ(seq.VoiceState(3)[4], 63);
    SEQLIB_CHECK_EQ(track[2], cart.Resource(kLoopSeqId) + 4);
    SEQLIB_CHECK_EQ(track[3], 1);
    SEQLIB_REQUIRE_EQ(seq.Notes().size(), 3);
}

// The conversion writes channel 9 on MIDI channel 11, away from the drum channel. Channel 9 plays in bank 9. Its
// program is the kit's offset. At tick 0, the track selects the bank. It sets CC7 and CC11 to 127, CC10 to 64, and
// CC91 and CC93 to 0. A velocity is 127 times the square root of the driver's volume over 64. The SoundFont's kit is
// in bank 9. Each key's zone plays the key's sample unpitched at the mixer's rate. The looping sample's loop is
// rounded up to a whole number of 8 points.
void TestConversion()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = TempPath("").string();
    opt.base_name = "ubimilan_once";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok && !sum.silent && sum.tracks == 1 && sum.loop_start < 0);
    SEQLIB_CHECK(sum.warnings.empty());
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x9A);
    SEQLIB_REQUIRE_EQ(ons.size(), 2);
    SEQLIB_CHECK(ons[0].bytes == std::vector<uint8_t>({0x9A, 40, 127}));
    SEQLIB_CHECK(ons[1].bytes == std::vector<uint8_t>({0x9A, 41, 90}));
    SEQLIB_REQUIRE_EQ(midi.Find(0xCA).size(), 1);
    std::map<int, int> controls;
    for (const auto& [tick, bytes] : midi.Find(0xBA))
    {
        SEQLIB_CHECK_EQ(tick, 0);
        controls[bytes[1]] = bytes[2];
    }
    const std::map<int, int> setup = {{0, 9}, {7, 127}, {10, 64}, {11, 127}, {32, 0}, {91, 0}, {93, 0}};
    SEQLIB_CHECK(controls == setup);
    SEQLIB_CHECK(midi.Find(0x99).empty() && midi.Find(0xB9).empty() && midi.Find(0xC9).empty());

    const test::Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));
    SEQLIB_CHECK_EQ(sf.Count("phdr", 38), 2);
    if (sf.Count("phdr", 38) == 2)
    {
        SEQLIB_CHECK_EQ(Le16(sf.Record("phdr", 38, 0) + 22), 9);
    }
    SEQLIB_CHECK_EQ(sf.Count("shdr", 46), 3);
    if (sf.Count("shdr", 46) == 3)
    {
        const uint8_t* looped = sf.Record("shdr", 46, 1);
        SEQLIB_CHECK_EQ(Le32(looped + 36), 8184);
        SEQLIB_CHECK_EQ(Le32(looped + 28) - Le32(looped + 20), 20);
        SEQLIB_CHECK_EQ(Le32(looped + 32) - Le32(looped + 20), 92);
    }
}

// The piece that loops gets loop markers over one pass of its sequence, and plays two passes.
void TestLoopMarkers()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = TempPath("").string();
    opt.base_name = "ubimilan_loops";

    const SongReport sum = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.loop_start == 0);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 30 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 60 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    SEQLIB_REQUIRE_EQ(midi.Find(0x9A).size(), 4);
    int markers = 0;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        markers += bytes.size() > 1 && bytes[1] == 0x06 ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(markers, 2);
}

// Returns the value of controller `number` on MIDI channel `channel` at tick `tick`: its last change at the tick or
// before it, or -1 if there was none.
int ControlAt(const test::MidiEvents& midi, int channel, int number, uint64_t tick)
{
    int value = -1;
    for (const auto& [at, bytes] : midi.Find(uint8_t(0xB0 | channel)))
    {
        if (at <= tick && bytes[1] == number)
        {
            value = bytes[2];
        }
    }

    return value;
}

// Returns the bends of MIDI channel `channel` in cents, in order. RPN 0 sets the bend range.
std::vector<double> BendCents(const test::MidiEvents& midi, int channel)
{
    int range = 2;
    for (const auto& [at, bytes] : midi.Find(uint8_t(0xB0 | channel)))
    {
        range = bytes[1] == 6 ? bytes[2] : range;
    }
    std::vector<double> cents;
    for (const auto& [at, bytes] : midi.Find(uint8_t(0xE0 | channel)))
    {
        cents.push_back((int(bytes[1] | (bytes[2] << 7)) - 8192) / 8192.0 * 100 * range);
    }

    return cents;
}

// Returns true if `writes` write `value` to `address`.
bool Wrote(const std::vector<RegisterWrite>& writes, uint32_t address, uint32_t value)
{
    return std::any_of(writes.begin(), writes.end(),
                       [&](const RegisterWrite& w) { return w.address == address && w.value == value; });
}

// The PSG's note at volume 7 plays on MIDI channel 1 at full velocity. The channel has square 1's bank and the program
// of the note's instrument. The note's level is 7 of a full-scale sample's 128: 2 for each step of the volume, at
// NR50's half volume. CC11 is 127 times the square root of the level over 127, 30. The note at volume 0 is left out,
// and so is the key outside the kit, whose warning the summary has.
void TestPsgConversion()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = TempPath("").string();
    opt.base_name = "ubimilan_mix";

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.tracks == 2);
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x90);
    SEQLIB_REQUIRE_EQ(ons.size(), 1);
    SEQLIB_CHECK(ons[0].bytes == std::vector<uint8_t>({0x90, 60, 127}));
    SEQLIB_CHECK_EQ(ControlAt(midi, 0, 0, ons[0].tick), PresetBank(0));
    SEQLIB_CHECK_EQ(ControlAt(midi, 0, 11, ons[0].tick), 30);
    SEQLIB_REQUIRE_EQ(midi.Find(0xC0).size(), 1);
    SEQLIB_CHECK_EQ(midi.Find(0xC0)[0].bytes[1], 1);
    SEQLIB_REQUIRE_EQ(midi.Find(0x9A).size(), 2);
    SEQLIB_CHECK(!sum.warnings.empty() &&
                 sum.warnings.front() == "channel 9's key 50 plays no sample, which the game doesn't check");
}

// The square 1 modulation waits 2 frames, then moves the frequency setting by 5 cents a frame from the place of the key
// in the fine table. It turns once it's past 20 cents either way. At 0 cents, the update writes nothing. On square 2,
// the first note in the pitch mode starts at its key, and the next note glides from there over 4 frames. That note
// starts at the setting of the last key. The glide reaches 400 cents up in 4 steps of 100 and stays there. An
// instrument without the pitch mode then starts its note at its key. A legato note writes the setting of its key
// without a restart. The wave program loads both banks of the wave RAM with the DAC off, and the wave note switches
// the DAC on. The noise key writes its entry of the noise table.
void TestPsgModel()
{
    Cart cart;
    cart.ReplaceMixSequence(kPsgSequence);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 2);
    SEQLIB_REQUIRE(seq.Valid());
    std::vector<std::vector<RegisterWrite>> frames;
    for (int f = 0; f < 32; f++)
    {
        frames.push_back(seq.Step());
    }
    const auto last = [&](int f, uint32_t address)
    {
        return test::LastWrite(frames[size_t(f)], address);
    };

    SEQLIB_CHECK_EQ(last(0, kNr13), 0x8000 | Setting(24));
    SEQLIB_CHECK(last(1, kNr13) < 0 && last(2, kNr13) < 0);
    SEQLIB_CHECK_EQ(last(3, kNr13), Setting(24.05));
    SEQLIB_CHECK_EQ(last(6, kNr13), Setting(24.2));
    SEQLIB_CHECK_EQ(last(7, kNr13), Setting(24.25));
    SEQLIB_CHECK_EQ(last(8, kNr13), Setting(24.2));
    SEQLIB_CHECK(last(12, kNr13) < 0);
    SEQLIB_CHECK_EQ(last(17, kNr13), Setting(23.75));
    SEQLIB_CHECK_EQ(last(18, kNr13), Setting(23.8));

    SEQLIB_CHECK(Wrote(frames[0], kNr23, 0x8000 | Setting(24)));
    for (int f = 0; f < 5; f++)
    {
        SEQLIB_CHECK_EQ(last(f, kNr23), Setting(24));
    }
    SEQLIB_CHECK(Wrote(frames[5], kNr23, 0x8000 | Setting(24)));
    for (int f = 5; f < 9; f++)
    {
        SEQLIB_CHECK_EQ(last(f, kNr23), Setting(f + 20));
    }
    SEQLIB_CHECK(last(9, kNr23) == Setting(28) && last(19, kNr23) == Setting(28));
    SEQLIB_CHECK_EQ(last(20, kNr21), 0x5040);
    SEQLIB_CHECK_EQ(last(20, kNr23), 0x8000 | Setting(24));
    SEQLIB_CHECK(last(21, kNr23) < 0);
    SEQLIB_CHECK(last(25, kNr21) < 0);
    SEQLIB_CHECK_EQ(last(25, kNr23), Setting(26));

    std::vector<uint32_t> dac;
    std::vector<long long> wave;
    for (const RegisterWrite& w : frames[0])
    {
        if (w.address == kNr30)
        {
            dac.push_back(w.value);
        }
        if (w.address == kWaveRam)
        {
            wave.push_back(w.value);
        }
    }
    SEQLIB_CHECK(dac == std::vector<uint32_t>({0x20, 0x60, 0xA0}));
    SEQLIB_CHECK(wave == std::vector<long long>({0xFF, 0x00}));
    SEQLIB_CHECK_EQ(last(0, kNr31), 0x2000);
    SEQLIB_CHECK_EQ(last(0, kNr33), 0x8000 | Setting(36));
    SEQLIB_CHECK_EQ(last(0, kNr41), 0xF01E);
    SEQLIB_CHECK_EQ(last(0, kNr43), 0xC000);

    std::vector<int> keys;
    std::vector<bool> legato;
    for (const Note& n : seq.Notes())
    {
        if (n.channel == 1)
        {
            keys.push_back(n.key);
            legato.push_back(n.legato);
        }
    }
    SEQLIB_CHECK(keys == std::vector<int>({60, 64, 60, 62}));
    SEQLIB_CHECK(legato == std::vector<bool>({false, false, false, true}));
}

// The PSG piece converts to four tracks. The squares and the wave play on MIDI channels 1 to 3, and the noise on MIDI
// channel 4, with the noise drums in bank 3. Each note plays at full velocity. CC11 sets the level: 127 times the
// square root of the level over 127. The level of a square is its volume. The level of the wave at full volume is 30,
// and the noise drum plays at the noise level of 15. The bends follow the frequency settings from the zone pitch: the
// square 1 modulation goes 25 cents either way, and the square 2 glide starts 300 cents down.
void TestPsgTracks()
{
    Cart cart;
    cart.ReplaceMixSequence(kPsgSequence);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = TempPath("").string();
    opt.base_name = "ubimilan_psg";

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);

    SEQLIB_CHECK(sum.ok && sum.tracks == 4 && sum.warnings.empty());
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    const std::array<std::vector<int>, 4> keys = {{{60}, {60, 64, 60, 62}, {48}, {kFirstDrumKey}}};
    const std::array<int, 4> levels = {30, 25, 62, 44};
    for (int c = 0; c < 4; c++)
    {
        const auto ons = midi.Find(uint8_t(0x90 | c));
        std::vector<int> found;
        for (const auto& [at, bytes] : ons)
        {
            found.push_back(bytes[1]);
            SEQLIB_CHECK_EQ(bytes[2], 127);
        }
        SEQLIB_CHECK(found == keys[size_t(c)]);
        if (!ons.empty())
        {
            SEQLIB_CHECK_EQ(ControlAt(midi, c, 11, ons[0].tick), levels[size_t(c)]);
            SEQLIB_CHECK_EQ(ControlAt(midi, c, 0, ons[0].tick), c == 3 ? kNoiseBank : PresetBank(c));
        }
    }
    SEQLIB_CHECK(midi.Find(0x9A).empty());

    const std::vector<double> square1 = BendCents(midi, 0);
    const std::vector<double> square2 = BendCents(midi, 1);
    SEQLIB_REQUIRE(!square1.empty() && !square2.empty());
    const auto [low1, high1] = std::minmax_element(square1.begin(), square1.end());
    SEQLIB_CHECK(std::fabs(*high1 - CentsBetween(Setting(24), Setting(24.25))) < 0.6);
    SEQLIB_CHECK(std::fabs(*low1 - CentsBetween(Setting(24), Setting(23.75))) < 0.6);
    const double low2 = *std::min_element(square2.begin(), square2.end());
    SEQLIB_CHECK(std::fabs(low2 - CentsBetween(Setting(28), Setting(25))) < 0.6);

    const test::Sf2Records sf = ReadSf2(sum.sf2_path);
    bool drums = false;
    for (size_t i = 0; i < sf.Count("phdr", 38); i++)
    {
        const uint8_t* preset = sf.Record("phdr", 38, i);
        const std::string name(preset, std::find(preset, preset + 20, 0));
        drums = drums || (name == "Noise drums" && Le16(preset + 20) == 0 && Le16(preset + 22) == kNoiseBank);
    }
    SEQLIB_CHECK(drums);
}

// The driver comes through OpenMusic with its name, and lists the pieces of music.
void TestMusic()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_CHECK(music && error.empty());
    if (music)
    {
        SEQLIB_CHECK(music->Log().front() == "Ubisoft Milan's sound driver");
        SEQLIB_CHECK_EQ(music->SongCount(), 3);
        SEQLIB_CHECK_EQ(music->InspectSong(0, ConvertSettings()).address, cart.Resource(kOnceId));
    }
}

// The mixer plays a loop in whole runs of 8 points. A sample that loops from point 20 to point 30 therefore loops over
// 16 points. The last 6 of them come from after the loop's end. That loop is repeated twice: 32 points, kMinLoop.
void TestShortLoop()
{
    Cart cart;
    constexpr uint32_t kSampleAt = kRomBase + 0xF000;
    cart.PutBytes(kSampleAt, {kSample, 1, 0x40, 0xFF, 40, 0, 0, 0, 20, 0, 0, 0, 30, 0, 0, 0});
    for (uint32_t i = 0; i < 40; i++)
    {
        cart.Put8(kSampleAt + 16 + i, uint8_t(i * 9));
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    InstrumentSet set;
    set.AddKitKey(0, 40, kSampleAt);
    Sf2File sf;
    set.Build(rom, info, sf);
    SEQLIB_REQUIRE_EQ(sf.samples.size(), 1);
    SEQLIB_CHECK(test::LengthenedLoop(sf.samples[0], 20, 16));
}

} // namespace

// Tomb Raider's samples that play as music follow the pieces, in the cart's numbers up to 108. Another game's samples
// don't. A sample's voice goes on looping, and each pass is a note. 4096 points are 512 of the mixer's runs of 16400
// cycles, 29.89 frames of 280896 cycles, so the passes start on frames 30, 60 and 90.
void TestSamplesAsMusic()
{
    const Cart cart(true);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.songs == std::vector<int>({kOnceId, kLoopsId, kMixId, 103, 104, 105, 106, 107, 108}));
    SEQLIB_CHECK_EQ(info.piece_count, 3);

    constexpr int kLoopSong = 3;
    constexpr int kOnceSong = 7;
    MusicPiece piece;
    SEQLIB_REQUIRE(ReadMusic(rom, info, kLoopSong, piece));
    SEQLIB_CHECK_EQ(piece.sample, kLoopSampleId);
    SEQLIB_CHECK(piece.Loops() && piece.Parts() == 1 && piece.sequences.empty());
    SEQLIB_REQUIRE(ReadMusic(rom, info, kOnceSong, piece));
    SEQLIB_CHECK_EQ(piece.sample, kOnceSampleId);
    SEQLIB_CHECK(!piece.Loops());

    Sequencer looping(rom, info, kLoopSong);
    SEQLIB_REQUIRE(looping.Valid());
    for (int f = 0; f < 95; f++)
    {
        looping.Step();
    }
    SEQLIB_CHECK(looping.SequenceStarts() == std::vector<uint32_t>({0, 30, 60, 90}));
    SEQLIB_REQUIRE_EQ(looping.Notes().size(), 4);
    SEQLIB_CHECK_EQ(looping.Notes()[0].key, kFirstSampleKey);
    SEQLIB_CHECK_EQ(looping.Notes()[0].velocity, kFullVolume);
    SEQLIB_CHECK_EQ(looping.Notes()[0].sample, cart.Resource(kLoopSampleId));
    SEQLIB_CHECK(looping.Notes()[0].ended && looping.Notes()[0].off == 30);
    SEQLIB_CHECK_EQ(looping.Notes()[1].on, 30);
    SEQLIB_CHECK(!looping.Notes()[3].ended);

    // 4000 points are 500 runs, which end on frame 29.
    Sequencer once(rom, info, kOnceSong);
    SEQLIB_REQUIRE(once.Valid());
    for (int f = 0; f < 40; f++)
    {
        once.Step();
    }
    SEQLIB_REQUIRE_EQ(once.Notes().size(), 1);
    SEQLIB_CHECK_EQ(once.Notes()[0].key, kFirstSampleKey + 4);
    SEQLIB_CHECK(once.Notes()[0].ended && once.Notes()[0].off == 30);
    SEQLIB_CHECK(!once.Playing());

    ConvertSettings opt;
    opt.out_dir = TempPath("").string();
    opt.base_name = "ubimilan_sample";
    const SongReport sum = ConvertSong(rom, info, kLoopSong, opt, nullptr);
    SEQLIB_CHECK(sum.ok && sum.loop_start == 0);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 30 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 60 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x9A);
    SEQLIB_REQUIRE_EQ(ons.size(), 2);
    SEQLIB_CHECK(ons[0].bytes == std::vector<uint8_t>({0x9A, uint8_t(kFirstSampleKey), 127}));
    const test::Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK_EQ(sf.Count("shdr", 46), 2);
    if (sf.Count("shdr", 46) == 2)
    {
        const uint8_t* looped = sf.Record("shdr", 46, 0);
        SEQLIB_CHECK_EQ(Le32(looped + 36), 8184);
        SEQLIB_CHECK_EQ(Le32(looped + 28) - Le32(looped + 20), 0);
        SEQLIB_CHECK_EQ(Le32(looped + 32) - Le32(looped + 20), 4096);
    }

    opt.base_name = "ubimilan_sample_once";
    const SongReport sum_once = ConvertSong(rom, info, kOnceSong, opt, nullptr);
    SEQLIB_CHECK(sum_once.ok && sum_once.loop_start < 0);
    SEQLIB_CHECK(std::fabs(sum_once.seconds - 30 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    SEQLIB_CHECK_EQ(ReadMidi(sum_once.midi_path).Find(0x9A).size(), 1);

    // A sample with points before its loop: 514 runs to the first loop's end, then 512 to each next, which end 30.01,
    // 59.90 and 89.79 frames in. Later passes play another key, from the loop's start, so the markers go on the second.
    constexpr int kInsideSong = 4;
    Sequencer inside(rom, info, kInsideSong);
    SEQLIB_REQUIRE(inside.Valid());
    for (int f = 0; f < 65; f++)
    {
        inside.Step();
    }
    SEQLIB_CHECK(inside.SequenceStarts() == std::vector<uint32_t>({0, 30, 60}));
    SEQLIB_REQUIRE_EQ(inside.Notes().size(), 3);
    SEQLIB_CHECK(inside.Notes()[0].key == kFirstSampleKey + 1 && inside.Notes()[0].skip == 0);
    for (size_t i = 1; i < 3; i++)
    {
        SEQLIB_CHECK(inside.Notes()[i].key == kFirstSampleKey + 1 + kLoopKeyOffset && inside.Notes()[i].skip == 16);
    }

    opt.base_name = "ubimilan_sample_inside";
    const SongReport sum_inside = ConvertSong(rom, info, kInsideSong, opt, nullptr);
    SEQLIB_CHECK(sum_inside.ok);
    SEQLIB_CHECK(std::fabs(sum_inside.loop_start - 30 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum_inside.loop_end - 60 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum_inside.seconds - 90 * double(kFrameCycles) / kSecondCycles) < 1e-9);
    const auto inside_ons = ReadMidi(sum_inside.midi_path).Find(0x9A);
    SEQLIB_REQUIRE_EQ(inside_ons.size(), 3);
    SEQLIB_CHECK_EQ(inside_ons[0].bytes[1], kFirstSampleKey + 1);
    SEQLIB_CHECK_EQ(inside_ons[1].bytes[1], kFirstSampleKey + 1 + kLoopKeyOffset);

    // The zone of the later passes' key starts at the loop's start, in the same sample.
    InstrumentSet set;
    set.AddKitKey(0, kFirstSampleKey + 1, cart.Resource(kInsideSampleId));
    set.AddKitKey(0, kFirstSampleKey + 1 + kLoopKeyOffset, cart.Resource(kInsideSampleId), 16);
    Sf2File file;
    set.Build(rom, info, file);
    SEQLIB_CHECK_EQ(file.samples.size(), 1);
    SEQLIB_REQUIRE_EQ(file.instruments.size(), 1);
    int skipped = 0;
    for (const sf2::Zone& zone : file.instruments[0].zones)
    {
        for (const sf2::Generator& g : zone.generators)
        {
            skipped += g.oper == sf2::kStartAddrsOffset ? int(g.amount) : 0;
        }
    }
    SEQLIB_CHECK_EQ(skipped, 16);
}

void RunTests()
{
    TestDetection();
    TestOlderBuild();
    TestTiming();
    TestVoices();
    TestKitSwitch();
    TestKitVersions();
    TestLoopingPiece();
    TestConversion();
    TestLoopMarkers();
    TestPsgConversion();
    TestPsgModel();
    TestPsgTracks();
    TestMusic();
    TestShortLoop();
    TestSamplesAsMusic();
}

} // namespace agbsf2::ubimilan
