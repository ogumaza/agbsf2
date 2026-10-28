// SPDX-License-Identifier: MIT

// Unit tests for Nintendo R&D2's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/text.h"

#include "files.h"
#include "gba.h"
#include "midi.h"
#include "music.h"
#include "rd2/convert.h"
#include "rd2/driver.h"
#include "rd2/sequencer.h"
#include "rd2/song.h"
#include "rd2/soundfont.h"
#include "rom.h"
#include "sf2.h"
#include "test_util.h"

namespace agbsf2::rd2
{
namespace
{

using test::HasGlobalZoneModulators;
using test::Le16;
using test::Le32;
using test::MidiEvents;
using test::ReadAll;
using test::ReadMidi;
using test::ReadSf2;
using test::Sf2Records;
using test::TempPath;
using test::TempSettings;

// Addresses in a test cartridge: the driver's code that detection reads, the game's call to its init, the driver's
// tables, and the game's settings and data.
constexpr uint32_t kInit = kRomBase + 0x100;
constexpr uint32_t kGameInit = kRomBase + 0x180;
constexpr uint32_t kCode = kRomBase + 0x200;
constexpr uint32_t kPitchTable = kRomBase + 0x800;
constexpr uint32_t kFrequencyTable = kRomBase + 0xA00;
constexpr uint32_t kNoiseTable = kRomBase + 0xB00;
constexpr uint32_t kLfoTable = kRomBase + 0xB80;
constexpr uint32_t kWaveVolumes = kRomBase + 0xC80;
constexpr uint32_t kVoiceClasses = kRomBase + 0xC88;
constexpr uint32_t kKeyEnvelope = kRomBase + 0xC90;
constexpr uint32_t kSettings = kRomBase + 0xD00;
constexpr uint32_t kSampleSets = kRomBase + 0xE00;
constexpr uint32_t kBanks = kRomBase + 0xF00;
constexpr uint32_t kBankSampleSets = kRomBase + 0xF80;
constexpr uint32_t kSequences = kRomBase + 0x1000;
constexpr uint32_t kBankLists = kRomBase + 0x1080;
constexpr uint32_t kSamples = kRomBase + 0x2000; // the sample set's table, then its samples
constexpr uint32_t kBank = kRomBase + 0x4000;
constexpr uint32_t kSequenceData = kRomBase + 0x6000;
constexpr uint32_t kEffects = kRomBase + 0x1100; // the table of the sound effects' sequences
constexpr uint32_t kEffectBankLists = kRomBase + 0x1180;
constexpr uint32_t kEffectData = kRomBase + 0x9000;

// The game's play-music routine, its table of music, and the routines that its bl's call.
constexpr uint32_t kPlayMusic = kRomBase + 0x600;
constexpr uint32_t kMusicTable = kRomBase + 0x780;
constexpr uint32_t kSetVoices = kRomBase + 0x7C0;
constexpr uint32_t kRequest = kRomBase + 0x7C4;
constexpr uint32_t kSetVolume2 = kRomBase + 0x7C8;
constexpr uint32_t kMuteRequest = kRomBase + 0x7D0;
constexpr uint32_t kYoshiMute = kRomBase + 0x700;
constexpr uint32_t kRequestQueue = 0x03000100; // the RAM word that holds the next request's address

// The test bank's instruments.
constexpr uint8_t kLooped = 0;      // a looped sample, with an envelope that rises and falls
constexpr uint8_t kPlain = 1;       // a sample without a loop, held at full level
constexpr uint8_t kSquare = 2;      // square 2 at a duty of 25%
constexpr uint8_t kDrums = 3;       // a drum kit from note 36: the plain sample at pan 0x20, square 2 at 0x40 and 0x50
constexpr uint8_t kKeyed = 4;       // a sample for each key
constexpr uint8_t kSplit = 5;       // the plain sample up to note 59, the looped one above
constexpr uint8_t kNoise = 6;       // noise
constexpr uint8_t kSquareSteps = 7; // square 2 with a table of duties: 12.5%, 25%, then 75%, a frame each
constexpr uint8_t kNoiseSteps = 8;  // noise with a table of widths: the 7-bit noise, then the 15-bit one
constexpr uint8_t kWave = 9;        // the wave voice: it leaves out its envelope, the one that rises and falls

// A track's data being written, one command at a time.
class TrackData
{
public:
    TrackData& Bytes(std::vector<uint8_t> b)
    {
        bytes_.insert(bytes_.end(), b.begin(), b.end());

        return *this;
    }

    // Adds a note with a length and velocity of its own.
    TrackData& Note(uint8_t note, uint16_t length, uint8_t velocity)
    {
        bytes_.push_back(uint8_t(0x60 + note));
        if (length >= 0x80)
        {
            bytes_.push_back(uint8_t(0x80 | length >> 8));
        }
        bytes_.push_back(uint8_t(length));
        bytes_.push_back(velocity);

        return *this;
    }

    TrackData& Wait(uint16_t ticks)
    {
        bytes_.push_back(0xC1);
        if (ticks >= 0x80)
        {
            bytes_.push_back(uint8_t(0x80 | ticks >> 8));
        }
        bytes_.push_back(uint8_t(ticks));

        return *this;
    }

    std::vector<uint8_t> End()
    {
        bytes_.push_back(0xFF);

        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, in a revision's
// form, the driver's tables, and the game's settings, samples, bank and sequences.
class Cart : public test::CartImage
{
public:
    explicit Cart(bool code = true, Revision revision = Revision::kLinkToThePast) : CartImage(0x10000)
    {
        WriteTables();
        if (code)
        {
            WriteCode(revision);
        }
        WriteSettings();
        WriteSamples();
        WriteBank();
    }

    // Writes the sequences, each a list of tracks, with their tables of offsets. Each plays bank 0.
    void Sequences(const std::vector<std::vector<std::vector<uint8_t>>>& sequences)
    {
        uint32_t at = kSequenceData;
        const uint32_t lists = kBankLists + 4 * uint32_t(sequences.size());
        for (size_t s = 0; s < sequences.size(); s++)
        {
            Put32(kSequences + 4 * uint32_t(s), at - kSequences);
            Put32(kBankLists + 4 * uint32_t(s), lists - kBankLists);
            const auto& tracks = sequences[s];
            Put8(at, uint8_t(tracks.size()));
            uint32_t data = at + 2 + 2 * uint32_t(tracks.size());
            for (size_t t = 0; t < tracks.size(); t++)
            {
                Put16(at + 2 + 2 * uint32_t(t), uint16_t(data - at));
                for (uint8_t b : tracks[t])
                {
                    Put8(data++, b);
                }
            }
            at = (data + 3) & ~3u;
        }
        Put16(lists, 0);
        Put16(lists + 2, 0);
    }

    // Writes the sound effects' sequences. Each is a list of tracks. A sequence starts with a halfword for each track:
    // the track's offset from the sequence's start, or 0 for an empty track. The game's settings point to the
    // sequences. Each sequence plays bank 0.
    void Effects(const std::vector<std::vector<std::vector<uint8_t>>>& sequences)
    {
        Put32(kSettings + 12, kEffects);
        Put32(kSettings + 24, kEffectBankLists);
        uint32_t at = kEffectData;
        const uint32_t lists = kEffectBankLists + 4 * uint32_t(sequences.size());
        for (size_t s = 0; s < sequences.size(); s++)
        {
            Put32(kEffects + 4 * uint32_t(s), at - kEffects);
            Put32(kEffectBankLists + 4 * uint32_t(s), lists - kEffectBankLists);
            const auto& tracks = sequences[s];
            uint32_t data = at + 2 * uint32_t(tracks.size());
            for (size_t t = 0; t < tracks.size(); t++)
            {
                Put16(at + 2 * uint32_t(t), tracks[t].empty() ? 0 : uint16_t(data - at));
                for (uint8_t b : tracks[t])
                {
                    Put8(data++, b);
                }
            }
            at = (data + 3) & ~3u;
        }
        Put16(lists, 0);
    }

    // Writes a play-music routine in the form of Super Mario Advance 3's, from its lookup of the music's entry on, and
    // its table, a halfword for each music number. The routine gives the driver 6 sample voices for music whose entry
    // has bit 10 set, and 7 for the rest. Each of `volumes` is a music number and the player's second volume that the
    // routine sets for it.
    void PlayMusic(const std::vector<uint16_t>& table, const std::vector<std::pair<int, int>>& volumes)
    {
        std::string code =
            "4900 0060 1840 8801 7805 2080 00C0 4008 2800 D003 2006 0000 0000 E002 2007 0000 0000 1C29 "
            "2013 0000 0000";
        for (const auto& [music, volume] : volumes)
        {
            code += " " + xsflib::Hex(0x2C00u | music, 4, false) + " D103 2013 " +
                    xsflib::Hex(0x2100u | volume, 4, false) + " 0000 0000";
        }
        code += " 4770";
        Code(kPlayMusic, code.c_str(), {{0, kMusicTable}});
        PutBl(kPlayMusic + 22, kSetVoices);
        PutBl(kPlayMusic + 30, kSetVoices);
        PutBl(kPlayMusic + 38, kRequest);
        for (uint32_t i = 0; i < volumes.size(); i++)
        {
            PutBl(kPlayMusic + 50 + 12 * i, kSetVolume2);
        }

        for (uint32_t m = 0; m < table.size(); m++)
        {
            Put16(kMusicTable + 2 * m, table[m]);
        }
    }

    // Writes a play-music routine in the form of the one in Super Mario Advance 2, from its lookup of the music's
    // sequence on, its table, a halfword for each music number, and the driver's routine that mutes tracks. The routine
    // mutes tracks of sequence `sequence`: those in `music_mask` for music number `music`, and those in `mask` for the
    // other music numbers that play the sequence. Each mask is a value of up to 8 bits, shifted left by up to 31.
    void MutingPlayMusic(const std::vector<uint16_t>& table, int sequence, int music, std::pair<int, int> mask,
                         std::pair<int, int> music_mask)
    {
        const auto movs_r1 = [](int value)
        {
            return xsflib::Hex(0x2100u | unsigned(value), 4, false);
        };
        const auto lsls_r1 = [](int shift)
        {
            return xsflib::Hex(0x0009u | unsigned(shift) << 6, 4, false);
        };
        const std::string code = "4800 0069 1809 780C 2013 1C21 0000 0000 1C28 0000 0000 " +
                                 xsflib::Hex(0x2C00u | unsigned(sequence), 4, false) + " D109 " + movs_r1(mask.first) +
                                 " " + lsls_r1(mask.second) + " " + xsflib::Hex(0x2D00u | unsigned(music), 4, false) +
                                 " D101 " + movs_r1(music_mask.first) + " " + lsls_r1(music_mask.second) +
                                 " 2013 2201 0000 0000 4770";
        Code(kPlayMusic, code.c_str(), {{0, kMusicTable}});
        PutBl(kPlayMusic + 12, kRequest);
        PutBl(kPlayMusic + 18, kSetVoices);
        PutBl(kPlayMusic + 42, kMuteRequest);
        Code(kMuteRequest, "B510 0612 0E12 4B00 681C 2380 005B 8023 0400 4310 6060 60A1 BD10", {{3, kRequestQueue}});

        for (uint32_t m = 0; m < table.size(); m++)
        {
            Put16(kMusicTable + 2 * m, table[m]);
        }
    }

    // Writes Super Mario Advance 2's routine that mutes the tracks in `mask` of the music whose entry has bit 9 set
    // unless the player rides Yoshi. The routine reads the table and makes the request that MutingPlayMusic() writes.
    void YoshiMute(int mask)
    {
        const std::string code =
            "B510 0600 0E04 0000 0000 0400 1400 2800 DB10 4900 0040 1840 8801 2080 0080 4008 "
            "2800 D007 2200 2C00 D100 2201 2013 " +
            xsflib::Hex(0x2100u | unsigned(mask), 4, false) + " 0000 0000 BC10 BC01 4700";
        Code(kYoshiMute, code.c_str(), {{9, kMusicTable}});
        PutBl(kYoshiMute + 6, kSetVoices);
        PutBl(kYoshiMute + 48, kMuteRequest);
    }

private:
    // Writes the halfwords of `code` at `at`, each ldr of `loads` (its halfword and its literal) loading from a pool
    // after the code. Returns the next free word.
    uint32_t Code(uint32_t at, const char* code, const std::vector<std::pair<int, uint32_t>>& loads = {})
    {
        std::istringstream in(code);
        std::string word;
        uint32_t a = at;
        while (in >> word)
        {
            Put16(a, uint16_t(std::stoul(word, nullptr, 16)));
            a += 2;
        }

        uint32_t pool = (a + 3) & ~3u;
        for (const auto& [index, literal] : loads)
        {
            const uint32_t ins = at + 2 * uint32_t(index);
            const uint16_t op = uint16_t(Get8(ins + 1) << 8);
            Put16(ins, uint16_t(op | ((pool - ((ins + 4) & ~3u)) / 4)));
            Put32(pool, literal);
            pool += 4;
        }

        return pool;
    }

    // Writes the tables: each pitch index's sample pitch, 0x8000 at index 0x30, and square frequency setting, middle C
    // at index 0x30; the noise settings; a sine; the wave volumes; the voice classes; and the envelope of a sample for
    // each key.
    void WriteTables()
    {
        for (int i = 0; i <= 0x78; i++)
        {
            Put32(kPitchTable + 4 * uint32_t(i), uint32_t(std::lround(0x800 * std::pow(2.0, i / 12.0))));
            const double hz = 261.6255653005986 * std::pow(2.0, (i - 0x30) / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(i), uint16_t(std::lround(std::max(0.0, 2048 - 131072 / hz))));
            Put8(kNoiseTable + uint32_t(i), uint8_t(std::max(0, 0x77 - i)));
        }
        for (uint32_t i = 0; i < 256; i++)
        {
            Put8(kLfoTable + i, uint8_t(int8_t(std::lround(127 * std::sin(i * 6.283185307179586 / 256)))));
        }

        const uint8_t kWaves[5] = {0x00, 0x60, 0x40, 0x80, 0x20};
        for (uint32_t i = 0; i < 5; i++)
        {
            Put8(kWaveVolumes + i, kWaves[i]);
            Put8(kVoiceClasses + i, uint8_t(i));
        }
        Put32(kKeyEnvelope, 0x7FFF0001);
        Put32(kKeyEnvelope + 4, 0x0000FFFF);
    }

    // Writes the driver's code that detection reads, as the revision's compiler built it, and the game's call to the
    // driver's init.
    void WriteCode(Revision revision)
    {
        Code(kInit, "B530 4900 6008 4900 2000 7008 2080 7008 3904 4A00 1C10 8008 3102 200D 7008 BD30");
        Put16(kGameInit, 0x4801);
        PutBl(kGameInit + 2, kInit);
        Put16(kGameInit + 6, 0x4770);
        Put32(kGameInit + 8, kSettings);

        uint32_t at = kCode;
        if (revision == Revision::kSuperMarioAdvance2)
        {
            at = Code(at,
                      "0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 "
                      "D108 4800 1104 8913 0918 0868 E00E 46C0 46C0 46C0 2804 D007 4800 4770",
                      {{19, kPitchTable}, {30, kFrequencyTable}});
            at = Code(at, "0400 0C01 2977 D900 2177 4800 1808 7800 4770", {{5, kNoiseTable}});
            at = Code(at, "68A0 6882 2A00 D000 6860 2800 D100 4800 6A29 0849 1809 7809 4770", {{7, kLfoTable}});
            at = Code(at, "4900 7830 1840 7800 1C29 3152 780A 1C29 4770", {{0, kVoiceClasses}});
            at = Code(at, "B530 1C05 7868 2801 D11A 7A6C 0224 4770");
        }
        else
        {
            at = Code(at,
                      "B500 0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 "
                      "2800 D107 4800 1104 8913 0918 0868 E00D 46C0 46C0 2804 D007 4800 4770",
                      {{20, kPitchTable}, {30, kFrequencyTable}});
            at = Code(at, "B500 0400 0C01 2977 D900 2177 4800 1808 7800 4770", {{6, kNoiseTable}});
            at = Code(at, "68A0 6883 2B00 D000 6860 2800 D100 4800 6A29 0849 1809 7809 4770", {{7, kLfoTable}});
            at = Code(at, "4900 7830 1840 7800 1C29 3152 7809 4770", {{0, kVoiceClasses}});
        }
        at = Code(at, "2904 D900 2104 0609 0E09 4A00 4800 1809 7808 7010 4770", {{6, kWaveVolumes}});
        Code(at, "8869 1859 4A00 0070 1840 8800 8050 6022 4800 6060 4770", {{8, kKeyEnvelope}});
    }

    // Writes the settings: one sample set, one bank that plays it, and the sequences and their lists of banks.
    void WriteSettings()
    {
        const uint32_t kTables[7] = {kSampleSets, kBanks, kSequences, 0, kBankSampleSets, kBankLists, 0};
        for (uint32_t i = 0; i < 7; i++)
        {
            Put32(kSettings + 4 * i, kTables[i]);
        }
        Put32(kSampleSets, kSamples - kSampleSets);
        Put32(kBanks, kBank - kBanks);
        Put16(kBankSampleSets, 0);
    }

    // Writes the samples: a looped saw at 10512 Hz, and a square wave without a loop at 21024 Hz.
    void WriteSamples()
    {
        const uint32_t kFirst = kSamples + 8;
        Put32(kSamples, kFirst - kSamples);
        Put32(kFirst, 64);
        Put32(kFirst + 4, 10512);
        Put32(kFirst + 8, 16);
        Put32(kFirst + 12, 64);
        for (uint32_t i = 0; i < 64; i++)
        {
            Put8(kFirst + 16 + i, uint8_t(int8_t(i * 4 - 128)));
        }

        const uint32_t kSecond = kFirst + 16 + 64;
        Put32(kSamples + 4, kSecond - kSamples);
        Put32(kSecond, 4000);
        Put32(kSecond + 4, 21024);
        Put32(kSecond + 8, 0);
        Put32(kSecond + 12, 0);
        for (uint32_t i = 0; i < 4000; i++)
        {
            Put8(kSecond + 16 + i, uint8_t(i % 20 < 10 ? 100 : -100));
        }
    }

    // Writes the bank: the instruments' offsets, then the instruments, their envelopes and tables, all from the bank's
    // start.
    void WriteBank()
    {
        uint32_t at = 0x40;
        auto add = [&](std::vector<uint8_t> bytes)
        {
            const uint32_t here = at;
            for (uint8_t b : bytes)
            {
                Put8(kBank + at++, b);
            }
            at = (at + 3) & ~3u;

            return uint16_t(here);
        };

        auto region = [](uint8_t type, uint8_t flags, uint16_t arg, uint16_t envelope, uint8_t release, uint8_t tuning)
        {
            return std::vector<uint8_t>{
                type,   flags, uint8_t(arg), uint8_t(arg >> 8), uint8_t(envelope), uint8_t(envelope >> 8), release,
                tuning, 8};
        };

        // Envelopes: up in 4 frames and down to half in 8, and up at once and held.
        const uint16_t rise = add({4, 0, 0xFF, 0x7F, 8, 0, 0x80, 0x3F, 0xFF, 0xFF, 0, 0});
        const uint16_t held = add({1, 0, 0xFF, 0x7F, 0xFF, 0xFF, 0, 0});

        Put16(kBank + 2 * kLooped, add(region(0, 0, 0, rise, 0x46, 60)));
        Put16(kBank + 2 * kPlain, add(region(0, 0, 1, held, 0x00, 48)));
        const uint16_t square = add(region(2, 0, 1, held, 0x40, 0x30));
        Put16(kBank + 2 * kSquare, square);
        const uint16_t plain = add(region(0, 0, 1, held, 0x00, 0x30));
        // Square 2 a semitone higher. Two PSG drums with the same tuning share a key and its zone.
        const uint16_t higher = add(region(2, 0, 1, held, 0x40, 0x2F));
        const uint16_t kit = add({uint8_t(plain), uint8_t(plain >> 8), 0x20, 0, uint8_t(square), uint8_t(square >> 8),
                                  0x40, 0, uint8_t(higher), uint8_t(higher >> 8), 0x50, 0});
        Put16(kBank + 2 * kDrums, add({0x10, 0, uint8_t(kit), uint8_t(kit >> 8), 36, 0, 0, 0}));
        std::vector<uint8_t> keys;
        for (int k = 0; k < 128; k++)
        {
            keys.push_back(uint8_t(k & 1));
            keys.push_back(0);
        }
        const uint16_t key_table = add(keys);
        Put16(kBank + 2 * kKeyed, add({0x11, 0, uint8_t(key_table), uint8_t(key_table >> 8), 0, 0, 0, 0}));
        const uint16_t looped = add(region(0, 0, 0, rise, 0x46, 60));
        const uint16_t low = add(region(0, 0, 1, held, 0x00, 48));
        const uint16_t split =
            add({59, 0, uint8_t(low), uint8_t(low >> 8), 255, 0, uint8_t(looped), uint8_t(looped >> 8)});
        Put16(kBank + 2 * kSplit, add({0x12, 0, uint8_t(split), uint8_t(split >> 8), 0, 0, 0, 0}));
        Put16(kBank + 2 * kNoise, add(region(4, 0, 0, held, 0x20, 0x30)));
        const uint16_t duties = add({3, 0, 0, 1, 3});
        Put16(kBank + 2 * kSquareSteps, add(region(2, 1, duties, held, 0x40, 0x30)));
        const uint16_t widths = add({2, 0, 1, 0});
        Put16(kBank + 2 * kNoiseSteps, add(region(4, 1, widths, held, 0x20, 0x30)));
        const uint16_t wave =
            add({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});
        Put16(kBank + 2 * kWave, add(region(3, 0, wave, rise, 0x46, 0x30)));
    }
};

// Returns the driver's tables in `rom`, which must have them.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

// Runs version `version` of a sequence for `frames` frames, and returns each frame's events.
std::vector<std::vector<Event>> Run(const Rom& rom, const DriverInfo& info, int sequence, int frames, int version = 0)
{
    Sequencer seq(rom, info, sequence, version);
    std::vector<std::vector<Event>> out;
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        out.push_back(seq.Events());
    }

    return out;
}

void TestDetection()
{
    // The driver's code gives its tables, and the game's call to its init gives its settings.
    Cart cart;
    cart.Sequences({{TrackData().End()}, {TrackData().End()}});
    const Rom rom = cart.ToRom();
    DriverInfo info;
    std::string error;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);

    SEQLIB_CHECK(found);
    SEQLIB_CHECK_EQ(info.init, kInit);
    SEQLIB_CHECK_EQ(info.settings, kSettings);
    SEQLIB_CHECK_EQ(info.pitch_table, kPitchTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.noise_table, kNoiseTable);
    SEQLIB_CHECK_EQ(info.lfo_table, kLfoTable);
    SEQLIB_CHECK_EQ(info.wave_volumes, kWaveVolumes);
    SEQLIB_CHECK_EQ(info.voice_classes, kVoiceClasses);
    SEQLIB_CHECK_EQ(info.key_envelope, kKeyEnvelope);
    SEQLIB_REQUIRE_EQ(info.sequence_addresses.size(), 2);
    SEQLIB_CHECK_EQ(info.sequence_addresses[0], kSequenceData);

    // A game without the driver's code has no sign of it, unless it's given the settings.
    Cart bare(false);
    bare.Sequences({{TrackData().End()}});
    const Rom none = bare.ToRom();
    DriverOverrides given;
    given.settings = kSettings;
    DriverInfo nothing, tables;

    SEQLIB_CHECK(!DetectDriver(none, DriverOverrides(), nothing, error));
    SEQLIB_CHECK(error.empty());
    SEQLIB_CHECK(!DetectDriver(none, given, tables, error));
    SEQLIB_CHECK(!error.empty());

    // Detection starts from nothing. A game without the driver keeps none of an earlier game's tables.
    SEQLIB_CHECK(!DetectDriver(none, DriverOverrides(), info, error));
    SEQLIB_CHECK(info.init == 0 && info.sequence_addresses.empty());
}

void TestSuperMarioAdvance2()
{
    // The Super Mario Advance 2 revision's code gives its tables too. Its E4 takes a byte, so a tempo of 150 plays a
    // tick a frame, and EA does nothing and takes no operand.
    Cart cart(true, Revision::kSuperMarioAdvance2);
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 150, 0xEA, 0xC2, kLooped, 0xE2, 2, 0xE1, 64})
                         .Note(60, 4, 127)
                         .Wait(4)
                         .Bytes({0xE1, uint8_t(-64)})
                         .Note(60, 4, 127)
                         .Wait(4)
                         .Bytes({0xE1, 0, 0xC2, kPlain, 0xE0, 0x55})
                         .Note(48, 4, 100)
                         .Wait(4)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    std::vector<std::pair<int, int>> notes;
    std::vector<uint32_t> pitches, volumes;
    for (int f = 0; f < 9; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kNote)
            {
                notes.emplace_back(f, e.voice);
            }
        }
        pitches.push_back(notes.empty() ? 0 : seq.Voices()[size_t(notes.back().second)].pitch);
        volumes.push_back(notes.empty() ? 0 : seq.Voices()[size_t(notes.back().second)].volume);
    }

    SEQLIB_CHECK(info.revision == Revision::kSuperMarioAdvance2);
    SEQLIB_CHECK_EQ(info.pitch_table, kPitchTable);
    SEQLIB_CHECK_EQ(info.frequency_table, kFrequencyTable);
    SEQLIB_CHECK_EQ(info.noise_table, kNoiseTable);
    SEQLIB_CHECK_EQ(info.lfo_table, kLfoTable);
    SEQLIB_CHECK_EQ(info.voice_classes, kVoiceClasses);
    SEQLIB_REQUIRE_EQ(notes.size(), 3);
    SEQLIB_CHECK_EQ(notes[1].first, 4);
    SEQLIB_CHECK_EQ(notes[2].first, 8);

    // A bend of 64 with a range of 2 scales the pitch by (4013 * 64 + 0x400000) >> 14 = 271 256ths, and a bend of -64
    // by 240 256ths, from 0x8000 for the looped sample's note 60.
    const uint32_t range = rom.U32(kPitchTable + 4 * 0x32);
    SEQLIB_CHECK_EQ(range, 36781);
    SEQLIB_CHECK_EQ(pitches[0], (0x8000u * 271) >> 8);
    SEQLIB_CHECK_EQ(pitches[4], (0x8000u * 240) >> 8);

    // A velocity of 100 at a track volume of 0x55 and the envelope's full level gives a level of 16999, without the
    // player's second volume. A Link to the Past's revision gives 16991.
    SEQLIB_CHECK_EQ(volumes[8], 16999);
}

void TestTiming()
{
    // At a tempo of 75, a tick takes two frames, and a note of 3 ticks lasts 6 frames before its release. With C8,
    // notes wait for their length.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 75, 0xC2, kLooped})
                         .Note(60, 3, 127)
                         .Wait(2)
                         .Note(62, 1, 127)
                         .Bytes({0xC8})
                         .Note(64, 2, 127)
                         .Note(65, 2, 127)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto frames = Run(rom, info, 0, 16);

    std::vector<std::pair<int, uint64_t>> ons, releases;
    for (size_t f = 0; f < frames.size(); f++)
    {
        for (const Event& e : frames[f])
        {
            if (e.kind == Event::kNote)
            {
                ons.emplace_back(int(f), e.units);
            }
            if (e.kind == Event::kRelease && e.id == 1)
            {
                releases.emplace_back(int(f), e.units);
            }
        }
    }
    SEQLIB_REQUIRE_EQ(ons.size(), 4);
    SEQLIB_CHECK_EQ(ons[0].first, 0);
    SEQLIB_CHECK_EQ(ons[1].first, 4);
    SEQLIB_CHECK_EQ(ons[1].second, 300);
    SEQLIB_CHECK_EQ(ons[2].first, 4);
    SEQLIB_CHECK_EQ(ons[3].first, 8);
    SEQLIB_CHECK_EQ(ons[3].second, 600);
    SEQLIB_REQUIRE_EQ(releases.size(), 1);
    SEQLIB_CHECK_EQ(releases[0].first, 5);
}

void TestVoices()
{
    // The mixer has 7 voices. An eighth note takes the oldest of the lowest priority, and a note of a lower priority
    // than every voice's gets none.
    Cart cart;
    TrackData t;
    t.Bytes({0xC2, kLooped});
    for (uint8_t n = 0; n < 8; n++)
    {
        t.Note(uint8_t(50 + n), 20, 100);
    }
    t.Wait(1).Bytes({0xC4, 1}).Note(70, 20, 100);
    cart.Sequences({{t.End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const auto frames = Run(rom, info, 0, 2);

    int voiced = 0, stopped = -1, eighth = -1, last = 0;
    for (const auto& frame : frames)
    {
        for (const Event& e : frame)
        {
            voiced += e.kind == Event::kNote && e.voice >= 0 ? 1 : 0;
            stopped = e.kind == Event::kStop ? int(e.id) : stopped;
            eighth = e.kind == Event::kNote && e.key == 57 ? e.voice : eighth;
            last = e.kind == Event::kNote && e.key == 70 ? e.voice : last;
        }
    }
    SEQLIB_CHECK_EQ(voiced, 8);
    SEQLIB_CHECK_EQ(stopped, 1);
    SEQLIB_CHECK_EQ(eighth, 0);
    SEQLIB_CHECK_EQ(last, -1);
}

// Returns a track that plays `notes` notes at once on the plain sample, from note 50 up, and ends once they have.
std::vector<uint8_t> Chord(int notes)
{
    TrackData t;
    t.Bytes({0xC2, kPlain});
    for (int n = 0; n < notes; n++)
    {
        t.Note(uint8_t(50 + n), 20, 127);
    }

    return t.Wait(20).End();
}

// Returns the first CC11 of a sequence's conversion, or -1 if it has none.
int FirstExpression(const Rom& rom, const DriverInfo& info, int sequence)
{
    ConvertSettings opt = TempSettings("rd2_music_settings");
    const SongReport sum = ConvertSong(rom, info, sequence, opt, nullptr);
    SEQLIB_CHECK(sum.ok);
    if (!sum.ok)
    {
        return -1;
    }

    for (const auto& e : ReadMidi(sum.midi_path).Find(0xB0))
    {
        if (e.bytes[1] == 11)
        {
            return e.bytes[2];
        }
    }

    return -1;
}

void TestMusicSettings()
{
    // The game's play-music routine gives the driver 6 sample voices for music 0, whose entry has bit 10 set, and the
    // player a second volume of 0x37 for music 2. Music 3 plays sequence 1 too, but music 0 played it first. Music 4's
    // sequence isn't in the game, so the table ends there, and music 5 isn't read.
    Cart cart;
    cart.Sequences({{Chord(7)}, {Chord(7)}, {Chord(1)}, {Chord(1)}});
    cart.PlayMusic({0x0401, 0x0000, 0x0002, 0x0001, 0x0009, 0x0403}, {{2, 0x37}});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 4);
    const int kVoiceCounts[4] = {7, 6, 7, 7};
    const int kSecondVolumes[4] = {0x80, 0x80, 0x37, 0x80};
    for (int s = 0; s < 4; s++)
    {
        SEQLIB_REQUIRE_EQ(info.music_settings[size_t(s)].size(), 1);
        SEQLIB_CHECK_EQ(info.music_settings[size_t(s)][0].sample_voices, kVoiceCounts[s]);
        SEQLIB_CHECK_EQ(info.music_settings[size_t(s)][0].volume2, kSecondVolumes[s]);
        SEQLIB_CHECK_EQ(SongVersions(info, s).size(), 1);
    }

    // With 7 voices, seven notes at once each have one. With 6, the seventh takes the first note's voice.
    for (int s = 0; s < 2; s++)
    {
        int voiced = 0, stopped = 0;
        for (const auto& frame : Run(rom, info, s, 2))
        {
            for (const Event& e : frame)
            {
                voiced += e.kind == Event::kNote && e.voice >= 0 ? 1 : 0;
                stopped += e.kind == Event::kStop ? 1 : 0;
            }
        }
        SEQLIB_CHECK_EQ(voiced, 7);
        SEQLIB_CHECK_EQ(stopped, s);
    }

    // CC11 scales the amplitude by its square, so the second volume of 0x37 takes it down to sqrt(0x37 / 0x80) of the
    // driver's default.
    const int full = FirstExpression(rom, info, 3);
    const int lower = FirstExpression(rom, info, 2);
    SEQLIB_CHECK(full > 0);
    SEQLIB_CHECK(std::abs(lower - full * std::sqrt(0x37 / 128.0)) <= 1);
}

void TestMusicSettingsWithCount()
{
    // An override converts 2 of the game's 3 sequences. Music 0 plays sequence 2, which isn't converted, but the game
    // has it, so the table goes on. Music 1 gives sequence 0 6 sample voices and a second volume of 0x37.
    Cart cart;
    cart.Sequences({{Chord(1)}, {Chord(1)}, {Chord(1)}});
    cart.PlayMusic({0x0002, 0x0400, 0x0009}, {{1, 0x37}});
    const Rom rom = cart.ToRom();
    DriverOverrides overrides;
    overrides.sequence_count = 2;
    DriverInfo info;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, overrides, info, error));

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 2);
    SEQLIB_REQUIRE_EQ(info.music_settings[0].size(), 1);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].sample_voices, 6);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].volume2, 0x37);
}

// Returns the keys of the note-ons on every channel of a MIDI file, from the lowest.
std::vector<int> NoteKeys(const std::string& path)
{
    const MidiEvents midi = ReadMidi(path);
    std::vector<int> keys;
    for (int channel = 0; channel < 16; channel++)
    {
        for (const auto& e : midi.Find(uint8_t(0x90 | channel)))
        {
            if (e.bytes[2])
            {
                keys.push_back(e.bytes[1]);
            }
        }
    }
    std::sort(keys.begin(), keys.end());

    return keys;
}

void TestMutedTracks()
{
    // The game's play-music routine mutes tracks of sequence 1: track 2 for music 2, and track 1 for the other music
    // numbers that play it. Music 1 plays the sequence first and gives the first version. Music 2 makes a second
    // version. Music 3 mutes the same track as music 1 and makes no version.
    Cart cart;
    cart.Sequences({{Chord(1)},
                    {TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(62, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(6).End()}});
    cart.MutingPlayMusic({0x0000, 0x0001, 0x0001, 0x0001, 0x0009}, 1, 2, {1, 1}, {1, 2});
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 2);
    SEQLIB_REQUIRE_EQ(info.music_settings[0].size(), 1);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].music, 0);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].muted_tracks, 0);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].sample_voices, 7);
    SEQLIB_REQUIRE_EQ(info.music_settings[1].size(), 2);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].music, 1);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].muted_tracks, 0x2);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].music, 2);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].muted_tracks, 0x4);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's music table: 0x08000780; muted tracks: in sequence 1, track 1 as music 1 and "
                           "track 2 as music 2") != info.log.end());

    const std::vector<SongVersion> versions = SongVersions(info, 1);
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].name.empty() && versions[0].suffix.empty());
    SEQLIB_CHECK(versions[1].name == "as music 2" && versions[1].suffix == "_music2");
    SEQLIB_CHECK_EQ(SongVersions(info, 0).size(), 1);

    // A muted track plays no notes. Each version's files have its suffix after the sequence's number.
    for (int v = 0; v < 2; v++)
    {
        int notes = 0;
        for (const auto& frame : Run(rom, info, 1, 4, v))
        {
            for (const Event& e : frame)
            {
                notes += e.kind == Event::kNote ? 1 : 0;
            }
        }
        SEQLIB_CHECK_EQ(notes, 2);

        ConvertSettings opt = TempSettings("rd2_muted_tracks");
        opt.version = v;
        const SongReport r = ConvertSong(rom, info, 1, opt, nullptr);
        SEQLIB_REQUIRE(r.ok);
        SEQLIB_CHECK_EQ(r.tracks, 2);
        SEQLIB_CHECK(r.midi_path.ends_with(v == 0 ? "_01.mid" : "_01_music2.mid"));
        SEQLIB_CHECK(NoteKeys(r.midi_path) == (v == 0 ? std::vector<int>({60, 64}) : std::vector<int>({60, 62})));
    }

    // The Music interface gives the versions too.
    std::string error;
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK_EQ(music->Versions(1).size(), 2);
    SEQLIB_CHECK(music->Versions(1)[1].name == "as music 2");
}

void TestYoshiTracks()
{
    // Unless the player rides Yoshi, the game mutes track 1 of the music whose entry has bit 9 set. Music 1 plays
    // sequence 1 with the bit. The first version mutes the track, and a second version, riding Yoshi, plays it. Music 0
    // plays sequence 0 without the bit, in one version.
    Cart cart;
    cart.Sequences({{Chord(1)},
                    {TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(62, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(6).End()}});
    cart.MutingPlayMusic({0x0000, 0x0201, 0x0009}, 5, 9, {1, 0}, {1, 1});
    cart.YoshiMute(0x02);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_REQUIRE_EQ(info.music_settings.size(), 2);
    SEQLIB_REQUIRE_EQ(info.music_settings[0].size(), 1);
    SEQLIB_CHECK_EQ(info.music_settings[0][0].muted_tracks, 0);
    SEQLIB_REQUIRE_EQ(info.music_settings[1].size(), 2);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].music, 1);
    SEQLIB_CHECK_EQ(info.music_settings[1][0].muted_tracks, 0x2);
    SEQLIB_CHECK(!info.music_settings[1][0].riding_yoshi);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].music, 1);
    SEQLIB_CHECK_EQ(info.music_settings[1][1].muted_tracks, 0);
    SEQLIB_CHECK(info.music_settings[1][1].riding_yoshi);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's music table: 0x08000780; muted tracks: none; muted unless riding Yoshi: track 1 "
                           "of sequence 1") != info.log.end());

    const std::vector<SongVersion> versions = SongVersions(info, 1);
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].name.empty() && versions[0].suffix.empty());
    SEQLIB_CHECK(versions[1].name == "riding Yoshi" && versions[1].suffix == "_yoshi");

    // The first version leaves out the track's notes. The second has them.
    for (int v = 0; v < 2; v++)
    {
        ConvertSettings opt = TempSettings("rd2_yoshi_tracks");
        opt.version = v;
        const SongReport r = ConvertSong(rom, info, 1, opt, nullptr);
        SEQLIB_REQUIRE(r.ok);
        SEQLIB_CHECK(r.midi_path.ends_with(v == 0 ? "_01.mid" : "_01_yoshi.mid"));
        SEQLIB_CHECK(NoteKeys(r.midi_path) == (v == 0 ? std::vector<int>({60, 64}) : std::vector<int>({60, 62, 64})));
    }
}

void TestEnvelope()
{
    // The envelope rises to 0x7FFF in 4 frames, a quarter each frame. The release then takes the voice's volume down to
    // 300/512 of itself each frame until it's silent, and the voice is freed 10 frames after the release starts.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kLooped}).Note(60, 6, 127).Wait(40).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    std::vector<int32_t> levels;
    std::vector<uint32_t> volumes;
    std::vector<uint8_t> states;
    for (int f = 0; f < 30; f++)
    {
        seq.Step();
        levels.push_back(seq.Voices()[0].level);
        volumes.push_back(seq.Voices()[0].volume);
        states.push_back(seq.Voices()[0].state);
    }

    SEQLIB_CHECK_EQ(levels[0], 0x7FFF / 4);
    SEQLIB_CHECK_EQ(levels[3], 4 * (0x7FFF / 4));
    SEQLIB_CHECK_EQ(levels[4], 0x7FFF + (0x3F80 - 0x7FFF) / 8);
    SEQLIB_CHECK_EQ(states[4], 1);
    SEQLIB_CHECK_EQ(states[5], 2);
    SEQLIB_CHECK(volumes[5] > 0 && volumes[6] == (300 * volumes[5]) >> 9);
    SEQLIB_CHECK_EQ(states[14], 2);
    SEQLIB_CHECK_EQ(states[15], 0);
    SEQLIB_CHECK_EQ(states[29], 0);
}

void TestPsg()
{
    // A square 2 note starts the channel at its pitch, with its envelope and duty, on both sides. With a tuning of
    // 0x30, note 0x30 plays middle C.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(10).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    seq.Step();

    std::vector<std::pair<uint32_t, uint32_t>> writes;
    for (const RegisterWrite& w : seq.Writes())
    {
        writes.emplace_back(w.address, w.value);
    }
    const uint16_t middle_c = rom.U16(kFrequencyTable + 2 * 0x30);
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x04000081u, 0xFFu)) != writes.end());
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x04000069u, 0xF8u)) != writes.end());
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x0400006Cu, 0x8000u | middle_c)) !=
                 writes.end());
    SEQLIB_CHECK(std::find(writes.begin(), writes.end(), std::make_pair(0x04000068u, 0x40u)) != writes.end());
}

void TestInstruments()
{
    // A drum kit plays its regions at their own pitch and pan, an instrument with a sample for each key picks one, and
    // a key split plays one region below its split and another above.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kDrums})
                         .Note(36, 4, 100)
                         .Wait(1)
                         .Bytes({0xC2, kKeyed})
                         .Note(40, 4, 100)
                         .Note(41, 4, 100)
                         .Wait(1)
                         .Bytes({0xC2, kSplit})
                         .Note(59, 4, 100)
                         .Note(60, 4, 100)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Event> notes;
    std::vector<uint32_t> pitches;
    for (int f = 0; f < 4; f++)
    {
        seq.Step();
        for (const Event& e : seq.Events())
        {
            if (e.kind == Event::kNote)
            {
                notes.push_back(e);
                pitches.push_back(e.voice >= 0 ? seq.Voices()[size_t(e.voice)].note_pitch : 0);
            }
        }
    }

    SEQLIB_REQUIRE_EQ(notes.size(), 5);
    SEQLIB_CHECK(notes[0].lookup.drum);
    SEQLIB_CHECK_EQ(notes[0].lookup.drum_pan, 0x20);
    SEQLIB_CHECK_EQ(pitches[0], 0x8000);
    SEQLIB_CHECK(notes[1].lookup.key_sample && notes[2].lookup.key_sample);
    SEQLIB_CHECK(notes[1].sample != notes[2].sample);
    SEQLIB_CHECK_EQ(notes[3].sample, notes[2].sample);
    SEQLIB_CHECK_EQ(notes[4].sample, notes[1].sample);
    SEQLIB_CHECK_EQ(pitches[3], rom.U32(kPitchTable + 4 * (59 + 0x30 - 48)));
}

void TestConversion()
{
    // The conversion of a sequence that loops has the tempo, the loop markers, the notes' keys and velocities, and the
    // SoundFont's zone for the instrument, which plays the sample at its tuning. The first track goes back to its
    // start, just after the sequence's header of 6 bytes.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 120, 0xC2, kPlain})
                         .Note(60, 6, 127)
                         .Wait(6)
                         .Note(64, 6, 64)
                         .Wait(6)
                         .Bytes({0xF0, 0x06, 0x00})
                         .End(),
                     TrackData().Bytes({0xC2, kLooped}).Note(48, 12, 100).Wait(48).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 1;
    opt.out_dir = Utf8(test::g_temp);
    opt.base_name = "rd2";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK_EQ(sum.tracks, 2);
    SEQLIB_CHECK(sum.loop_start >= 0 && sum.loop_end > sum.loop_start);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    SEQLIB_CHECK_EQ(midi.division, 3600);
    const auto first = midi.Find(0x90);
    SEQLIB_REQUIRE(first.size() >= 2);
    SEQLIB_CHECK_EQ(first[0].tick, 0);
    SEQLIB_CHECK_EQ(first[0].bytes[1], 60);
    SEQLIB_CHECK_EQ(first[0].bytes[2], 127);
    SEQLIB_CHECK_EQ(first[1].tick, 900);
    SEQLIB_CHECK_EQ(first[1].bytes[1], 64);
    SEQLIB_CHECK_EQ(first[1].bytes[2], int(std::lround(127 * std::sqrt(64 / 127.0))));

    const auto metas = midi.Find(0xFF);
    const auto is_tempo = [](const auto& e)
    {
        return e.bytes.size() > 5 && e.bytes[1] == 0x51 && (e.bytes[3] << 16 | e.bytes[4] << 8 | e.bytes[5]) == 502281;
    };
    const auto is_marker = [](const auto& e)
    {
        return e.bytes.size() > 1 && e.bytes[1] == 0x06;
    };
    SEQLIB_CHECK(std::any_of(metas.begin(), metas.end(), is_tempo));
    SEQLIB_CHECK(std::any_of(metas.begin(), metas.end(), is_marker));

    const Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));
    SEQLIB_CHECK_EQ(sf.Count("phdr", 38), 3);
    SEQLIB_CHECK_EQ(sf.Count("shdr", 46), 3);
    const auto zone = sf.ZoneGens(1); // the first instrument's zone after its global zone
    SEQLIB_CHECK_EQ(zone.at(58), 48);
    SEQLIB_CHECK_EQ(Le32(sf.Record("shdr", 46, 0) + 36), 21024);
}

void TestPsgTables()
{
    // A square that steps through a table of duties, a frame each, holds the last one, 75%. A noise holds the last of
    // its widths, the 15-bit noise. Each SoundFont zone plays what its voice holds.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kSquareSteps})
                         .Note(0x30, 12, 127)
                         .Wait(12)
                         .Bytes({0xC2, kNoiseSteps})
                         .Note(0x30, 12, 127)
                         .Wait(12)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_psg_tables");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    std::vector<std::string> squares;
    std::vector<int> noises;
    for (size_t s = 0; s + 1 < sf.Count("shdr", 46); s++)
    {
        const std::string name(reinterpret_cast<const char*>(sf.Record("shdr", 46, s)), 20);
        const std::string trimmed = name.substr(0, name.find('\0'));
        if (trimmed.rfind("Square", 0) == 0)
        {
            squares.push_back(trimmed);
        }
        if (trimmed.rfind("Noise ", 0) == 0)
        {
            noises.push_back(std::stoi(trimmed.substr(6), nullptr, 16));
        }
    }
    SEQLIB_CHECK(squares == std::vector<std::string>{"Square 75%"});
    SEQLIB_CHECK(noises.size() == 1 && (noises[0] & 8) == 0);
}

void TestPsgSampleLevels()
{
    // On the GBA, a square at full volume, the noise and a wave from 0 to 15 all swing between the same two levels.
    // Their samples therefore span the same range: 2 × PsgShare() × 32767. A full-scale sample spans 2 × 32767.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kSquare})
                         .Note(0x30, 4, 127)
                         .Wait(4)
                         .Bytes({0xC2, kNoise})
                         .Note(0x30, 4, 127)
                         .Wait(4)
                         .Bytes({0xC2, kWave})
                         .Note(48, 4, 127)
                         .Wait(4)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_psg_levels");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    std::vector<std::pair<std::string, int>> spans;
    for (size_t s = 0; s + 1 < sf.Count("shdr", 46); s++)
    {
        const uint8_t* header = sf.Record("shdr", 46, s);
        const std::string name(reinterpret_cast<const char*>(header), 20);
        int low = 0;
        int high = 0;
        for (uint32_t i = Le32(header + 20); i < Le32(header + 24); i++)
        {
            const int point = int16_t(Le16(&pcm[2 * i]));
            low = std::min(low, point);
            high = std::max(high, point);
        }
        spans.emplace_back(name.substr(0, name.find(' ')), high - low);
    }

    const double expected = 2 * PsgShare(Revision::kLinkToThePast) * 32767;
    for (const char* kind : {"Square", "Noise", "Wave"})
    {
        const auto it = std::find_if(spans.begin(), spans.end(), [&](const auto& s) { return s.first == kind; });
        SEQLIB_REQUIRE(it != spans.end());
        SEQLIB_CHECK(std::fabs(it->second - expected) <= 1);
    }
}

void TestDrumPanAndWave()
{
    // A drum keeps its pan whatever its track's pan is. So the drum kit's zones replace the default modulator from CC10
    // with one of no amount, and the plain instrument's zone keeps the default. The wave voice leaves its region's
    // envelope out, and its notes' CC11 follows its release (TestWaveVolume()). Its zone has no envelope.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x60, 0xC2, kDrums})
                         .Note(36, 4, 100)
                         .Note(37, 4, 100)
                         .Wait(4)
                         .Note(38, 4, 100)
                         .Wait(4)
                         .Bytes({0xC2, kWave})
                         .Note(48, 8, 100)
                         .Wait(8)
                         .Bytes({0xC2, kPlain})
                         .Note(60, 4, 100)
                         .Wait(4)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_drum_wave");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));

    // The zones after the global zone of each instrument whose name ends in `suffix`.
    auto zones_of = [&](const std::string& suffix)
    {
        std::vector<size_t> zones;
        for (size_t i = 0; i + 1 < sf.Count("inst", 22); i++)
        {
            const std::string name(reinterpret_cast<const char*>(sf.Record("inst", 22, i)));
            if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                const size_t first = Le16(sf.Record("inst", 22, i) + 20) + 1;
                for (size_t z = first; z < Le16(sf.Record("inst", 22, i + 1) + 20); z++)
                {
                    zones.push_back(z);
                }
            }
        }

        return zones;
    };
    auto keeps_pan = [&](size_t zone)
    {
        bool found = false;
        for (size_t m = Le16(sf.Record("ibag", 4, zone) + 2); m < Le16(sf.Record("ibag", 4, zone + 1) + 2); m++)
        {
            const uint8_t* mod = sf.Record("imod", 10, m);
            found = found || (Le16(mod) == 0x028A && Le16(mod + 2) == sf2::kPan && Le16(mod + 4) == 0);
        }

        return found;
    };
    const std::vector<size_t> drums = zones_of(" instrument 3"), plain = zones_of(" instrument 1");
    const std::vector<size_t> wave = zones_of(" instrument 9");
    SEQLIB_REQUIRE_EQ(drums.size(), 3);
    SEQLIB_CHECK(std::all_of(drums.begin(), drums.end(), keeps_pan));

    // The sample drum's pan of 0x20 gives it 95/63 of the left level at the centre and 32/64 of the right level. Pan
    // -296 gives the sides that ratio under a sin/cos pan law. The square drum at the centre plays in the middle. The
    // one at 0x50 plays on the right alone.
    const auto sample_drum = sf.ZoneGens(drums[0]), right_drum = sf.ZoneGens(drums[2]);
    SEQLIB_CHECK(sample_drum.count(sf2::kPan) && int16_t(sample_drum.at(sf2::kPan)) == -296);
    SEQLIB_CHECK(!sf.ZoneGens(drums[1]).count(sf2::kPan));
    SEQLIB_CHECK(right_drum.count(sf2::kPan) && int16_t(right_drum.at(sf2::kPan)) == 500);
    SEQLIB_CHECK(plain.size() == 1 && !keeps_pan(plain[0]));
    SEQLIB_REQUIRE_EQ(wave.size(), 1);
    const auto gens = sf.ZoneGens(wave[0]);
    for (uint16_t op : {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv,
                        sf2::kSustainVolEnv, sf2::kReleaseVolEnv})
    {
        SEQLIB_CHECK(!gens.count(op));
    }
}

// Returns the values of controller `controller` on MIDI channel `channel`, with their ticks.
std::vector<std::pair<uint32_t, int>> ControllerValues(const MidiEvents& midi, uint8_t channel, uint8_t controller)
{
    std::vector<std::pair<uint32_t, int>> found;
    for (const auto& [tick, bytes] : midi.Find(uint8_t(0xB0 | channel)))
    {
        if (bytes[1] == controller)
        {
            found.emplace_back(uint32_t(tick), bytes[2]);
        }
    }

    return found;
}

// Returns the value that a list of ControllerValues() sets at `tick`, or -1 before the first.
int ValueAt(const std::vector<std::pair<uint32_t, int>>& values, uint32_t tick)
{
    int value = -1;
    for (const auto& [at, v] : values)
    {
        if (at <= tick)
        {
            value = v;
        }
    }

    return value;
}

// Returns CC11 for a note at full velocity on both sides of the PSG at volume `volume`, or on one side alone with
// `one_side`. Volume 15 plays at PsgShare() of a full-scale sample (TestPsgSampleLevels()).
int VolumeExpression(double volume, bool one_side)
{
    const double level = volume / PsgShare(Revision::kLinkToThePast);
    int cc10 = 64, cc11 = 0;
    LevelsToControllers(level, one_side ? 0 : level, cc10, cc11);

    return cc11;
}

// Returns the number of zones of the SoundFont's instruments after their global zones. Checks that each of them plays
// at a constant level, with no envelope and no attenuation.
size_t ConstantZones(const Sf2Records& sf)
{
    size_t zones = 0;
    for (size_t i = 0; i + 1 < sf.Count("inst", 22); i++)
    {
        for (size_t z = Le16(sf.Record("inst", 22, i) + 20) + 1; z < Le16(sf.Record("inst", 22, i + 1) + 20); z++)
        {
            const auto gens = sf.ZoneGens(z);
            for (uint16_t op : {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv,
                                sf2::kSustainVolEnv, sf2::kReleaseVolEnv})
            {
                SEQLIB_CHECK(!gens.count(op));
            }
            zones++;
        }
    }

    return zones;
}

void TestPsgPan()
{
    // The PSG plays a note on both sides at the centre pan, 0x40. At any other pan, the driver doubles the note's
    // level and plays it on one side alone. The volume register holds 15 at most. Track 0 plays the square at full
    // velocity at 0x20, 0x40 and 0x50, at volume 15 on each side it plays on. Its CC10 goes to 0, 64 and 127. Each note
    // starts with CC11 for volume 15 on its sides, and lasts until the next takes square 2. The square of CC11 sets the
    // level. A sin/cos player plays each side of a centred note at 1/√2 of the level. The square of CC11 on one side is
    // then 1/√2 of the square at the centre. Track 1's sample and track 2's square drum keep the rule of the sample
    // voices. Pan 0x20 gives them CC10 26. The drum comes after track 0's notes. Otherwise it would take square 2 from
    // them.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x20, 0xC2, kSquare})
                         .Note(0x30, 4, 127)
                         .Wait(8)
                         .Bytes({0xC3, 0x40})
                         .Note(0x30, 4, 127)
                         .Wait(8)
                         .Bytes({0xC3, 0x50})
                         .Note(0x30, 4, 127)
                         .Wait(8)
                         .End(),
                     TrackData().Bytes({0xC3, 0x20, 0xC2, kPlain}).Note(60, 4, 127).Wait(24).End(),
                     TrackData().Bytes({0xC3, 0x20, 0xC2, kDrums}).Wait(30).Note(37, 4, 127).Wait(4).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_pan"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<int> pans;
    for (const auto& value : ControllerValues(midi, 0, cc::kPan))
    {
        pans.push_back(value.second);
    }
    SEQLIB_CHECK((pans == std::vector<int>{0, 64, 127}));
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);
    std::vector<int> starts;
    std::vector<uint32_t> ons, offs;
    for (const auto& note_on : midi.Find(0x90))
    {
        starts.push_back(ValueAt(loudness, uint32_t(note_on.tick)));
        ons.push_back(uint32_t(note_on.tick));
    }
    for (const auto& note_off : midi.Find(0x80))
    {
        offs.push_back(uint32_t(note_off.tick));
    }
    SEQLIB_REQUIRE_EQ(starts.size(), 3);
    SEQLIB_CHECK((starts == std::vector<int>{VolumeExpression(15, true), VolumeExpression(15, false),
                                             VolumeExpression(15, true)}));
    const double ratio = std::pow(double(starts[0]) / starts[1], 2);
    SEQLIB_CHECK(std::fabs(ratio * std::sqrt(2.0) - 1) < 0.025);
    SEQLIB_REQUIRE_EQ(offs.size(), 3);
    SEQLIB_CHECK(offs[0] == ons[1] && offs[1] == ons[2]);
    SEQLIB_CHECK((ControllerValues(midi, 1, cc::kPan) == std::vector<std::pair<uint32_t, int>>{{0, 26}}));
    SEQLIB_CHECK((ControllerValues(midi, 2, cc::kPan) == std::vector<std::pair<uint32_t, int>>{{0, 26}}));
}

void TestPsgVolume()
{
    // A square's or the noise's note plays at full velocity, and its CC11 follows its channel's volume, frame by frame.
    // The square's region holds volume 15 until the release. The release writes the volume to NR22 with a period of 2,
    // and the noise's to NR42 with a period of 1. The PSG's envelope then steps the volume down to 0, a step every
    // 2/64 s or 1/64 s: 1 or 2 frames apart for the square, and 1 frame apart, or now and then 2 steps in a frame, for
    // the noise. Each note ends at the start of the frame whose volume is 0. Their zones play at a constant level, with
    // no envelope and no attenuation.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(40).End(),
                     TrackData().Bytes({0xC2, kNoise}).Note(0x30, 4, 127).Wait(40).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_volume"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    for (uint8_t channel : {uint8_t(0), uint8_t(1)})
    {
        const auto ons = midi.Find(uint8_t(0x90 | channel)), offs = midi.Find(uint8_t(0x80 | channel));
        SEQLIB_REQUIRE(ons.size() == 1 && offs.size() == 1);
        SEQLIB_CHECK_EQ(int(ons[0].bytes[2]), 127);

        // CC11 takes the volumes from 15 down to 0. Each of the square's lasts a frame or two. CC11 therefore takes
        // every one. A frame can take two of the noise's. The release comes 4 frames after the note on. At this tempo,
        // a frame lasts 150 ticks. The last 14 steps take 14 of the envelope's periods, to within a frame.
        const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, channel, cc::kExpression);
        std::vector<int> volumes;
        for (const auto& value : loudness)
        {
            int volume = 15;
            while (volume > 0 && VolumeExpression(volume, false) != value.second)
            {
                volume--;
            }
            volumes.push_back(volume);
        }
        SEQLIB_REQUIRE(volumes.size() >= 2);
        SEQLIB_CHECK(volumes.front() == 15 && volumes.back() == 0 && loudness.back().second == 0);
        for (size_t i = 1; i < volumes.size(); i++)
        {
            const int step = volumes[i - 1] - volumes[i];
            SEQLIB_CHECK(step == 1 || (channel == 1 && step == 2));
        }
        SEQLIB_CHECK_EQ(loudness[0].first, uint32_t(ons[0].tick));
        SEQLIB_CHECK(loudness[1].first >= ons[0].tick + 4 * 150);
        for (size_t i = 2; i < loudness.size(); i++)
        {
            SEQLIB_CHECK(loudness[i].first - loudness[i - 1].first <= 2 * 150);
        }
        const double period = (channel == 0 ? 2 : 1) * kFrameRate / 64;
        SEQLIB_CHECK(std::fabs((loudness.back().first - loudness[1].first) / 150.0 - 14 * period) <= 1);
        SEQLIB_CHECK_EQ(uint32_t(offs[0].tick), loudness.back().first);
    }

    // The square's and the noise's zones.
    SEQLIB_CHECK_EQ(ConstantZones(ReadSf2(sum.sf2_path)), size_t(2));
}

void TestSampleLegato()
{
    // A track in legato plays its notes on one voice. The voice keeps its envelope from note to note. The envelope
    // rises to its peak in 4 frames and falls to half of it over the next 8. Both notes play on a zone whose envelope
    // has only the release, in a preset of the instrument's notes in legato. The track's CC11 follows the voice's
    // envelope frame by frame. The second note starts in frame 6, where the envelope falls. The first note ends there.
    // C6 releases the voice in frame 12, and the second note ends there.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kLooped, 0xC5})
                         .Note(60, 6, 127)
                         .Wait(6)
                         .Note(64, 6, 127)
                         .Wait(6)
                         .Bytes({0xC6})
                         .Wait(40)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_legato"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x90), offs = midi.Find(0x80);
    SEQLIB_REQUIRE(ons.size() == 2 && offs.size() == 2);
    SEQLIB_CHECK_EQ(ons[1].tick, 6 * 150);
    SEQLIB_CHECK_EQ(offs[0].tick, ons[1].tick);
    SEQLIB_CHECK_EQ(offs[1].tick, 12 * 150);

    // CC11 starts at a quarter of the peak's, rises to the peak's, and falls from there. The second note doesn't start
    // it again. The release keeps the last value.
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);
    SEQLIB_REQUIRE(loudness.size() >= 4);
    int peak = 0;
    for (const auto& value : loudness)
    {
        peak = std::max(peak, value.second);
    }
    const int first = ValueAt(loudness, uint32_t(ons[0].tick));
    const int second = ValueAt(loudness, uint32_t(ons[1].tick));
    SEQLIB_CHECK(first > 0 && first < peak && second > first && second < peak);
    SEQLIB_CHECK(loudness.back().second > first && loudness.back().second < second);

    // The SoundFont holds the one preset. Its zones play at a constant level until the release.
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_REQUIRE_EQ(sf.Count("phdr", 38), 2);
    SEQLIB_CHECK(std::string(reinterpret_cast<const char*>(sf.Record("phdr", 38, 0))) == "Bank 0 legato 0");
    size_t zones = 0;
    for (size_t z = Le16(sf.Record("inst", 22, 0) + 20) + 1; z < Le16(sf.Record("inst", 22, 1) + 20); z++)
    {
        const auto gens = sf.ZoneGens(z);
        for (uint16_t op :
             {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv, sf2::kSustainVolEnv})
        {
            SEQLIB_CHECK(!gens.count(op));
        }
        SEQLIB_CHECK(gens.count(sf2::kReleaseVolEnv));
        zones++;
    }
    SEQLIB_CHECK(zones > 0);
}

void TestWaveVolume()
{
    // The wave voice's note plays at full velocity, and its CC11 follows the wave channel's level, frame by frame. At
    // the start of each segment of the voice's envelope, the driver turns the velocity times the volumes into one of
    // the channel's five levels (NR32): 0, 25, 50, 75 or 100%. Velocities 127, 100, 64 and 40 give 100, 75, 50 and
    // 25%. The release comes 6 frames after each note on, partway through the envelope's second segment. Each frame
    // from the release on takes (0x46 + 230) / 512 of the level, and NR32 steps down with it. The driver turns the
    // channel off once the level's step is 0. The note ends at the start of that frame. Its zone plays at a constant
    // level.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kWave})
                         .Note(48, 6, 127)
                         .Wait(12)
                         .Note(48, 6, 100)
                         .Wait(12)
                         .Note(48, 6, 64)
                         .Wait(12)
                         .Note(48, 6, 40)
                         .Wait(12)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_wave_volume"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x90), offs = midi.Find(0x80);
    SEQLIB_REQUIRE(ons.size() == 4 && offs.size() == 4);
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);

    // Each note's shares of the full level: the one it holds until its release, then a frame of each of the release's
    // steps, down to 0. At this tempo, a frame lasts 150 ticks.
    const std::vector<std::vector<double>> kShares = {
        {1, 0.5, 0.25, 0}, {0.75, 0.5, 0.25, 0}, {0.5, 0.25, 0}, {0.25, 0}};
    for (size_t i = 0; i < ons.size(); i++)
    {
        SEQLIB_CHECK_EQ(int(ons[i].bytes[2]), 127);
        std::vector<int> values, expected;
        for (const auto& [tick, value] : loudness)
        {
            if (tick >= ons[i].tick && tick <= offs[i].tick)
            {
                values.push_back(value);
            }
        }
        for (double share : kShares[i])
        {
            expected.push_back(VolumeExpression(15 * share, false));
        }
        SEQLIB_CHECK(values == expected);
        SEQLIB_CHECK_EQ(offs[i].tick - ons[i].tick, uint64_t(150 * (6 + kShares[i].size() - 2)));
    }
    SEQLIB_CHECK_EQ(ConstantZones(ReadSf2(sum.sf2_path)), size_t(1));
}

void TestPsgRunOut()
{
    // A sequence without a loop ends with its last track, but the PSG can still sound then. The square's track ends 2
    // frames after the square's release. The release's envelope steps the volume down from 15 to 0, a step every
    // 2/64 s (TestPsgVolume()), and runs on for about 26 frames after the end. The wave's track ends a frame after the
    // wave's release, with a frame of the release's fade left: 50% in the release's frame, then 25% (TestWaveVolume()).
    // The model runs on until both channels fall silent. Each note's CC11 follows its channel's volume down to 0, and
    // the MIDI file ends where the square falls silent.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kWave}).Note(48, 6, 127).Wait(7).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_run_out"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<uint32_t> ends;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes[1] == 0x2F)
        {
            ends.push_back(uint32_t(tick));
        }
    }
    SEQLIB_REQUIRE(!ends.empty());
    const uint32_t end = *std::max_element(ends.begin(), ends.end());

    // The square's CC11 takes every volume from 15 down to 1. Volume 1 lasts a frame or two. The note then ends at the
    // start of the frame whose volume is 0. The file ends there too. At this tempo, a frame lasts 150 ticks.
    const auto square_ons = midi.Find(0x90), square_offs = midi.Find(0x80);
    SEQLIB_REQUIRE(square_ons.size() == 1 && square_offs.size() == 1);
    const std::vector<std::pair<uint32_t, int>> square = ControllerValues(midi, 0, cc::kExpression);
    std::vector<int> values, expected;
    for (const auto& [tick, value] : square)
    {
        values.push_back(value);
    }
    for (int volume = 15; volume >= 1; volume--)
    {
        expected.push_back(VolumeExpression(volume, false));
    }
    SEQLIB_CHECK(values == expected);
    SEQLIB_REQUIRE(!square.empty());
    const uint32_t last_step = square_offs[0].tick - square.back().first;
    SEQLIB_CHECK(last_step == 150 || last_step == 300);
    SEQLIB_CHECK(square_offs[0].tick > 6 * 150 + 20 * 150);
    SEQLIB_CHECK_EQ(uint32_t(square_offs[0].tick), end);

    // The wave's CC11 takes the release's two steps. The wave's track ends in frame 7. The note ends 8 frames after
    // the note on.
    const auto wave_ons = midi.Find(0x91), wave_offs = midi.Find(0x81);
    SEQLIB_REQUIRE(wave_ons.size() == 1 && wave_offs.size() == 1);
    values.clear();
    for (const auto& [tick, value] : ControllerValues(midi, 1, cc::kExpression))
    {
        values.push_back(value);
    }
    expected.clear();
    for (double share : {1.0, 0.5, 0.25, 0.0})
    {
        expected.push_back(VolumeExpression(15 * share, false));
    }
    SEQLIB_CHECK(values == expected);
    SEQLIB_CHECK_EQ(wave_offs[0].tick - wave_ons[0].tick, uint64_t(8 * 150));
}

void TestPsgPanPasses()
{
    // The sequence loops back to its start. The first pass plays the square at 0x20 and then at 0x10. The second pass
    // keeps 0x10. The PSG plays both passes on the left alone at the same level. The passes then repeat each other. The
    // MIDI file's loop therefore starts with the first pass. A sample voice would play the first note of each pass at
    // another level.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x20, 0xC2, kSquare})
                         .Note(0x30, 6, 127)
                         .Wait(6)
                         .Bytes({0xC3, 0x10})
                         .Note(0x30, 6, 127)
                         .Wait(6)
                         .Bytes({0xF0, 8, 0})
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_pan_passes"), nullptr);

    SEQLIB_CHECK(sum.ok);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& [tick, bytes] : ReadMidi(sum.midi_path).Find(0xFF))
    {
        if (bytes[1] == 0x06)
        {
            markers.emplace_back(uint32_t(tick), std::string(bytes.begin() + 3, bytes.end()));
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{0, "loopStart"}, {1800, "loopEnd"}};
    SEQLIB_CHECK(markers == kMarkers);
}

void TestZoneAttenuation()
{
    // An envelope that rises at once to half the full level, 0x4000, and holds it plays 6 dB down: 60 cB. The zone
    // holds 2.5 times that in initialAttenuation: 150 cB. Synthesizers that apply 0.4 of the generator then play 60 cB.
    // A silent envelope gives the generator's limit of 1440 cB. Those synthesizers play it as 576 cB. A modulator
    // without a source adds the other 864 cB.
    test::CartImage image(0x20);
    image.PutBytes(kRomBase, {1, 0, 0x00, 0x40});
    image.PutBytes(kRomBase + 0x10, {1, 0, 0, 0});
    const Rom rom = image.ToRom();
    const auto attenuation = [](const sf2::Zone& zone)
    {
        int value = 0;
        for (const sf2::Generator& g : zone.generators)
        {
            value = g.oper == sf2::kInitialAttenuation ? int(g.amount) : value;
        }

        return value;
    };

    sf2::Zone half;
    AddEnvelope(half, EnvelopeFor(rom, kRomBase, kInstant));
    SEQLIB_CHECK_EQ(attenuation(half), 150);
    SEQLIB_CHECK(half.modulators.empty());

    sf2::Zone silent;
    AddEnvelope(silent, EnvelopeFor(rom, kRomBase + 0x10, kInstant));
    SEQLIB_CHECK_EQ(attenuation(silent), 1440);
    SEQLIB_REQUIRE_EQ(silent.modulators.size(), 1);
    const sf2::Modulator& rest = silent.modulators[0];
    SEQLIB_CHECK(rest.source == sf2::kNoController && rest.dest == sf2::kInitialAttenuation && rest.amount == 864 &&
                 rest.amount_source == sf2::kNoController && rest.transform == sf2::kLinear);
}

void TestLevelOrder()
{
    // At a tempo of 300, a frame lasts two ticks. Track 0 sets its pan at tick 4, and then, in the same frame, track 1
    // sets the player's volume to 0 at tick 3. Track 0 plays with both from that frame on. So the MIDI file gives them
    // both at tick 4, and track 0's last CC11 silences it.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 0x81, 0x2C, 0xC2, kPlain})
                         .Note(60, 20, 127)
                         .Wait(4)
                         .Bytes({0xC3, 0x10})
                         .Wait(16)
                         .End(),
                     TrackData().Wait(3).Bytes({0xEA, 0}).Wait(17).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_level_order");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    std::vector<std::pair<uint32_t, int>> expression;
    for (const auto& e : ReadMidi(sum.midi_path).Find(0xB0))
    {
        if (e.bytes[1] == 11)
        {
            expression.emplace_back(uint32_t(e.tick), e.bytes[2]);
        }
    }
    SEQLIB_CHECK(!expression.empty() && expression.back() == std::make_pair(600u, 0));
}

void TestHighVelocities()
{
    // The driver plays a sample note's level in proportion to its velocity, up to 255. Track 0's note at 255 plays at
    // velocity 127. Its note at 127 plays at 127 × √(127 / 255). The track's CC11 raises its levels by 255 / 127 to
    // make up for it. CC11 sets the level by its square. Track 1 plays at velocity 127 and at its levels. Track 2's
    // volume of 0xFF leaves CC11 too little room. Its CC11 reaches 127. Its note at 100 then plays between its velocity
    // at no scale and at the full one.
    Cart cart;
    cart.Sequences(
        {{TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 255).Wait(6).Note(62, 6, 127).Wait(6).End(),
          TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(12).End(),
          TrackData().Bytes({0xE0, 0xFF, 0xC2, kPlain}).Note(67, 6, 255).Wait(6).Note(69, 6, 100).Wait(6).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_high_velocities"), nullptr);

    SEQLIB_REQUIRE(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto velocities = [&midi](uint8_t channel)
    {
        std::vector<int> found;
        for (const auto& e : midi.Find(uint8_t(0x90 | channel)))
        {
            if (e.bytes[2] > 0)
            {
                found.push_back(e.bytes[2]);
            }
        }
        return found;
    };
    SEQLIB_CHECK(velocities(0) == std::vector<int>({127, int(std::lround(127 * std::sqrt(127 / 255.0)))}));
    SEQLIB_CHECK(velocities(1) == std::vector<int>({127}));
    const auto scaled = ControllerValues(midi, 0, cc::kExpression), plain = ControllerValues(midi, 1, cc::kExpression);
    const auto capped = ControllerValues(midi, 2, cc::kExpression);
    SEQLIB_REQUIRE(!scaled.empty() && !plain.empty() && !capped.empty());
    SEQLIB_CHECK(std::abs(scaled[0].second - plain[0].second * std::sqrt(255 / 127.0)) <= 1);
    SEQLIB_CHECK_EQ(capped[0].second, 127);
    const std::vector<int> loud = velocities(2);
    SEQLIB_REQUIRE_EQ(loud.size(), 2);
    SEQLIB_CHECK_EQ(loud[0], 127);
    SEQLIB_CHECK(loud[1] > std::lround(127 * std::sqrt(100 / 255.0)) &&
                 loud[1] < std::lround(127 * std::sqrt(100 / 127.0)));
}

void TestLoopLengths()
{
    // Two tracks loop 12 and 18 ticks from their starts, after the sequence's header of 8 bytes, and a third, without a
    // loop, plays a note of 9 ticks and ends after 24. The loop starts at tick 9, where that note ends, and lasts until
    // both loops are back where they started, 36 ticks, and the sequence plays it twice. A tick is 150 of the MIDI
    // file's.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xE4, 120, 0xC2, kLooped}).Note(60, 6, 127).Wait(12).Bytes({0xF0, 8, 0}).End(),
                     TrackData().Bytes({0xC2, kLooped}).Note(64, 6, 100).Wait(18).Bytes({0xF0, 21, 0}).End(),
                     TrackData().Bytes({0xC2, kLooped}).Note(67, 9, 100).Wait(24).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : midi.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{1350, "loopStart"}, {6750, "loopEnd"}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_CHECK(sum.loop_end > 0 && std::fabs(sum.seconds / sum.loop_end - 12150.0 / 6750) < 0.01);
}

// A track sets volume 200 and plays a note, and then loops: volume 200 and a note, and volume 100, pan 100 and a note.
// The game keeps pan 100 when the track jumps back, so the loop's first note plays in the middle the first time through
// and at pan 100 after that. The marked loop therefore starts at the second pass, at the MIDI file's tick 2700, and a
// player that jumps back there from the loop's end has the loudness and pan that the file's next pass starts with.
void TestLoopSettingsAgain()
{
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 120, 0xC2, kLooped, 0xE0, 200})
                         .Note(60, 6, 127)
                         .Wait(6)
                         .Bytes({0xE0, 200})
                         .Note(64, 6, 127)
                         .Wait(6)
                         .Bytes({0xE0, 100, 0xC3, 100})
                         .Note(67, 6, 127)
                         .Wait(6)
                         .Bytes({0xF0, 15, 0})
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_again");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    const test::MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes[1] == 0x06)
        {
            markers.emplace_back(uint32_t(tick), std::string(bytes.begin() + 3, bytes.end()));
        }
    }

    // Returns a controller's value after the events up to and including `tick`.
    const auto value_at = [&](int controller, uint32_t tick)
    {
        int value = -1;
        for (const auto& [t, bytes] : midi.Find(0xB0))
        {
            if (t <= tick && bytes[1] == controller)
            {
                value = bytes[2];
            }
        }

        return value;
    };

    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{2700, "loopStart"}, {4500, "loopEnd"}};
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_CHECK(value_at(cc::kPan, 900) == 64 && value_at(cc::kPan, 2700) > 64);
    SEQLIB_CHECK_EQ(value_at(cc::kPan, 2700), value_at(cc::kPan, 4500));
    SEQLIB_CHECK_EQ(value_at(cc::kExpression, 2700), value_at(cc::kExpression, 4500));
}

// The listing follows each track in the sequence's header, and then each track that F8 starts.
void TestDump()
{
    // The track starts track 3 at the note after its end. The track's data starts at offset 4, after the sequence's
    // header and the track's offset. F8 takes 4 bytes.
    const std::vector<uint8_t> first =
        TrackData().Bytes({0xE4, 120, 0xC2, kPlain, 0xC3, 0x20}).Note(60, 6, 127).Wait(6).End();
    const std::vector<uint8_t> started = TrackData().Note(62, 6, 127).Wait(6).End();
    const uint16_t offset = uint16_t(4 + 4 + first.size());
    std::vector<uint8_t> track = {0xF8, 3, uint8_t(offset), uint8_t(offset >> 8)};
    track.insert(track.end(), first.begin(), first.end());
    track.insert(track.end(), started.begin(), started.end());
    Cart cart;
    cart.Sequences({{track}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    std::string error;
    const std::string path = Utf8(TempPath("rd2_dump.txt"));

    const bool ok = DumpSong(rom, info, 0, path, error);

    const std::vector<uint8_t> data = ReadAll(path);
    const std::string text(data.begin(), data.end());
    SEQLIB_CHECK(ok);
    SEQLIB_CHECK(text.find("tempo 120") != std::string::npos);
    SEQLIB_CHECK(text.find("pan 32") != std::string::npos);
    SEQLIB_CHECK(text.find("note 60 (C4), length 6, velocity 127") != std::string::npos);
    SEQLIB_CHECK(text.find("      6  FF") != std::string::npos);
    SEQLIB_CHECK(text.find("start track 3 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find("; ---- track 3 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find(", started by F8 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find("note 62 (D4), length 6, velocity 127") != std::string::npos);
}

void TestEffects()
{
    // Two sequences of sound effects. The first has three tracks. The second of them is empty. The second sequence has
    // one track. The first sequence's table of three tracks takes 6 bytes. Its first track starts at offset 6. The
    // third track starts another track with F8. That track comes after the third track's end. The third track's C4 and
    // C2 take 2 bytes each, its F8 4, its note 3 and its end 1.
    const std::vector<uint8_t> first = TrackData().Bytes({0xC2, kPlain}).Note(60, 3, 127).Note(64, 2, 127).End();
    const uint16_t other = uint16_t(6 + first.size() + 12);
    std::vector<uint8_t> third =
        TrackData().Bytes({0xC4, 5, 0xC2, kPlain, 0xF8, 1, uint8_t(other), uint8_t(other >> 8)}).Note(67, 2, 127).End();
    const std::vector<uint8_t> started = TrackData().Note(69, 2, 127).Note(71, 2, 127).End();
    third.insert(third.end(), started.begin(), started.end());
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    cart.Effects({{first, {}, third}, {TrackData().Bytes({0xC2, kPlain}).Note(72, 1, 127).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // Songs 1 to 4 are the sound effects. Song 0 is the sequence.
    SEQLIB_CHECK_EQ(SongCount(info), 5);
    SEQLIB_CHECK(SongEffect(info, 0) == nullptr && SongEffect(info, 5) == nullptr);
    SEQLIB_REQUIRE(SongEffect(info, 1) != nullptr && SongEffect(info, 2) != nullptr && SongEffect(info, 4) != nullptr);
    SEQLIB_CHECK_EQ(SongEffect(info, 1)->address, kEffectData + 6);
    SEQLIB_CHECK_EQ(SongEffect(info, 2)->address, 0);
    SEQLIB_CHECK_EQ(SongEffect(info, 4)->sequence, 1);
    SEQLIB_CHECK_EQ(SongEffect(info, 4)->track, 0);
    SEQLIB_CHECK(std::any_of(info.log.begin(), info.log.end(), [](const std::string& line)
                             { return line.find("(2 sequences, 4 effects: songs 1-4)") != std::string::npos; }));
    SEQLIB_CHECK(!Sequencer(rom, info, 2).Valid());

    // The first track plays at a priority of 12 with an echo send of 7F. Its second note waits for the first.
    Sequencer effect(rom, info, 1);
    effect.Step();
    const int slot = effect.GetPlayer().tracks[0];
    SEQLIB_REQUIRE(slot >= 0);
    SEQLIB_CHECK(effect.GetPlayer().effect);
    SEQLIB_CHECK_EQ(effect.Tracks()[size_t(slot)].priority, kEffectPriority);
    SEQLIB_CHECK_EQ(effect.Tracks()[size_t(slot)].echo, kEffectEcho);
    SEQLIB_CHECK_EQ(effect.Voices()[0].priority, kEffectPriority);
    const auto frames = Run(rom, info, 1, 8);
    std::vector<int> ons;
    for (size_t f = 0; f < frames.size(); f++)
    {
        for (const Event& e : frames[f])
        {
            ons.push_back(e.kind == Event::kNote ? int(f) : -1);
        }
    }
    ons.erase(std::remove(ons.begin(), ons.end(), -1), ons.end());
    SEQLIB_REQUIRE_EQ(ons.size(), 2);
    SEQLIB_CHECK_EQ(ons[0], 0);
    SEQLIB_CHECK_EQ(ons[1], 3);

    // F8 starts a track at the sound effect's defaults. Then it copies the starter's priority. The new track's notes
    // wait for their length too.
    Sequencer starter(rom, info, 3);
    starter.Step();
    const int second = starter.GetPlayer().tracks[1];
    SEQLIB_REQUIRE(second >= 0);
    SEQLIB_CHECK_EQ(starter.Tracks()[size_t(second)].priority, 5);
    SEQLIB_CHECK_EQ(starter.Tracks()[size_t(second)].echo, kEffectEcho);
    SEQLIB_CHECK_EQ(starter.Tracks()[size_t(second)].timed_notes, 1);

    // The Music interface lists the sound effects as songs. The empty entry has none.
    std::string error;
    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK_EQ(music->SongCount(), 5);
    SEQLIB_CHECK(music->HasSong(1) && !music->HasSong(2) && music->HasSong(3));
    SEQLIB_CHECK_EQ(music->InspectSong(1, ConvertSettings()).address, kEffectData + 6);

    // The conversion names the sound effect. The listing gives the times of its notes.
    const SongReport sum = ConvertSong(rom, info, 1, TempSettings("rd2_effect"), nullptr);
    SEQLIB_CHECK(sum.ok);
    const std::vector<uint8_t> midi = ReadAll(sum.midi_path);
    SEQLIB_CHECK(std::string(midi.begin(), midi.end()).find("track 0 of the effects' sequence 0") != std::string::npos);
    const std::string path = Utf8(TempPath("rd2_effect.txt"));
    SEQLIB_CHECK(DumpSong(rom, info, 3, path, error));
    const std::vector<uint8_t> data = ReadAll(path);
    const std::string text(data.begin(), data.end());
    SEQLIB_CHECK(text.find("sound effect 3: track 2 of the effects' sequence 0") != std::string::npos);
    SEQLIB_CHECK(text.find(", started by F8 at 0x") != std::string::npos);
    SEQLIB_CHECK(text.find("      2  A7 02 7F") != std::string::npos);

    // The track that F8 starts ends after the track that starts it. The sound effect lasts until the later end. The
    // track that F8 starts plays on MIDI channel 1.
    const SongReport layered = ConvertSong(rom, info, 3, TempSettings("rd2_effect_f8"), nullptr);
    SEQLIB_REQUIRE(layered.ok);
    std::vector<int> keys;
    for (const auto& e : ReadMidi(layered.midi_path).Find(0x91))
    {
        keys.push_back(e.bytes[1]);
    }
    SEQLIB_CHECK(keys == std::vector<int>({69, 71}));
}

void TestMusic()
{
    // The Music interface finds the driver, names it, and reports a sequence's tracks and address.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End()}});
    const Rom rom = cart.ToRom();
    std::string error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);

    SEQLIB_REQUIRE(music != nullptr);
    SEQLIB_CHECK(music->Log()[0] == "Nintendo R&D2's sound driver");
    SEQLIB_CHECK_EQ(music->SongCount(), 1);

    const SongReport report = music->InspectSong(0, ConvertSettings());

    SEQLIB_CHECK(report.ok && report.tracks == 1);
    SEQLIB_CHECK_EQ(report.address, kSequenceData);
}

} // namespace

void RunTests()
{
    TestDetection();
    TestSuperMarioAdvance2();
    TestTiming();
    TestVoices();
    TestMusicSettings();
    TestMusicSettingsWithCount();
    TestMutedTracks();
    TestYoshiTracks();
    TestEnvelope();
    TestPsg();
    TestInstruments();
    TestConversion();
    TestPsgTables();
    TestPsgSampleLevels();
    TestDrumPanAndWave();
    TestPsgPan();
    TestPsgVolume();
    TestSampleLegato();
    TestWaveVolume();
    TestPsgRunOut();
    TestPsgPanPasses();
    TestZoneAttenuation();
    TestLevelOrder();
    TestHighVelocities();
    TestLoopLengths();
    TestLoopSettingsAgain();
    TestDump();
    TestEffects();
    TestMusic();
}

} // namespace agbsf2::rd2
