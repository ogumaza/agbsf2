// SPDX-License-Identifier: MIT

// Unit tests for Brownie Brown's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "brownie/convert.h"
#include "brownie/driver.h"
#include "brownie/sequencer.h"
#include "brownie/song.h"
#include "files.h"
#include "midi.h"
#include "music.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"

namespace agbsf2::brownie
{
namespace
{

using test::FullBendRange;
using test::Le16;
using test::Le32;
using test::MidiEvents;
using test::ReadAll;
using test::ReadMidi;
using test::ReadSf2;
using test::Sf2Records;
using test::TempPath;

// Addresses in a test cartridge: the init's setting of Timer 0 and its reload value, the per-frame routine's code that
// detection reads and its literal pool, the command table and its routines, and the driver's tables.
constexpr uint32_t kTimer = kRomBase + 0x100;
constexpr uint32_t kReload = kRomBase + 0x1F0;
constexpr uint32_t kFrame = kRomBase + 0x200;
constexpr uint32_t kFramePool = kRomBase + 0x400;
constexpr uint32_t kCommands = kRomBase + 0x500;
constexpr uint32_t kCommandCode = kRomBase + 0x600; // 0x20 bytes for each command's routine
constexpr uint32_t kLengthTable = kRomBase + 0xA00;
constexpr uint32_t kFrequencyTable = kRomBase + 0xB00;
constexpr uint32_t kControlRegisters = kRomBase + 0xC00;
constexpr uint32_t kFrequencyRegisters = kControlRegisters + 0x20;
constexpr uint32_t kPanTable = kControlRegisters + 0x40;
constexpr uint32_t kEnvelopeTable = kRomBase + 0xD00;
constexpr uint32_t kEnvelopes = kRomBase + 0xD40;
constexpr uint32_t kSampleSets = kRomBase + 0xE00;
constexpr uint32_t kSampleEntries = kRomBase + 0xE40;
constexpr uint32_t kWaveTable = kRomBase + 0xF00;
constexpr uint32_t kWaves = kRomBase + 0xF40;
constexpr uint32_t kSongTable = kRomBase + 0x1000;
constexpr uint32_t kSongLists = kRomBase + 0x1100;
constexpr uint32_t kSequences = kRomBase + 0x2000;
constexpr uint32_t kRateTable = kRomBase + 0x6000; // the Magical Vacation revision's
constexpr uint32_t kSamples = kRomBase + 0x8000;   // sample s is at kSamples + 0x100 * s

// The bytes of each sample, the semitones of a sample set and the envelopes of the test cartridge.
constexpr uint32_t kSampleBytes = 0x40;
constexpr int kSemitones = 12;
constexpr int kPsgHold = 0;     // volume 15 for as long as the note lasts
constexpr int kPsgSteps = 1;    // 15 for 2 frames, 8 for 3 frames, then 4
constexpr int kPsgRelease = 2;  // the DAC off
constexpr int kSampleHold = 3;  // level 255 for as long as the note lasts
constexpr int kSampleRamp = 4;  // level 255, down 8 a frame
constexpr int kPsgHardware = 5; // volume 15 with the hardware's envelope going down at each of its steps

// A sound's channel: its number, its data, and the places in the data, as offsets, that F6 calls and F8 jumps to.
struct TestChannel
{
    int channel = 0;
    std::vector<uint8_t> data;
    std::vector<uint32_t> targets;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, the driver's tables
// and the sounds, for a revision of the driver. Sound 0 lists no channels, since the driver's queue takes 0 for no
// request.
class Cart
{
public:
    explicit Cart(bool code = true, Revision revision = Revision::kSwordOfMana)
        : d_(0x10000, 0), vacation_(revision == Revision::kMagicalVacation)
    {
        WriteTables();
        if (code)
        {
            WriteCode();
        }
        Sound(0, {});
    }

    void Put8(uint32_t a, uint8_t v)
    {
        d_[a - kRomBase] = v;
    }

    void Put16(uint32_t a, uint16_t v)
    {
        Put8(a, uint8_t(v));
        Put8(a + 1, uint8_t(v >> 8));
    }

    void Put32(uint32_t a, uint32_t v)
    {
        Put16(a, uint16_t(v));
        Put16(a + 2, uint16_t(v >> 16));
    }

    // Writes entry `sound` of the song table: the list of its channels, and each one's data and subroutines.
    void Sound(int sound, const std::vector<TestChannel>& channels)
    {
        Put32(kSongTable + 4 * uint32_t(sound), next_list_);
        for (const TestChannel& c : channels)
        {
            const uint32_t data = next_data_;
            for (size_t i = 0; i < c.data.size(); i++)
            {
                Put8(data + uint32_t(i), c.data[i]);
            }

            const uint32_t subroutines = (data + uint32_t(c.data.size()) + 3) & ~3u;
            for (size_t i = 0; i < c.targets.size(); i++)
            {
                Put32(subroutines + 4 * uint32_t(i), data + c.targets[i]);
            }
            next_data_ = subroutines + 4 * uint32_t(c.targets.size()) + 4;

            Put32(next_list_, uint32_t(c.channel));
            Put32(next_list_ + 4, data);
            Put32(next_list_ + 8, subroutines);
            next_list_ += 12;
        }
        Put32(next_list_, 0xFF);
        next_list_ += 4;
    }

    Rom ToRom() const
    {
        Rom r;
        r.Assign(d_);

        return r;
    }

private:
    // Writes `insn`, an ldr from the pc with an offset of 0, at `at`, giving it the offset of `pool`, which holds
    // `value`.
    void Ldr(uint32_t at, uint32_t insn, uint32_t pool, uint32_t value)
    {
        Put32(at, insn | (pool - (at + 8)));
        Put32(pool, value);
    }

    // Writes the driver's tables: lengths, frequency settings tuned a third of a semitone below key 37 + n, the PSG's
    // registers and pans, envelopes, a sample set of 12 samples, of which the even ones loop, which the Magical
    // Vacation revision plays one by one, a wave, and that revision's timer reload values, 1024 Hz times 2 to the n /
    // 144.
    void WriteTables()
    {
        const uint8_t kLengths[16] = {4, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192};
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(kLengthTable + i, kLengths[i]);
        }
        for (int n = 0; n < 84; n++)
        {
            const double hz = 440 * std::pow(2.0, (37 + n - 1.0 / 3 - 69) / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(n), uint16_t(std::lround(std::max(0.0, 2048 - 131072 / hz))));
        }

        const uint32_t kControls[4] = {0x04000062, 0x04000068, 0x04000072, 0x04000078};
        const uint32_t kFrequencies[4] = {0x04000064, 0x0400006C, 0x04000074, 0x0400007C};
        for (uint32_t c = 0; c < 8; c++)
        {
            Put32(kControlRegisters + 4 * c, kControls[c & 3]);
            Put32(kFrequencyRegisters + 4 * c, kFrequencies[c & 3]);
        }
        for (uint32_t c = 0; c < 12; c++)
        {
            Put8(kPanTable + c, uint8_t(0x11 << (c & 3)));
            Put8(kPanTable + 12 + c, uint8_t(~(0x11 << (c & 3))));
        }

        const std::vector<std::vector<uint8_t>> kEnvelopeData = {
            {0, 0xF0}, {2, 0xF0, 3, 0x80, 0, 0x40}, {0, 0x00}, {0, 0x00, 0xFF}, {0, 0x88, 0xFF}, {0, 0xF1}};
        uint32_t at = kEnvelopes;
        for (size_t e = 0; e < kEnvelopeData.size(); e++)
        {
            Put32(kEnvelopeTable + 4 * uint32_t(e), at);
            for (uint8_t b : kEnvelopeData[e])
            {
                Put8(at++, b);
            }
        }

        Put32(kSampleSets, kSampleEntries);
        for (uint32_t s = 0; s < uint32_t(kSemitones); s++)
        {
            const uint32_t start = kSamples + 0x100 * s;
            const uint32_t end = start + kSampleBytes;
            Put32(kSampleEntries + 4 * s, kSampleEntries + 0x30 + 12 * s);
            Put32(kSampleEntries + 0x30 + 12 * s, start);
            Put32(kSampleEntries + 0x30 + 12 * s + 4, end);
            Put32(kSampleEntries + 0x30 + 12 * s + 8, s % 2 ? end : start);
            for (uint32_t i = 0; i < kSampleBytes; i++)
            {
                Put8(start + i, uint8_t(i * (s + 3) % 256 - 128));
            }
        }

        Put32(kWaveTable, kWaves);
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(kWaves + i, uint8_t((2 * i) << 4 | (2 * i + 1)));
        }

        for (int i = 0; i < 85 * kRateSteps; i++)
        {
            Put16(kRateTable + 2 * uint32_t(i), uint16_t(0x10000 - std::lround(16384 * std::pow(2.0, -i / 144.0))));
        }
    }

    // Writes the code that detection reads: the init's setting of Timer 0, the parts of the per-frame routine, and the
    // routines of commands F0, E1 and FC. The Magical Vacation revision's routine pushes r8 too, runs 12 channels and
    // loads the rate table, and its init doesn't set Timer 0.
    void WriteCode()
    {
        if (!vacation_)
        {
            Ldr(kTimer, 0xE59F1000, kTimer + 0x20, kReload);
            Put32(kTimer + 4, 0xE5910000);
            Put32(kTimer + 8, 0xE1C30AB0);
            Put16(kReload, 0xFC00);
        }

        // The start of the routine, its loop over the channels, and its loads of the tables.
        const uint32_t kStart[10] = {0xE92D00F0, 0xE3A07000, 0xE59F1000, 0xE5D12000, 0xE3120001,
                                     0x0A000000, 0xE5D12001, 0xE2422001, 0xE3520000, 0x0A000000};
        uint32_t at = kFrame;
        for (uint32_t w : kStart)
        {
            Put32(at, w);
            at += 4;
        }
        Ldr(kFrame + 8, 0xE59F1000, kFramePool, 0x03002ACC);
        for (uint32_t w : {0xE2811038u, 0xE2877001u, vacation_ ? 0xE337000Cu : 0xE337000Du})
        {
            Put32(at, w);
            at += 4;
        }
        if (vacation_)
        {
            Put32(kFrame, 0xE92D01F0);
        }

        uint32_t pool = kFramePool + 4;
        const auto load = [&](uint32_t insn, uint32_t value)
        {
            Ldr(at, insn, pool, value);
            at += 4;
            pool += 4;
        };

        const auto word = [&](uint32_t w)
        {
            Put32(at, w);
            at += 4;
        };

        word(0xE200001F);
        load(0xE59F3000, kCommands);
        word(0xE793F100);
        load(0xE59F2000, kSongTable);
        word(0xE7920101);
        load(0xE59F3000, kLengthTable);
        word(0xE7D30000);
        load(0xE59F2000, kFrequencyTable);
        word(0xE1A00080);
        word(0xE19250B0);
        load(0xE59F3000, kPanTable);
        word(0xE7F34001);
        load(0xE59F2000, kControlRegisters);
        word(0xE7922107);
        word(0xE1C230B0);
        load(0xE59F2000, kFrequencyRegisters);
        word(0xE7922107);
        if (vacation_)
        {
            load(0xE59F2000, kRateTable);
            word(0xE1A00080);
            word(0xE19200B0);
        }

        // The command routines, of which F0's, E1's and FC's load the envelopes, the sample sets or samples and the
        // waves.
        for (uint32_t c = 0; c < 32; c++)
        {
            const uint32_t routine = kCommandCode + 0x20 * c;
            Put32(kCommands + 4 * c, routine);
            Put32(routine, 0xE4D20001);
            const uint32_t samples = vacation_ ? kSampleEntries : kSampleSets;
            const uint32_t table = c == 0x10 ? kEnvelopeTable : c == 0x01 ? samples : kWaveTable;
            const uint32_t use = c == 0x10 ? 0xE0833100 : c == 0x01 ? 0xE7930100 : c == 0x1C ? 0xE7934100 : 0xE1A00000;
            Ldr(routine + 4, 0xE59F3000, routine + 0x10, table);
            Put32(routine + 8, use);
        }
    }

    std::vector<uint8_t> d_;
    bool vacation_;
    uint32_t next_list_ = kSongLists;
    uint32_t next_data_ = kSequences;
};

// Returns the driver info of a cartridge, failing the test if detection fails.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));
    SEQLIB_CHECK(error.empty());

    return info;
}

// Runs a sound for `frames` frames and returns each frame's register writes.
std::vector<std::vector<RegisterWrite>> Writes(const Rom& rom, const DriverInfo& info, int sound, int frames)
{
    Sequencer seq(rom, info, sound);
    std::vector<std::vector<RegisterWrite>> out;
    for (int f = 0; f < frames; f++)
    {
        out.push_back(seq.Step());
    }

    return out;
}

// Returns the value of the last write to `address` in `writes`, or -1 if there was none.
long long LastWrite(const std::vector<RegisterWrite>& writes, uint32_t address)
{
    long long value = -1;
    for (const RegisterWrite& w : writes)
    {
        if (w.address == address)
        {
            value = w.value;
        }
    }

    return value;
}

// Returns the frequency setting of key `key` in the test cartridge's table.
uint16_t Frequency(const Rom& rom, int key)
{
    return rom.U16(kFrequencyTable + 2 * uint32_t(key));
}

void TestDetection()
{
    Cart cart;
    cart.Sound(1, {{0, {0xFF}, {}}});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.revision == Revision::kSwordOfMana);
    SEQLIB_CHECK_EQ(info.frame_routine, kFrame);
    SEQLIB_CHECK_EQ(info.command_table, kCommands);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK_EQ(info.length_table, kLengthTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.pan_table, kPanTable);
    SEQLIB_CHECK_EQ(info.control_registers, kControlRegisters);
    SEQLIB_CHECK_EQ(info.frequency_registers, kFrequencyRegisters);
    SEQLIB_CHECK_EQ(info.envelope_table, kEnvelopeTable);
    SEQLIB_CHECK_EQ(info.sample_table, kSampleSets);
    SEQLIB_CHECK_EQ(info.wave_table, kWaveTable);
    SEQLIB_CHECK_EQ(info.point_cycles, 1024);
    SEQLIB_CHECK_EQ(info.mix_rate, 16384);
    SEQLIB_CHECK(info.warnings.empty());

    // A cartridge without the driver's code shows no sign of it, and with a song table given, its tables are missing.
    Cart bare(false);
    bare.Sound(1, {{0, {0xFF}, {}}});
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = kSongTable;
    DriverInfo none, given;
    std::string error;

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error));
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(!DetectDriver(bare_rom, table, given, error));
    SEQLIB_CHECK(error == "the driver's command table wasn't found");
}

// A PSG note writes the envelope's first NRx2 and starts the channel; each step of the envelope writes the next NRx2
// and starts it again; and the end turns the channel off.
void TestPsgNotes()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgSteps, 0xF1, 0x80, 0x89, 24, 0xFF}, {}}});
    cart.Sound(2, {{3, {0xF0, kPsgHold, 0x89, 0x25, 0xED, 0xFF}, {}}});
    cart.Sound(3, {{1, {0xF0, kPsgHold, 0xF2, 0xF8, 0xF9, 3, 0x89, 30, 0xFF}, {}}});
    cart.Sound(4, {{0, {0xF0, kPsgHold, 0xF1, 0x80, 0x89, 24, 0xE0, kPsgRelease, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const uint16_t note = Frequency(rom, 24);

    const auto writes = Writes(rom, info, 1, 30);
    const auto noise = Writes(rom, info, 2, 30);
    const auto quiet = Writes(rom, info, 3, 3);
    const auto release = Writes(rom, info, 4, 28);

    SEQLIB_CHECK(writes[0].empty());
    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000060), 0);
    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000062), 0xF080);
    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000064), note | 0x8000);
    SEQLIB_CHECK(writes[2].empty());
    SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x04000062), 0x8080);
    SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x04000064), note | 0x8000);
    SEQLIB_CHECK_EQ(LastWrite(writes[6], 0x04000062), 0x4080);
    SEQLIB_CHECK_EQ(LastWrite(writes[25], 0x04000062), 0);
    SEQLIB_CHECK_EQ(LastWrite(writes[25], 0x04000064), 0x8000);

    // A noise note's byte with its nibbles swapped is NR43, and the PSG's rest turns the channel off, writing the
    // routine's r5, 0 at the start of a frame, to the frequency register.
    SEQLIB_CHECK_EQ(LastWrite(noise[1], 0x0400007C), 0x8052);
    SEQLIB_CHECK_EQ(LastWrite(noise[25], 0x04000078), 0);
    SEQLIB_CHECK_EQ(LastWrite(noise[25], 0x0400007C), 0x8000);

    // A level below the channel's volume writes NRx2 8, and the detune adds to the frequency setting.
    SEQLIB_CHECK_EQ(LastWrite(quiet[1], 0x04000068), 0x0800);
    SEQLIB_CHECK_EQ(LastWrite(quiet[1], 0x0400006C), (Frequency(rom, 30) + 3) | 0x8000);

    // E0 goes on from the release envelope at the next frame.
    SEQLIB_CHECK(release[25].empty());
    SEQLIB_CHECK_EQ(LastWrite(release[26], 0x04000062), 0x0080);
    SEQLIB_CHECK_EQ(LastWrite(release[26], 0x04000064), note | 0x8000);
}

// A sample note sets its voice from the semitone's entry of the sample set, at the octave's rate, and starts the FIFOs.
// The mixer takes 16 points of each voice for every 16 points the FIFOs play, or fewer that it repeats, loops a sample
// at its end, and leaves one without a loop silent there. The end of the channel stops the FIFOs.
void TestSamples()
{
    Cart cart;
    cart.Sound(1, {{8, {0xE1, 0, 0xEA, 0xFF, 0xF0, kSampleHold, 0xF2, 0xFF, 0x89, 38, 0xFF}, {}}});
    cart.Sound(2, {{9, {0xE1, 0, 0xEA, 0x8F, 0xF0, kSampleRamp, 0xF2, 0x7F, 0x8F, 1, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 1);
    Sequencer odd(rom, info, 2);
    const uint32_t kStart = kSamples + 0x100 * 2;
    const uint32_t kOddStart = kSamples + 0x100;

    seq.Step();
    seq.Step();
    const Voice first = seq.VoicesBeforeMix()[0];
    const Voice after = seq.GetVoice(0);
    const bool mixing = seq.Mixing();
    for (int f = 2; f < 26; f++)
    {
        seq.Step();
    }

    odd.Step();
    odd.Step();
    const Voice odd_first = odd.GetVoice(1);
    for (int f = 2; f < 4; f++)
    {
        odd.Step();
    }
    const Voice odd_later = odd.GetVoice(1);

    // Key 38 plays semitone 2 at the mixer's rate, which takes 17 runs of 16 points in the first frame: 4 to the end of
    // the 64-point sample and back to its loop at its start, 3 times, and then one more.
    SEQLIB_CHECK_EQ(first.mode, 0);
    SEQLIB_CHECK_EQ(first.point, kStart);
    SEQLIB_CHECK_EQ(first.end, kStart + kSampleBytes);
    SEQLIB_CHECK_EQ(first.loop, kStart);
    SEQLIB_CHECK_EQ(first.right, 255);
    SEQLIB_CHECK_EQ(first.left, 255);
    SEQLIB_CHECK(mixing);
    SEQLIB_CHECK_EQ(after.point, kStart + 16);
    SEQLIB_CHECK(!seq.Mixing());
    SEQLIB_CHECK_EQ(seq.GetVoice(0).end, 0);

    // Key 1 plays semitone 1 at an eighth of the rate, 2 points a run, and the sample doesn't loop. The pan's nibbles
    // give the right side 0xFF and the left 0x8F, and the volume and the ramp scale the levels.
    SEQLIB_CHECK_EQ(odd_first.mode, 4);
    SEQLIB_CHECK_EQ(odd_first.point, kOddStart + 34);
    SEQLIB_CHECK_EQ(odd_first.right, 127);
    SEQLIB_CHECK_EQ(odd_first.left, 71);
    SEQLIB_CHECK_EQ(odd_later.point, kOddStart + kSampleBytes);
    SEQLIB_CHECK_EQ(odd_later.right, (((128 * 239) >> 8) * 256 >> 8) * 256 >> 8);
}

// A jump back with F8 is a loop. The conversion's loop starts where the channel first read the place it goes back to,
// and the song plays it twice.
void TestLoops()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0xF1, 0x80, 0x89, 24, 26, 0xF8, 0}, {0}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 1);
    std::vector<Event> loops;
    for (int f = 0; f < 60; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kLoop)
            {
                loops.push_back(e);
            }
        }
    }
    ConvertOptions opt;

    const SongSummary sum = InspectSong(rom, info, 1, opt);

    SEQLIB_REQUIRE_EQ(loops.size(), 1);
    SEQLIB_CHECK_EQ(loops.size() ? loops[0].frame : 0, 49);
    SEQLIB_CHECK_EQ(loops.size() ? loops[0].first : 0, 1);
    SEQLIB_CHECK(sum.ok && sum.tracks == 1);
    SEQLIB_CHECK(std::fabs(sum.loop_start - 280896.0 / 16777216) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 49 * 280896.0 / 16777216) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 97 * 280896.0 / 16777216) < 1e-9);
}

// A channel comes back to the place it loops from with the settings that the end of its loop leaves. Here that's a
// transpose of -12, where the channel first read the place with none, so its first pass plays an octave higher than the
// others, and the conversion's loop starts with its second pass.
void TestLoopSettings()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xF3, 0xF4, 24, 0xF8, 0}, {3}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;

    const SongSummary sum = InspectSong(rom, info, 1, opt);

    SEQLIB_CHECK(sum.ok && sum.tracks == 1);
    SEQLIB_CHECK(std::fabs(sum.loop_start - 49 * 280896.0 / 16777216) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 97 * 280896.0 / 16777216) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 145 * 280896.0 / 16777216) < 1e-9);
}

// A square on both sides plays a note at volume 0, and then loops a note at volume 0 and one at 6. The first time
// through, the loop's first note has the level that the note before it left, but a player that jumps back to the loop's
// start comes from the note at volume 6, so the level is written again there, and so is the tempo.
void TestLoopSettingsAgain()
{
    Cart cart;
    cart.Sound(1, {{0, {0xFD, 0x11, 0xF0, kPsgHold, 0xF2, 0, 0x89, 24, 0xF2, 0, 26, 0xF2, 6, 28, 0xF8, 0}, {8}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "loop_again";

    const SongSummary sum = ConvertSong(rom, info, 1, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    uint32_t loop_start = ~0u;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes.size() > 3 && bytes[1] == 0x06 && std::string(bytes.begin() + 3, bytes.end()) == "loopStart")
        {
            loop_start = uint32_t(tick);
        }
    }

    std::vector<std::pair<uint32_t, int>> levels;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == cc::kExpression)
        {
            levels.emplace_back(uint32_t(tick), bytes[2]);
        }
    }

    // The setup's level of 0, the first note's, the same again at the loop's start, and then the note at volume 6's.
    SEQLIB_CHECK(sum.ok && loop_start > 0);
    SEQLIB_CHECK(test::RepeatsTempoAtLoopStart(midi));
    SEQLIB_REQUIRE(levels.size() > 3);
    SEQLIB_CHECK(levels[2] == std::make_pair(loop_start, levels[1].second));
    SEQLIB_CHECK(levels[3].first > loop_start && levels[3].second < levels[2].second);
}

// A sound with too few notes for a beat plays on its frames, at 30 frames a quarter note and 480 ticks to the quarter,
// from its first frame. Each note is on the key that the driver's key stands for, and the SoundFont tunes the squares
// as the frequency table is tuned. The hardware's envelope fades a square that FD lets play on both sides, which CC11
// follows. A sample note's zone plays the semitone's sample at the key's octave.
void TestConversion()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0xF1, 0x40, 0x89, 24, 0xFF}, {}},
                   {1, {0xFD, 0x11, 0xF0, kPsgHardware, 0x89, 24, 0xFF}, {}},
                   {8, {0xE1, 0, 0xEA, 0xFF, 0xF0, kSampleHold, 0xF2, 0xFF, 0x89, 2, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "brownie";
    int base = 0;

    const int cents = PsgTuning(rom, info, base);
    const SongSummary sum = ConvertSong(rom, info, 1, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto square_on = midi.Find(0x90);
    const auto sample_on = midi.Find(0x98);
    std::vector<int> fading;
    for (const auto& [tick, bytes] : midi.Find(0xB1))
    {
        if (bytes[1] == cc::kExpression && bytes[2])
        {
            fading.push_back(bytes[2]);
        }
    }
    const Sf2Records sf2 = ReadSf2(sum.sf2_path);
    bool tuned = false, rooted = false;
    for (size_t z = 0; z + 1 < sf2.Count("ibag", 4); z++)
    {
        const auto gens = sf2.ZoneGens(z);
        tuned = tuned || (gens.count(sf2::kFineTune) && int16_t(gens.at(sf2::kFineTune)) == -33);
        rooted = rooted || (gens.count(sf2::kOverridingRootKey) && gens.at(sf2::kOverridingRootKey) == 75 &&
                            gens.count(sf2::kKeyRange) && gens.at(sf2::kKeyRange) == (39 | 39 << 8));
    }

    SEQLIB_CHECK_EQ(base, 37);
    SEQLIB_CHECK_EQ(cents, -33);
    SEQLIB_CHECK(sum.ok && sum.tracks == 3);
    SEQLIB_CHECK_EQ(midi.division, 480);
    SEQLIB_REQUIRE_EQ(square_on.size(), 1);
    SEQLIB_CHECK_EQ(square_on.size() ? square_on[0].tick : 1, 0);
    SEQLIB_CHECK_EQ(square_on.size() ? square_on[0].bytes[1] : 0, 61);
    SEQLIB_CHECK_EQ(sample_on.size() ? sample_on[0].bytes[1] : 0, 39);
    SEQLIB_CHECK(tuned);
    SEQLIB_CHECK(rooted);
    SEQLIB_CHECK(FullBendRange(sf2));
    SEQLIB_CHECK(fading.size() > 10 && std::is_sorted(fading.rbegin(), fading.rend()));
}

// ED's rest mutes the wave channel, as does an envelope's step to a level of 0, and either ends its note. The rest
// writes the routine's r5, the square's frequency setting here, to the wave's, which bends nothing.
void TestWaveRests()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 30, 30, 30, 0xFF}, {}},
                   {2, {0xFC, 0, 0xF0, kPsgHold, 0x89, 24, 0xED, 24, 0xE0, kPsgRelease, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "brownie_wave";

    const SongSummary sum = ConvertSong(rom, info, 1, opt, nullptr);

    // The sound plays on its frames, 16 ticks each. The wave's notes start in frames 1 and 49, and end at the rest in
    // frame 25 and the release's step in frame 74.
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x92);
    const auto offs = midi.Find(0x82);
    SEQLIB_CHECK(sum.ok && sum.tracks == 2);
    SEQLIB_REQUIRE_EQ(ons.size(), 2);
    SEQLIB_REQUIRE_EQ(offs.size(), 2);
    SEQLIB_CHECK_EQ(ons.size() == 2 ? ons[1].tick : 0, 48 * 16);
    SEQLIB_CHECK_EQ(offs.size() == 2 ? offs[0].tick : 0, 24 * 16);
    SEQLIB_CHECK_EQ(offs.size() == 2 ? offs[1].tick : 0, 73 * 16);
    SEQLIB_CHECK(midi.Find(0xE2).empty());
}

void TestDump()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xF8, 0}, {0}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::string path = Utf8(TempPath("brownie_dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 1, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("08002002       1  89     length 9 of the set: 24 frames") != std::string::npos);
    SEQLIB_CHECK(s.find("08002003       1  18     note 24") != std::string::npos);
    SEQLIB_CHECK(s.find("08002004      25  F8 00  jump to 0") != std::string::npos);
    SEQLIB_CHECK(s.find("; goes back to 0x08002000, frame 1") != std::string::npos);
}

// A trace prints the channels' records, the voices and the variables as the driver keeps them in memory.
void TestRecords()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}},
                   {8, {0xE1, 0, 0xEA, 0xFF, 0xF0, kSampleHold, 0xF2, 0xFF, 0x89, 38, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 1);

    seq.Step();
    const Globals queued = seq.GetGlobals();
    seq.Step();
    const std::array<uint8_t, 0x38> record = Sequencer::RecordBytes(seq.GetChannel(0));
    const std::array<uint8_t, 0x10> voice = Sequencer::VoiceBytes(seq.VoicesBeforeMix()[0]);

    // The queue's offsets after the request, the square's flags, wait, envelope count, data and envelope, and the
    // voice's mode, levels and sample.
    const uint32_t envelope = rom.U32(kEnvelopeTable + 4 * kPsgHold);
    const uint32_t kStart = kSamples + 0x100 * 2;
    SEQLIB_CHECK_EQ(queued[kQueueRead], 2);
    SEQLIB_CHECK_EQ(queued[kQueueWrite], 2);
    SEQLIB_CHECK_EQ(queued[kMaster], 0xFF);
    SEQLIB_CHECK_EQ(record[0x00], kFlagOn);
    SEQLIB_CHECK_EQ(record[0x01], 24);
    SEQLIB_CHECK_EQ(record[0x02], 0);
    SEQLIB_CHECK_EQ(Le32(&record[0x04]), kSequences + 4);
    SEQLIB_CHECK_EQ(Le16(&record[0x22]), 0xF000);
    SEQLIB_CHECK_EQ(Le32(&record[0x30]), envelope);
    SEQLIB_CHECK_EQ(Le32(&record[0x34]), envelope + 2);
    SEQLIB_CHECK_EQ(voice[0], 0);
    SEQLIB_CHECK_EQ(voice[2], 255);
    SEQLIB_CHECK_EQ(voice[3], 255);
    SEQLIB_CHECK_EQ(Le32(&voice[4]), kStart);
    SEQLIB_CHECK_EQ(Le32(&voice[8]), kStart + kSampleBytes);
    SEQLIB_CHECK_EQ(Le32(&voice[12]), kStart);
}

// OpenMusic() finds the driver from its code. A game without the driver's code isn't read, even with the song table
// given, and only --driver brownie makes that an error.
void TestMusic()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    Cart bare(false);
    bare.Sound(1, {{0, {0xFF}, {}}});
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = kSongTable;
    Overrides forced = table;
    forced.driver = Driver::kBrownie;
    std::string error, table_error, forced_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = brownie::OpenMusic(bare_rom, table, table_error);
    const std::unique_ptr<Music> bare_forced = brownie::OpenMusic(bare_rom, forced, forced_error);

    SEQLIB_CHECK(music && music->Log().front() == "Brownie Brown's sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 2);
    SEQLIB_CHECK(!bare_music && table_error.empty());
    SEQLIB_CHECK(!bare_forced && forced_error == "the driver's command table wasn't found");
}

// The Magical Vacation revision: 12 channels, with a sample on each FIFO, which a sample note starts at the rate
// table's rate for its key and its detune. The FIFO's interrupt gives it 4 points whenever it holds 16 or fewer after
// playing one, and stops its timer at the end of a sample that doesn't loop. EC stops the timer of the FIFO of the
// channel's number & 1, on a PSG channel too, and E2-E9 are F2-F9 again.
void TestVacation()
{
    Cart cart(true, Revision::kMagicalVacation);
    cart.Sound(1, {{8, {0xE1, 1, 0xF0, kSampleHold, 0xE2, 0xFF, 0x80, 48, 0xFF}, {}}});
    cart.Sound(2, {{9, {0xE1, 0, 0xF0, kSampleHold, 0xF2, 0xFF, 0xF9, 3, 0x89, 48, 0xEC, 0xFF}, {}}});
    cart.Sound(3,
               {{9, {0xE1, 0, 0xF0, kSampleHold, 0xF2, 0xFF, 0x89, 48, 0xFF}, {}}, {1, {0x82, 0xFE, 0xEC, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer one_shot(rom, info, 1);
    Sequencer looped(rom, info, 2);
    Sequencer stopped(rom, info, 3);
    const uint32_t kStart = kSamples + 0x100;
    const uint32_t kLoopStart = kSamples;

    one_shot.Step();
    const Globals queued = one_shot.GetGlobals();
    one_shot.Step();
    const Fifo started = one_shot.FifosBeforeInterrupts()[0];
    const FifoVoice voice = one_shot.FifoVoicesBeforeInterrupts()[0];
    const std::array<uint8_t, 0x10> played = one_shot.VoiceRecord(0);
    one_shot.Step();
    const bool ended = !one_shot.FifosBeforeInterrupts()[0].running;

    for (int f = 0; f < 25; f++)
    {
        looped.Step();
    }
    const Fifo looping = looped.FifosBeforeInterrupts()[1];
    const uint32_t point = looped.FifoVoicesBeforeInterrupts()[1].point;
    looped.Step();
    const bool rested = !looped.FifosBeforeInterrupts()[1].running;

    for (int f = 0; f < 3; f++)
    {
        stopped.Step();
    }
    const bool playing = stopped.FifosBeforeInterrupts()[1].running;
    stopped.Step();
    const bool stopped_by_psg = !stopped.FifosBeforeInterrupts()[1].running;

    // Key 48 plays at 16384 Hz. The one-shot 64-byte sample runs out in its first frame, which stops the timer, and the
    // trace keeps the FIFO's sample at its end.
    SEQLIB_CHECK(info.revision == Revision::kMagicalVacation);
    SEQLIB_CHECK_EQ(info.rate_table, kRateTable);
    SEQLIB_CHECK_EQ(info.sample_table, kSampleEntries);
    SEQLIB_CHECK_EQ(info.envelope_table, kEnvelopeTable);
    SEQLIB_CHECK_EQ(one_shot.ChannelCount(), 12);
    SEQLIB_CHECK_EQ(one_shot.VoiceCount(), 2);
    SEQLIB_CHECK_EQ(one_shot.GlobalsSize(), 44);
    SEQLIB_CHECK_EQ(queued[kQueueRead], 2);
    SEQLIB_CHECK_EQ(queued[kQueueWrite], 2);
    SEQLIB_CHECK_EQ(Le16(&queued[kVacationSoundcntLCopy]), 0x77);
    SEQLIB_CHECK_EQ(queued[kVacationMaster], 0xFF);
    SEQLIB_CHECK(started.running);
    SEQLIB_CHECK_EQ(started.reload, 0xFC00);
    SEQLIB_CHECK_EQ(started.owner, 8);
    SEQLIB_CHECK_EQ(voice.point, kStart + 4);
    SEQLIB_CHECK_EQ(voice.end, kStart + kSampleBytes);
    SEQLIB_CHECK_EQ(voice.loop, kStart + kSampleBytes);
    SEQLIB_CHECK_EQ(voice.volume, 255);
    SEQLIB_CHECK_EQ(Le32(&played[0]), kStart + kSampleBytes);
    SEQLIB_CHECK(ended);

    // The detune of 3 moves the rate by a quarter of a semitone, and the looping sample goes back to its start.
    SEQLIB_CHECK(looping.running);
    SEQLIB_CHECK_EQ(looping.reload, rom.U16(kRateTable + 2 * (48 * kRateSteps + 3)));
    SEQLIB_CHECK(point >= kLoopStart && point < kLoopStart + kSampleBytes);
    SEQLIB_CHECK(rested);
    SEQLIB_CHECK(playing);
    SEQLIB_CHECK(stopped_by_psg);
}

// The Magical Vacation revision's conversion: each key of a sample's instrument plays the sample at the rate table's
// rate for the key, through the zone's root key and fine tune, and the detune becomes a bend.
void TestVacationConversion()
{
    Cart cart(true, Revision::kMagicalVacation);
    cart.Sound(1, {{8, {0xE1, 0, 0xF0, kSampleHold, 0xF2, 0xFF, 0x89, 49, 0xF9, 3, 48, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "vacation";

    const SongSummary sum = ConvertSong(rom, info, 1, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto notes = midi.Find(0x98);
    const auto bends = midi.Find(0xE8);
    const Sf2Records sf2 = ReadSf2(sum.sf2_path);

    bool tuned = false, exact = false;
    for (size_t z = 0; z + 1 < sf2.Count("ibag", 4); z++)
    {
        const auto gens = sf2.ZoneGens(z);
        const auto has = [&](uint16_t gen, uint16_t value)
        {
            return gens.count(gen) && gens.at(gen) == value;
        };

        tuned = tuned || (has(sf2::kKeyRange, 86 | 86 << 8) && has(sf2::kOverridingRootKey, 85) &&
                          has(sf2::kFineTune, uint16_t(-1)));
        exact = exact ||
                (has(sf2::kKeyRange, 85 | 85 << 8) && has(sf2::kOverridingRootKey, 85) && !gens.count(sf2::kFineTune));
    }

    const double bend = 12 * std::log2(1024.0 / (0x10000 - rom.U16(kRateTable + 2 * (48 * kRateSteps + 3))));
    const int expected = 8192 + int(std::lround(bend / 2 * 8192));
    int last = 8192;
    for (const auto& [tick, bytes] : bends)
    {
        last = bytes[1] | bytes[2] << 7;
    }

    // Key 49 plays at 17349 Hz, a cent below the semitone above the samples' 16384 Hz, so its zone has key 85 for its
    // root and a fine tune of -1, and key 48 plays at 16384 Hz on its own key.
    SEQLIB_CHECK(sum.ok && sum.tracks == 1);
    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    SEQLIB_CHECK_EQ(notes.size() == 2 ? notes[0].bytes[1] : 0, 86);
    SEQLIB_CHECK_EQ(notes.size() == 2 ? notes[1].bytes[1] : 0, 85);
    SEQLIB_CHECK(tuned);
    SEQLIB_CHECK(exact);
    SEQLIB_CHECK_EQ(last, expected);
}

} // namespace

void RunTests()
{
    TestDetection();
    TestPsgNotes();
    TestSamples();
    TestLoops();
    TestLoopSettings();
    TestLoopSettingsAgain();
    TestConversion();
    TestWaveRests();
    TestDump();
    TestRecords();
    TestMusic();
    TestVacation();
    TestVacationConversion();
}

} // namespace agbsf2::brownie
