// SPDX-License-Identifier: MIT

// Unit tests for MP2K. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"
#include "xsflib/bytes.h"

#include "files.h"
#include "gba.h"
#include "midi.h"
#include "mp2k/convert.h"
#include "mp2k/driver.h"
#include "mp2k/sequencer.h"
#include "mp2k/song.h"
#include "mp2k/soundfont.h"
#include "music.h"
#include "rom.h"
#include "sf2.h"
#include "song_banks.h"
#include "test_util.h"

namespace agbsf2::mp2k
{
namespace
{

using test::HasGlobalZoneModulators;
using test::Le16;
using test::MidiEvents;
using test::ReadAll;
using test::ReadMidi;
using test::ReadSf2;
using test::Sf2Records;
using test::TempPath;
using test::TempSettings;

// Addresses of the synthetic driver's parts in a test cartridge.
constexpr uint32_t kInit = kRomBase + 0x100;          // the init's setting of the driver's mode
constexpr uint32_t kInitPool = kRomBase + 0x120;      // its literal pool
constexpr uint32_t kSongStart = kRomBase + 0x200;     // the routine that starts a song
constexpr uint32_t kSongStartPool = kRomBase + 0x228; // its literal pool
constexpr uint32_t kSpecialTest = kRomBase + 0x300;   // the mixer's test for reversed and compressed samples
constexpr uint32_t kMonoVolume = kRomBase + 0x340;    // the mono mixer's volume code, where a test puts it
constexpr uint32_t kPlayerTable = kRomBase + 0x400;

// Addresses of Camelot's changes in a test cartridge that has them.
constexpr uint32_t kUnusedSongStart = kRomBase + 0x180; // a copy of the routine that starts a song, its pool zeroed
constexpr uint32_t kCamelotSearch = kRomBase + 0x360;   // the note start's search for a channel
constexpr uint32_t kCamelotPulse = kRomBase + 0x380;    // the mixer's pulse wave
constexpr uint32_t kSongTable = kRomBase + 0x500;
constexpr uint32_t kData = kRomBase + 0x1000; // storage for songs, voices and samples

// The driver's mode that the test cartridges' init sets: 13379 Hz, 5 DirectSound channels, master volume 12.
constexpr uint32_t kMode = 0x0094C500;

// The MIDI file's ticks for each of the driver's, without frame timing: 960 to the quarter note.
constexpr uint32_t kMidiTicks = 40;

// A voice type: a DirectSound sample that plays at the mixer's rate.
constexpr uint8_t kFixed = kVoiceFixed;

// The routine that starts a song, with its call to MPlayStart, whose literal pool is 0x28 bytes after its start.
constexpr uint16_t kSongStartCode[18] = {0xB500, 0x0400, 0x4A08, 0x4909, 0x0B40, 0x1840, 0x8883, 0x0059, 0x18C9,
                                         0x0089, 0x1889, 0x680A, 0x6801, 0x1C10, 0xF000, 0xF800, 0xBC01, 0x4700};

// Returns the index of `ticks` in the driver's table of note lengths and waits, or -1 if it isn't one of them.
int LengthIndex(int ticks)
{
    for (int i = 0; i < 49; i++)
    {
        if (ClockLength(i) == ticks)
        {
            return i;
        }
    }

    return -1;
}

// Appends `v` to `bytes` in little-endian byte order.
void AppendLe32(std::vector<uint8_t>& bytes, uint32_t v)
{
    bytes.resize(bytes.size() + 4);
    xsflib::StoreLe32(&bytes[bytes.size() - 4], v);
}

// A track's commands, with jumps to labels in the same track, which Bytes() turns into addresses.
class Track
{
public:
    // Returns the position of the next command, for jumps to it.
    size_t Here() const
    {
        return bytes_.size();
    }

    // Adds waits that add up to `ticks`, each the longest of the driver's lengths that fits.
    Track& Wait(int ticks)
    {
        while (ticks > 0)
        {
            int step = std::min(ticks, 96);
            int index = LengthIndex(step);
            while (index < 0)
            {
                index = LengthIndex(--step);
            }

            bytes_.push_back(uint8_t(kCmdWait + index));
            ticks -= ClockLength(index);
        }

        return *this;
    }

    // Adds a note of `ticks`, which has to be one of the driver's lengths, with the arguments that aren't -1.
    Track& Note(int ticks, int key = -1, int velocity = -1, int extra = -1)
    {
        const int index = LengthIndex(ticks);
        SEQLIB_CHECK(index > 0);
        bytes_.push_back(uint8_t(kCmdTie + std::max(index, 1)));
        for (int arg : {key, velocity, extra})
        {
            if (arg >= 0)
            {
                bytes_.push_back(uint8_t(arg));
            }
        }

        return *this;
    }

    // The commands below add their bytes one at a time, since GCC wrongly warns about a null memmove when a short list
    // is inserted at -O3.
    Track& Tie(int key, int velocity)
    {
        bytes_.push_back(kCmdTie);
        bytes_.push_back(uint8_t(key));
        bytes_.push_back(uint8_t(velocity));

        return *this;
    }

    Track& Eot(int key)
    {
        bytes_.push_back(kCmdEot);
        bytes_.push_back(uint8_t(key));

        return *this;
    }

    // Adds a command with one argument.
    Track& Cmd(uint8_t command, int arg)
    {
        bytes_.push_back(command);
        bytes_.push_back(uint8_t(arg));

        return *this;
    }

    Track& Raw(std::vector<uint8_t> bytes)
    {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());

        return *this;
    }

    Track& Goto(size_t label)
    {
        return Jump(kCmdGoto, {}, label);
    }

    Track& Patt(size_t label)
    {
        return Jump(kCmdPatt, {}, label);
    }

    Track& Rept(int count, size_t label)
    {
        return Jump(kCmdRept, {uint8_t(count)}, label);
    }

    Track& Fine()
    {
        bytes_.push_back(kCmdFine);

        return *this;
    }

    // Returns the track's bytes for placing them at `at`.
    std::vector<uint8_t> Bytes(uint32_t at) const
    {
        std::vector<uint8_t> out = bytes_;
        for (const auto& [where, label] : fixups_)
        {
            xsflib::StoreLe32(&out[where], at + uint32_t(label));
        }

        return out;
    }

private:
    Track& Jump(uint8_t command, std::vector<uint8_t> args, size_t label)
    {
        bytes_.push_back(command);
        bytes_.insert(bytes_.end(), args.begin(), args.end());
        fixups_.push_back({bytes_.size(), label});
        bytes_.insert(bytes_.end(), 4, 0);

        return *this;
    }

    std::vector<uint8_t> bytes_;
    std::vector<std::pair<size_t, size_t>> fixups_;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, and songs, voices
// and samples placed one after another.
class Cart : public test::CartImage
{
public:
    // Makes a cartridge whose init gives the driver `mode`, with music players of the given track counts. Without
    // `code`, it has neither the init nor the routine that starts a song, and without `special`, its mixer doesn't play
    // reversed or compressed samples.
    explicit Cart(uint32_t mode = kMode, std::vector<int> players = {10, 3}, bool code = true, bool special = true)
        : CartImage(0x40000), next_(kData)
    {
        for (size_t p = 0; p < players.size(); p++)
        {
            const uint32_t at = kPlayerTable + 12 * uint32_t(p);
            Put32(at, 0x03007420 + 0x40 * uint32_t(p));
            Put32(at + 4, 0x03001340 + 0x500 * uint32_t(p));
            Put8(at + 8, uint8_t(players[p]));
        }
        if (code)
        {
            WriteDriver(mode, special);
        }
    }

    // Places `bytes` at the next free address, aligned to `align`. Returns the address.
    uint32_t Place(const std::vector<uint8_t>& bytes, uint32_t align = 4)
    {
        next_ = (next_ + align - 1) & ~(align - 1);
        const uint32_t at = next_;
        PutBytes(at, bytes);
        next_ += uint32_t(bytes.size());

        return at;
    }

    // Places a sample that plays at `rate` for key 60, looping from `loop_start` to its end if that isn't -1.
    uint32_t Wave(const std::vector<int8_t>& pcm, uint32_t rate = 13379, int loop_start = -1)
    {
        std::vector<uint8_t> bytes;
        AppendLe32(bytes, loop_start >= 0 ? 0x40000000u : 0);
        AppendLe32(bytes, rate * 1024);
        AppendLe32(bytes, loop_start >= 0 ? uint32_t(loop_start) : 0);
        AppendLe32(bytes, uint32_t(pcm.size()));
        bytes.insert(bytes.end(), pcm.begin(), pcm.end());

        return Place(bytes);
    }

    // Places a synth voice's sample for Camelot's mixer: a looping sample of no length, whose data is 0x80, the type,
    // and `params`. It plays at 13379 Hz for key 60.
    uint32_t Synth(uint8_t type, const std::vector<uint8_t>& params)
    {
        std::vector<uint8_t> bytes = {0, 0, 0, 0x40};
        AppendLe32(bytes, 13379 * 1024);
        bytes.resize(16, 0);
        bytes.push_back(0x80);
        bytes.push_back(type);
        bytes.insert(bytes.end(), params.begin(), params.end());

        return Place(bytes);
    }

    // Writes Camelot's changes: an unused copy of the routine that starts a song before the one the game uses, with its
    // literal pool zeroed, the note start's search for a channel, and the mixer's pulse wave.
    void WriteCamelot()
    {
        for (uint32_t i = 0; i < 18; i++)
        {
            Put16(kUnusedSongStart + 2 * i, kSongStartCode[i]);
        }

        const uint8_t kSearch[16] = {0x0A, 0x23, 0x50, 0x34, 0xC7, 0x27, 0x00, 0x22,
                                     0x00, 0x26, 0x03, 0x20, 0x21, 0x78, 0x39, 0x42};
        const uint8_t kPulse[20] = {0x02, 0x60, 0xD3, 0xE5, 0x06, 0x2C, 0x82, 0xE0, 0x04, 0x60,
                                    0xD3, 0xE5, 0x06, 0x6C, 0x92, 0xE0, 0x06, 0x60, 0xE0, 0x41};
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(kCamelotSearch + i, kSearch[i]);
        }
        for (uint32_t i = 0; i < 20; i++)
        {
            Put8(kCamelotPulse + i, kPulse[i]);
        }
    }

    // Places a compressed sample of `points` points from its blocks of 33 bytes, which plays at 13379 Hz for key 60.
    uint32_t CompressedWave(const std::vector<uint8_t>& blocks, uint32_t points)
    {
        std::vector<uint8_t> bytes(16, 0);
        bytes[0] = 1;
        xsflib::StoreLe32(&bytes[4], 13379 * 1024);
        xsflib::StoreLe32(&bytes[12], points);
        bytes.insert(bytes.end(), blocks.begin(), blocks.end());

        return Place(bytes);
    }

    // Returns a voice's 12 bytes.
    static std::vector<uint8_t> Voice(uint8_t type, uint32_t wave, uint8_t key = 60,
                                      std::vector<uint8_t> envelope = {255, 0, 255, 0}, uint8_t pan_sweep = 0)
    {
        return {type,
                key,
                0,
                pan_sweep,
                uint8_t(wave),
                uint8_t(wave >> 8),
                uint8_t(wave >> 16),
                uint8_t(wave >> 24),
                envelope[0],
                envelope[1],
                envelope[2],
                envelope[3]};
    }

    // Places a voice group of 128 voices, with the given ones and the rest zeroed. Returns its address.
    uint32_t VoiceGroup(const std::map<int, std::vector<uint8_t>>& voices)
    {
        std::vector<uint8_t> bytes(128 * 12, 0);
        for (const auto& [index, voice] : voices)
        {
            std::copy(voice.begin(), voice.end(), bytes.begin() + 12 * index);
        }

        return Place(bytes);
    }

    // Returns a key split's voice: keys up to `split` play voice 0 of `table`, and the others voice 1.
    std::vector<uint8_t> KeySplit(uint32_t table, int split)
    {
        std::vector<uint8_t> keys(128, 1);
        std::fill(keys.begin(), keys.begin() + split + 1, uint8_t(0));
        const uint32_t key_table = Place(keys, 1);
        std::vector<uint8_t> voice = Voice(kVoiceKeySplit, table);
        xsflib::StoreLe32(&voice[8], key_table);

        return voice;
    }

    // Places a song with these tracks, and adds it to the song table. Returns its header's address.
    uint32_t Song(const std::vector<Track>& tracks, uint32_t voices, int player = 0, uint8_t priority = 0,
                  uint8_t reverb = 0)
    {
        // Place the tracks, and list the header's words: the voice group, then each track's address.
        std::vector<uint32_t> words = {voices};
        for (const Track& t : tracks)
        {
            next_ = (next_ + 3) & ~3u;
            words.push_back(Place(t.Bytes(next_), 1));
        }

        std::vector<uint8_t> header = {uint8_t(tracks.size()), 0, priority, reverb};
        for (uint32_t v : words)
        {
            AppendLe32(header, v);
        }

        const uint32_t header_at = Place(header);
        Put32(kSongTable + 8 * songs_, header_at);
        Put16(kSongTable + 8 * songs_ + 4, uint16_t(player));
        Put16(kSongTable + 8 * songs_ + 6, uint16_t(player));
        songs_++;

        return header_at;
    }

private:
    // Writes the parts of the driver that detection reads: the init's setting of the mode, the routine that starts a
    // song with its literal pool, and the mixer's test for reversed and compressed samples.
    void WriteDriver(uint32_t mode, bool special)
    {
        // ldr r0, =mode; bl m4aSoundMode, with the music player table in the same literal pool.
        Put16(kInit, uint16_t(0x4800 | ((kInitPool - ((kInit + 4) & ~3u)) / 4)));
        Put16(kInit + 2, 0xF000);
        Put16(kInit + 4, 0xF800);
        Put32(kInitPool, mode);
        Put32(kInitPool + 4, kPlayerTable);

        for (uint32_t i = 0; i < 18; i++)
        {
            Put16(kSongStart + 2 * i, kSongStartCode[i]);
        }
        Put32(kSongStartPool, kPlayerTable);
        Put32(kSongStartPool + 4, kSongTable);

        if (special)
        {
            const uint8_t kTest[] = {0x01, 0x00, 0xD4, 0xE5, 0x30, 0x00, 0x10, 0xE3};
            for (uint32_t i = 0; i < 8; i++)
            {
                Put8(kSpecialTest + i, kTest[i]);
            }
        }
    }

    uint32_t next_;
    uint32_t songs_ = 0;
};

// Returns a sample of a saw wave, `length` points long.
std::vector<int8_t> Saw(size_t length = 64)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 4 % 256) - 128));
    }

    return pcm;
}

// Returns the driver info of a cartridge, failing the test if detection fails.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

// Returns the bank and program that each program change on `channel` selects: the bank of the last bank select before
// it in its track.
std::vector<std::pair<int, int>> Selections(const MidiEvents& m, int channel)
{
    std::vector<std::pair<int, int>> out;
    for (const auto& t : m.tracks)
    {
        int bank = 0;
        for (const auto& e : t.events)
        {
            if (e.bytes[0] == 0xB0 + channel && e.bytes[1] == cc::kBankSelect)
            {
                bank = e.bytes[2];
            }
            else if (e.bytes[0] == 0xC0 + channel)
            {
                out.push_back({bank, e.bytes[1]});
            }
        }
    }

    return out;
}

// Returns the tempo events of a MIDI file's first track, as (tick, microseconds a quarter note).
std::vector<std::pair<uint32_t, uint32_t>> Tempos(const MidiEvents& m)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x51)
        {
            out.push_back({uint32_t(e.tick), uint32_t((e.bytes[3] << 16) | (e.bytes[4] << 8) | e.bytes[5])});
        }
    }

    return out;
}

// Returns true if `seconds` is the length of `frames` frames.
bool AtFrames(double seconds, double frames)
{
    return std::fabs(seconds - frames / kFrameRate) < 1e-9;
}

// Runs a sequencer for `frames` frames, and returns the actions of the kind given.
std::vector<Action> Actions(Sequencer& seq, int frames, Action::Kind kind)
{
    std::vector<Action> out;
    for (int f = 0; f < frames; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == kind)
            {
                out.push_back(a);
            }
        }
    }

    return out;
}

// Returns the CC10 and CC11 events of the MIDI track named after track `t`, in order. Each is a tick, a controller and
// a value.
std::vector<std::tuple<uint32_t, int, int>> LevelEvents(const MidiEvents& m, int t)
{
    const std::string name = "Track " + std::to_string(t);
    std::vector<std::tuple<uint32_t, int, int>> out;
    for (const auto& track : m.tracks)
    {
        if (track.events.empty())
        {
            continue;
        }

        const std::vector<uint8_t>& first = track.events.front().bytes;
        if (first.size() < 3 || std::string(first.begin() + 3, first.end()) != name)
        {
            continue;
        }

        for (const auto& e : track.events)
        {
            if ((e.bytes[0] & 0xF0) == 0xB0 && (e.bytes[1] == cc::kPan || e.bytes[1] == cc::kExpression))
            {
                out.emplace_back(uint32_t(e.tick), e.bytes[1], e.bytes[2]);
            }
        }
    }

    return out;
}

void TestDecoding()
{
    Cart cart;
    Track t;
    const size_t start = t.Here();
    t.Cmd(kCmdKeySh, 0)
        .Cmd(kCmdVoice, 5)
        .Raw({0x30})
        .Note(8, 60, 100, 2)
        .Raw({0x3E})
        .Wait(24)
        .Tie(64, 127)
        .Eot(64)
        .Raw({kCmdXcmd, kXcmdEchoVol, 16})
        .Raw({kCmdXcmd, kXcmdWave, 0x56, 0x34, 0x12, 0x08})
        .Raw({kCmdXcmd, kXcmdOffset, 0x40, 0x01, 0x00, 0x00})
        .Patt(start)
        .Rept(2, start)
        .Raw({kCmdMemAcc, 6, 1, 2, 0, 0, 0, 0})
        .Goto(start)
        .Fine();
    const uint32_t at = cart.Place(t.Bytes(kData), 1);
    const Rom rom = cart.ToRom();

    uint32_t a = at;
    uint8_t running = 0;
    Event e;
    auto next = [&]()
    {
        const bool ok = DecodeEvent(rom, a, running, e);
        running = RunningStatus(e, running);
        a += e.size;

        return ok;
    };

    SEQLIB_CHECK(next() && e.command == kCmdKeySh && e.size == 2 && DescribeEvent(e) == "KEYSH 0");
    SEQLIB_CHECK(next() && e.command == kCmdVoice && e.arg[0] == 5);
    SEQLIB_CHECK(next() && e.command == kCmdVoice && e.size == 1 && e.arg[0] == 0x30);
    SEQLIB_CHECK(next() && e.size == 4 && DescribeEvent(e) == "N08 key 60 vel 100 gate+2");
    SEQLIB_CHECK(next() && e.size == 1 && DescribeEvent(e) == "N08 key 62");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "W24");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "TIE key 64 vel 127");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "EOT key 64");
    SEQLIB_CHECK(next() && e.size == 3 && DescribeEvent(e) == "XCMD xIECV 16");
    SEQLIB_CHECK(next() && e.size == 6 && e.target == 0x08123456 && DescribeEvent(e) == "XCMD xWAVE 0x08123456");
    SEQLIB_CHECK(next() && e.size == 6 && e.target == 320 && DescribeEvent(e) == "XCMD x0D 320");
    SEQLIB_CHECK(next() && e.command == kCmdPatt && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdRept && e.size == 6 && e.arg[0] == 2 && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdMemAcc && e.size == 8 && e.target == 0);
    SEQLIB_CHECK(next() && e.command == kCmdGoto && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdFine);

    // A byte below 0x80 without a running status, and an extended command the driver doesn't have.
    const uint32_t bad = cart.Place({0x40, kCmdXcmd, 0x20}, 1);
    const Rom bad_rom = cart.ToRom();

    SEQLIB_CHECK(!DecodeEvent(bad_rom, bad, 0, e));
    SEQLIB_CHECK(!DecodeEvent(bad_rom, bad + 1, 0, e));
}

void TestDetection()
{
    // The routine that starts a song gives the tables, and the init gives the mixer's settings. The song table ends
    // before the first entry that isn't a song.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
    cart.Song({Track().Fine()}, voices);
    cart.Song({Track().Fine(), Track().Fine()}, voices, 1);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK_EQ(info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.player_table, kPlayerTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_REQUIRE_EQ(info.players.size(), 2);
    SEQLIB_CHECK(info.players[1].track_count == 3);
    SEQLIB_CHECK_EQ(info.sound_mode, kMode);
    SEQLIB_CHECK_EQ(info.max_channels, 5);
    SEQLIB_CHECK_EQ(info.master_volume, 12);
    SEQLIB_CHECK_EQ(info.mix_rate, 13379);
    SEQLIB_CHECK_EQ(info.samples_per_frame, 224);
    SEQLIB_CHECK_EQ(info.step_scale, 627);
    SEQLIB_CHECK(info.special_samples);
    SEQLIB_CHECK(info.warnings.empty());

    // Overrides take the place of what detection finds.
    DriverOverrides overrides;
    overrides.song_count = 1;
    DriverInfo one;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, overrides, one, error));
    SEQLIB_CHECK_EQ(one.song_count, 1);

    // An all-zero entry, as a GSF rip can leave for a song it doesn't hold, is empty and doesn't end the song table,
    // but those after the last song don't count. A table without a song fails, with a hint.
    Cart gaps;
    for (int s = 0; s < 4; s++)
    {
        gaps.Song({Track().Fine()}, voices);
    }
    for (const uint32_t entry : {kSongTable + 8, kSongTable + 24})
    {
        gaps.Put32(entry, 0);
        gaps.Put32(entry + 4, 0);
    }
    const Rom gaps_rom = gaps.ToRom();
    const Rom empty_rom = Cart().ToRom();
    DriverInfo empty;

    const DriverInfo gaps_info = Detect(gaps_rom);
    const bool found_empty = DetectDriver(empty_rom, DriverOverrides(), empty, error);

    SEQLIB_CHECK_EQ(gaps_info.song_count, 3);
    SEQLIB_CHECK_EQ(SongAddress(gaps_rom, gaps_info, 1), 0);
    SEQLIB_CHECK(SongAddress(gaps_rom, gaps_info, 0) != 0 && SongAddress(gaps_rom, gaps_info, 2) != 0);
    SEQLIB_CHECK(!Sequencer(gaps_rom, gaps_info, 1).Valid() && Sequencer(gaps_rom, gaps_info, 2).Valid());
    SEQLIB_CHECK(!found_empty);
    SEQLIB_CHECK(error.find("--song-count") != std::string::npos);

    // Another mode, an older mixer, and no mode at all: then the driver's defaults stand.
    Cart slow(0x0093F800, {4}, true, false);
    slow.Song({Track().Fine()}, voices);
    Cart unset(0x00000500);
    unset.Song({Track().Fine()}, voices);

    const DriverInfo slow_info = Detect(slow.ToRom());
    const DriverInfo unset_info = Detect(unset.ToRom());

    SEQLIB_CHECK_EQ(slow_info.mix_rate, 10512);
    SEQLIB_CHECK_EQ(slow_info.max_channels, 8);
    SEQLIB_CHECK(!slow_info.special_samples);
    SEQLIB_CHECK_EQ(unset_info.sound_mode, 0);
    SEQLIB_CHECK_EQ(unset_info.max_channels, 8);
    SEQLIB_CHECK_EQ(unset_info.mix_rate, 13379);
    SEQLIB_CHECK(!unset_info.warnings.empty());

    // Some games' compiler swaps r2 and r3 in the routine that starts a song. The mono mixer's volume code marks a
    // mixer that plays every channel on both sides.
    Cart swapped;
    swapped.Song({Track().Fine()}, voices);
    const uint16_t kSwapped[] = {0x4B08, 0x4909, 0x0B40, 0x1840, 0x8882, 0x0051, 0x1889, 0x0089, 0x18C9};
    for (uint32_t i = 0; i < 9; i++)
    {
        swapped.Put16(kSongStart + 4 + 2 * i, kSwapped[i]);
    }
    const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
    for (uint32_t i = 0; i < 12; i++)
    {
        swapped.Put8(kMonoVolume + i, kMono[i]);
    }

    const DriverInfo swapped_info = Detect(swapped.ToRom());

    SEQLIB_CHECK_EQ(swapped_info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(swapped_info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(swapped_info.player_table, kPlayerTable);
    SEQLIB_CHECK(swapped_info.mono && !info.mono);

    // A cartridge without the driver's code fails, unless the song table is given.
    Cart bare(kMode, {10}, false);
    bare.Song({Track().Fine()}, voices);
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = kSongTable;
    DriverInfo none;

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error) && error.empty());
    SEQLIB_CHECK(DetectDriver(bare_rom, table, none, error));
    SEQLIB_CHECK_EQ(none.song_count, 1);
    SEQLIB_CHECK_EQ(SongPlayer(bare_rom, none, 0).track_count, 16);
}

void TestTiming()
{
    // The driver adds the tempo to a counter each frame, and plays a tick for each 150 in it. At the start, before a
    // tempo command, it adds 150. A tempo command of 75 sets a tempo of 150 (150 quarter notes a minute at 60 frames a
    // second), so a tick is a frame. One of 60 sets 120, so a tick takes 1.25 frames, and tick 24 is on frame 30.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(24).Note(24, 60, 100).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 60).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(24).Note(24, 60, 100).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer fast(rom, info, 0);
    Sequencer slow(rom, info, 1);
    const std::vector<Action> fast_notes = Actions(fast, 40, Action::kNoteOn);
    const std::vector<Action> slow_notes = Actions(slow, 40, Action::kNoteOn);

    SEQLIB_CHECK(fast_notes.size() == 1 && fast_notes[0].frame == 24 && fast_notes[0].tick == 24);
    SEQLIB_CHECK(slow_notes.size() == 1 && slow_notes[0].frame == 30 && slow_notes[0].tick == 24);
}

void TestNotes()
{
    // A note of 24 ticks is released 24 ticks after it starts, a tied note at its end command, and a note with an extra
    // length that much later.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 100)
                   .Wait(48)
                   .Tie(62, 100)
                   .Wait(12)
                   .Eot(62)
                   .Wait(12)
                   .Note(12, 64, 100, 3)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> ons, releases;
    for (int f = 0; f < 120; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                ons.push_back(a);
            }
            if (a.kind == Action::kRelease)
            {
                releases.push_back(a);
            }
        }
    }

    SEQLIB_REQUIRE_EQ(ons.size(), 3);
    SEQLIB_REQUIRE_EQ(releases.size(), 3);
    SEQLIB_CHECK(ons[0].tick == 0 && ons[1].tick == 48 && ons[2].tick == 72);
    SEQLIB_CHECK(releases[0].tick == 24 && releases[1].tick == 60 && releases[2].tick == 87);
    SEQLIB_CHECK(ons[0].key == 60 && ons[0].velocity == 100 && ons[0].program == 0 && ons[0].channel == 0);
}

void TestChannels()
{
    // Two DirectSound channels. Track 0, of priority 10, holds both. A note of track 1, of priority 0, finds none it
    // can take. Once track 0 releases a note, track 1 takes its channel, released notes first.
    Cart cart(0x00940200);
    const uint32_t kSquare = 0;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {1, Cart::Voice(1, kSquare, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdPrio, 10)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Tie(62, 100)
                   .Wait(2)
                   .Eot(60)
                   .Wait(10)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Wait(1).Tie(64, 100).Wait(2).Tie(65, 100).Wait(10).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(70, 100).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1).Tie(72, 100).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> actions;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            actions.push_back(a);
        }
    }

    std::vector<std::pair<int, int>> ons, dropped;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            ons.push_back({a.key, a.channel});
        }
        if (a.kind == Action::kNoteDropped)
        {
            dropped.push_back({a.key, int(a.tick)});
        }
    }

    // Keys 60 and 62 take channels 0 and 1, and 70 takes the first PSG channel. Key 64 and track 3's 72, of the same
    // priority as 70 but from a later track, are dropped. Key 65 takes the released key 60's channel.
    const std::vector<std::pair<int, int>> kOns = {{60, 0}, {62, 1}, {70, kFirstPsgChannel}, {65, 0}};
    const std::vector<std::pair<int, int>> kDropped = {{64, 1}, {72, 1}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK(dropped == kDropped);
}

void TestEnvelope()
{
    // An attack of 100 a frame, a decay of 128/256 a frame to a sustain level of 64, and a release of 128/256 a frame
    // after the note's 96 ticks. With an echo volume of 16 and an echo length of 3, the release holds at 16 for 3
    // frames and then stops.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {100, 128, 64, 128})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(96, 60, 127).Wait(96).Wait(24).Fine()},
        voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Raw({kCmdXcmd, kXcmdEchoVol, 16, kCmdXcmd, kXcmdEchoLen, 3})
                   .Note(96, 60, 127)
                   .Wait(96)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The envelope's level after each frame: frames 0-4, then 96-103.
    auto levels = [&](int song)
    {
        Sequencer seq(rom, info, song);
        std::vector<int> out;
        for (int f = 0; f < 104; f++)
        {
            seq.Step();
            if (f < 5 || f >= 96)
            {
                out.push_back(seq.GetChannel(0).status ? seq.GetChannel(0).envelope : -1);
            }
        }

        return out;
    };

    const std::vector<int> kPlain = {100, 200, 255, 127, 64, 32, 16, 8, 4, 2, 1, -1, -1};
    const std::vector<int> kEcho = {100, 200, 255, 127, 64, 32, 16, 16, 16, -1, -1, -1, -1};
    SEQLIB_CHECK(levels(0) == kPlain);
    SEQLIB_CHECK(levels(1) == kEcho);
}

void TestEnvelopeHold()
{
    // The frame in which a sample note's attack reaches 255 plays at 255, and the decay starts on the next frame. The
    // SoundFont holds the full level for that frame where the decay changes the level: below a sustain of 255. A PSG
    // voice's envelope steps on the hardware's counter instead.
    const DriverInfo info;
    Voice sample;
    sample.attack = 255;
    sample.sustain = 128;
    Voice full = sample;
    full.sustain = 255;
    Voice square;
    square.type = 1;
    square.attack = 0;
    square.decay = 2;
    square.sustain = 7;

    const VolumeEnvelope held = EnvelopeFor(info, sample);
    const VolumeEnvelope sustained = EnvelopeFor(info, full);
    const VolumeEnvelope psg = EnvelopeFor(info, square);

    SEQLIB_CHECK_EQ(held.hold, Timecents(1.0 / kFrameRate));
    SEQLIB_CHECK_EQ(held.decay, kInstant);
    SEQLIB_CHECK_EQ(sustained.hold, kInstant);
    SEQLIB_CHECK_EQ(psg.hold, kInstant);
}

void TestPsgEnvelopeBytes()
{
    // The driver counts a PSG voice's whole attack, decay and release bytes. An attack of 10 takes 10 counts a step, 16
    // counts every 15 frames. A sustain above 15 never falls below the full level. The zone holds it.
    const DriverInfo info;
    Voice slow;
    slow.type = 2;
    slow.attack = 10;
    slow.decay = 10;
    slow.sustain = 18;
    slow.release = 10;
    Voice fast = slow;
    fast.release = 2;

    const VolumeEnvelope env = EnvelopeFor(info, slow);
    const VolumeEnvelope fast_env = EnvelopeFor(info, fast);

    SEQLIB_CHECK_EQ(env.attack, Timecents(15.0 * 10 / (kFrameRate * 16 / 15)));
    SEQLIB_CHECK_EQ(env.decay, kInstant);
    SEQLIB_CHECK_EQ(env.sustain, 0);
    SEQLIB_CHECK(env.release > fast_env.release);
}

void TestPitch()
{
    // A sample's rate for a key: its rate at key 60, in 1/1024 Hz, times 2^(n/12), with a fine tune that runs in a
    // straight line in rate to the next key.
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 60, 0), 13379);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 72, 0), 26758);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 48, 0), 6689);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 60, 128), 13776);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 66, 0), 18920);

    // The PSG's square settings: C4 is 1547, and keys up to 36 all play C2. The noise settings run from 0xD7 at key 21
    // to 0 at key 80.
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 60, 0), 1547);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 36, 0), 44);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 20, 0), 44);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 21, 0), 0xD7);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 26, 0), 0xC6);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 77, 0), 0x03);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 90, 0), 0x00);

    // A bend of +16 with a range of 12 raises the pitch by 3 semitones, and the channel's rate follows at the end of
    // the frame.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Wait(2)
                   .Cmd(kCmdBendR, 12)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.Step();
    const uint32_t before = seq.GetChannel(0).frequency;
    seq.Step();
    seq.Step();
    const uint32_t after = seq.GetChannel(0).frequency;

    SEQLIB_CHECK_EQ(before, 13379);
    SEQLIB_CHECK_EQ(after, KeyToFrequency(13379 * 1024, 63, 0));
}

void TestLevelReports()
{
    // A track sets volume 100 and pan 0x50, and a tick later volume 1 and pan 0x20.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 100)
                   .Cmd(kCmdPan, 0x50)
                   .Wait(1)
                   .Cmd(kCmdVol, 1)
                   .Cmd(kCmdPan, 0x20)
                   .Wait(1)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    const std::vector<Action> reports = Actions(seq, 4, Action::kLevels);

    // Volume 100 gives a level of 200. Pan 0x50 gives the left 95/256 of it and the right 160/256: 74 and 125, rounded
    // down. Volume 1 gives a level of 2, and pan 0x20 gives the left 191/256 of it and the right 64/256. Rounded down,
    // the left plays at 1 and the right not at all.
    SEQLIB_REQUIRE_EQ(reports.size(), 2);
    SEQLIB_CHECK(reports[0].tick == 0 && reports[0].left == 74 && reports[0].right == 125);
    SEQLIB_CHECK(reports[1].tick == 1 && reports[1].left == 1 && reports[1].right == 0);
}

void TestLoops()
{
    // A track that goes back to a command after 12 ticks plays its loop of 24 ticks twice, and the MIDI file marks it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
    const size_t loop = t.Here();
    t.Note(12, 60, 100).Wait(24).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loops");

    const TrackLayout layout = ScanTrack(rom, rom.U32(rom.U32(kSongTable) + 8));
    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(layout.loops && layout.loop_start == 12 && layout.loop_end == 36);
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(AtFrames(r.loop_start, 12));
    SEQLIB_CHECK(AtFrames(r.loop_end, 36));
    SEQLIB_CHECK(AtFrames(r.seconds, 60));

    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{12 * kMidiTicks, "loopStart"},
                                                                    {36 * kMidiTicks, "loopEnd"}};
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_REQUIRE_EQ(m.Find(0x90).size(), 2);
}

void TestLoopStarts()
{
    // The first track loops 24 ticks from tick 4, the second 48 ticks from tick 16, and the third 12 ticks from tick 8.
    // Every track is looping from tick 16. The loop goes from there for the 48 ticks that each track's loop divides, to
    // tick 64, and the song plays it twice. The middle track's loop starts last and is the longest. Neither the first
    // track's loop nor the last track's loop gives the same times.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(4);
    const size_t first_loop = first.Here();
    first.Note(12, 60, 100).Wait(24).Goto(first_loop);
    Track middle;
    middle.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(16);
    const size_t middle_loop = middle.Here();
    middle.Note(12, 64, 100).Wait(48).Goto(middle_loop);
    Track last;
    last.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(8);
    const size_t last_loop = last.Here();
    last.Note(6, 67, 100).Wait(12).Goto(last_loop);
    cart.Song({first, middle, last}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_starts");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(AtFrames(r.loop_start, 16));
    SEQLIB_CHECK(AtFrames(r.loop_end, 64));
    SEQLIB_CHECK(AtFrames(r.seconds, 112));
}

void TestLoopStartsAtSecondPass()
{
    // A track sets tempo 75 on tick 12, just before its loop, and tempo 60 halfway through the loop's 24 ticks, which
    // the next pass keeps. The first pass starts at tempo 75 and the later ones at 60, so the marked loop starts at the
    // second pass, at tick 36, and the song plays the first pass before it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12).Cmd(kCmdTempo, 75);
    const size_t loop = t.Here();
    t.Note(12, 60, 100).Wait(12).Cmd(kCmdTempo, 60).Wait(12).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("second_pass");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{36 * kMidiTicks, "loopStart"},
                                                                    {60 * kMidiTicks, "loopEnd"}};
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_REQUIRE_EQ(m.Find(0x90).size(), 3);
}

void TestLoopLevelsAtSecondPass()
{
    // A track plays at volume 100 and the centre before its loop. The loop sets the volume as it starts. The loop's
    // second note plays at another pan. The next pass keeps that pan. In song 0 it's 0x7F. In song 1 the loop also sets
    // the pan to the centre as it starts. In song 2 the second note's pan is 0x42.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    for (int song = 0; song < 3; song++)
    {
        Track t;
        t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x40).Wait(12);
        const size_t loop = t.Here();
        t.Cmd(kCmdVol, 100);
        if (song == 1)
        {
            t.Cmd(kCmdPan, 0x40);
        }
        t.Note(12, 60, 100).Wait(12).Cmd(kCmdPan, song == 2 ? 0x42 : 0x7F).Note(12, 62, 100).Wait(12).Goto(loop);
        cart.Song({t}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_levels");

    std::vector<SongReport> reports;
    for (int song = 0; song < 3; song++)
    {
        reports.push_back(ConvertSong(rom, info, song, opt, nullptr));
    }

    // In song 0, a player that loops on the markers would play the loop's first note at the centre. The game plays it
    // on the right in the later passes. The marked loop therefore starts at the second pass, from tick 36. In song 1
    // the passes play alike. In song 2 each side of the later passes' first note plays less than 1 dB away from the
    // centre's level. Both keep the loop at the first pass, from tick 12.
    SEQLIB_CHECK(reports[0].ok && AtFrames(reports[0].loop_start, 36) && AtFrames(reports[0].loop_end, 60));
    SEQLIB_CHECK(reports[1].ok && AtFrames(reports[1].loop_start, 12) && AtFrames(reports[1].loop_end, 36));
    SEQLIB_CHECK(reports[2].ok && AtFrames(reports[2].loop_start, 12) && AtFrames(reports[2].loop_end, 36));
}

void TestLoopLevelsWithDroppedNote()
{
    // Two tracks loop from tick 12 for 24 ticks, at one volume and pan throughout. Track 0 plays a note on square 1 at
    // tick 30 that lasts past the loop's end. Track 1 plays square 1 as its loop starts. The channel is free in the
    // first pass. In the later ones, track 0's note holds it. The driver then drops track 1's note.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(12);
    const size_t first_loop = first.Here();
    first.Wait(18).Note(12, 70, 127).Wait(6).Goto(first_loop);
    Track second;
    second.Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(12);
    const size_t second_loop = second.Here();
    second.Note(6, 60, 127).Wait(24).Goto(second_loop);
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_dropped");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The passes start other notes. The notes that both start play at the same levels. The loop therefore stays at the
    // first pass.
    SEQLIB_CHECK(r.ok && AtFrames(r.loop_start, 12) && AtFrames(r.loop_end, 36));
    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<uint32_t> ons;
    for (const auto& e : m.Find(0x91))
    {
        ons.push_back(uint32_t(e.tick));
    }
    SEQLIB_CHECK((ons == std::vector<uint32_t>{12 * kMidiTicks}));
}

void TestLoopLengths()
{
    // Two tracks loop 36 and 48 ticks from the start, and a third, without a loop, plays a note of 30 ticks at tick 10.
    // The loop starts at tick 40, where that note ends, and lasts until both loops are back where they started, 144
    // ticks, to tick 184, and the song plays it twice.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track three;
    three.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t three_loop = three.Here();
    three.Note(12, 60, 100).Wait(36).Goto(three_loop);
    Track four;
    four.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t four_loop = four.Here();
    four.Note(12, 64, 100).Wait(48).Goto(four_loop);
    Track once;
    once.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(10).Note(30, 67, 100).Wait(40).Fine();
    cart.Song({three, four, once}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(AtFrames(r.loop_start, 40));
    SEQLIB_CHECK(AtFrames(r.loop_end, 184));
    SEQLIB_CHECK(AtFrames(r.seconds, 328));
}

void TestLoopAfterTie()
{
    // In both songs, one track loops 24 ticks from the start, and another, without a loop, holds a tie and ends at tick
    // 96. In the first song only the track's end releases the tie, so the loop starts there. In the second, an end of
    // tie releases it at tick 30, where the loop starts.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track looping;
    looping.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t loop = looping.Here();
    looping.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({looping, Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Tie(64, 100).Wait(96).Fine()}, voices);
    cart.Song({looping, Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Tie(64, 100).Wait(30).Eot(64).Wait(66).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loop_tie");

    const SongReport held = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport ended = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(held.ok && ended.ok);
    SEQLIB_CHECK(AtFrames(held.loop_start, 96));
    SEQLIB_CHECK(AtFrames(held.loop_end, 120));
    SEQLIB_CHECK(AtFrames(ended.loop_start, 30));
    SEQLIB_CHECK(AtFrames(ended.loop_end, 54));
}

// A track's volume is 100 when its loop starts the first time, and the loop sets 60 for its second note. In song 0 the
// loop sets 100 again as it starts, and in song 1 just before it jumps back, in the tick of the loop's end, which the
// next pass starts on. Either way the marked pass gives the levels of volume 100 again at its start: CC10 64 and CC11
// 112. A player that jumps back there from volume 60 then plays at volume 100. In song 2 the loop doesn't set it again.
// The game then plays the later passes' first note at 60, and so does such a player. Nothing is written again.
void TestLoopSettingsAgain()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    for (int song = 0; song < 3; song++)
    {
        Track t;
        t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
        const size_t loop = t.Here();
        if (song == 0)
        {
            t.Cmd(kCmdVol, 100);
        }
        t.Note(12, 60, 100).Wait(12).Cmd(kCmdVol, 60).Note(12, 62, 100).Wait(12);
        if (song == 1)
        {
            t.Cmd(kCmdVol, 100);
        }
        cart.Song({t.Goto(loop)}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loop_again");

    for (int song = 0; song < 3; song++)
    {
        const SongReport r = ConvertSong(rom, info, song, opt, nullptr);

        std::map<int, int> at_start;
        for (const auto& [tick, bytes] : ReadMidi(r.midi_path).Find(0xB0))
        {
            if (tick == 12 * kMidiTicks && (bytes[1] == cc::kPan || bytes[1] == cc::kExpression))
            {
                at_start[bytes[1]] = bytes[2];
            }
        }

        const std::map<int, int> kAgain = {{cc::kPan, 64}, {cc::kExpression, 112}};
        SEQLIB_CHECK(r.ok && AtFrames(r.loop_start, 12));
        SEQLIB_CHECK(at_start == (song < 2 ? kAgain : std::map<int, int>()));
    }
}

void TestHourLimit()
{
    // A song asked to play its loop of 3000 ticks, 50 s, a hundred times stops where the model gives up, after 216000
    // frames. The loop starts 12 ticks in, so the limit falls in the middle of a note: the song's length, its MIDI file
    // and that note all end there.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
    const size_t loop = t.Here();
    t.Tie(60, 100).Wait(3000).Eot(60).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("hour");
    opt.loops = 100;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto cut_off = [](const std::string& w)
    {
        return w.find("cut off after an hour") != std::string::npos;
    };
    SEQLIB_CHECK(r.ok && std::any_of(r.warnings.begin(), r.warnings.end(), cut_off));
    SEQLIB_CHECK(std::fabs(r.seconds - 216000 / kFrameRate) < 0.1);

    const MidiEvents m = ReadMidi(r.midi_path);
    uint64_t last = 0;
    for (const auto& track : m.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }
    const auto offs = m.Find(0x80);
    SEQLIB_CHECK(last <= 216001 * kMidiTicks);
    SEQLIB_CHECK(!offs.empty() && offs.back().tick >= 215999 * kMidiTicks);
}

void TestConversion()
{
    // A sample voice with a bend, a key split of a sample and a square wave, and a drum kit. The kit has a fixed-pitch
    // sample panned left and three noise voices: one with no pan, one panned right and one panned a little left.
    Cart cart;
    const uint32_t lead = cart.Wave(Saw(), 11025, 32);
    const uint32_t low = cart.Wave(Saw(100), 8000);
    const uint32_t kick = cart.Wave(Saw(50), 13379);
    const uint32_t split_table = cart.VoiceGroup({{0, Cart::Voice(0, low)}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const uint32_t kit_table = cart.VoiceGroup({{36, Cart::Voice(kFixed, kick, 50, {255, 0, 255, 0}, 0xA0)},
                                                {38, Cart::Voice(4, 0, 40, {0, 2, 0, 0})},
                                                {40, Cart::Voice(4, 0, 40, {0, 2, 0, 0}, 0xD8)},
                                                {41, Cart::Voice(4, 0, 40, {0, 2, 0, 0}, 0xB0)}});
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(0, lead, 60, {255, 200, 128, 200})},
                                             {2, cart.KeySplit(split_table, 59)},
                                             {3, Cart::Voice(kVoiceDrumKit, kit_table)}});

    // The drum kit's track is track 9, after seven that only end.
    std::vector<Track> tracks = {
        Track()
            .Cmd(kCmdTempo, 75)
            .Cmd(kCmdVoice, 1)
            .Cmd(kCmdVol, 100)
            .Cmd(kCmdPan, 0x50)
            .Cmd(kCmdBendR, 12)
            .Cmd(kCmdBend, 0x50)
            .Note(24, 60, 100)
            .Wait(48)
            .Fine(),
        Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Note(24, 50, 90).Note(24, 70, 90).Wait(48).Fine()};
    tracks.resize(9, Track().Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 3).Cmd(kCmdVol, 127).Note(12, 36, 127).Wait(12).Note(12, 38, 127).Wait(36).Fine());
    cart.Song(tracks, voices, 0, 0, 0x80 | 40);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("convert");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK_EQ(r.tracks, 3);
    SEQLIB_CHECK(std::fabs(r.bpm - 60e6 / 401825) < 1e-9);
    SEQLIB_CHECK(PathFromUtf8(r.midi_path).filename() == "convert_00.mid");

    // 960 ticks a quarter note, the tempo at the GBA's speed, and a conductor track, then a track on the MIDI channel
    // of each of the song's tracks that plays notes. Track 9 moves off the drum channel to channel 11, the first free
    // one after it.
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK_EQ(m.division, 24 * kMidiTicks);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 4);
    const auto tempos = Tempos(m);
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), [](const auto& t) { return t.second == 401825; }));

    // A velocity of v is written as 127 × √(v / 127), rounded: the lead's 100 as 113.
    const auto lead_notes = m.Find(0x90);
    SEQLIB_CHECK(lead_notes.size() == 1 && lead_notes[0].tick == 0 && lead_notes[0].bytes[1] == 60 &&
                 lead_notes[0].bytes[2] == 113);
    const auto lead_offs = m.Find(0x80);
    SEQLIB_CHECK(lead_offs.size() == 1 && lead_offs[0].tick == 24 * kMidiTicks);
    SEQLIB_REQUIRE_EQ(m.Find(0x91).size(), 2);
    SEQLIB_REQUIRE_EQ(m.Find(0x9A).size(), 2);
    SEQLIB_CHECK(m.Find(0x99).empty());

    // The program change, full volume and the song's reverb. The track's volume of 100 and pan of 0x50 give it levels
    // of 74 on the left and 125 on the right, out of 128. CC10 84 and CC11 114 give those levels under a constant-power
    // pan law and the SF2 default curve of CC11. A bend range of 3 semitones fills the bend of 3 semitones.
    const auto programs = m.Find(0xC0);
    SEQLIB_CHECK(programs.size() == 1 && programs[0].bytes[1] == 1);
    std::map<int, int> controls;
    for (const auto& e : m.Find(0xB0))
    {
        controls[e.bytes[1]] = e.bytes[2];
    }
    SEQLIB_CHECK(controls[cc::kVolume] == 127 && controls[cc::kReverb] == 40);
    SEQLIB_CHECK(controls[cc::kPan] == 84 && controls[cc::kExpression] == 114);
    SEQLIB_CHECK_EQ(controls[cc::kDataEntry], 3);
    const auto bends = m.Find(0xE0);
    SEQLIB_CHECK(!bends.empty() && bends.back().bytes[1] == 0x7F && bends.back().bytes[2] == 0x7F);

    // The SoundFont: a preset in bank 0 for each voice. No note plays on the drum channel. Bank 128 has no presets.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<std::pair<int, int>, int> presets;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        presets[{Le16(h + 22), Le16(h + 20)}] =
            Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
    }
    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({0, 2}) && presets.count({0, 3}));
    SEQLIB_REQUIRE_EQ(presets.size(), 3);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));

    auto zone_of = [&](int instrument, int index)
    {
        return sf.ZoneGens(Le16(sf.Record("inst", 22, size_t(instrument)) + 20) + size_t(index));
    };

    // The lead's zone: its envelope, and no tuning, since its sample's rate is a whole number. Its sample holds the
    // master volume of 12, at 13/16 of the saw's -128 widened to 16 bits, rather than the zone's attenuation.
    const auto lead_zone = zone_of(presets[{0, 1}], 1);
    SEQLIB_CHECK(lead_zone.count(sf2::kDecayVolEnv) && lead_zone.count(sf2::kSustainVolEnv) &&
                 lead_zone.count(sf2::kReleaseVolEnv) && !lead_zone.count(sf2::kAttackVolEnv));
    SEQLIB_CHECK(!lead_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK_EQ(sf.SamplePeak(lead_zone.at(sf2::kSampleId)), 128 * 256 * 13 / 16);
    SEQLIB_CHECK(!lead_zone.count(sf2::kCoarseTune) && !lead_zone.count(sf2::kFineTune));
    SEQLIB_CHECK_EQ(lead_zone.at(sf2::kSampleModes), 1);

    // The key split's zones split after key 59, and its square's sample is 6.3 dB quieter than a full-scale sample.
    const auto square_zone = zone_of(presets[{0, 2}], 2);
    SEQLIB_CHECK_EQ(zone_of(presets[{0, 2}], 1).at(sf2::kKeyRange), 0x3B00);
    SEQLIB_CHECK_EQ(square_zone.at(sf2::kKeyRange) & 0xFF, 60);
    SEQLIB_CHECK(!square_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK(std::fabs(20 * std::log10(sf.SamplePeak(square_zone.at(sf2::kSampleId)) / 32767.0) + 6.3) < 0.01);

    // The square's zones follow one another up to key 127. Each tunes its keys to the driver's frequency settings. On
    // key 96, the driver shifts C's entry of -2004 right 5 times and adds 2048. The setting of 1985 plays 2080.5 Hz, 10
    // cents below C7 in equal temperament.
    const size_t split_zones = Le16(sf.Record("inst", 22, size_t(presets[{0, 2}]) + 1) + 20) -
                               Le16(sf.Record("inst", 22, size_t(presets[{0, 2}])) + 20);
    int next_key = 60, key_96_cents = 0;
    for (size_t z = 2; z < split_zones; z++)
    {
        const auto zone = zone_of(presets[{0, 2}], int(z));
        const int lo = zone.at(sf2::kKeyRange) & 0xFF, hi = zone.at(sf2::kKeyRange) >> 8;
        SEQLIB_CHECK_EQ(lo, next_key);
        next_key = hi + 1;
        if (lo <= 96 && hi >= 96)
        {
            SEQLIB_CHECK(!zone.count(sf2::kCoarseTune));
            key_96_cents = zone.count(sf2::kFineTune) ? int16_t(zone.at(sf2::kFineTune)) : 0;
        }
    }
    SEQLIB_CHECK_EQ(next_key, 128);
    SEQLIB_CHECK_EQ(key_96_cents, -10);

    // The drum kit's fixed-pitch sample plays at the mixer's rate whatever the key or the bend. Its pan of 0xA0 gives
    // it 191/127 of the left level of a note with no pan and 64/128 of the right level. Pan -296 gives the sides that
    // ratio under a sin/cos pan law. The noise voice with no pan plays the noise setting of its key. Its zone holds no
    // pan.
    const auto kick_zone = zone_of(presets[{0, 3}], 1);
    SEQLIB_CHECK_EQ(kick_zone.at(sf2::kKeyRange), 0x2424);
    SEQLIB_CHECK_EQ(kick_zone.at(sf2::kScaleTuning), 0);
    SEQLIB_CHECK_EQ(int16_t(kick_zone.at(sf2::kPan)), -296);
    const auto noise_zone = zone_of(presets[{0, 3}], 2);
    SEQLIB_CHECK_EQ(noise_zone.at(sf2::kKeyRange), 0x2626);
    SEQLIB_CHECK_EQ(noise_zone.at(sf2::kScaleTuning), 0);
    SEQLIB_CHECK(!noise_zone.count(sf2::kPan));

    // The PSG plays a note on one side alone where that side's level is at least twice the other's. It plays in the
    // middle otherwise. The pan of 0xD8 gives 176 on the right and 79 on the left. The pan of 0xB0 gives 96 on the
    // right and 159 on the left. On one side alone, the zone's attenuation cancels the 3 dB that the RP-036 pan law
    // adds there. Most players apply 0.4 of a zone's attenuation. The zone therefore holds 75 cB.
    const auto right_zone = zone_of(presets[{0, 3}], 3), middle_zone = zone_of(presets[{0, 3}], 4);
    SEQLIB_CHECK(right_zone.at(sf2::kKeyRange) == 0x2828 && middle_zone.at(sf2::kKeyRange) == 0x2929);
    SEQLIB_CHECK(right_zone.count(sf2::kPan) && int16_t(right_zone.at(sf2::kPan)) == 500);
    SEQLIB_CHECK(right_zone.count(sf2::kInitialAttenuation) && right_zone.at(sf2::kInitialAttenuation) == 75);
    SEQLIB_CHECK(!middle_zone.count(sf2::kPan) && !middle_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK(!noise_zone.count(sf2::kInitialAttenuation));
    bool wheel_off = false;
    for (size_t i = 0; i < sf.Count("imod", 10); i++)
    {
        const uint8_t* mod = sf.Record("imod", 10, i);
        wheel_off = wheel_off || (Le16(mod) == 0x020E && Le16(mod + 2) == sf2::kFineTune && Le16(mod + 4) == 0);
    }
    SEQLIB_CHECK(wheel_off);
}

void TestLevels()
{
    // A sample voice, and a voice for each square channel.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))},
                                             {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {2, Cart::Voice(2, 2, 60, {0, 0, 15, 0})}});

    // Tracks 0 to 2 play the sample voice: at volume 80 and pan 0x7F, at volume 127 and pan 0x7F, and at volume 100
    // and pan 0x50. Tracks 3 to 5 play square 1 at volume 100 in turn: at pan 0x50 from tick 0, 0x60 from tick 24 and
    // 0x20 from tick 48. Track 6 plays square 2, the sample voice and square 2 again from ticks 0, 24 and 48, at volume
    // 100 and pan 0x60.
    std::vector<Track> tracks;
    const std::vector<std::pair<int, int>> kSamples = {{80, 0x7F}, {127, 0x7F}, {100, 0x50}};
    for (const auto& [volume, pan] : kSamples)
    {
        tracks.push_back(
            Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, volume).Cmd(kCmdPan, pan).Note(12, 60, 127).Wait(12).Fine());
    }
    tracks.push_back(Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x50).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x60).Wait(24).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x20).Wait(48).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(Track()
                         .Cmd(kCmdVol, 100)
                         .Cmd(kCmdPan, 0x60)
                         .Cmd(kCmdVoice, 2)
                         .Note(12, 60, 127)
                         .Wait(24)
                         .Cmd(kCmdVoice, 0)
                         .Note(12, 60, 127)
                         .Wait(24)
                         .Cmd(kCmdVoice, 2)
                         .Note(12, 60, 127)
                         .Wait(12)
                         .Fine());
    cart.Song(tracks, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A sample plays at the levels that the volume and pan give each side, out of 128. Volume 80 at pan 0x7F gives 0
    // and 158. Volume 127 gives 0 and 252. CC11 can't reach that: it stops at the centre's loudest level. Volume 100 at
    // pan 0x50 gives 74 and 125.
    using Levels = std::vector<std::tuple<uint32_t, int, int>>;
    SEQLIB_REQUIRE(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK((LevelEvents(m, 0) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 119}}));
    SEQLIB_CHECK((LevelEvents(m, 1) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 127}}));
    SEQLIB_CHECK((LevelEvents(m, 2) == Levels{{0, cc::kPan, 84}, {0, cc::kExpression, 114}}));

    // A square plays on one side alone where that side's level is at least twice the other's. That side plays at the
    // level of the centre: the average of the two. Otherwise the square plays on both sides at that level. Pan 0x50
    // gives 74 and 125. The square then plays on both sides at 99.5. Pan 0x60 gives 49 and 150, on the right alone.
    // Pan 0x20 gives 149 and 50, on the left alone. A track's controllers follow the PSG's rule from the start where
    // its first note is a PSG note.
    SEQLIB_CHECK((LevelEvents(m, 3) == Levels{{0, cc::kPan, 64}, {0, cc::kExpression, 112}}));
    SEQLIB_CHECK((LevelEvents(m, 4) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 95}}));
    SEQLIB_CHECK((LevelEvents(m, 5) == Levels{{0, cc::kPan, 0}, {0, cc::kExpression, 95}}));

    // A track that plays both kinds of voice follows the rule of the notes that it plays.
    SEQLIB_CHECK((LevelEvents(m, 6) == Levels{{0, cc::kPan, 127},
                                              {0, cc::kExpression, 95},
                                              {24 * kMidiTicks, cc::kPan, 101},
                                              {24 * kMidiTicks, cc::kExpression, 119},
                                              {48 * kMidiTicks, cc::kPan, 127},
                                              {48 * kMidiTicks, cc::kExpression, 95}}));
}

void TestMonoLevels()
{
    // A cartridge with the mono mixer, a sample voice and a square voice.
    Cart cart;
    const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
    for (uint32_t i = 0; i < 12; i++)
    {
        cart.Put8(kMonoVolume + i, kMono[i]);
    }
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});

    // Track 0 plays the sample voice at volume 80 and pan 0x7F, and track 1 the square at volume 100 and pan 0x60.
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 80).Cmd(kCmdPan, 0x7F).Note(12, 60, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x60).Note(12, 60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("mono_levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The mixer plays the sample on both sides at the average of its levels, 0 and 158. The PSG still plays the square
    // on the right alone.
    using Levels = std::vector<std::tuple<uint32_t, int, int>>;
    SEQLIB_REQUIRE(info.mono && r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK((LevelEvents(m, 0) == Levels{{0, cc::kPan, 64}, {0, cc::kExpression, 100}}));
    SEQLIB_CHECK((LevelEvents(m, 1) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 95}}));
}

void TestSlowAttackAtEnd()
{
    // A square note with the slowest attack starts on the last tick of each pass through a loop of 23 ticks from tick
    // 1, and the conversion ends after the second pass, on tick 47. The second pass's note goes on into the next pass,
    // and its level gets above 0 only after the end. So the model runs on past the end, and the MIDI file keeps it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {7, 0, 15, 0})}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1);
    const size_t loop = track.Here();
    track.Wait(22).Note(24, 60, 100).Wait(1).Goto(loop);
    cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("slow_attack");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok);
    std::vector<uint32_t> ons;
    for (const auto& e : ReadMidi(r.midi_path).Find(0x90))
    {
        ons.push_back(uint32_t(e.tick));
    }
    SEQLIB_CHECK(ons == (std::vector<uint32_t>{23 * kMidiTicks, 46 * kMidiTicks}));
}

void TestTwoVoicesInATick()
{
    // A track sets voice 1 and plays a note, then sets voice 4 and plays another, in one tick. The driver plays each
    // note with the voice set before it. So the MIDI file gives the program changes and notes in the track's order.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup(
        {{1, Cart::Voice(0, cart.Wave(Saw(80), 13379, 0))}, {4, Cart::Voice(0, cart.Wave(Saw(90), 13379, 0))}});
    cart.Song({Track().Cmd(kCmdVoice, 1).Note(24, 60, 100).Cmd(kCmdVoice, 4).Note(24, 64, 100).Wait(24).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("two_voices");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok);
    std::vector<std::pair<int, int>> order;
    for (const auto& t : ReadMidi(r.midi_path).tracks)
    {
        for (const auto& [tick, bytes] : t.events)
        {
            if (tick == 0 && (bytes[0] == 0xC0 || bytes[0] == 0x90))
            {
                order.emplace_back(bytes[0], bytes[1]);
            }
        }
    }
    const std::vector<std::pair<int, int>> kOrder = {{0xC0, 1}, {0x90, 60}, {0xC0, 4}, {0x90, 64}};
    SEQLIB_CHECK(order == kOrder);
}

void TestSharedSoundfont()
{
    // Two voice groups with different voices as program 1 and the same voice as program 4.
    Cart cart;
    const std::vector<uint8_t> common = Cart::Voice(0, cart.Wave(Saw(), 13379, 0));
    const uint32_t first_group = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(80)))}, {4, common}});
    const uint32_t second_group = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(90)))}, {4, common}});

    // The first song plays program 1 on track 0 and program 4 on track 9, and the second plays both on track 0.
    std::vector<Track> tracks = {Track().Cmd(kCmdVoice, 1).Note(24, 60, 100).Wait(24).Fine()};
    tracks.resize(9, Track().Fine());
    tracks.push_back(Track().Cmd(kCmdVoice, 4).Note(24, 36, 100).Wait(24).Fine());
    cart.Song(tracks, first_group);
    cart.Song(
        {Track().Cmd(kCmdVoice, 1).Note(24, 60, 100).Wait(24).Cmd(kCmdVoice, 4).Note(24, 62, 100).Wait(24).Fine()},
        second_group);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("shared");
    SoundfontBuilder shared(rom, info);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport second = ConvertSong(rom, info, 1, opt, &shared);

    // Program 1 has the first song's voice in bank 0 and the second's in bank 1, the common voice has one preset, and
    // the drum bank has none.
    SEQLIB_CHECK(first.ok && second.ok && first.sf2_path.empty() && second.sf2_path.empty());
    std::map<std::pair<int, int>, int> presets;
    for (const sf2::Preset& p : shared.File().presets)
    {
        presets[{p.bank, p.program}] = PresetInstrument(p);
    }
    SEQLIB_REQUIRE_EQ(presets.size(), 3);
    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({1, 1}) && presets.count({0, 4}));
    SEQLIB_CHECK((presets[{0, 1}] != presets[{1, 1}]));

    // Each program change selects its preset's bank, and track 9 plays on channel 11, away from the drum channel.
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    using Slots = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK(a.Find(0x99).empty() && a.Find(0x9A).size() == 1);
    SEQLIB_CHECK(Selections(a, 0) == (Slots{{0, 1}}));
    SEQLIB_CHECK(Selections(a, 10) == (Slots{{0, 4}}));
    SEQLIB_CHECK(Selections(b, 0) == (Slots{{1, 1}, {0, 4}}));
}

void TestSharedPresets()
{
    // 129 instruments that songs play as program 5, which fill its 128 banks, and then another as program 0.
    // SharedPreset() doesn't look at the instruments, the ROM or the driver, so they needn't exist.
    const Rom rom = Cart().ToRom();
    const DriverInfo info;
    SoundfontBuilder sf(rom, info);
    std::vector<BankProgram> slots;

    for (int i = 0; i < 129; i++)
    {
        slots.push_back(sf.SharedPreset(5, i));
    }
    const BankProgram again = sf.SharedPreset(5, 128);
    const BankProgram next = sf.SharedPreset(0, 200);

    // The 129th gets the first free program of bank 0, which it keeps, and the next one as program 0 gets bank 1.
    SEQLIB_CHECK(slots[0].bank == 0 && slots[0].program == 5);
    SEQLIB_CHECK(slots[127].bank == 127 && slots[127].program == 5);
    SEQLIB_CHECK(slots[128].bank == 0 && slots[128].program == 0);
    SEQLIB_CHECK(again.bank == 0 && again.program == 0);
    SEQLIB_CHECK(next.bank == 1 && next.program == 0);
    SEQLIB_REQUIRE_EQ(sf.File().presets.size(), 130);
}

void TestSharedDrumChannel()
{
    // Two songs whose 16 tracks all play, one after another, so track 9 has no free channel to move to, with different
    // voices as program 2.
    Cart cart(kMode, {16});
    const uint32_t first_group = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(70)))}});
    const uint32_t second_group = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(90)))}});
    std::vector<Track> tracks;
    for (int t = 0; t < 16; t++)
    {
        tracks.push_back(Track().Wait(24 * t).Cmd(kCmdVoice, 2).Note(24, 60, 100).Wait(24).Fine());
    }
    cart.Song(tracks, first_group);
    cart.Song(tracks, second_group);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("drums");
    SoundfontBuilder shared(rom, info);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport second = ConvertSong(rom, info, 1, opt, &shared);

    auto warns = [](const SongReport& s)
    {
        return std::any_of(s.warnings.begin(), s.warnings.end(),
                           [](const std::string& w) { return w.find("drum bank") != std::string::npos; });
    };

    // Track 9 stays on the drum channel and selects bank 0 there. The first song's voice gets a copy in the drum bank,
    // and the second song's, which can't have one, gets a warning.
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    using Slots = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK(first.ok && second.ok);
    SEQLIB_CHECK(a.Find(0x99).size() == 1 && b.Find(0x99).size() == 1);
    SEQLIB_CHECK(Selections(a, 9) == (Slots{{0, 2}}) && Selections(b, 9) == (Slots{{0, 2}}));
    SEQLIB_CHECK(Selections(b, 0) == (Slots{{1, 2}}));
    int drum_presets = 0;
    for (const sf2::Preset& p : shared.File().presets)
    {
        drum_presets += p.bank == 128;
        SEQLIB_CHECK(p.bank != 128 ||
                     (p.program == 2 && PresetInstrument(p) == PresetInstrument(shared.File().presets[0])));
    }
    SEQLIB_CHECK_EQ(drum_presets, 1);
    SEQLIB_CHECK(!warns(first) && warns(second));
}

void TestDrumChannel()
{
    // Two songs whose tracks play one after another. In song 0, track 8 only ends. In song 1, all 16 tracks play.
    Cart cart(kMode, {16});
    const uint32_t voices = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(70)))}});
    for (int song = 0; song < 2; song++)
    {
        std::vector<Track> tracks;
        for (int t = 0; t < 16; t++)
        {
            Track track;
            if (song == 1 || t != 8)
            {
                track.Wait(24 * t).Cmd(kCmdVoice, 2).Note(24, 60, 100).Wait(24);
            }
            tracks.push_back(track.Fine());
        }
        cart.Song(tracks, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("drum_channel");

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    // In song 0, a track plays on every channel after the drum channel. Track 9 moves to channel 9, the last free one
    // before it. Its voice gets no copy in the drum bank. In song 1, track 9 stays on the drum channel. Its voice gets
    // a copy in the drum bank there.
    auto preset_list = [](const std::string& path)
    {
        const Sf2Records sf = ReadSf2(path);
        std::set<std::pair<int, int>> presets;
        for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
        {
            const uint8_t* h = sf.Record("phdr", 38, p);
            presets.insert({Le16(h + 22), Le16(h + 20)});
        }
        return presets;
    };
    using Presets = std::set<std::pair<int, int>>;
    SEQLIB_REQUIRE(first.ok && second.ok);
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    SEQLIB_CHECK(a.Find(0x99).empty() && a.Find(0x98).size() == 1);
    SEQLIB_CHECK(b.Find(0x99).size() == 1);
    SEQLIB_CHECK(preset_list(first.sf2_path) == (Presets{{0, 2}}));
    SEQLIB_CHECK(preset_list(second.sf2_path) == (Presets{{0, 2}, {128, 2}}));
}

void TestVoiceChannels()
{
    // Track 0 plays a note with a slow release and turns its volume down while the note is held, and again while it
    // fades. Track 1 then plays a chord of 5 notes, with another voice, and the last of them takes the channel of track
    // 0's note, which is still fading.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {2, Cart::Voice(0, cart.Wave(Saw(80), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 60, 100)
                   .Wait(6)
                   .Cmd(kCmdVol, 80)
                   .Wait(12)
                   .Cmd(kCmdVol, 50)
                   .Wait(30)
                   .Fine(),
               Track()
                   .Wait(24)
                   .Cmd(kCmdVoice, 2)
                   .Cmd(kCmdVol, 100)
                   .Note(24, 62, 100)
                   .Note(24, 64, 100)
                   .Note(24, 65, 100)
                   .Note(24, 67, 100)
                   .Note(24, 69, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voices");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A track for each of the driver's 5 channels, named after it, after the conductor track.
    SEQLIB_CHECK(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 6);
    std::vector<std::string> names;
    for (size_t t = 1; t < m.tracks.size(); t++)
    {
        const std::vector<uint8_t>& e = m.tracks[t].events.front().bytes;
        names.push_back(std::string(e.begin() + 3, e.end()));
    }
    SEQLIB_CHECK((names == std::vector<std::string>{"DirectSound 1", "DirectSound 2", "DirectSound 3", "DirectSound 4",
                                                    "DirectSound 5"}));

    // The first channel's events, without its track's meta events.
    std::vector<seqlib::midi::Event> first;
    for (const auto& e : m.tracks[1].events)
    {
        if (e.bytes[0] < 0xF0)
        {
            first.push_back(e);
        }
    }

    // Returns where an event comes among them, or -1 if it isn't there.
    auto find = [&](uint32_t tick, std::vector<uint8_t> bytes)
    {
        for (size_t i = 0; i < first.size(); i++)
        {
            if (first[i].tick == tick * kMidiTicks && first[i].bytes == bytes)
            {
                return int(i);
            }
        }

        return -1;
    };

    // On the first channel, track 0's note is released at tick 12. The note follows its track's volume before and
    // after that, at the centre: volumes of 127, 80 and 50 give CC11 127, 100 and 79. At tick 24 an All Sound Off cuts
    // off its release. The channel then takes track 1's voice and volume for the chord's last note. A velocity of 100
    // is written as 127 × √(100 / 127), rounded: 113.
    SEQLIB_CHECK(find(0, {0xC0, 1}) >= 0 && find(0, {0x90, 60, 113}) >= 0 && find(12, {0x80, 60, 0}) >= 0);
    SEQLIB_CHECK(find(0, {0xB0, cc::kVolume, 127}) >= 0 && find(0, {0xB0, cc::kPan, 64}) >= 0);
    SEQLIB_CHECK(find(0, {0xB0, cc::kExpression, 127}) >= 0 && find(6, {0xB0, cc::kExpression, 100}) >= 0 &&
                 find(18, {0xB0, cc::kExpression, 79}) >= 0);
    const int cut = find(24, {0xB0, cc::kAllSoundOff, 0});
    const int chord = find(24, {0x90, 69, 113});
    SEQLIB_CHECK(cut >= 0 && chord > cut);
    SEQLIB_CHECK(find(24, {0xC0, 2}) >= 0 && find(24, {0xB0, cc::kExpression, 112}) >= 0);

    // The chord's other notes take the 4 free channels.
    for (uint8_t ch = 1; ch < 5; ch++)
    {
        const auto ons = m.Find(uint8_t(0x90 | ch));
        SEQLIB_CHECK(ons.size() == 1 && ons[0].tick == 24 * kMidiTicks);
    }
}

void TestVoiceChannelsDrums()
{
    // A sample voice for each of the driver's 12 DirectSound channels, and a voice for each of its 4 PSG channels.
    Cart cart(0x0094CC00, {16});
    std::map<int, std::vector<uint8_t>> voice_map;
    for (int v = 0; v < 12; v++)
    {
        voice_map[v] = Cart::Voice(0, cart.Wave(Saw(size_t(40 + 4 * v)), 13379, 0));
    }
    voice_map[12] = Cart::Voice(1, 2, 60, {0, 0, 15, 0});
    voice_map[13] = Cart::Voice(2, 2, 60, {0, 0, 15, 0});
    voice_map[14] = Cart::Voice(3, cart.Place(std::vector<uint8_t>(16, 0x5A)), 60, {0, 0, 15, 0});
    voice_map[15] = Cart::Voice(4, 0, 60, {0, 0, 15, 0});
    const uint32_t voices = cart.VoiceGroup(voice_map);

    // A song whose 16 tracks play them all at once, so the 16th channel, the noise channel, gets the drum channel.
    std::vector<Track> tracks;
    for (int t = 0; t < 16; t++)
    {
        tracks.push_back(Track().Cmd(kCmdVoice, t).Cmd(kCmdVol, 127).Note(24, 60, 100).Wait(24).Fine());
    }
    cart.Song(tracks, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voice_drums");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The noise note plays on channel 10, and its voice has a copy in the drum bank.
    SEQLIB_CHECK(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 17);
    const auto drum_notes = m.Find(0x99);
    SEQLIB_CHECK(drum_notes.size() == 1 && drum_notes[0].bytes[1] == 60);
    const Sf2Records sf = ReadSf2(r.sf2_path);
    bool drum_copy = false;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        drum_copy = drum_copy || (Le16(h + 22) == 128 && Le16(h + 20) == 15);
    }
    SEQLIB_CHECK(drum_copy);
}

void TestVoiceChannelsLoop()
{
    // Two voices that play on square channel 1.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}, {2, Cart::Voice(1, 1, 60, {0, 0, 15, 0})}});

    // Track 0 plays program 1 at volume 100 on tick 24, and every 48 ticks after that, looping from the start. Track 1
    // plays program 2 at volume 60 on ticks 0 and 12, and every 48 ticks after that, looping from tick 6. So the loop
    // runs from tick 6 to tick 54, and ends with track 0's settings on the channel.
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100);
    const size_t first_loop = first.Here();
    first.Wait(24).Note(12, 48, 100).Wait(24).Goto(first_loop);
    Track second;
    second.Cmd(kCmdVoice, 2).Cmd(kCmdVol, 60).Note(12, 52, 100).Wait(6);
    const size_t second_loop = second.Here();
    second.Wait(6).Note(12, 52, 100).Wait(42).Goto(second_loop);
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voice_loop");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop's first note, at tick 12, sets the channel's program, CC10, CC11 and pitch bend again. The note before
    // it left them the same. A player that jumps back to the loop's start comes from track 0's note. Both tracks play
    // at the centre. CC11 is 87 for volume 60 and 112 for volume 100.
    using Events = std::vector<seqlib::midi::Event>;
    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(AtFrames(r.loop_start, 6));
    SEQLIB_CHECK(AtFrames(r.loop_end, 54));
    const MidiEvents m = ReadMidi(r.midi_path);
    Events levels;
    for (const auto& e : m.Find(0xB0))
    {
        if (e.bytes[1] == cc::kPan || e.bytes[1] == cc::kExpression)
        {
            levels.push_back(e);
        }
    }
    SEQLIB_CHECK((m.Find(0xC0) == Events{{0, {0xC0, 2}},
                                         {12 * kMidiTicks, {0xC0, 2}},
                                         {24 * kMidiTicks, {0xC0, 1}},
                                         {60 * kMidiTicks, {0xC0, 2}},
                                         {72 * kMidiTicks, {0xC0, 1}}}));
    SEQLIB_CHECK((levels == Events{{0, {0xB0, cc::kPan, 64}},
                                   {0, {0xB0, cc::kExpression, 87}},
                                   {12 * kMidiTicks, {0xB0, cc::kPan, 64}},
                                   {12 * kMidiTicks, {0xB0, cc::kExpression, 87}},
                                   {24 * kMidiTicks, {0xB0, cc::kExpression, 112}},
                                   {60 * kMidiTicks, {0xB0, cc::kExpression, 87}},
                                   {72 * kMidiTicks, {0xB0, cc::kExpression, 112}}}));
    SEQLIB_CHECK((m.Find(0xE0) == Events{{12 * kMidiTicks, {0xE0, 0x00, 0x40}}}));
}

// Returns the zones of the instrument that each preset of bank 0 plays, by the preset's program: for each zone after
// the global zone, its key range and its exclusive class, or 0 for none.
std::map<int, std::vector<std::pair<int, int>>> ExclusiveClasses(const Sf2Records& sf)
{
    std::map<int, std::vector<std::pair<int, int>>> out;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        if (Le16(h + 22) != 0)
        {
            continue;
        }

        const size_t instrument = Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
        const size_t first = Le16(sf.Record("inst", 22, instrument) + 20);
        const size_t end = Le16(sf.Record("inst", 22, instrument + 1) + 20);
        for (size_t z = first + 1; z < end; z++)
        {
            const std::map<uint16_t, uint16_t> gens = sf.ZoneGens(z);
            const auto found = gens.find(sf2::kExclusiveClass);
            out[Le16(h + 20)].push_back({gens.at(sf2::kKeyRange), found != gens.end() ? found->second : 0});
        }
    }

    return out;
}

// Returns true if each zone in `zones` has the exclusive class `expected`.
bool AllOfClass(const std::vector<std::pair<int, int>>& zones, int expected)
{
    return !zones.empty() &&
           std::all_of(zones.begin(), zones.end(), [&](const auto& zone) { return zone.second == expected; });
}

// A test cartridge, and the driver that detection finds in it.
struct ExclusiveSong
{
    Rom rom;
    DriverInfo info;
};

// Returns a cartridge whose song plays programs 0 to 9: a voice of each PSG channel with a slow release, a square 1
// voice with the fixed-frequency bit, two square 1 voices with quick releases, a sample with a slow release, a drum kit
// and a key split.
ExclusiveSong MakeExclusiveSong()
{
    Cart cart;
    const uint32_t wave = cart.Place(std::vector<uint8_t>(16, 0x5A));
    const uint32_t sample = cart.Wave(Saw(), 13379, 0);
    const uint32_t kit = cart.VoiceGroup({{36, Cart::Voice(4, 0, 50, {0, 0, 15, 3})},
                                          {38, Cart::Voice(4, 0, 50, {0, 0, 15, 0})},
                                          {40, Cart::Voice(0, sample, 60, {255, 0, 255, 250})}});
    const uint32_t split = cart.VoiceGroup({{0, Cart::Voice(2, 1, 60, {0, 0, 15, 5})}, {1, Cart::Voice(0, sample)}});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 7})},
                                             {1, Cart::Voice(2, 2, 60, {0, 0, 15, 1})},
                                             {2, Cart::Voice(3, wave, 60, {0, 0, 15, 4})},
                                             {3, Cart::Voice(4, 0, 60, {0, 0, 15, 2})},
                                             {4, Cart::Voice(1 | kVoiceFixed, 2, 60, {0, 0, 15, 7})},
                                             {5, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {6, Cart::Voice(1, 2, 60, {0, 0, 2, 1})},
                                             {7, Cart::Voice(0, sample, 60, {255, 0, 255, 250})},
                                             {8, Cart::Voice(kVoiceDrumKit, kit)},
                                             {9, cart.KeySplit(split, 59)}});

    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 127);
    for (int v = 0; v < 8; v++)
    {
        t.Cmd(kCmdVoice, v).Note(12, 60, 100).Wait(24);
    }
    t.Cmd(kCmdVoice, 8).Note(12, 36, 100).Wait(24).Note(12, 38, 100).Wait(24).Note(12, 40, 100).Wait(24);
    t.Cmd(kCmdVoice, 9).Note(12, 50, 100).Wait(24).Note(12, 70, 100).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();

    return {rom, Detect(rom)};
}

// With --psg-exclusive, each zone of a PSG voice whose release is slower than FluidSynth's fade for an exclusive class
// gets the class of its channel: 1 and 2 for the squares, 3 for the wave and 4 for the noise. The fixed-frequency bit
// doesn't change the channel. A PSG voice with a quicker release and a sample voice get no class. The MIDI file stays
// the same, and the SoundFont without the option has no class.
void TestPsgExclusive()
{
    const ExclusiveSong song = MakeExclusiveSong();
    ConvertSettings plain = TempSettings("exclusive_plain");
    ConvertSettings exclusive = TempSettings("exclusive");
    exclusive.psg_exclusive = true;

    const SongReport a = ConvertSong(song.rom, song.info, 0, plain, nullptr);
    const SongReport b = ConvertSong(song.rom, song.info, 0, exclusive, nullptr);

    SEQLIB_CHECK(a.ok && b.ok);
    SEQLIB_CHECK(ReadAll(a.midi_path) == ReadAll(b.midi_path));
    const auto without = ExclusiveClasses(ReadSf2(a.sf2_path));
    const auto with = ExclusiveClasses(ReadSf2(b.sf2_path));
    SEQLIB_REQUIRE_EQ(without.size(), 10);
    SEQLIB_REQUIRE_EQ(with.size(), 10);
    for (const auto& [program, zones] : without)
    {
        SEQLIB_CHECK(AllOfClass(zones, 0));
    }

    SEQLIB_CHECK(AllOfClass(with.at(0), 1));
    SEQLIB_CHECK(AllOfClass(with.at(1), 2));
    SEQLIB_CHECK(AllOfClass(with.at(2), 3));
    SEQLIB_CHECK(AllOfClass(with.at(3), 4));
    SEQLIB_CHECK(AllOfClass(with.at(4), 1));
    SEQLIB_CHECK(AllOfClass(with.at(5), 0));
    SEQLIB_CHECK(AllOfClass(with.at(6), 0));
    SEQLIB_CHECK(AllOfClass(with.at(7), 0));

    // The drum kit's slow noise on key 36 has the class, and its quick noise and its sample don't. The key split's
    // square 2 has class 2 on the keys up to 59, and its sample has none. The square's zones from key 36 tune runs of
    // keys to the driver's frequency settings.
    using Zones = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK((with.at(8) == Zones{{0x2424, 4}, {0x2626, 0}, {0x2828, 0}}));
    const Zones& split = with.at(9);
    SEQLIB_REQUIRE(split.size() >= 3);
    SEQLIB_CHECK((split.front() == std::pair{0x2300, 2}));
    SEQLIB_CHECK((split.back() == std::pair{0x7F3C, 0}));
    int next_key = 36;
    for (size_t z = 1; z + 1 < split.size(); z++)
    {
        SEQLIB_CHECK_EQ(split[z].first & 0xFF, next_key);
        SEQLIB_CHECK_EQ(split[z].second, 2);
        next_key = (split[z].first >> 8) + 1;
    }
    SEQLIB_CHECK_EQ(next_key, 60);
}

// The exclusive classes go in a shared SoundFont too. They also go in the SoundFont of a conversion with a MIDI channel
// for each of the driver's sound channels. The option doesn't change that conversion's MIDI file either.
void TestPsgExclusiveShared()
{
    const ExclusiveSong song = MakeExclusiveSong();
    ConvertSettings exclusive = TempSettings("exclusive_shared");
    exclusive.psg_exclusive = true;
    ConvertSettings voices = TempSettings("exclusive_voices_plain");
    voices.voice_channels = true;
    ConvertSettings exclusive_voices = TempSettings("exclusive_voices");
    exclusive_voices.voice_channels = true;
    exclusive_voices.psg_exclusive = true;
    SoundfontBuilder shared(song.rom, song.info, true);
    const std::string shared_path = Utf8(TempPath("exclusive_shared.sf2"));
    std::string error;

    const SongReport s = ConvertSong(song.rom, song.info, 0, exclusive, &shared);
    const bool written = shared.File().Write(shared_path, error);
    const SongReport v = ConvertSong(song.rom, song.info, 0, voices, nullptr);
    const SongReport w = ConvertSong(song.rom, song.info, 0, exclusive_voices, nullptr);

    SEQLIB_CHECK(s.ok && written && v.ok && w.ok);
    SEQLIB_CHECK(ReadAll(v.midi_path) == ReadAll(w.midi_path));
    for (const std::string& path : {shared_path, w.sf2_path})
    {
        const auto classes = ExclusiveClasses(ReadSf2(path));
        SEQLIB_REQUIRE_EQ(classes.size(), 10);
        SEQLIB_CHECK(AllOfClass(classes.at(0), 1) && AllOfClass(classes.at(3), 4) && AllOfClass(classes.at(5), 0));
    }
}

// For each preset of bank 0, by program, returns one flag for each zone of the instrument that the preset plays, after
// the global zone. A flag is true if its zone holds a modulator from CC91 to the reverb send with an amount of 0.
std::map<int, std::vector<bool>> DryZones(const Sf2Records& sf)
{
    std::map<int, std::vector<bool>> out;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        if (Le16(h + 22) != 0)
        {
            continue;
        }

        const size_t instrument = Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
        const size_t first = Le16(sf.Record("inst", 22, instrument) + 20);
        const size_t end = Le16(sf.Record("inst", 22, instrument + 1) + 20);
        for (size_t z = first + 1; z < end; z++)
        {
            bool dry = false;
            for (size_t m = Le16(sf.Record("ibag", 4, z) + 2); m < Le16(sf.Record("ibag", 4, z + 1) + 2); m++)
            {
                const uint8_t* mod = sf.Record("imod", 10, m);
                dry =
                    dry || (Le16(mod) == (sf2::kController | 91) && Le16(mod + 2) == sf2::kReverbEffectsSend &&
                            Le16(mod + 4) == 0 && Le16(mod + 6) == sf2::kNoController && Le16(mod + 8) == sf2::kLinear);
            }
            out[Le16(h + 20)].push_back(dry);
        }
    }

    return out;
}

// Every zone of a PSG voice holds kNoReverb: the default modulator from CC91 to the reverb send, at an amount of 0. A
// sample's zone keeps the default.
void TestPsgNoReverb()
{
    const ExclusiveSong song = MakeExclusiveSong();
    const SongReport r = ConvertSong(song.rom, song.info, 0, TempSettings("no_reverb"), nullptr);

    SEQLIB_CHECK(r.ok);
    const auto dry = DryZones(ReadSf2(r.sf2_path));
    SEQLIB_REQUIRE_EQ(dry.size(), 10);
    for (int program = 0; program < 7; program++)
    {
        const std::vector<bool>& zones = dry.at(program);
        SEQLIB_CHECK(!zones.empty() && std::all_of(zones.begin(), zones.end(), [](bool d) { return d; }));
    }
    SEQLIB_CHECK(!dry.at(7).empty() && std::none_of(dry.at(7).begin(), dry.at(7).end(), [](bool d) { return d; }));

    // In the drum kit, the zones of both noises are dry. The sample's zone isn't. In the key split, the zones of
    // square 2 cover the keys up to 59 and are dry. The sample's zone isn't.
    SEQLIB_CHECK((dry.at(8) == std::vector<bool>{true, true, false}));
    const std::vector<bool>& split = dry.at(9);
    SEQLIB_CHECK(split.size() >= 3 && std::all_of(split.begin(), split.end() - 1, [](bool d) { return d; }) &&
                 !split.back());
}

// A compressed sample decodes to its first point and then a step from the table for each of the others: from the low
// half of the second byte, and then from the high and low halves of each byte after it.
void TestCompressedSample()
{
    // One block: the first point is 10, the second byte's low half (2) adds 4, the third byte's halves (1 and 8) add 1
    // and -64, and the rest add 0. The second byte's high half isn't used.
    Cart cart;
    std::vector<uint8_t> block(33, 0);
    block[0] = 10;
    block[1] = 0xF2;
    block[2] = 0x18;
    const uint32_t wave = cart.CompressedWave(block, 64);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, wave)}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("compressed");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The sample holds the master volume of 12 too, at 13/16, so each of its points is 208 times the decoded one.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    const uint32_t start = test::Le32(sf.Record("shdr", 46, 0) + 20);
    const auto point = [&](uint32_t i)
    {
        return int16_t(Le16(&pcm[2 * (start + i)])) / (256 * 13 / 16);
    };

    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(point(0) == 10 && point(1) == 14 && point(2) == 15 && point(3) == -49 && point(63) == -49);
}

void TestDroppedNotes()
{
    // With one DirectSound channel, the second of two notes at once is left out of the MIDI file, as in the game. After
    // a tempo command of 150, a frame plays two ticks, so a note of one tick that starts on a frame's first tick is
    // released before the mixer has started it. It never sounds, and it's left out too.
    Cart cart(0x00940100);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 150)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 100)
                   .Wait(25)
                   .Note(1, 64, 100)
                   .Wait(24)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 62, 100).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("dropped");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK(r.ok && r.warnings.size() == 1 && r.warnings[0].find("1 note had no available sound channel") == 0);
    SEQLIB_CHECK(m.Find(0x90).size() == 1 && m.Find(0x91).empty());
}

void TestDoubledNote()
{
    // Two notes of one key that a track starts on one tick make one MIDI note, which goes on as long as either of them
    // sounds. After a tempo command of 150, the note of one tick is released before the mixer has started it, but the
    // note of 24 ticks still plays.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 11025, 32), 60, {255, 0, 255, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 150)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 100)
                   .Wait(1)
                   .Note(1, 60, 100)
                   .Note(24, 60, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("doubled");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    const auto ons = m.Find(0x90);
    const auto offs = m.Find(0x80);
    SEQLIB_CHECK(ons.size() == 1 && offs.size() == 1);
    if (ons.size() == 1 && offs.size() == 1)
    {
        SEQLIB_CHECK(ons[0].tick == kMidiTicks && ons[0].bytes[1] == 60);
        SEQLIB_CHECK_EQ(offs[0].tick, 25 * kMidiTicks);
    }
}

// Returns the microseconds a quarter note lasts when a tick takes `frames` frames.
uint32_t Micros(double frames)
{
    return uint32_t(std::lround(1e6 * 24 * frames / kFrameRate));
}

void TestTempoChange()
{
    // At a tempo of 200, a frame adds 200 to the tick counter. Tick 2 runs in frame 2 with 100 of the counter left
    // over, so a change to a tempo of 50 there makes tick 3 come a frame later, in frame 3, rather than 3 frames later.
    // The MIDI file's tick 2 takes the 100 at the old tempo and the other 50 at the new one: 1.5 frames.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 100)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Wait(2)
                   .Cmd(kCmdTempo, 25)
                   .Wait(1)
                   .Note(1, 60, 100)
                   .Wait(2)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("tempo");

    Sequencer seq(rom, info, 0);
    const std::vector<Action> notes = Actions(seq, 10, Action::kNoteOn);
    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(notes.size() == 1 && notes[0].tick == 3 && notes[0].frame == 3);
    const std::vector<std::pair<uint32_t, uint32_t>> kTempos = {
        {0, Micros(0.75)}, {2 * kMidiTicks, Micros(1.5)}, {3 * kMidiTicks, Micros(3)}};
    SEQLIB_CHECK(Tempos(ReadMidi(r.midi_path)) == kTempos);
    SEQLIB_CHECK(AtFrames(r.seconds, 2 * 0.75 + 1.5 + 2 * 3));
}

void TestPsgPitch()
{
    // A noise note bent up 3 semitones plays the noise setting 3 keys up, 1.6 times as fast, so the MIDI file bends it
    // by 8.14 semitones, with a range of 9. A square note at key 38 bent down 4 semitones plays at key 36, the lowest,
    // so the MIDI file bends it by 2.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(4, 0, 60, {0, 0, 15, 0})}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 12)
                   .Tie(60, 127)
                   .Wait(1)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Eot(60)
                   .Fine(),
               Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 8)
                   .Tie(38, 127)
                   .Wait(1)
                   .Cmd(kCmdBend, 0x20)
                   .Wait(4)
                   .Eot(38)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_pitch");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    std::map<int, int> ranges;
    for (int ch = 0; ch < 2; ch++)
    {
        for (const auto& e : m.Find(uint8_t(0xB0 | ch)))
        {
            ranges[ch] = e.bytes[1] == cc::kDataEntry ? e.bytes[2] : ranges[ch];
        }
    }

    const auto noise = m.Find(0xE0);
    const auto square = m.Find(0xE1);
    const int noise_bend = noise.empty() ? 0 : noise.back().bytes[1] | (noise.back().bytes[2] << 7);
    const int square_bend = square.empty() ? -1 : square.back().bytes[1] | (square.back().bytes[2] << 7);
    const int kExpected = 8192 + int(std::lround(256.0 * 12.0 * std::log2(1.6) * 8192 / (9 * 256)));

    SEQLIB_CHECK(ranges[0] == 9 && ranges[1] == 2);
    SEQLIB_CHECK(std::abs(noise_bend - kExpected) <= 1);
    SEQLIB_CHECK_EQ(square_bend, 0);
}

void TestPsgRelease()
{
    // A noise note bent up 3 semitones plays the noise setting 3 keys up, which the MIDI file bends up 8.14 semitones,
    // with a range of 9. Its release goes on at that pitch after the note ends on tick 6, so the bend stays there until
    // the track's next note, a sample on tick 24, which plays with the track's bend of 3 semitones.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(4, 0, 60, {0, 0, 15, 4})}, {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 12)
                   .Cmd(kCmdBend, 0x50)
                   .Note(6, 60, 127)
                   .Wait(24)
                   .Cmd(kCmdVoice, 1)
                   .Note(12, 62, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_release");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto bends = ReadMidi(r.midi_path).Find(0xE0);
    const int kNoise = 8192 + int(std::lround(256.0 * 12.0 * std::log2(1.6) * 8192 / (9 * 256)));
    const int kSample = 8192 + int(std::lround(768.0 * 8192 / (9 * 256)));

    SEQLIB_CHECK(r.ok);
    SEQLIB_REQUIRE_EQ(bends.size(), 2);
    SEQLIB_CHECK(bends[0].tick == 0 && bends[1].tick == 24 * kMidiTicks);
    SEQLIB_CHECK(std::abs((bends[0].bytes[1] | (bends[0].bytes[2] << 7)) - kNoise) <= 1);
    SEQLIB_CHECK((bends[1].bytes[1] | (bends[1].bytes[2] << 7)) == kSample);
}

void TestQuietPsg()
{
    // A PSG note's level is its two volumes added up and divided by 16. At a velocity of 32 and a track volume of 31,
    // that rounds to 0, so the note never sounds and the MIDI file leaves it out. At a velocity of 96 it sounds.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 31)
                   .Note(12, 60, 32)
                   .Wait(12)
                   .Note(12, 62, 96)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("quiet");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto ons = ReadMidi(r.midi_path).Find(0x90);
    SEQLIB_CHECK(ons.size() == 1 && ons[0].tick == 12 * kMidiTicks && ons[0].bytes[1] == 62);
}

void TestDump()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0);
    const size_t loop = t.Here();
    t.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const std::string path = Utf8(TempPath("mp2k_dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("TEMPO 75 (150 BPM)") != std::string::npos);
    SEQLIB_CHECK(s.find("N24 key 60 vel 100") != std::string::npos);
    SEQLIB_CHECK(s.find("      24  B2 ") != std::string::npos);
}

// OpenMusic() finds MP2K from its code. A game without the code is read with the driver only when --driver mp2k asks
// for it, with the song table given. An entry whose header has no tracks isn't a song.
void TestMusic()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({}, voices);
    const uint32_t header = cart.Song({Track().Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    Cart bare(kMode, {10}, false);
    bare.Song({Track().Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine()}, voices);
    const Rom bare_rom = bare.ToRom();
    Overrides table;
    table.song_table = kSongTable;
    Overrides forced = table;
    forced.driver = Driver::kMp2k;
    std::string error, bare_error, forced_error;

    const std::unique_ptr<Music> music = agbsf2::OpenMusic(rom, Overrides(), error);
    const std::unique_ptr<Music> bare_music = agbsf2::OpenMusic(bare_rom, table, bare_error);
    const std::unique_ptr<Music> bare_forced = agbsf2::OpenMusic(bare_rom, forced, forced_error);

    SEQLIB_CHECK(music && music->Log().front() == "MP2K, Nintendo's MusicPlayer2000 sound driver");
    SEQLIB_CHECK(music && music->SongCount() == 2 && !music->HasSong(0) && music->HasSong(1));
    SEQLIB_CHECK(music && music->InspectSong(1, ConvertSettings()).address == header);
    SEQLIB_CHECK(!bare_music);
    SEQLIB_CHECK(bare_forced && bare_forced->SongCount() == 1);
}

// Camelot's version of the driver has an unused copy of the routine that starts a song, whose literal pool is zeroed,
// before the one the game uses. Its note start and mixer have changes of their own, which detection finds by their
// code.
void TestCamelotDetection()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
    cart.Song({Track().Fine()}, voices);
    Cart plain;
    plain.Song({Track().Fine()}, voices);

    const DriverInfo info = Detect(cart.ToRom());
    const DriverInfo plain_info = Detect(plain.ToRom());

    SEQLIB_CHECK_EQ(info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.sound_mode, kMode);
    SEQLIB_CHECK(info.camelot_sequencer && info.camelot_mixer);
    SEQLIB_CHECK(!plain_info.camelot_sequencer && !plain_info.camelot_mixer);
}

// Camelot's note start takes the third free channel, or the last free one when there are only one or two, unless a
// released note of the same track has a channel. It works out the left side's level from 128 rather than 127, and
// rounds a fine-tuned key's rate a little differently. Its LFO counts its delay down at a speed of 0 too.
void TestCamelotSequencer()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 127)
                   .Tie(62, 127)
                   .Tie(64, 127)
                   .Tie(65, 127)
                   .Wait(1)
                   .Eot(60)
                   .Wait(1)
                   .Tie(67, 127)
                   .Wait(1)
                   .Tie(69, 127)
                   .Wait(10)
                   .Fine()},
              voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdMod, 30)
                   .Cmd(kCmdLfoDl, 4)
                   .Cmd(kCmdLfoS, 0)
                   .Tie(60, 127)
                   .Wait(2)
                   .Cmd(kCmdLfoS, 32)
                   .Wait(10)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<std::pair<int, int>> ons;
    int left = -1;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                ons.push_back({a.key, a.channel});
            }
        }
        if (f == 0)
        {
            left = seq.GetChannel(2).left;
        }
    }

    Sequencer lfo(rom, info, 1);
    std::vector<Action> pitches = Actions(lfo, 12, Action::kPitch);
    pitches.erase(std::remove_if(pitches.begin(), pitches.end(), [](const Action& a) { return a.value == 0; }),
                  pitches.end());

    // Of 5 channels, keys 60, 62 and 64 take the third free one each time, and 65 the last of the two left. Once 60 is
    // released, 67 takes its channel rather than the free channel 0, which 69 takes.
    const std::vector<std::pair<int, int>> kOns = {{60, 2}, {62, 3}, {64, 4}, {65, 1}, {67, 2}, {69, 0}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK_EQ(left, 125);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 11, 255), 836);
    SEQLIB_CHECK_EQ(CamelotKeyToFrequency(13379 * 1024, 11, 255), 835);
    SEQLIB_CHECK_EQ(CamelotKeyToFrequency(13379 * 1024, 60, 0), 13379);

    // The LFO's delay of 4 ticks has run out by tick 4, two ticks after its speed is set: a depth of 30 on the first
    // step of 32 bends the pitch by 15/16 of a semitone.
    SEQLIB_CHECK(!pitches.empty() && pitches[0].tick == 4 && pitches[0].value == 240);
}

// Camelot's mixer takes 256 less the release off the level each frame. It leaves out a note that comes out silent on
// both sides, which doesn't move on through its sample. It plays a sample of no length as a synth voice, which goes on
// until it's released: a pulse wave moves its duty on each frame, and a saw wave keeps its filter in the count.
void TestCamelotMixer()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x00, 0x00});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 200})},
                                             {1, Cart::Voice(0, pulse)},
                                             {2, Cart::Voice(0, saw)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(12).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 1).Tie(60, 1).Wait(12).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(60, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Tie(60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The note's level after each frame from its release, on frame 4, until it stops.
    Sequencer release(rom, info, 0);
    std::vector<int> levels;
    for (int f = 0; f < 9; f++)
    {
        release.Step();
        if (f >= 4)
        {
            levels.push_back(release.GetChannel(2).status ? release.GetChannel(2).envelope : -1);
        }
    }

    Sequencer silent(rom, info, 1);
    for (int f = 0; f < 3; f++)
    {
        silent.Step();
    }

    Sequencer synths(rom, info, 2);
    synths.Step();
    const int32_t saw_filter = synths.GetChannel(3).count;
    synths.Step();
    synths.Step();
    const std::vector<int16_t> saw_points = RenderSynth(rom, info, Synth::kSaw, saw + 16, 13379, 224);

    const std::vector<int> kLevels = {199, 143, 87, 31, -1};
    SEQLIB_CHECK(levels == kLevels);
    SEQLIB_CHECK(silent.GetChannel(2).status && silent.GetChannel(2).count == 64);

    // The pulse wave's phase moves on by 8 times a sample's step for each of the mixer's 224 points a frame, and its
    // duty's phase by its fourth byte. The saw wave's filter ends the frame where the SoundFont's sample does.
    const Channel& p = synths.GetChannel(2);
    SEQLIB_CHECK(p.status && p.synth && uint32_t(p.count) == 0x30000000u);
    SEQLIB_CHECK_EQ(p.fraction, uint32_t(3 * uint64_t((13379u * 627u) << 3) * 224 % 0x100000000u));
    SEQLIB_CHECK(synths.GetChannel(3).status && saw_points.size() == 224 && saw_points[223] == saw_filter * 128);
}

// A pulse or saw synth voice's SoundFont instrument has a zone for each key that the song plays it at, with a sample
// made for that key at the mixer's rate. A triangle wave's has one cycle for every key. The MIDI file's reverb is the
// echo of Camelot's mixer, whatever the song sets, and a PSG voice is quieter against the samples.
void TestCamelotConversion()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t pulse = cart.Synth(0, {0x80, 0x00, 0x00, 0x00});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup(
        {{0, Cart::Voice(0, pulse)}, {1, Cart::Voice(0, triangle)}, {2, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 60, 127)
                   .Wait(12)
                   .Note(12, 64, 127)
                   .Wait(12)
                   .Note(12, 60, 127)
                   .Wait(12)
                   .Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Note(12, 48, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Note(12, 72, 127).Wait(12).Fine()},
              voices, 0, 0, 0x80 | 10);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("camelot");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);
    const std::vector<int16_t> points = RenderSynth(rom, info, Synth::kPulse, pulse + 16, 13379, 64);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    int reverbs = 0;
    for (uint8_t ch = 0; ch < 3; ch++)
    {
        for (const auto& e : m.Find(uint8_t(0xB0 + ch)))
        {
            if (e.bytes[1] == cc::kReverb)
            {
                reverbs++;
                SEQLIB_CHECK_EQ(e.bytes[2], 53);
            }
        }
    }
    SEQLIB_CHECK_EQ(reverbs, 3);

    // The SoundFont's instrument for each program.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<int, int> instrument;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        instrument[Le16(h + 20)] = Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
    }

    // Each zone's key range, its sample's name, rate and root key, and its attenuation.
    auto zones = [&](int program)
    {
        const size_t first = Le16(sf.Record("inst", 22, size_t(instrument[program])) + 20);
        const size_t last = Le16(sf.Record("inst", 22, size_t(instrument[program]) + 1) + 20);
        std::vector<std::map<uint16_t, uint16_t>> out;
        for (size_t z = first + 1; z < last; z++)
        {
            out.push_back(sf.ZoneGens(z));
        }

        return out;
    };

    auto sample_name = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        const uint8_t* h = sf.Record("shdr", 46, zone.at(sf2::kSampleId));
        return std::string(reinterpret_cast<const char*>(h), std::find(h, h + 20, 0) - h);
    };

    auto sample_rate = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        return test::Le32(sf.Record("shdr", 46, zone.at(sf2::kSampleId)) + 36);
    };

    auto root_key = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        return int(sf.Record("shdr", 46, zone.at(sf2::kSampleId))[40]);
    };

    // The names of the pulse wave's samples for keys 60 and 64, and of the triangle wave's.
    char pulse_60[32], pulse_64[32], triangle_name[32];
    std::snprintf(pulse_60, sizeof pulse_60, "Pulse %08X 60", unsigned(pulse));
    std::snprintf(pulse_64, sizeof pulse_64, "Pulse %08X 64", unsigned(pulse));
    std::snprintf(triangle_name, sizeof triangle_name, "Triangle %08X", unsigned(triangle));

    const auto pulse_zones = zones(0);
    SEQLIB_REQUIRE_EQ(pulse_zones.size(), 2);
    SEQLIB_CHECK(pulse_zones[0].at(sf2::kKeyRange) == 0x3C3C && sample_name(pulse_zones[0]) == pulse_60);
    SEQLIB_CHECK(pulse_zones[1].at(sf2::kKeyRange) == 0x4040 && sample_name(pulse_zones[1]) == pulse_64);
    SEQLIB_CHECK(sample_rate(pulse_zones[1]) == 13379 && root_key(pulse_zones[1]) == 64);
    SEQLIB_CHECK(!pulse_zones[0].count(sf2::kInitialAttenuation) && pulse_zones[0].count(sf2::kSampleModes));
    const auto triangle_zones = zones(1);
    SEQLIB_CHECK(triangle_zones.size() == 1 && triangle_zones[0].at(sf2::kKeyRange) == 0x7F00 &&
                 sample_name(triangle_zones[0]) == triangle_name && sample_rate(triangle_zones[0]) == 13379);
    // Camelot's mixer plays a sample at 9/8 of the SDK mixer's level, so the square's sample is that much quieter than
    // the SDK's, 6.3 dB and 1 dB more below a full-scale sample.
    const auto square_zones = zones(2);
    SEQLIB_CHECK(!square_zones.empty() && !square_zones[0].count(sf2::kInitialAttenuation));
    if (!square_zones.empty())
    {
        const double decibels = 20 * std::log10(sf.SamplePeak(square_zones[0].at(sf2::kSampleId)) / 32767.0);
        SEQLIB_CHECK(std::fabs(decibels + 6.3 + 20 * std::log10(9.0 / 8.0)) < 0.01);
    }

    // A pulse wave with a duty of 128/256 at the mixer's rate: 32 points high and 32 low, at half the full level.
    SEQLIB_CHECK(points.size() == 64 && points[0] == 16384 && points[31] == 16384 && points[32] == -16384 &&
                 points[63] == -16384);
}

} // namespace

void RunTests()
{
    TestDecoding();
    TestDetection();
    TestTiming();
    TestNotes();
    TestChannels();
    TestEnvelope();
    TestEnvelopeHold();
    TestPsgEnvelopeBytes();
    TestPitch();
    TestLevelReports();
    TestLoops();
    TestLoopStarts();
    TestLoopStartsAtSecondPass();
    TestLoopLevelsAtSecondPass();
    TestLoopLevelsWithDroppedNote();
    TestLoopLengths();
    TestLoopAfterTie();
    TestLoopSettingsAgain();
    TestHourLimit();
    TestConversion();
    TestLevels();
    TestMonoLevels();
    TestSlowAttackAtEnd();
    TestTwoVoicesInATick();
    TestSharedSoundfont();
    TestSharedPresets();
    TestSharedDrumChannel();
    TestDrumChannel();
    TestVoiceChannels();
    TestVoiceChannelsDrums();
    TestVoiceChannelsLoop();
    TestPsgExclusive();
    TestPsgExclusiveShared();
    TestPsgNoReverb();
    TestCompressedSample();
    TestDroppedNotes();
    TestDoubledNote();
    TestTempoChange();
    TestPsgPitch();
    TestPsgRelease();
    TestQuietPsg();
    TestDump();
    TestMusic();
    TestCamelotDetection();
    TestCamelotSequencer();
    TestCamelotMixer();
    TestCamelotConversion();
}

} // namespace agbsf2::mp2k
