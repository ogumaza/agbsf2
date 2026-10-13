// SPDX-License-Identifier: MIT

// Unit tests for Quintet's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "files.h"
#include "midi.h"
#include "music.h"
#include "quintet/convert.h"
#include "quintet/driver.h"
#include "quintet/sequencer.h"
#include "quintet/song.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"

namespace agbsf2::quintet
{
namespace
{

using test::Le16;
using test::Le32;
using test::MidiEvents;
using test::ReadAll;
using test::ReadMidi;
using test::ReadSf2;
using test::Sf2Records;
using test::TempPath;

// Addresses in a test cartridge: the driver's code that detection reads, the game's file lookup and the code that
// passes the files to the driver, the driver's tables, and the game's files.
constexpr uint32_t kPlay = kRomBase + 0x100;
constexpr uint32_t kCode = kRomBase + 0x200;
constexpr uint32_t kLookup = kRomBase + 0x600;
constexpr uint32_t kGameCode = kRomBase + 0x700;
constexpr uint32_t kLengthTable = kRomBase + 0x900;
constexpr uint32_t kFrequencyTable = kRomBase + 0x910;
constexpr uint32_t kPcmPitchTable = kRomBase + 0x9C0;
constexpr uint32_t kLfoTable = kRomBase + 0xA40;
constexpr uint32_t kFiles = kRomBase + 0x1000; // file n is at kFiles + n * 0x1000

// The game's files, and the driver's variables that the game stores them in.
constexpr int kWaveFile = 1;
constexpr int kMacroFile = 2;
constexpr int kSfxMacroFile = 3;
constexpr int kSampleFile = 4;
constexpr int kSfxSampleFile = 5;
constexpr int kMusicFile = 6;
constexpr uint32_t kWaveVariable = 0x03000790;
constexpr uint32_t kMacroVariable = 0x03000794; // the A revision's music macros, or the J revision's two tables

// Returns the address of file `file`.
constexpr uint32_t FileAddress(int file)
{
    return kFiles + uint32_t(file) * 0x1000;
}

// A channel's data being written, one command at a time.
class Channel
{
public:
    Channel& Bytes(std::vector<uint8_t> b)
    {
        bytes_.insert(bytes_.end(), b.begin(), b.end());

        return *this;
    }

    // Adds a note of `pitch` (0-11, 12 to play again, 13 to rest) with the length that index `length` of the length
    // table gives: 384 ticks divided by 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 or 128.
    Channel& Note(int pitch, int length)
    {
        return Bytes({uint8_t(length << 4 | pitch)});
    }

    Channel& Rest(int length)
    {
        return Note(13, length);
    }

    std::vector<uint8_t> End()
    {
        bytes_.push_back(0xFF);

        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
};

// Returns a song's bytes: its length, its 6 channels' offsets and their data. Channels left out end at once.
std::vector<uint8_t> Song(std::vector<std::vector<uint8_t>> channels)
{
    channels.resize(6, {0xFF});
    std::vector<uint8_t> data;
    std::vector<uint16_t> offsets;
    for (const std::vector<uint8_t>& c : channels)
    {
        offsets.push_back(uint16_t(14 + data.size()));
        data.insert(data.end(), c.begin(), c.end());
    }
    if (data.size() % 2)
    {
        data.push_back(0);
    }

    std::vector<uint8_t> song;
    auto put16 = [&](uint16_t v)
    {
        song.push_back(uint8_t(v));
        song.push_back(uint8_t(v >> 8));
    };

    put16(uint16_t(14 + data.size()));
    for (uint16_t o : offsets)
    {
        put16(o);
    }
    song.insert(song.end(), data.begin(), data.end());

    return song;
}

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, for revision
// `revision`, the driver's tables and the game's files.
class Cart
{
public:
    explicit Cart(Revision revision, bool code = true) : d_(0x10000, 0), revision_(revision)
    {
        WriteTables();
        if (code)
        {
            WriteCode();
        }

        // A wave of a saw, and the second wave a square, after the table's 2-byte header.
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(FileAddress(kWaveFile) + 2 + i, uint8_t((2 * i) << 4 | (2 * i + 1)));
            Put8(FileAddress(kWaveFile) + 18 + i, i < 8 ? 0xFF : 0x00);
        }
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

    // Writes the songs into the music file, one after another.
    void Songs(const std::vector<std::vector<uint8_t>>& songs)
    {
        uint32_t at = FileAddress(kMusicFile);
        for (const std::vector<uint8_t>& s : songs)
        {
            for (size_t i = 0; i < s.size(); i++)
            {
                Put8(at + uint32_t(i), s[i]);
            }
            at += uint32_t(s.size());
        }
    }

    // Writes noise macros into a macro file: macro m starts at the offset that entry m + 1 of the table at the file's
    // start gives.
    void Macros(int file, const std::vector<std::vector<uint8_t>>& macros)
    {
        const uint32_t table = FileAddress(file);
        uint32_t at = table + 2 + 2 * uint32_t(macros.size() + 1);
        for (size_t m = 0; m < macros.size(); m++)
        {
            Put16(table + 2 + 2 * uint32_t(m + 1), uint16_t(at - table));
            for (uint8_t b : macros[m])
            {
                Put8(at++, b);
            }
        }
    }

    // Writes samples into a sample file, each with a header of its size and rate. Returns the address of each one's
    // data.
    std::vector<uint32_t> Samples(int file, const std::vector<std::pair<std::vector<int8_t>, int>>& samples)
    {
        std::vector<uint32_t> data;
        uint32_t at = FileAddress(file);
        for (const auto& [pcm, rate] : samples)
        {
            const uint32_t size = 6 + uint32_t(pcm.size());
            Put32(at, size);
            Put16(at + 4, uint16_t(rate));
            for (size_t i = 0; i < pcm.size(); i++)
            {
                Put8(at + 6 + uint32_t(i), uint8_t(pcm[i]));
            }
            data.push_back(at + 6);
            at += size;
        }
        Put32(at, 0);
        SEQLIB_CHECK(at + 4 <= FileAddress(file + 1));

        return data;
    }

    Rom ToRom() const
    {
        Rom r;
        r.Assign(d_);

        return r;
    }

private:
    // Writes `pattern`'s halfwords at `at`, with its ldr at halfword `ldr` (if any) loading `literal` from a pool after
    // it. Returns the next free word.
    uint32_t Pattern(uint32_t at, const char* pattern, int ldr = -1, uint32_t literal = 0)
    {
        std::istringstream in(pattern);
        std::string word;
        uint32_t a = at;
        while (in >> word)
        {
            Put16(a, uint16_t(std::stoul(word, nullptr, 16)));
            a += 2;
        }

        const uint32_t pool = (a + 3) & ~3u;
        if (ldr >= 0)
        {
            const uint32_t ins = at + 2 * uint32_t(ldr);
            const uint16_t op = uint16_t(d_[ins - kRomBase + 1] << 8);
            Put16(ins, uint16_t(op | ((pool - ((ins + 4) & ~3u)) / 4)));
            Put32(pool, literal);
        }

        return pool + 4;
    }

    // Writes a routine at the word `at` that stores r0 with `store` (str r0, [r1] or [r1, #4]) into `variable`. Returns
    // the next free word.
    uint32_t Setter(uint32_t at, uint16_t store, uint32_t variable)
    {
        Put16(at, 0x4901);
        Put16(at + 2, store);
        Put16(at + 4, 0x4770);
        Put32(at + 8, variable);

        return at + 12;
    }

    // Writes a bl from `at` to `target`.
    void Bl(uint32_t at, uint32_t target)
    {
        const int32_t offset = int32_t(target) - int32_t(at + 4);
        Put16(at, uint16_t(0xF000 | ((offset >> 12) & 0x7FF)));
        Put16(at + 2, uint16_t(0xF800 | ((offset >> 1) & 0x7FF)));
    }

    // Writes the game's code that looks file `file` up and passes it to `routine`, at `at`. Returns the next free word.
    uint32_t PassFile(uint32_t at, int file, uint32_t routine)
    {
        Put16(at, 0x2000);
        Put16(at + 2, uint16_t(0x2100 | file));
        Bl(at + 4, kLookup);
        Bl(at + 8, routine);

        return at + 12;
    }

    // Writes the tables: the note values, the square channels' frequency settings from C2, each note's PCM rate change
    // in 1/1000s of the sample's rate, from two octaves down, and a sine.
    void WriteTables()
    {
        const uint8_t kLengths[13] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128};
        for (uint32_t i = 0; i < 13; i++)
        {
            Put8(kLengthTable + i, kLengths[i]);
        }

        for (int n = 0; n < 84; n++)
        {
            const double hz = 65.40639 * std::pow(2.0, n / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(n), uint16_t(std::lround(std::max(0.0, 2048 - 131072 / hz))));
        }

        for (int n = 0; n < 53; n++)
        {
            Put16(kPcmPitchTable + 2 * uint32_t(n),
                  uint16_t(int16_t(std::lround(1000 * (std::pow(2.0, (n - 24) / 12.0) - 1)))));
        }

        for (uint32_t i = 0; i < 256; i++)
        {
            Put8(kLfoTable + i, uint8_t(int8_t(std::lround(127 * std::sin(i * 6.283185307179586 / 256)))));
        }
    }

    // Writes the driver's code that detection reads, the game's lookup of its files, and the game's code that gives the
    // driver its files and plays the songs.
    void WriteCode()
    {
        const bool j = revision_ == Revision::kJ;
        if (j)
        {
            Pattern(kPlay, "B5F0 1C07 2500 4E00 00AC CE01 2100 F000 F800 4800 1824 6820 4770");
        }
        else
        {
            Pattern(kPlay, "B5F0 1C07 2400 4E00 4D00 CE01 F000 F800 CD01 4770");
        }

        uint32_t at = Pattern(kCode, "250D DD00 E100 220C DD00 E100 4800 1810 7801 20C0 0040", 6, kLengthTable);
        at = Pattern(at, "2E53 DD00 2653 4D00 1C30 210C", 3, kFrequencyTable);
        at = Pattern(at, "2A34 DD00 2234 4800 0051", 3, kPcmPitchTable);
        at = Pattern(at, "4800 0411 1409 1809 2000 5608", 0, kLfoTable);
        at = Pattern(at, "B530 0600 0E02 4800 6801 1C05 2900", 3, kWaveVariable);
        if (j)
        {
            at = Pattern(at, "B510 0600 0E03 4800 6802 1C14 3490 2000 6020 2B00 D000 4900 7B52 0090 1840", 11,
                         kMacroVariable);
        }
        else
        {
            at = Pattern(at, "0600 0E03 1C19 4800 7800 2800 D000 4800 6802", 7, kMacroVariable);
        }

        const uint32_t loader = at;
        at = Pattern(loader, j ? "B570 4B00 008A 18D2 1C03 6013 4800 2200 024D 247F" : "B570 1C02 4800 2500 024C 237F");

        // The setters: ldr r1, =variable; str r0, [r1] (or [r1, #4] for the second table); bx lr, and the routines that
        // pass a sample file to the loader with its bank.
        const uint32_t wave_setter = at;
        at = Setter(at, 0x6008, kWaveVariable);
        const uint32_t macro_setter = at;
        at = Setter(at, 0x6008, kMacroVariable);
        const uint32_t sfx_macro_setter = at;
        at = Setter(at, 0x6048, kMacroVariable);
        std::array<uint32_t, 2> sample_setters = {};
        for (uint32_t bank = 0; bank < 2; bank++)
        {
            sample_setters[bank] = at;
            Put16(at, 0xB500);
            Put16(at + 2, uint16_t(0x2100 | bank));
            Bl(at + 4, loader);
            Put16(at + 8, 0xBC01);
            Put16(at + 10, 0x4700);
            at += 12;
        }

        // The game's lookup: file n of any archive is at kFiles + n * 0x1000.
        Put16(kLookup, 0x0309);     // lsls r1, r1, #12
        Put16(kLookup + 2, 0x4801); // ldr r0, =kFiles
        Put16(kLookup + 4, 0x1840); // adds r0, r0, r1
        Put16(kLookup + 6, 0x4770); // bx lr
        Put32(kLookup + 8, kFiles);

        // The game's sound init gives the driver its files, and its play-song routine passes the music file.
        at = PassFile(kGameCode, kWaveFile, wave_setter);
        at = PassFile(at, kMacroFile, macro_setter);
        at = PassFile(at, kSampleFile, sample_setters[0]);
        if (j)
        {
            at = PassFile(at, kSfxMacroFile, sfx_macro_setter);
            at = PassFile(at, kSfxSampleFile, sample_setters[1]);
        }
        PassFile(at, kMusicFile, kPlay);
    }

    std::vector<uint8_t> d_;
    Revision revision_;
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

// Runs a song for `frames` frames and returns each frame's register writes, with the play routine's first.
std::vector<std::vector<RegisterWrite>> Writes(const Rom& rom, const DriverInfo& info, int song, int frames)
{
    Sequencer seq(rom, info, song);
    std::vector<std::vector<RegisterWrite>> out = {seq.StartWrites()};
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

// Returns a saw wave of `length` signed bytes.
std::vector<int8_t> Saw(size_t length)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 7 % 256) - 128));
    }

    return pcm;
}

void TestDetection()
{
    // The A revision: the driver's tables, and the game's files from its code.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Rest(4).End()}), Song({Channel().Rest(4).End()})});
    cart.Samples(kSampleFile, {{Saw(200), 8000}, {Saw(300), 11025}});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.revision == Revision::kA);
    SEQLIB_CHECK_EQ(info.play, kPlay);
    SEQLIB_CHECK_EQ(info.length_table, kLengthTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.pcm_pitch_table, kPcmPitchTable);
    SEQLIB_CHECK_EQ(info.lfo_table, kLfoTable);
    SEQLIB_CHECK_EQ(info.waves, FileAddress(kWaveFile));
    SEQLIB_CHECK_EQ(info.macros[0], FileAddress(kMacroFile));
    SEQLIB_CHECK_EQ(info.macros[1], 0);
    SEQLIB_CHECK_EQ(info.samples[0], FileAddress(kSampleFile));
    SEQLIB_CHECK_EQ(info.samples[1], 0);
    SEQLIB_CHECK_EQ(info.songs, FileAddress(kMusicFile));
    SEQLIB_REQUIRE_EQ(info.song_addresses.size(), 2);
    SEQLIB_REQUIRE_EQ(info.sample_addresses[0].size(), 2);
    SEQLIB_CHECK(info.warnings.empty());

    // Detection starts from nothing. A game without the driver keeps none of an earlier game's tables.
    DriverInfo reused = info;
    std::string reused_error;
    Rom blank;
    blank.Assign(std::vector<uint8_t>(0x1000, 0));

    const bool found_blank = DetectDriver(blank, DriverOverrides(), reused, reused_error);

    SEQLIB_CHECK(!found_blank && reused.play == 0 && reused.song_addresses.empty());

    // The J revision has the sound effects' noise macros and samples too.
    Cart cart_j(Revision::kJ);
    cart_j.Songs({Song({Channel().Rest(4).End()})});
    cart_j.Samples(kSampleFile, {{Saw(200), 8000}});
    cart_j.Samples(kSfxSampleFile, {{Saw(100), 8000}, {Saw(100), 8000}, {Saw(100), 8000}});
    const Rom rom_j = cart_j.ToRom();

    const DriverInfo info_j = Detect(rom_j);

    SEQLIB_CHECK(info_j.revision == Revision::kJ);
    SEQLIB_CHECK_EQ(info_j.macros[0], FileAddress(kMacroFile));
    SEQLIB_CHECK_EQ(info_j.macros[1], FileAddress(kSfxMacroFile));
    SEQLIB_CHECK_EQ(info_j.samples[1], FileAddress(kSfxSampleFile));
    SEQLIB_REQUIRE_EQ(info_j.sample_addresses[1].size(), 3);
    SEQLIB_REQUIRE_EQ(info_j.song_addresses.size(), 1);

    // An override of the song count.
    DriverOverrides one;
    one.song_count = 1;
    DriverInfo first;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, one, first, error));
    SEQLIB_REQUIRE_EQ(first.song_addresses.size(), 1);

    // A cartridge without the driver shows no sign of it, unless the music file is given.
    Cart bare(Revision::kA, false);
    bare.Songs({Song({Channel().Rest(4).End()})});
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = FileAddress(kMusicFile);
    DriverInfo none, given;

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error));
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(!DetectDriver(bare_rom, table, given, error));
    SEQLIB_CHECK(error == "no Quintet sound driver found, so it has no length table");
}

void TestDecoding()
{
    Cart cart(Revision::kA);
    const uint32_t kAt = FileAddress(kMusicFile) + 0x800;
    const std::vector<uint8_t> kBytes = {0x4A, 0x3D, 0xD2, 0x64, 0x50, 0x3C, 0x03,
                                         0x0F, 0xE7, 0x00, 0x62, 0x00, 0xF0, 0xFF};
    for (size_t i = 0; i < kBytes.size(); i++)
    {
        cart.Put8(kAt + uint32_t(i), kBytes[i]);
    }
    const Rom rom = cart.ToRom();
    DriverInfo info;
    info.length_table = kLengthTable;
    DriverInfo info_j = info;
    info_j.revision = Revision::kJ;

    const Command note = DecodeCommand(rom, info, kAt, 0);
    const Command rest = DecodeCommand(rom, info, kAt + 1, 0);
    const Command envelope = DecodeCommand(rom, info, kAt + 2, 0);
    const Command range = DecodeCommand(rom, info, kAt + 8, 4);
    const Command bank_a = DecodeCommand(rom, info, kAt + 12, 4);
    const Command bank_j = DecodeCommand(rom, info_j, kAt + 12, 4);
    const Command macros_j = DecodeCommand(rom, info_j, kAt + 12, 3);
    const Command end = DecodeCommand(rom, info, kAt + 13, 0);

    SEQLIB_CHECK(note.note && note.ticks == 64 && note.text == "note A#, 64 ticks (1/6)");
    SEQLIB_CHECK(rest.note && rest.ticks == 96 && rest.text == "rest, 96 ticks (1/4)");
    SEQLIB_CHECK(envelope.size == 6 &&
                 envelope.text == "volume envelope: 100% to 80% over 3 frames, then to 60% over 15 frames");
    SEQLIB_CHECK(range.size == 4 && range.text == "play 0% to 98% of the sample, then loop from 0%");
    SEQLIB_CHECK(bank_a.text == "nothing");
    SEQLIB_CHECK(bank_j.text == "switch to the other bank of samples");
    SEQLIB_CHECK(macros_j.text == "switch to the other bank of noise macros");
    SEQLIB_CHECK(end.end && end.text == "end, or back to the loop point");
}

void TestTiming()
{
    // At tempo 90, a quarter note of 96 ticks takes 96 × 37 / 90 = 39.47 frames, and the driver plays each note at the
    // start of the frame its tick falls in. A loop starts the count again where the driver is, so the passes drift from
    // the song's ticks: the fourth note plays at frame 117, where tick 288 would fall in frame 118.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<uint32_t> frames;
    for (int f = 0; f < 160; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kNote)
            {
                frames.push_back(e.frame);
            }
        }
    }

    SEQLIB_CHECK(frames.size() >= 4 && frames[1] == 39 && frames[2] == 78 && frames[3] == 117);

    // The MIDI file follows: a frame at tempo 90 is 90 ticks, and each note after a loop starts its frame.
    ConvertOptions opt;
    opt.loops = 4;
    opt.track_mask = 1;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "timing";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<uint32_t> ons;
    for (const auto& [tick, bytes] : midi.Find(0x90))
    {
        ons.push_back(uint32_t(tick));
    }

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(midi.division, 3552);
    SEQLIB_CHECK(ons.size() == 4 && ons[0] == 0 && ons[1] == 3510 && ons[2] == 7020 && ons[3] == 10530);
}

// A channel that stops at FF on its loop point holds its note, and counts as finished. Here the only channel holds
// after a quarter note at tempo 90, 96 × 37 / 90 = 39.47 frames or 0.66 s, and the song ends there without a loop, with
// the LFO's bends in those frames and no more.
void TestHold()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xDF, 0, 8, 64}).Note(0, 3).Bytes({0xBF}).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "hold";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    SEQLIB_CHECK(sum.ok && sum.seconds > 0.65 && sum.seconds < 0.67 && sum.loop_start < 0);
    SEQLIB_CHECK(!midi.Find(0xE0).empty() && midi.Find(0xE0).size() <= 41);
}

// Square 2 repeats a command 255 times 255 times, without a note, and the guard ends it after 10000 commands. Square 1
// loops a quarter note. The plan takes square 2 as ended. The song therefore loops, without running for the hour that
// a song cut off there takes.
void TestCommandGuard()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0xBF}).Note(0, 3).End(),
                      Channel().Note(0, 3).Bytes({0xD6, 0xD6, 0xAF, 0x00, 0xD7, 0xFF, 0xD7, 0xFF}).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "guard";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const auto has = [&sum](const std::string& text)
    {
        return std::any_of(sum.warnings.begin(), sum.warnings.end(),
                           [&text](const std::string& w) { return w.find(text) != std::string::npos; });
    };
    SEQLIB_CHECK(sum.ok && sum.seconds < 2);
    SEQLIB_CHECK(has("without reaching a note") && !has("cut off"));
}

void TestWaveSwitch()
{
    // A wave note that switches from a wave whose halves match (wave 0, made a 16-step saw played twice) to one whose
    // halves don't (wave 1, a square) plays both zones from the same root key, so the second sounds at the note's pitch
    // and not an octave higher.
    Cart cart(Revision::kA);
    for (uint32_t i = 0; i < 16; i++)
    {
        cart.Put8(FileAddress(kWaveFile) + 2 + i, uint8_t((2 * (i % 8)) << 4 | (2 * (i % 8) + 1)));
    }
    cart.Songs({Song({{0xFF}, {0xFF}, Channel().Bytes({0xDA, 0x00, 0xDA, 0x81, 0x5E, 3, 0xEA, 10}).Note(0, 2).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "wave_switch";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const size_t first = Le16(sf2.Record("inst", 22, 0) + 20);
    const size_t zones = Le16(sf2.Record("inst", 22, 1) + 20) - first;

    // A zone's root key: its overriding root key, or else its sample's.
    const auto root = [&sf2](size_t zone)
    {
        const std::map<uint16_t, uint16_t> gens = sf2.ZoneGens(zone);
        const auto key = gens.find(sf2::kOverridingRootKey);
        return key != gens.end() ? int(key->second) : int(sf2.Record("shdr", 46, gens.at(sf2::kSampleId))[40]);
    };

    SEQLIB_CHECK(sum.ok && zones == 2);
    if (zones == 2)
    {
        SEQLIB_CHECK_EQ(root(first), 72);
        SEQLIB_CHECK_EQ(root(first + 1), 72);
    }
}

void TestLoopStarts()
{
    // Square 1 loops two notes from its start, and square 2 plays a note and then loops the same two notes. The loop
    // starts after square 2's first note, within a frame, where both channels are looping, lasts two notes, and plays
    // twice before the song ends.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(4, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(2, 3).Bytes({0xBF}).Note(0, 3).Note(4, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "loop_starts";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double length = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    SEQLIB_CHECK(std::fabs(sum.loop_start - length / 2) < 0.02);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 2 * length)) < 0.001);
}

void TestLoopAfterTempoChange()
{
    // Square 1 plays two notes at tempo 200, slows to tempo 60 and loops two notes, and square 2 loops four notes from
    // its start, so that its first pass starts at the faster tempo. The loop starts at the second pass, a loop after
    // square 1's loop point, and the song still plays it twice in full.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 200, 0x5E, 15})
                          .Note(0, 3)
                          .Note(2, 3)
                          .Bytes({0x7F, 60, 0xBF})
                          .Note(4, 3)
                          .Note(5, 3)
                          .End(),
                      Channel().Bytes({0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "loop_tempo";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double length = sum.loop_end - sum.loop_start;
    SEQLIB_CHECK(sum.ok && sum.loop_start > length);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 2 * length)) < 0.001);
}

void TestLoopLengths()
{
    // Square 1 loops three notes and square 2 four, both from the start, and the wave channel plays two notes and ends.
    // The loop starts where the wave channel ends, after two notes, and lasts until both squares are back where they
    // started, twelve notes, and the song plays it twice. The driver rounds the notes to frames, which the checks allow
    // for.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15, 0xBF}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End(),
                      Channel().Bytes({0x7F, 90, 0x5E, 15}).Note(7, 3).Note(9, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "loop_lengths";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const double note = sum.loop_start / 2;
    SEQLIB_CHECK(sum.ok && sum.loop_start > 0);
    SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - 12 * note) < 0.1);
    SEQLIB_CHECK(std::fabs(sum.seconds - (sum.loop_start + 24 * note)) < 0.1);
}

// Square 1's loop sets octave 2 after its first note, so the first time through, that note plays at the octave 3 set
// before the loop, and every time after at octave 2. The loop starts at the second pass, which the driver goes on
// repeating, and the song plays the first pass and then the loop twice. A loop that plays the same each time starts at
// its first pass.
void TestLoopAtSecondPass()
{
    for (const bool moves : {true, false})
    {
        Cart cart(Revision::kA);
        cart.Songs({Song({Channel()
                              .Bytes({0x7F, 90, 0x5E, 15, 0x4F, uint8_t(moves ? 3 : 2), 0xBF})
                              .Note(0, 3)
                              .Bytes({0x4F, 2})
                              .Note(4, 3)
                              .End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ConvertOptions opt;
        opt.out_dir = Utf8(test::g_temp);
        opt.base_name = moves ? "second_pass" : "first_pass";

        const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

        // The keys of the notes from the loop's start on.
        const MidiEvents midi = ReadMidi(sum.midi_path);
        uint32_t loop_start = ~0u;
        for (const auto& [tick, bytes] : midi.Find(0xFF))
        {
            if (bytes.size() > 3 && bytes[1] == 0x06 && std::string(bytes.begin() + 3, bytes.end()) == "loopStart")
            {
                loop_start = uint32_t(tick);
            }
        }

        std::vector<int> keys;
        for (const auto& [tick, bytes] : midi.Find(0x90))
        {
            if (tick >= loop_start)
            {
                keys.push_back(bytes[1]);
            }
        }

        // At tempo 90, the two quarter notes take 2 × 96 × 37 / 90 = 78.9 frames, which the driver plays in 78.
        const double kPass = 78 * 280896.0 / 16777216;
        SEQLIB_CHECK(sum.ok);
        SEQLIB_CHECK(std::fabs(sum.loop_start - (moves ? kPass : 0)) < 0.001);
        SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - kPass) < 0.001);
        SEQLIB_CHECK(std::fabs(sum.seconds - sum.loop_start - 2 * kPass) < 0.001);
        SEQLIB_CHECK(keys == std::vector<int>({60, 64, 60, 64}));
    }
}

// Square 1 loops two quarter notes from its start, and square 2 the same after a rest of 56 ticks. At tempo 90, a frame
// is 90 of the MIDI file's ticks and a tick is 37, so square 2's loop point is 2 MIDI ticks into frame 23, and square
// 1's is at the start of frame 0. Each loop drops the part of a frame that the channel has counted, 84 MIDI ticks for
// square 1 and 86 for square 2, so square 2's second pass starts 2 ticks before the end of a loop from its loop point,
// and its first note would play twice at the seam. The loop starts at the second pass, where both squares start each
// pass as the one before.
void TestLoopSeam()
{
    Cart cart(Revision::kA);
    cart.Songs(
        {Song({Channel().Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3, 0xBF}).Note(0, 3).Note(4, 3).End(),
               Channel().Bytes({0x5E, 15, 0x4F, 3}).Rest(5).Rest(10).Bytes({0xBF}).Note(7, 3).Note(11, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "loop_seam";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    // Each square's notes from the loop's start to its end.
    const MidiEvents midi = ReadMidi(sum.midi_path);
    uint32_t loop_start = ~0u, loop_end = 0;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        const std::string text = bytes.size() > 3 ? std::string(bytes.begin() + 3, bytes.end()) : "";
        if (bytes[1] == 0x06 && text == "loopStart")
        {
            loop_start = uint32_t(tick);
        }
        else if (bytes[1] == 0x06 && text == "loopEnd")
        {
            loop_end = uint32_t(tick);
        }
    }

    std::array<int, 2> notes = {};
    for (const int c : {0, 1})
    {
        for (const auto& [tick, bytes] : midi.Find(uint8_t(0x90 + c)))
        {
            notes[size_t(c)] += tick >= loop_start && tick < loop_end ? 1 : 0;
        }
    }

    // Square 1's first pass ends at frame 78, and the loop starts as much after that as square 2's loop point is into
    // the song, 2072 MIDI ticks, and lasts 78 frames.
    const double kFrame = 280896.0 / 16777216;
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(std::fabs(sum.loop_start - (78 + 2072 / 90.0) * kFrame) < 1e-6);
    SEQLIB_CHECK(std::fabs(sum.loop_end - sum.loop_start - 78 * kFrame) < 1e-6);
    SEQLIB_CHECK(notes == (std::array<int, 2>{2, 2}));
}

// Square 1 plays a note at volume 15, and then loops a note at volume 15 and one at 8. The first time through, the
// loop's first note has the level that the note before it left, but a player that jumps back to the loop's start comes
// from the note at volume 8, so the level is written again there.
void TestLoopSettingsAgain()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 90, 0x5E, 15, 0x4F, 3})
                          .Note(0, 3)
                          .Bytes({0xBF, 0x5E, 15})
                          .Note(4, 3)
                          .Bytes({0x5E, 8})
                          .Note(7, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "loop_again";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

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

    // The setup's level of 0, the first note's, the same again at the loop's start, and then the note at volume 8's.
    SEQLIB_CHECK(sum.ok && loop_start > 0);
    SEQLIB_REQUIRE(levels.size() > 3);
    SEQLIB_CHECK(levels[2] == std::make_pair(loop_start, levels[1].second));
    SEQLIB_CHECK(levels[3].first > loop_start && levels[3].second < levels[2].second);
}

void TestHourLimit()
{
    // A song asked to play its loop, a whole note at tempo 6 or 384 × 37 / 6 = 2368 frames, a hundred times stops where
    // the model gives up, after 216000 frames of 280896 cycles at 16.78 MHz, and says so.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 6, 0x5E, 15, 0xBF}).Note(0, 0).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "hour";
    opt.loops = 100;

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const auto cut_off = [](const std::string& w)
    {
        return w.find("cut off after an hour") != std::string::npos;
    };
    SEQLIB_CHECK(sum.ok && std::any_of(sum.warnings.begin(), sum.warnings.end(), cut_off));
    SEQLIB_CHECK(std::fabs(sum.seconds - 216000 * 280896.0 / 16777216) < 0.1);
}

void TestRegisters()
{
    // A note on square 1: the frequency table's entry for octave 2's C, and the note's volume, duty and length.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x5E, 12, 0x2F, 1}).Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto writes = Writes(rom, info, 0, 2);

    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000062), 0xC87F);
    SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x04000064), 0x8000 | rom.U16(kFrequencyTable + 48));
}

void TestWaveVolumes()
{
    // The wave channel at volume 3 plays at 100% in the A revision, and at 75% in the J revision, which has 4 for 100%.
    // NR32's volume code is the top bits of NR31's halfword, which the driver fills with the length × 4.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        Cart cart(revision);
        cart.Songs(
            {Song({{0xFF}, {0xFF}, Channel().Bytes({0xDA, 0x00, 0x5E, 3}).Note(0, 3).Bytes({0x5F}).Note(0, 3).End()})});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 60);
        const long long first = LastWrite(writes[1], 0x04000072);
        long long second = -1;
        for (size_t f = 2; f < writes.size() && second < 0; f++)
        {
            second = LastWrite(writes[f], 0x04000072);
        }

        if (revision == Revision::kA)
        {
            SEQLIB_CHECK_EQ(first, 0x20FC);
            SEQLIB_CHECK_EQ(second, 0x20FC);
        }
        else
        {
            SEQLIB_CHECK_EQ(first, 0x80FC);
            SEQLIB_CHECK_EQ(second, 0x20FC);
        }
    }
}

void TestPcm()
{
    // A PCM note points FIFO A's DMA 32 bytes into the sample, the 32 before going into the FIFO, and sets timer 0 for
    // the sample's rate. The J revision sets the timer at the end of the frame.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        Cart cart(revision);
        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0xDC, 0x00, 0x5E, 1}).Note(12, 3).End()})});
        const std::vector<uint32_t> data = cart.Samples(kSampleFile, {{Saw(4000), 10000}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 2);
        const std::vector<RegisterWrite>& frame = writes[1];

        SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000BC), data[0] + 32);
        SEQLIB_CHECK_EQ(LastWrite(frame, 0x040000C6), 0xB640);
        SEQLIB_CHECK_EQ(LastWrite(frame, 0x04000100), 0x800000 | (0x10000 - 16780000 / 10000));
        SEQLIB_CHECK(!frame.empty() && (frame.back().address == 0x04000100) == (revision == Revision::kJ));
    }

    // The driver checks a FIFO once a frame, and stops or loops it when the next frame would reach the end: from 0, at
    // 600 60ths of a byte a frame and an end of 100 bytes, it does so at the 9th check, after 8 frames.
    SEQLIB_CHECK_EQ(FifoFrames(0, 600, 100), 8);
    SEQLIB_CHECK_EQ(FifoFrames(5000, 600, 100), 0);
    SEQLIB_CHECK_EQ(FifoFrames(0, 0, 100), 0);
}

void TestBanks()
{
    // In the J revision, F0 switches a PCM channel to the sound effects' samples, which the samples it then selects
    // come from.
    Cart cart(Revision::kJ);
    cart.Songs({Song({{0xFF},
                      {0xFF},
                      {0xFF},
                      {0xFF},
                      Channel()
                          .Bytes({0xDC, 0x00, 0x5E, 1})
                          .Note(12, 3)
                          .Bytes({0xF0, 0xDC, 0x01})
                          .Note(12, 3)
                          .Bytes({0xDC, 0x00})
                          .Note(12, 3)
                          .End()})});
    const std::vector<uint32_t> music = cart.Samples(kSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
    const std::vector<uint32_t> effects = cart.Samples(kSfxSampleFile, {{Saw(1500), 10000}, {Saw(1500), 10000}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto writes = Writes(rom, info, 0, 200);
    std::vector<long long> sources;
    for (size_t f = 1; f < writes.size(); f++)
    {
        const long long source = LastWrite(writes[f], 0x040000BC);
        if (source >= 0)
        {
            sources.push_back(source - 32);
        }
    }

    SEQLIB_REQUIRE(sources.size() >= 3);
    SEQLIB_CHECK_EQ(sources[0], music[0]);
    SEQLIB_CHECK_EQ(sources[1], effects[1]);
    SEQLIB_CHECK_EQ(sources[2], effects[0]);
}

void TestNoiseDrums()
{
    // D2 fades the noise channel from volume 15 to 0 over 3 frames, restarting it at each new volume, and the drum's
    // sample has those volumes in it. Two notes with the same writes share a drum.
    Cart cart(Revision::kA);
    cart.Songs({Song(
        {{0xFF}, {0xFF}, {0xFF}, Channel().Bytes({0x5E, 15, 0xD2, 100, 0, 0, 3, 1}).Note(12, 3).Note(12, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "noise";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x93);
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(ons.size() == 2 && ons[0].bytes[1] == ons[1].bytes[1]);
    SEQLIB_CHECK_EQ(sf2.Count("shdr", 46), 2);

    // The sample's peak in each of its first 4 frames of 1097 points.
    const std::vector<uint8_t>& pcm = sf2.chunks.at("smpl");
    const uint32_t start = Le32(sf2.Record("shdr", 46, 0) + 20);
    std::vector<int> volumes;
    for (uint32_t f = 0; f < 4; f++)
    {
        int peak = 0;
        for (uint32_t i = 100; i < 1000; i++)
        {
            const uint32_t at = 2 * (start + f * 1097 + i);
            peak = std::max(peak, std::abs(int(int16_t(Le16(&pcm[at])))));
        }
        volumes.push_back(int(std::lround(peak * 15.0 / 32767)));
    }

    SEQLIB_CHECK(volumes == std::vector<int>({15, 10, 5, 0}));
}

void TestNoiseMacros()
{
    // A noise macro sets NR43 a frame at a time, restarting the channel with the note, and its end silences the
    // channel. In the J revision, F0 switches the noise channel to the sound effects' macros.
    for (Revision revision : {Revision::kA, Revision::kJ})
    {
        const bool j = revision == Revision::kJ;
        Cart cart(revision);
        std::vector<uint8_t> commands = {0x5E, 15, 0xDB, 0x01};
        if (j)
        {
            commands.insert(commands.begin(), 0xF0);
        }
        cart.Songs({Song({{0xFF}, {0xFF}, {0xFF}, Channel().Bytes(commands).Note(12, 3).End()})});
        cart.Macros(kMacroFile, {{0x27, 0x35, 0xFF}});
        cart.Macros(kSfxMacroFile, {{0x45, 0x56, 0xFF}});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        const auto writes = Writes(rom, info, 0, 4);

        SEQLIB_CHECK_EQ(LastWrite(writes[1], 0x0400007C), j ? 0x8045 : 0x8027);
        SEQLIB_CHECK_EQ(LastWrite(writes[2], 0x0400007C), j ? 0x0056 : 0x0035);
        SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x0400007C), 0x8000);
        SEQLIB_CHECK_EQ(LastWrite(writes[3], 0x04000078) >> 12, 0);
    }
}

void TestConversion()
{
    // A song with a loop: the tempo, the loop markers, and the level of a square that plays on the left only, in CC10
    // and CC11. A sweep down lowers the pitch within each frame, which the MIDI file bends to between the frame's
    // changes.
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 90, 0x5E, 15, 0xD0, 0xEE, 0x10, 0xBF, 0x1F, 1, 0x0E, 3, 0x1E, 0x4F, 5})
                          .Note(0, 3)
                          .Note(4, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertOptions opt;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "conversion";

    const SongSummary sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto tempos = midi.Find(0xFF);
    const auto cc = midi.Find(0xB0);
    const auto bends = midi.Find(0xE0);

    int start_cc10 = -1, cc10 = -1, cc11 = -1;
    for (const auto& [tick, bytes] : cc)
    {
        start_cc10 = bytes[1] == cc::kPan && tick == 0 ? bytes[2] : start_cc10;
        cc10 = bytes[1] == cc::kPan && tick > 0 ? bytes[2] : cc10;
        cc11 = bytes[1] == cc::kExpression && tick == 0 && bytes[2] ? bytes[2] : cc11;
    }

    bool between = false;
    for (const auto& [tick, bytes] : bends)
    {
        between = between || tick % 90 != 0;
    }

    const auto is_tempo = [](const auto& e)
    {
        return e.bytes.size() > 5 && e.bytes[1] == 0x51 && (e.bytes[3] << 16 | e.bytes[4] << 8 | e.bytes[5]) == 660779;
    };
    const auto is_marker = [](const auto& e)
    {
        return e.bytes.size() > 1 && e.bytes[1] == 0x06;
    };

    // A level of 30 of 128 on the left alone is panned hard left, and CC11 is 127 × sqrt(30/128 / (sqrt(2) × 127/128)),
    // about 52.
    SEQLIB_CHECK(sum.ok && sum.loop_start == 0 && sum.loop_end > 0);
    SEQLIB_CHECK_EQ(cc11, 52);
    SEQLIB_CHECK_EQ(start_cc10, 0);
    SEQLIB_CHECK_EQ(cc10, -1);
    SEQLIB_CHECK(between);
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), is_tempo));
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), is_marker));
}

// The listing follows the driver's repeats. A count of 0 makes 256 passes there. The listing also says what a range of
// 0, 0 and 0 plays.
void TestDump()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 90, 0xD6}).Note(0, 3).Bytes({0xD7, 2}).End(),
                      Channel().Bytes({0xE7, 0, 0, 0, 0xD6}).Note(0, 3).Bytes({0xD7, 0}).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::string path = Utf8(TempPath("dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("tempo 90, for every channel") != std::string::npos);
    SEQLIB_CHECK(s.find("0x08007011       96  30                 note C, 96 ticks (1/4)") != std::string::npos);
    SEQLIB_CHECK(s.find("0x08007014      192  FF                 end, or back to the loop point") != std::string::npos);
    SEQLIB_CHECK(s.find("play the sample once, from the last range's start, or from its start after DC or DD") !=
                 std::string::npos);
    SEQLIB_CHECK(s.find("play the repeat 256 times") != std::string::npos);
    SEQLIB_CHECK(s.find("   24576  FF ") != std::string::npos);
}

// OpenMusic() finds Quintet's driver from its code. A game without the driver's code isn't read, even with the music
// file given, and only --driver quintet makes that an error.
void TestMusic()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Note(0, 3).End()})});
    const Rom rom = cart.ToRom();
    Cart bare(Revision::kA, false);
    bare.Songs({Song({Channel().Note(0, 3).End()})});
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = FileAddress(kMusicFile);
    Overrides forced = table;
    forced.driver = Driver::kQuintet;
    std::string error, table_error, forced_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = quintet::OpenMusic(bare_rom, table, table_error);
    const std::unique_ptr<Music> bare_forced = quintet::OpenMusic(bare_rom, forced, forced_error);

    SEQLIB_CHECK(music && music->Log().front() == "Quintet's sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 1);
    SEQLIB_CHECK(!bare_music && table_error.empty());
    SEQLIB_CHECK(!bare_forced && forced_error == "no Quintet sound driver found, so it has no length table");
}

} // namespace

void RunTests()
{
    TestDetection();
    TestDecoding();
    TestTiming();
    TestHold();
    TestCommandGuard();
    TestWaveSwitch();
    TestLoopStarts();
    TestLoopAfterTempoChange();
    TestLoopLengths();
    TestLoopAtSecondPass();
    TestLoopSeam();
    TestLoopSettingsAgain();
    TestHourLimit();
    TestRegisters();
    TestWaveVolumes();
    TestPcm();
    TestBanks();
    TestNoiseDrums();
    TestNoiseMacros();
    TestConversion();
    TestDump();
    TestMusic();
}

} // namespace agbsf2::quintet
