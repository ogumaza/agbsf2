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
constexpr uint32_t kSquare1Table = kRomBase + 0x600;
constexpr uint32_t kSquare2Table = kRomBase + 0x7E0;
constexpr uint32_t kWaveInstruments = kRomBase + 0x9C0;
constexpr uint32_t kFrequencyTable = kRomBase + 0xC00;
constexpr uint32_t kNoiseTable = kRomBase + 0xD00;
constexpr uint32_t kWaveTable = kRomBase + 0xD80;
constexpr uint32_t kBank = kRomBase + 0x1000;

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

class Cart : public test::CartImage
{
public:
    Cart() : CartImage(0x10000)
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

private:
    // The driver's code that detection reads: the sequencer's step, the routine that takes the sound bank, the game's
    // routine that calls it, the archive's lookup, which returns the bank, the call of the two with file 2, the PSG's
    // lookups of its tables, and the sound init with Timer 0's float.
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
    }

    // Square 1's instrument 1: a 50% duty and an envelope that the velocity sets; the frequency table, equal-tempered
    // from key 36; a noise key; and a wave.
    void WriteTables()
    {
        Put8(kSquare1Table + 16 + 1, 0x80);
        Put8(kSquare1Table + 16 + 3, 0x80);
        for (int i = 0; i < 128; i++)
        {
            const double hz = 440.0 * std::pow(2.0, (i + 36 - 69) / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(i), uint16_t(std::max(0L, std::lround(2048 - 131072 / hz))));
        }
        Put32(kNoiseTable, 0xC000F01E);
        for (uint32_t i = 0; i < 32; i++)
        {
            Put8(kWaveTable + i, uint8_t(i * 8));
        }
    }

    // The sound bank: its counts and the offsets of its tables, the table of resources, the kit table, and the
    // resources, each at a word's boundary.
    void WriteBank()
    {
        uint32_t next = kBank + 0x80;
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

        Put16(kBank + 2, kResourceCount);
        Put32(kBank + 4, 0x10);
        Put32(kBank + 8, 0x10);
        Put32(kBank + 12, 0x60);
        Put16(kBank + 0x60, kKitId);

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

    std::array<uint32_t, kResourceCount> resources_ = {};
};

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, DriverOverrides(), info, error);

    return info;
}

// Detection finds the sequencer's step by its code, the sound bank through the archive's lookup that the game calls
// with file 2, the pieces of music among the resources, channel 9's kit, the PSG's tables and Timer 0's reload from the
// sound init's float. A cartridge without the code shows no sign of the driver.
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
    SEQLIB_CHECK_EQ(info.timer_reload, 0xFDFF);
    SEQLIB_CHECK(info.warnings.empty());
    SEQLIB_CHECK(!found_blank && blank_error.empty());

    // Detection starts from nothing. A game without the driver keeps none of an earlier game's tables.
    SEQLIB_CHECK(!DetectDriver(blank, DriverOverrides(), info, error));
    SEQLIB_CHECK(info.step_routine == 0 && info.songs.empty());
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
// reaches the end of its 4000 points, in frame 14. The PSG's note at volume 7 plays until its note-off, the one at
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
    SEQLIB_CHECK(notes[1].key == 42 && notes[1].on == 0 && notes[1].off == 15);
    SEQLIB_CHECK(notes[2].key == 41 && notes[2].on == 5 && notes[2].off == 10);
    SEQLIB_CHECK(notes[3].channel == 0 && notes[3].key == 62 && notes[3].envelope == 0);
    SEQLIB_CHECK(notes[3].on == 20 && notes[3].off == 25);
    SEQLIB_CHECK(warnings ==
                 std::vector<std::string>({"channel 9's key 50 plays no sample, which the game doesn't check"}));
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
        SEQLIB_CHECK_EQ(Le32(looped + 36), 16352);
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

// The PSG's note at volume 7 plays on MIDI channel 1. The channel has square 1's bank and the program of the note's
// instrument. The velocity is 127 times the square root of 7 over 15. The note at volume 0 is left out, and so is the
// key outside the kit, whose warning the summary has.
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
    SEQLIB_CHECK(ons[0].bytes == std::vector<uint8_t>({0x90, 60, 87}));
    SEQLIB_REQUIRE_EQ(midi.Find(0xC0).size(), 1);
    SEQLIB_REQUIRE_EQ(midi.Find(0x9A).size(), 2);
    SEQLIB_CHECK(!sum.warnings.empty() &&
                 sum.warnings.front() == "channel 9's key 50 plays no sample, which the game doesn't check");
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

void RunTests()
{
    TestDetection();
    TestTiming();
    TestVoices();
    TestLoopingPiece();
    TestConversion();
    TestLoopMarkers();
    TestPsgConversion();
    TestMusic();
    TestShortLoop();
}

} // namespace agbsf2::ubimilan
