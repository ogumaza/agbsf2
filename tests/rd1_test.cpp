// SPDX-License-Identifier: MIT

// Unit tests for Nintendo R&D1's MIDI-file driver. They build small synthetic cartridges in memory, so no game ROM is
// needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "gba.h"
#include "music.h"
#include "rd1/convert.h"
#include "rd1/driver.h"
#include "rd1/sequencer.h"
#include "rd1/song.h"
#include "rd1/soundfont.h"
#include "rom.h"
#include "test_util.h"

namespace agbsf2::rd1
{
namespace
{

using test::ReadMidi;
using test::ReadSf2;
using test::TempPath;

// Addresses in a test cartridge.
constexpr uint32_t kPlay = kRomBase + 0x100;
constexpr uint32_t kStart = kRomBase + 0x140;
constexpr uint32_t kFrame = kRomBase + 0x160;
constexpr uint32_t kInit = kRomBase + 0x180;
constexpr uint32_t kBoot = kRomBase + 0x280;
constexpr uint32_t kEcho = kRomBase + 0x2A0;
constexpr uint32_t kLfo = kRomBase + 0x2C0;
constexpr uint32_t kNoise = kRomBase + 0x300;
constexpr uint32_t kWave = kRomBase + 0x320;
constexpr uint32_t kPitchTable = kRomBase + 0x400;
constexpr uint32_t kSineTable = kRomBase + 0x600;
constexpr uint32_t kNoiseTable = kRomBase + 0x800;
constexpr uint32_t kWaveLevels = kRomBase + 0x840;
constexpr uint32_t kPlayerTable = kRomBase + 0x860;
constexpr uint32_t kPlayerSetup = kRomBase + 0x880;
constexpr uint32_t kBankTable = kRomBase + 0x8C0;
constexpr uint32_t kBank = kRomBase + 0x900;
constexpr uint32_t kSongTable = kRomBase + 0xB00;
constexpr uint32_t kRecords = kRomBase + 0xB40;
constexpr uint32_t kSweepInstrument = kRomBase + 0xBC0;
constexpr uint32_t kName = kRomBase + 0xBF0;
constexpr uint32_t kSampleInstrument = kRomBase + 0xC00;
constexpr uint32_t kSquareInstrument = kRomBase + 0xC20;
constexpr uint32_t kSampleHeader = kRomBase + 0xC60;
constexpr uint32_t kSamplePoints = kRomBase + 0xC80;
constexpr uint32_t kFiles = kRomBase + 0x1000;

constexpr int kTempoBpm120 = 500000;
constexpr int kDivision = 24;

// A track's events, with a delta time before each: a tempo, a marker, a controller, a program, a note on and off.
class TrackBytes
{
public:
    TrackBytes& Tempo(uint32_t delta, int micros)
    {
        Delta(delta);
        Add({0xFF, 0x51, 3, uint8_t(micros >> 16), uint8_t(micros >> 8), uint8_t(micros)});
        return *this;
    }
    TrackBytes& Marker(uint32_t delta, char text)
    {
        Delta(delta);
        Add({0xFF, 0x06, 1, uint8_t(text)});
        return *this;
    }
    TrackBytes& Control(uint32_t delta, int controller, int value)
    {
        Delta(delta);
        Add({0xB0, uint8_t(controller), uint8_t(value)});
        return *this;
    }
    TrackBytes& Program(uint32_t delta, int program)
    {
        Delta(delta);
        Add({0xC0, uint8_t(program)});
        return *this;
    }
    TrackBytes& Bend(uint32_t delta, int value)
    {
        Delta(delta);
        Add({0xE0, uint8_t(value & 0x7F), uint8_t(value >> 7)});
        return *this;
    }
    TrackBytes& On(uint32_t delta, int key, int velocity)
    {
        Delta(delta);
        Add({0x90, uint8_t(key), uint8_t(velocity)});
        return *this;
    }
    TrackBytes& Off(uint32_t delta, int key)
    {
        Delta(delta);
        Add({0x80, uint8_t(key), 0});
        return *this;
    }
    TrackBytes& End(uint32_t delta)
    {
        Delta(delta);
        Add({0xFF, 0x2F, 0});
        return *this;
    }
    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
    }

private:
    void Delta(uint32_t delta)
    {
        if (delta >= 0x80)
        {
            bytes_.push_back(uint8_t(0x80 | (delta >> 7)));
        }
        bytes_.push_back(uint8_t(delta & 0x7F));
    }
    void Add(std::initializer_list<uint8_t> list)
    {
        bytes_.insert(bytes_.end(), list);
    }
    std::vector<uint8_t> bytes_;
};

// Returns a Standard MIDI File of format 1.
std::vector<uint8_t> MidiFileBytes(const std::vector<TrackBytes>& tracks)
{
    const auto word32 = [](std::vector<uint8_t>& out, uint32_t v)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            out.push_back(uint8_t(v >> shift));
        }
    };
    std::vector<uint8_t> out = {'M', 'T', 'h', 'd'};
    word32(out, 6);
    out.insert(out.end(), {0, 1, 0, uint8_t(tracks.size()), 0, kDivision});
    for (const TrackBytes& t : tracks)
    {
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        word32(out, uint32_t(t.Bytes().size()));
        out.insert(out.end(), t.Bytes().begin(), t.Bytes().end());
    }
    return out;
}

// A cartridge with the driver's code that detection reads, two empty entries and four songs in its song table. Song 2
// loops over ticks 24 to 72. Song 3 doesn't loop. It plays a square wave with a bend, then square 1 with a sweep.
// Songs 4 and 5 play a square's note of 12 ticks, then wait 420 and 600 ticks before a CC10 and their end.
class Cart : public test::CartImage
{
public:
    Cart() : CartImage(0x4000)
    {
        WriteCode();
        WriteTables();
        WriteInstruments();
        WriteSongs();
    }

private:
    // Writes the Thumb `ldr rN, =value` at `at`, with its literal at `literal`. `op` holds the instruction's register.
    void PutLoad(uint32_t at, uint16_t op, uint32_t literal, uint32_t value)
    {
        Put16(at, uint16_t(op | ((literal - ((at + 4) & ~3u)) / 4)));
        Put32(literal, value);
    }

    void WriteCode()
    {
        PutHalfwords(kPlay, {0xB500, 0x0400, 0x4B00, 0x4900, 0x0B40, 0x1840, 0x8882, 0x0051, 0x1889, 0x0089, 0x18C9,
                             0x680A, 0x6801, 0x1C10});
        PutBl(kPlay + 0x1C, kStart);
        PutHalfwords(kPlay + 0x20, {0xBC01, 0x4700});
        PutLoad(kPlay + 4, 0x4B00, kPlay + 0x24, kPlayerTable);
        PutLoad(kPlay + 6, 0x4900, kPlay + 0x28, kSongTable);

        PutHalfwords(kStart, {0x4900, 0x88A8, 0x0440, 0x0D80});
        PutLoad(kStart, 0x4900, kStart + 8, kBankTable);

        PutHalfwords(kFrame,
                     {0xB5F0, 0x464F, 0x4646, 0xB4C0, 0x4800, 0x2600, 0x5786, 0x2701, 0x57C7, 0x2102, 0x5641, 0x4688});

        PutHalfwords(kInit, {0xB5F0, 0x4647, 0xB480, 0xB084, 0x4900, 0x22C4, 0x00D2, 0x4B00, 0x2080, 0x9000});
        PutLoad(kInit + 8, 0x4900, kInit + 0x80, 13379);
        PutHalfwords(kInit + 0x20, {0x2500, 0x4E00, 0x46B0, 0x2700});
        PutLoad(kInit + 0x22, 0x4E00, kInit + 0x84, kPlayerSetup);
        PutHalfwords(kInit + 0x30, {0x3614, 0x3714, 0x3501, 0x2D01, 0xD900});
        PutHalfwords(kInit + 0x40, {0x4900, 0x2008});
        PutBl(kInit + 0x44, kBoot);
        Put16(kInit + 0x48, 0x2500);

        PutBl(kBoot, kInit);
        PutHalfwords(kBoot + 4, {0x2023, 0x2102, 0x2202, 0x2304});
        PutBl(kBoot + 12, kEcho);

        PutHalfwords(kEcho, {0xB510, 0x4C00, 0x7020, 0x7061, 0x70A2, 0x70E3, 0xBC10, 0xBC01, 0x4700});
        PutHalfwords(kLfo, {0xB570, 0x1C06, 0x1C0B, 0x0159, 0x69B0, 0x1844, 0x7D60, 0x2800, 0xD000, 0x3801, 0x2100,
                            0x7560, 0x8261, 0xE000, 0x8A20, 0x8A61, 0x1840, 0x8260, 0x6861, 0x02C9, 0x0E49, 0x4A00});
        PutLoad(kLfo + 0x2A, 0x4A00, kLfo + 0x30, kSineTable);
        PutHalfwords(kNoise, {0x4800, 0x3915, 0x1809, 0x780D});
        PutLoad(kNoise, 0x4800, kNoise + 8, kNoiseTable);
        PutHalfwords(kWave, {0x4800, 0x4662, 0x0891, 0x0049, 0x1809});
        PutLoad(kWave, 0x4800, kWave + 0xC, kWaveLevels);
    }

    void WriteTables()
    {
        for (int key = 0; key < 128; key++)
        {
            const int hz = key == 0 ? 8 : int(std::lround(440 * std::exp2((key - 69) / 12.0)));
            Put16(kPitchTable + 2 * uint32_t(key), uint16_t(key == 127 ? 12544 : hz));
        }
        for (int i = 0; i < 256; i++)
        {
            Put16(kSineTable + 2 * uint32_t(i), uint16_t(int16_t(std::lround(256 * std::sin(i * 6.283185307 / 256)))));
        }
        Put16(kPlayerSetup, uint16_t(16 << 5));
        Put16(kPlayerSetup + 20, uint16_t(4 << 5 | 1 << 10));
        Put32(kBankTable, kBank);
    }

    void WriteInstruments()
    {
        Put32(kBank, kSampleInstrument);
        Put32(kBank + 4, kSquareInstrument);
        Put32(kBank + 8, kSweepInstrument);

        Put8(kSampleInstrument, 0x41);
        Put8(kSampleInstrument + 1, 60);
        Put16(kSampleInstrument + 2, 0x7F);
        Put32(kSampleInstrument + 4, kSampleHeader);
        Put32(kSampleInstrument + 8, 0x7F0000);
        Put32(kSampleInstrument + 12, 0x7F0000);
        Put32(kSampleInstrument + 28, 0x60000);

        Put32(kSampleHeader, 32);
        Put32(kSampleHeader + 4, 13379);
        Put32(kSampleHeader + 8, 60);
        Put32(kSampleHeader + 12, 0);
        Put32(kSampleHeader + 16, 32);
        Put32(kSampleHeader + 20, kSamplePoints);
        for (uint32_t i = 0; i < 32; i++)
        {
            Put8(kSamplePoints + i, uint8_t(i < 16 ? 64 : -64));
        }

        Put8(kSquareInstrument, 0x50);
        Put8(kSquareInstrument + 1, 60);
        Put16(kSquareInstrument + 2, 0x7F);
        Put32(kSquareInstrument + 8, 0x7F0000);
        Put32(kSquareInstrument + 12, 0x7F0000);
        Put32(kSquareInstrument + 28, 0x7F0000);
        Put8(kSquareInstrument + 32, 1);
        Put16(kSquareInstrument + 34, 2 << 1);

        // Square 1 with a rising sweep: NR10 0x14 adds a sixteenth of the frequency setting each 1/128 s.
        Put8(kSweepInstrument, 0x50);
        Put8(kSweepInstrument + 1, 60);
        Put16(kSweepInstrument + 2, 0x7F);
        Put32(kSweepInstrument + 8, 0x7F0000);
        Put32(kSweepInstrument + 12, 0x7F0000);
        Put32(kSweepInstrument + 28, 0x7F0000);
        Put32(kSweepInstrument + 32, 0x14 << 10 | (2 << 1) << 16);
    }

    void WriteRecord(int entry, uint32_t file, int player, int bank, int volume, int priority)
    {
        const uint32_t record = kRecords + 20 * uint32_t(entry);
        Put32(kSongTable + 8 * uint32_t(entry), record);
        Put16(kSongTable + 8 * uint32_t(entry) + 4, uint16_t(player));
        Put32(record, file);
        Put32(record + 4, uint32_t(player) | uint32_t(bank) << 5 | uint32_t(volume) << 15 | uint32_t(priority) << 22);
        Put32(record + 12, kName);
    }

    void WriteSongs()
    {
        PutBytes(kName, {'m', '_', 'B', 'G', 'M', 0});

        TrackBytes conductor;
        conductor.Tempo(0, kTempoBpm120).Marker(24, '[').Marker(48, ']').End(24);
        TrackBytes notes;
        notes.Control(0, 7, 127)
            .Control(0, 10, 64)
            .Program(0, 0)
            .On(0, 60, 100)
            .Off(12, 60)
            .On(12, 64, 127)
            .Off(24, 64);
        notes.End(48);
        const std::vector<uint8_t> looped = MidiFileBytes({conductor, notes});
        PutBytes(kFiles, looped);
        WriteRecord(2, kFiles, 0, 0, 100, 3);

        TrackBytes tempo;
        tempo.Tempo(0, kTempoBpm120).End(48);
        TrackBytes rest;
        rest.End(0);
        TrackBytes square;
        square.Control(0, 20, 12).Control(0, 72, 1).Control(0, 10, 40).Program(0, 1).On(0, 69, 127);
        square.Bend(12, 0x3FFF).Off(12, 69).End(12);
        TrackBytes sweep;
        sweep.Program(0, 2).On(36, 69, 127).Off(12, 69).End(0);
        const uint32_t second = kFiles + uint32_t(looped.size());
        const std::vector<uint8_t> bend = MidiFileBytes({tempo, rest, square, sweep});
        PutBytes(second, bend);
        WriteRecord(3, second, 0, 0, 127, 0);

        uint32_t next = second + uint32_t(bend.size());
        for (int entry = 4; entry <= 5; entry++)
        {
            TrackBytes note;
            note.Control(0, 7, 127).Program(0, 1).On(0, 69, 127).Off(12, 69);
            note.Control(entry == 4 ? 420 : 600, 10, 64).End(0);
            const std::vector<uint8_t> file = MidiFileBytes({tempo, note});
            PutBytes(next, file);
            WriteRecord(entry, next, 0, 0, 127, 0);
            next += uint32_t(file.size());
        }

        // The word after the last entry isn't a record.
        Put32(kSongTable + 8 * 6, 0xFFFFFFFF);
    }
};

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, info, error));
    SEQLIB_CHECK(error.empty());
    return info;
}

// Detection reads the tables that the game's code names. The empty entries stay in the song table's count.
void TestDetection()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.song_count, 6);
    SEQLIB_CHECK_EQ(info.bank_count, 1);
    SEQLIB_CHECK_EQ(info.mix_rate, 13379);
    SEQLIB_CHECK_EQ(info.voices, 8);
    SEQLIB_REQUIRE_EQ(info.players.size(), 2);
    SEQLIB_CHECK(!info.players[0].effects && info.players[1].effects);
    SEQLIB_CHECK(info.echo[0] == 0x23 && info.echo[1] == 2 && info.echo[2] == 2 && info.echo[3] == 4);
    SEQLIB_CHECK(info.echo_call != 0);

    SongRecord record;
    SEQLIB_CHECK(!ReadSongRecord(rom, info, 0, record));
    SEQLIB_REQUIRE(ReadSongRecord(rom, info, 2, record));
    SEQLIB_CHECK_EQ(record.midi, kFiles);
    SEQLIB_CHECK_EQ(record.player, 0);
    SEQLIB_CHECK_EQ(record.bank, 0);
    SEQLIB_CHECK_EQ(record.volume, 100);
    SEQLIB_CHECK(record.name == "m_BGM");
}

// A game with no sign of the driver gives no driver and no error.
void TestNoDriver()
{
    const test::CartImage empty(0x1000);
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(!DetectDriver(empty.ToRom(), info, error));
    SEQLIB_CHECK(error.empty());
}

// The step comes from the tempo in whole beats a minute. The loop's end sends every track back to where it stood at
// the start of the frame that held the loop's start. The channel keeps its settings.
void TestSequencer()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SongRecord record;
    SEQLIB_REQUIRE(ReadSongRecord(rom, info, 2, record));
    Sequencer seq(rom, info, record);
    SEQLIB_REQUIRE(seq.Valid());
    SEQLIB_CHECK_EQ(seq.Division(), kDivision);

    std::vector<SeqEvent> ons;
    int ends = 0;
    for (int f = 0; f < 400 && seq.LoopPasses() < 2; f++)
    {
        for (const SeqEvent& e : seq.Step())
        {
            if (e.kind == SeqEvent::Kind::kNoteOn)
            {
                ons.push_back(e);
            }
            ends += e.kind == SeqEvent::Kind::kLoopEnd ? 1 : 0;
        }
    }
    SEQLIB_CHECK_EQ(seq.TickStep(), 120 * 256 * kDivision / 3600);
    SEQLIB_CHECK_EQ(seq.Bpm(), 120);
    SEQLIB_CHECK_EQ(ends, 2);
    SEQLIB_REQUIRE(ons.size() >= 3);
    SEQLIB_CHECK_EQ(ons[0].channel, 1);
    SEQLIB_CHECK_EQ(ons[0].key, 60);
    SEQLIB_CHECK_EQ(ons[0].tick, 0);
    SEQLIB_CHECK_EQ(ons[1].key, 64);
    SEQLIB_CHECK_EQ(ons[1].tick, 24);
    SEQLIB_CHECK_EQ(ons[2].key, 64);
    SEQLIB_CHECK_EQ(ons[2].tick, 72);
    SEQLIB_CHECK_EQ(seq.Channel(1).Volume(), 127);
}

// The MIDI file keeps the song's ticks, with 20 of its ticks to each one of the song's. The tempo is the step's, not
// the file's. The velocity holds the note's level. The channel's level goes to CC11. The loop's markers fall on the
// song's markers, and the notes of the next pass stay out past the end.
void TestConversion()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = test::TempSettings("rd1_loop");
    opt.loops = 1;

    const SongReport sum = ConvertSong(rom, info, 2, opt, nullptr);
    SEQLIB_CHECK(sum.ok && !sum.silent);
    SEQLIB_CHECK_EQ(sum.tracks, 1);
    SEQLIB_CHECK(sum.loop_start > 0 && sum.loop_end > sum.loop_start);

    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    SEQLIB_CHECK_EQ(midi.division, 480);
    const auto tempos = midi.Find(0xFF);
    bool tempo_found = false;
    std::vector<uint64_t> markers;
    for (const auto& [tick, bytes] : tempos)
    {
        if (bytes.size() > 1 && bytes[1] == 0x51 && !tempo_found)
        {
            tempo_found = true;
            const int micros = bytes[3] << 16 | bytes[4] << 8 | bytes[5];
            const int frames = kDivision * 256;
            const int step = 120 * 256 * kDivision / 3600;
            SEQLIB_CHECK(std::abs(micros - FrameSeconds(double(frames) / step) * 1e6) < 1);
        }
        if (bytes.size() > 1 && bytes[1] == 0x06)
        {
            markers.push_back(tick);
        }
    }
    SEQLIB_CHECK(tempo_found);
    SEQLIB_CHECK(markers == std::vector<uint64_t>({24 * 20, 72 * 20}));

    const auto ons = midi.Find(0x90);
    SEQLIB_REQUIRE_EQ(ons.size(), 2);
    SEQLIB_CHECK_EQ(ons[0].tick, 0);
    SEQLIB_CHECK(ons[0].bytes == std::vector<uint8_t>({0x90, 60, 113}));
    SEQLIB_CHECK_EQ(ons[1].tick, 24 * 20);
    SEQLIB_CHECK(ons[1].bytes == std::vector<uint8_t>({0x90, 64, 127}));
    const auto offs = midi.Find(0x80);
    SEQLIB_REQUIRE_EQ(offs.size(), 2);
    SEQLIB_CHECK_EQ(offs[0].tick, 12 * 20);
    SEQLIB_CHECK_EQ(offs[1].tick, 48 * 20);

    const test::Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK_EQ(sf.Count("phdr", 38), 2);

    // The mixer echoes every sample voice, so the track sends the echo's first setting to the reverb.
    int sends = 0;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == 91)
        {
            SEQLIB_CHECK_EQ(bytes[2], info.echo[0]);
            sends++;
        }
    }
    SEQLIB_CHECK(sends > 0);
}

// A song with no loop markers ends with its tracks. The bend takes the driver's curve: a full bend is the whole range.
// The channel sets CC72, but the PSG plays outside the mixer and its echo. The note sends 0 to the reverb. The square
// plays at the channel's level 98 and velocity 127, with an envelope at its full level and a release that ends the note
// at once. The voice's level v is 98 x 127 x 127 >> 14 = 96, and the PSG's volume is the smaller of 15 and (96 >> 3) +
// (96 >> 4). The zone plays 3v/16 = 18.09 at velocity 127. The note's level is therefore 127 x 15 / 18.09 = 105.3, and
// its MIDI velocity 127 x sqrt(105.3 / 127) = 116. The channel's CC10 of 40 enables the PSG's left side alone, so the
// track pans hard left. Channel 3 plays key 69 on square 1 with a rising sweep. The driver writes 2048 - 131072 / 440 =
// 1751. The sweep's first step makes that 1751 + 1751 / 16 = 1860, 7.92 semitones up. Its second makes 1976, and the
// hardware's check of the step after, 2099, passes the top and turns the channel off. The track's bend range is 8, so
// the step bends to 16299. The note ends at the second step, within a frame of its start and long before the end that
// its file gives. The bend goes back to the middle there.
void TestBend()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const SongReport sum = ConvertSong(rom, info, 3, test::TempSettings("rd1_bend"), nullptr);
    SEQLIB_CHECK(sum.ok && sum.loop_start < 0);
    SEQLIB_CHECK_EQ(sum.tracks, 2);
    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    const auto bends = midi.Find(0xE0);
    SEQLIB_REQUIRE(!bends.empty());
    const auto& last = bends.back();
    SEQLIB_CHECK_EQ(last.tick, 12 * 20);
    SEQLIB_CHECK(last.bytes[2] >= 0x7E);
    const auto ons = midi.Find(0x90);
    SEQLIB_REQUIRE_EQ(ons.size(), 1);
    SEQLIB_CHECK(ons[0].bytes == std::vector<uint8_t>({0x90, 69, 116}));
    int pan = -1;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == 10 && tick <= ons[0].tick)
        {
            pan = bytes[2];
        }
    }
    SEQLIB_CHECK_EQ(pan, 0);

    const auto swept_on = midi.Find(0x91);
    const auto swept_off = midi.Find(0x81);
    SEQLIB_REQUIRE_EQ(swept_on.size(), 1);
    SEQLIB_REQUIRE_EQ(swept_off.size(), 1);
    const uint64_t frame_ticks = 16; // 960 ticks a second
    SEQLIB_CHECK_EQ(swept_on[0].tick, 36 * 20);
    SEQLIB_CHECK(swept_off[0].tick > swept_on[0].tick && swept_off[0].tick <= swept_on[0].tick + frame_ticks);
    std::vector<int> sweep_bends;
    uint64_t back_at = 0;
    for (const auto& [tick, bytes] : midi.Find(0xE1))
    {
        const int value = bytes[1] | bytes[2] << 7;
        if (tick > swept_on[0].tick && tick < swept_off[0].tick)
        {
            sweep_bends.push_back(value);
        }
        if (tick == swept_off[0].tick && value == 8192)
        {
            back_at = tick;
        }
    }
    SEQLIB_CHECK(sweep_bends == std::vector<int>({16299}));
    SEQLIB_CHECK_EQ(back_at, swept_off[0].tick);
    bool range = false;
    for (const auto& [tick, bytes] : midi.Find(0xB1))
    {
        range = range || (bytes[1] == 6 && bytes[2] == 8);
    }
    SEQLIB_CHECK(range);
    int sends = 0;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == 91)
        {
            SEQLIB_CHECK_EQ(bytes[2], 0);
            sends++;
        }
    }
    SEQLIB_CHECK(sends > 0);
}

// The square's release ends its voice at once, in the frame of the note's end. The square's zone fades out over about
// 3 frames after the note's end (EnvelopeOf()). The driver steps 204/256 of a tick a frame at 120 BPM
// (TestSequencer()). After a wait of 420 ticks, the song's end comes about 8.8 s after the square falls silent, and the
// song ends there, 432 ticks from its start. After a wait of 600 ticks, the end comes about 12.6 s after, more than
// kMaxSilentEndSeconds. The song then ends with the frame after the zone's fade. Its MIDI file holds nothing after the
// song's 17th tick, at 20 MIDI ticks to each.
void TestSilentEndAfterLastSound()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const SongReport near = ConvertSong(rom, info, 4, test::TempSettings("rd1_silent_end_near"), nullptr);
    const SongReport far = ConvertSong(rom, info, 5, test::TempSettings("rd1_silent_end_far"), nullptr);
    const double frame = 1 / kFrameRate;
    const double tick = 256.0 / 204 * frame;
    SEQLIB_CHECK(near.ok && std::fabs(near.seconds - 432 * tick) < 2 * frame);
    SEQLIB_CHECK(far.ok && far.seconds > 12 * tick + 2 * frame && far.seconds < 12 * tick + 5 * frame);

    const test::MidiEvents midi = ReadMidi(far.midi_path);
    uint64_t last = 0;
    for (const auto& track : midi.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }
    SEQLIB_CHECK_EQ(midi.Find(0x90).size(), 1);
    SEQLIB_CHECK(last >= 12 * 20 && last <= 17 * 20);
}

// A note that ends while its decay still falls releases from above the sustain level, and the driver's release then
// takes longer. The record decays by 0x10000 a frame towards half of the full level and releases by 0x80000 a frame.
// From the sustain level the release takes 0x3F8000 / 0x80000 = 7.9 frames. A note that ends after 6 frames releases
// from 0x790000, level 121, and takes 15.1 frames. The set pools a sample program's notes over its keys. Without a note
// that releases, the zone falls back on the sustain level.
void TestReleaseFrom()
{
    InstrumentRecord r;
    r.kind = InstrumentKind::kSample;
    r.start = 0x7F0000;
    r.sustain = 0x3F8000;
    r.decay = 0x10000;
    r.release = 0x80000;
    SEQLIB_CHECK_EQ(ZoneEnvelope(r).release, Timecents(FadeSeconds(0x3F8000 / double(0x80000))));
    SEQLIB_CHECK_EQ(ZoneEnvelope(r, 121 / 127.0).release, Timecents(FadeSeconds(0x790000 / double(0x80000))));
    SEQLIB_CHECK(ZoneEnvelope(r, 121 / 127.0).release > ZoneEnvelope(r).release);

    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    InstrumentSet set(rom, info);
    set.Add(0, 0, 67, -1);
    SEQLIB_CHECK(set.ReleaseFrom(0, 0, 67) < 0);
    set.Add(0, 0, 60, 121);
    set.Add(0, 0, 64, 101);
    SEQLIB_CHECK(std::fabs(set.ReleaseFrom(0, 0, 60) - 111 / 127.0) < 1e-9);
    SEQLIB_CHECK(std::fabs(set.ReleaseFrom(0, 0, 67) - 111 / 127.0) < 1e-9);
}

// The song's listing holds its header and its events.
void TestDump()
{
    const Cart cart;
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    std::string error;
    const std::string path = TempPath("rd1_dump.txt").string();
    SEQLIB_REQUIRE(DumpSong(rom, info, 2, path, error));
    const std::vector<uint8_t> text = test::ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("m_BGM") != std::string::npos);
    SEQLIB_CHECK(s.find("meta 06: 5B") != std::string::npos);
    SEQLIB_CHECK(!DumpSong(rom, info, 0, path, error));
}

} // namespace

void RunTests()
{
    TestDetection();
    TestNoDriver();
    TestSequencer();
    TestConversion();
    TestBend();
    TestSilentEndAfterLastSound();
    TestReleaseFrom();
    TestDump();
}

} // namespace agbsf2::rd1
