// SPDX-License-Identifier: MIT
// The SoundFont of Nintendo R&D1's driver: its instrument records as presets.

#include "rd1/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gba.h"
#include "psg.h"
#include "rd1/driver.h"
#include "rom.h"
#include "sf2.h"

namespace agbsf2::rd1
{

namespace
{

// The envelope's full level.
constexpr uint32_t kFullLevel = 0x7F0000;

// The longest sample read, in points.
constexpr uint32_t kMaxSamplePoints = 1u << 24;

// The Game Boy's square duties: the steps of 8 that are high.
constexpr int kDutyHigh[4] = {1, 2, 4, 6};

// The points a square sample gives each of its 8 steps.
constexpr int kStepPoints = 8;

// The rate a noise sample is rendered at, and its length. Both games set SOUNDBIAS to 0x200. The GBA then samples the
// PSG's output 32768 times a second. All the noise's power therefore lies under 16384 Hz. A sample at a higher rate
// spreads the power wider, and a player at 44.1 or 48 kHz loses the top of it.
constexpr uint32_t kNoiseRate = 32768;
constexpr uint32_t kNoisePoints = kNoiseRate;
constexpr uint32_t kNoiseStep = 524288 / kNoiseRate; // the LFSR's clock with a divisor of 1, in steps a point

// The keys that the noise's table covers. The driver holds the key to this range (0x080F29D6).
constexpr int kNoiseLow = 0x15;
constexpr int kNoiseHigh = 0x50;

// The PSG's level at kPsgFullVolume against a sample voice at the mixer's full level, which the mixer writes to the
// FIFO at full scale. Under SOUNDCNT_L 0xBB77 and the PSG at 100%, volume 15 plays at 30/128 of a full-scale FIFO
// sample (psg.h).
constexpr double kPsgShare = kPsgFullVolume * kPsgLevelPerVolume / kFifoLevel;

// The PSG doesn't pass through the mixer, so the echo doesn't reach it. A PSG zone holds this in place of the default
// modulator from CC91 to the reverb send.
constexpr sf2::Modulator kNoReverb = {sf2::kController | 91, sf2::kReverbEffectsSend, 0, sf2::kNoController,
                                      sf2::kLinear};

// The driver doesn't bend a fixed-pitch sample or the noise. Their zones hold this in place of the global zone's
// modulator from the pitch wheel.
constexpr sf2::Modulator kNoBend = {sf2::kSrcPitchWheel, sf2::kFineTune, 0, sf2::kSrcPitchWheelSensitivity,
                                    sf2::kLinear};

// Returns the equal-tempered pitch of key `key` in Hz, with key 60 at middle C.
double KeyHz(int key)
{
    return kMiddleC * std::exp2((key - 60) / 12.0);
}

// Returns the cents by which the driver's pitch table puts a sample's key `key` off equal temperament, with the
// sample's root key `root` at its rate.
int SampleCents(const Rom& rom, const DriverInfo& info, int key, int root)
{
    const double ratio = double(KeyPitch(rom, info, key)) / KeyPitch(rom, info, root);
    return int(std::lround(1200 * std::log2(ratio) - 100.0 * (key - root)));
}

// Returns the cents by which the square or wave channel plays key `key` off equal temperament.
int PsgCents(const Rom& rom, const DriverInfo& info, int key)
{
    return SettingCents(PsgSetting(KeyPitch(rom, info, key)), KeyHz(key));
}

// Returns the noise's NR43 setting for key `key` of record `r`. The note-on keeps the noise's key in place of its pitch
// (0x080F21B6), and the update indexes the table by it (0x080F29D0).
uint8_t NoiseSetting(const Rom& rom, const DriverInfo& info, const InstrumentRecord& r, int key)
{
    const int index = std::clamp(key, kNoiseLow, kNoiseHigh);
    return uint8_t(rom.U8(info.noise_table + uint32_t(index - kNoiseLow)) | (r.psg_setting & ~7));
}

// Returns the SoundFont envelope of an instrument record. The driver's level moves in a straight line each frame: up by
// the attack from the start level, down by the decay to the sustain level, then down by the fall, and down by the
// release after the note's end. The release starts from `release_from`, or from the sustain level when it's negative.
VolumeEnvelope EnvelopeOf(const InstrumentRecord& r, double release_from)
{
    VolumeEnvelope env;
    if (r.start < kFullLevel)
    {
        const double frames = r.attack ? std::ceil(double(kFullLevel - r.start) / double(r.attack)) : 0;
        env.attack = r.attack ? Timecents(frames / kFrameRate) : kInstant;
    }

    // A decay to a sustain level reaches it in the same time. A level that keeps falling from there fades to silence,
    // in the time that the driver takes to reach it.
    const double sustain = std::min(1.0, double(r.sustain) / kFullLevel);
    const double decay_frames = r.decay && sustain < 1 ? (1 - sustain) * kFullLevel / double(r.decay) : 0;
    if (r.fall && sustain > 0)
    {
        env.decay = Timecents(FadeSeconds(decay_frames + sustain * kFullLevel / double(r.fall)));
        env.sustain = kSilent;
    }
    else if (sustain < 1)
    {
        env.sustain = sustain > 0 ? Centibels(sustain) : kSilent;
        if (!r.decay)
        {
            env.sustain = 0;
        }
        else if (sustain > 0)
        {
            env.decay = Timecents(decay_frames / kFrameRate * 1000.0 / env.sustain);
        }
        else
        {
            env.decay = Timecents(FadeSeconds(decay_frames));
        }
    }

    // The driver's release takes longer from a higher level. A note that ends before its decay reaches the sustain
    // level releases from above it. A release of 0 leaves the level where it is until the voice stops.
    const double from = release_from >= 0 ? release_from : sustain > 0 && !r.fall ? sustain : 1.0;
    env.release = r.release ? Timecents(FadeSeconds(from * kFullLevel / double(r.release))) : Timecents(100);
    return env;
}

// The SoundFont's samples, made once each.
class SampleCache
{
public:
    explicit SampleCache(const Rom& rom, Sf2File& file) : rom_(rom), file_(file)
    {
    }

    // Returns the sample of the header at `address`, or -1 if it can't be read. Sets `loops` if it loops.
    int GameSample(uint32_t address, bool& loops)
    {
        const auto cached = game_.find(address);
        if (cached != game_.end())
        {
            loops = cached->second.second;
            return cached->second.first;
        }

        // The header holds the length, the rate, the root key, the loop's start and end, then the address of the
        // points. A sample with a loop plays up to the loop's end and goes back to its start.
        const uint32_t length = rom_.U32(address);
        const uint32_t rate = rom_.U32(address + 4);
        const uint32_t root = rom_.U32(address + 8);
        const uint32_t loop_start = rom_.U32(address + 12);
        const uint32_t loop_end = rom_.U32(address + 16);
        const uint32_t data = rom_.U32(address + 20);
        const bool looped = loop_start || loop_end;
        const uint32_t points = looped ? loop_end : length;
        if (!rom_.Contains(address, 24) || !points || points > kMaxSamplePoints || !rate || root > 127 ||
            !rom_.Contains(data, points) || (looped && loop_start >= loop_end))
        {
            game_[address] = {-1, false};
            loops = false;
            return -1;
        }

        sf2::Sample s;
        s.name = AddressName("Sample", address);
        s.sample_rate = rate;
        s.original_key = uint8_t(root);
        s.data.reserve(points + kMinLoop + kLoopGuard);
        for (uint32_t i = 0; i < points; i++)
        {
            s.data.push_back(int16_t(rom_.S8(data + i) * 256));
        }
        if (looped)
        {
            s.loop_start = loop_start;
            s.loop_end = loop_end;
            LengthenLoop(s);
            AddLoopGuard(s);
        }
        file_.samples.push_back(std::move(s));
        game_[address] = {int(file_.samples.size()) - 1, looped};
        loops = looped;
        return int(file_.samples.size()) - 1;
    }

    // Returns the square sample of duty `duty`, at middle C on key 60.
    int Square(int duty)
    {
        duty &= 3;
        const auto cached = squares_.find(duty);
        if (cached != squares_.end())
        {
            return cached->second;
        }

        // The high steps come first. Without the DC offset, which the GBA's output removes, the levels sit either side
        // of 0. They lie as far apart as a wave's levels 0 and 15.
        const int high = kDutyHigh[duty];
        std::vector<int16_t> cycle;
        for (int step = 0; step < 8; step++)
        {
            const double level = step < high ? 2.0 - high / 4.0 : -high / 4.0;
            for (int i = 0; i < kStepPoints; i++)
            {
                cycle.push_back(int16_t(std::lround(level * kPsgShare * 32767)));
            }
        }
        sf2::Sample s;
        s.name = SquareName(duty);
        s.sample_rate = uint32_t(std::lround(8 * kStepPoints * kMiddleC));
        s.original_key = 60;
        SetCycle(s, cycle);
        file_.samples.push_back(std::move(s));
        squares_[duty] = int(file_.samples.size()) - 1;
        return squares_[duty];
    }

    // Returns the sample of the wave channel's pattern at `address`. Key 60 plays it an octave below middle C, as the
    // channel plays it an octave below the square channels at the same setting.
    int Wave(uint32_t address)
    {
        const auto cached = waves_.find(address);
        if (cached != waves_.end())
        {
            return cached->second;
        }
        if (!rom_.Contains(address, 16))
        {
            waves_[address] = -1;
            return -1;
        }

        // The wave channel plays 32 steps of 4 bits, the high nibble of each byte first.
        std::vector<int16_t> cycle;
        for (uint32_t i = 0; i < 16; i++)
        {
            const uint8_t b = rom_.U8(address + i);
            for (int v : {b >> 4, b & 15})
            {
                cycle.push_back(int16_t(std::lround((v - 7.5) / 7.5 * kPsgShare * 32767)));
            }
        }
        sf2::Sample s;
        s.name = AddressName("Wave", address);
        s.sample_rate = uint32_t(std::lround(32 * kMiddleC / 2));
        s.original_key = 60;
        SetCycle(s, cycle);
        file_.samples.push_back(std::move(s));
        waves_[address] = int(file_.samples.size()) - 1;
        return waves_[address];
    }

    // Returns the noise sample of NR43 setting `nr43`.
    int Noise(uint8_t nr43)
    {
        const auto cached = noises_.find(nr43);
        if (cached != noises_.end())
        {
            return cached->second;
        }
        sf2::Sample s;
        char name[32];
        std::snprintf(name, sizeof name, "Noise %02X", unsigned(nr43));
        s.name = name;
        s.sample_rate = kNoiseRate;
        s.original_key = 60;

        // The LFSR's clock is 524288 / r / 2^(s+1) Hz, with r = 0 counting as 0.5. Each point moves it on by
        // kNoiseStep / ((r ? 2r : 1) << s) steps. The output is high while the LFSR's low bit is 0.
        const int shift = nr43 >> 4;
        const int ratio = nr43 & 7;
        uint32_t lfsr = 0x7FFF;
        uint32_t clock = 0;
        for (uint32_t i = 0; i < kNoisePoints; i++)
        {
            s.data.push_back(shift < 14 ? int16_t(std::lround(((lfsr & 1) ? -32767 : 32767) * kPsgShare)) : 0);
            if (shift >= 14)
            {
                continue;
            }
            clock += kNoiseStep;
            const uint32_t den = uint32_t(ratio ? 2 * ratio : 1) << shift;
            for (; clock >= den; clock -= den)
            {
                lfsr = NextLfsr(lfsr, (nr43 & 8) != 0);
            }
        }
        s.loop_start = 0;
        s.loop_end = uint32_t(s.data.size());
        AddLoopGuard(s);
        file_.samples.push_back(std::move(s));
        noises_[nr43] = int(file_.samples.size()) - 1;
        return noises_[nr43];
    }

private:
    const Rom& rom_;
    Sf2File& file_;
    std::map<uint32_t, std::pair<int, bool>> game_;
    std::map<int, int> squares_;
    std::map<uint32_t, int> waves_;
    std::map<uint8_t, int> noises_;
};

// Returns the zone that plays sample `sample` of record `r` on keys `lo` to `hi`: its key range, then its pan and
// envelope, its sample mode and its sample. A PSG zone has no pan generator: the driver pans the PSG by the channel's
// CC10 alone (0x080F2BE4). The release falls from `release_from` (ZoneEnvelope()).
sf2::Zone RecordZone(const InstrumentRecord& r, int sample, bool loops, int lo, int hi, double release_from = -1)
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, lo, hi));
    const bool psg = r.kind == InstrumentKind::kPsg;
    if (!psg && r.pan != 0x7F && r.pan != 0)
    {
        // The offset moves the channel's pan, which sits at the middle unless the song moves it.
        const int pan = std::clamp(64 + r.pan, 0, 127);
        z.generators.push_back(
            sf2::Generator::Value(sf2::kPan, SidesPan(pan <= 63 ? 127 : 254 - 2 * pan, pan <= 63 ? 2 * pan : 127)));
    }
    AddVolumeEnvelope(z, ZoneEnvelope(r, release_from));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, loops ? 1 : 0));
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));
    if (psg)
    {
        z.modulators.push_back(kNoReverb);
    }
    return z;
}

// Adds the zones of record `r` on keys `lo` to `hi` to `zones`. `fixed_key` is the key that a drum kit's part plays
// at, or -1 for a part that follows the key. A sample zone's release falls from `release_from` (ZoneEnvelope()). A PSG
// zone's release falls from the sustain level, as PsgVelocity() expects. Returns false if the record's sample can't be
// read.
bool AddZones(const Rom& rom, const DriverInfo& info, SampleCache& cache, const InstrumentRecord& r, int lo, int hi,
              int fixed_key, double release_from, std::vector<sf2::Zone>& zones)
{
    KeyTuning tuning = UniformTuning(0);
    std::vector<sf2::Zone> made;
    switch (r.kind)
    {
    case InstrumentKind::kSample:
    case InstrumentKind::kFixed:
        {
            bool loops = false;
            const int sample = cache.GameSample(r.sample, loops);
            if (sample < 0)
            {
                return false;
            }
            const int root = int(rom.U32(r.sample + 8));
            sf2::Zone z = RecordZone(r, sample, loops, lo, hi, release_from);
            if (r.kind == InstrumentKind::kFixed || fixed_key >= 0)
            {
                // A fixed part plays one pitch on every key: the mixer's rate for 'F', the drum's key's pitch for a
                // kit. The frequency routine gives 'F' a rate of 0 (0x080F1990), which the step routine takes as a
                // step of 1.0 (0x080F0E4C), whatever rate the sample's header gives.
                const int played = r.kind == InstrumentKind::kFixed ? root : fixed_key;
                z.generators.insert(z.generators.begin() + 1, sf2::Generator::Value(sf2::kScaleTuning, 0));
                sf2::Zone tune;
                const uint32_t rate = rom.U32(r.sample + 4);
                const int cents = r.kind == InstrumentKind::kFixed
                                      ? int(std::lround(1200 * std::log2(double(info.mix_rate) / rate)))
                                      : 100 * (played - root) + SampleCents(rom, info, played, root);
                AddTuning(tune, cents);
                z.generators.insert(z.generators.begin() + 1, tune.generators.begin(), tune.generators.end());
                if (r.kind == InstrumentKind::kFixed)
                {
                    z.modulators.push_back(kNoBend);
                }
                zones.push_back(std::move(z));
                return true;
            }
            for (int key = lo; key <= hi; key++)
            {
                tuning[size_t(key)] = SampleCents(rom, info, key, root);
            }
            made = TunedZones(z, tuning);
            break;
        }
    case InstrumentKind::kPsg:
        {
            if (r.psg_channel == 3)
            {
                // The noise's setting comes from the key, so each run of keys with the same setting gets a zone.
                for (int key = lo; key <= hi;)
                {
                    const uint8_t nr43 = NoiseSetting(rom, info, r, fixed_key >= 0 ? fixed_key : key);
                    int last = key;
                    while (fixed_key < 0 && last < hi && NoiseSetting(rom, info, r, last + 1) == nr43)
                    {
                        last++;
                    }
                    if (fixed_key >= 0)
                    {
                        last = hi;
                    }
                    sf2::Zone z = RecordZone(r, cache.Noise(nr43), true, key, last);
                    z.generators.insert(z.generators.begin() + 1, sf2::Generator::Value(sf2::kScaleTuning, 0));
                    z.modulators.push_back(kNoBend);
                    zones.push_back(std::move(z));
                    key = last + 1;
                }
                return true;
            }
            const int sample = r.psg_channel == 2 ? cache.Wave(r.sample) : cache.Square((r.psg_setting >> 1) & 3);
            if (sample < 0)
            {
                return false;
            }
            sf2::Zone z = RecordZone(r, sample, true, lo, hi);
            if (fixed_key >= 0)
            {
                z.generators.insert(z.generators.begin() + 1, sf2::Generator::Value(sf2::kScaleTuning, 0));
                sf2::Zone tune;
                AddTuning(tune, 100 * (fixed_key - 60) + PsgCents(rom, info, fixed_key));
                z.generators.insert(z.generators.begin() + 1, tune.generators.begin(), tune.generators.end());
                zones.push_back(std::move(z));
                return true;
            }
            for (int key = lo; key <= hi; key++)
            {
                tuning[size_t(key)] = PsgCents(rom, info, key);
            }
            made = TunedZones(z, tuning);
            break;
        }
    default:
        return false;
    }

    // TunedZones() covers every key. The zones outside the record's keys go.
    for (sf2::Zone& z : made)
    {
        const sf2::Generator& range = z.generators[0];
        const int first = range.amount & 0xFF;
        const int last = range.amount >> 8;
        if (last < lo || first > hi)
        {
            continue;
        }
        z.generators[0] = sf2::Generator::Range(sf2::kKeyRange, std::max(first, lo), std::min(last, hi));
        zones.push_back(std::move(z));
    }
    return true;
}

// Returns true if zone `z` contains generator `g` with the same amount.
bool ContainsGenerator(const sf2::Zone& z, const sf2::Generator& g)
{
    return std::any_of(z.generators.begin(), z.generators.end(),
                       [&](const sf2::Generator& h) { return h.oper == g.oper && h.amount == g.amount; });
}

// Moves each generator that every local zone of `instrument` contains with the same amount into the global zone, the
// first. Each local zone keeps its key range and its sample. The sound stays the same. A tuned instrument has a zone
// for each key, and a noise instrument one for each key from 21 to 80. Without the move, --single-sf2's file of AZWE
// contains more than the format's 65535 generators.
void MoveSharedGenerators(sf2::Instrument& instrument)
{
    std::vector<sf2::Zone>& zones = instrument.zones;
    if (zones.size() < 3)
    {
        return;
    }
    std::vector<sf2::Generator> shared;
    for (const sf2::Generator& g : zones[1].generators)
    {
        if (g.oper != sf2::kKeyRange && g.oper != sf2::kVelRange && g.oper != sf2::kSampleId &&
            std::all_of(zones.begin() + 2, zones.end(), [&](const sf2::Zone& z) { return ContainsGenerator(z, g); }))
        {
            shared.push_back(g);
        }
    }
    const auto moved = [&](const sf2::Generator& g)
    {
        return std::any_of(shared.begin(), shared.end(), [&](const sf2::Generator& h) { return h.oper == g.oper; });
    };
    for (auto z = zones.begin() + 1; z != zones.end(); ++z)
    {
        std::erase_if(z->generators, moved);
    }
    zones[0].generators.insert(zones[0].generators.end(), shared.begin(), shared.end());
}

} // namespace

int KeyPitch(const Rom& rom, const DriverInfo& info, int key)
{
    return rom.U16(info.pitch_table + 2 * uint32_t(std::clamp(key, 0, 127)));
}

int PsgSetting(int hz)
{
    if (hz <= 0)
    {
        return 0;
    }
    return std::clamp(2048 - 131072 / hz, 0, 2047);
}

bool Sweeps(const InstrumentRecord& r)
{
    return r.kind == InstrumentKind::kPsg && r.psg_channel == 0 && (r.psg_sweep & 0x70) != 0;
}

VolumeEnvelope ZoneEnvelope(const InstrumentRecord& r, double release_from)
{
    return Sweeps(r) ? VolumeEnvelope() : EnvelopeOf(r, release_from);
}

InstrumentRecord ReadInstrument(const Rom& rom, uint32_t address)
{
    InstrumentRecord r;
    r.address = address;
    if (!rom.Contains(address, 8))
    {
        return r;
    }
    const uint8_t type = rom.U8(address);
    r.key = rom.U8(address + 1) & 0x7F;
    switch (type)
    {
    case 0x52:
        r.kind = InstrumentKind::kDrums;
        r.table = rom.U32(address + 4);
        return r;
    case 0x53:
        r.kind = rom.Contains(address, 12) ? InstrumentKind::kKeySplit : InstrumentKind::kNone;
        r.table = rom.U32(address + 4);
        r.parts = rom.U32(address + 8);
        return r;
    case 0x50:
    case 0x51:
        r.kind = InstrumentKind::kPsg;
        break;
    case 0x46:
        r.kind = InstrumentKind::kFixed;
        break;
    default:
        r.kind = InstrumentKind::kSample;
        break;
    }
    if (!rom.Contains(address, r.kind == InstrumentKind::kPsg ? 36 : 32))
    {
        r.kind = InstrumentKind::kNone;
        return r;
    }
    r.pan = int16_t(rom.U16(address + 2));
    r.sample = rom.U32(address + 4);
    r.start = rom.U32(address + 8);
    r.sustain = rom.U32(address + 12);
    r.attack = rom.U32(address + 16);
    r.decay = rom.U32(address + 20);
    r.fall = rom.U32(address + 24);
    r.release = rom.U32(address + 28);
    if (r.kind == InstrumentKind::kPsg)
    {
        const uint32_t word = rom.U32(address + 32);
        r.psg_channel = int(word & 3);
        r.psg_length = int((word >> 2) & 0xFF);
        r.psg_sweep = int((word >> 10) & 0x7F);
        r.psg_setting = rom.U16(address + 34);
    }
    return r;
}

uint32_t ProgramInstrument(const Rom& rom, const DriverInfo& info, int bank, int program)
{
    if (bank < 0 || bank >= info.bank_count || program < 0 || program > 127)
    {
        return 0;
    }
    const uint32_t table = rom.U32(info.bank_table + 4 * uint32_t(bank));
    return table ? rom.U32(table + 4 * uint32_t(program)) : 0;
}

InstrumentRecord PlayedInstrument(const Rom& rom, const InstrumentRecord& program, int key)
{
    const int index = key - program.key;
    uint32_t part = 0;
    if (program.kind == InstrumentKind::kDrums)
    {
        part = index >= 0 ? rom.U32(program.table + 4 * uint32_t(index)) : 0;
    }
    else if (program.kind == InstrumentKind::kKeySplit)
    {
        part = index >= 0 ? rom.U32(program.parts + 4 * uint32_t(rom.U8(program.table + uint32_t(index)))) : 0;
    }
    else
    {
        return program;
    }
    const InstrumentRecord r = ReadInstrument(rom, part);
    if (r.kind == InstrumentKind::kDrums || r.kind == InstrumentKind::kKeySplit)
    {
        return InstrumentRecord();
    }
    return r;
}

InstrumentSet::InstrumentSet(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

void InstrumentSet::Add(int bank, int program, int key, int release_level)
{
    keys_[{bank, program}].insert(key);
    if (release_level >= 0)
    {
        auto& [sum, count] = releases_[{bank, program, key}];
        sum += release_level;
        count++;
    }
}

double InstrumentSet::ReleaseFrom(int bank, int program, int key) const
{
    const InstrumentRecord r = ReadInstrument(rom_, ProgramInstrument(rom_, info_, bank, program));
    const bool each_key = r.kind == InstrumentKind::kDrums || r.kind == InstrumentKind::kKeySplit;
    int64_t sum = 0;
    int count = 0;
    for (auto it = releases_.lower_bound({bank, program, each_key ? key : 0}); it != releases_.end(); ++it)
    {
        const auto& [b, p, k] = it->first;
        if (b != bank || p != program || (each_key && k != key))
        {
            break;
        }
        sum += it->second.first;
        count += it->second.second;
    }
    return count ? double(sum) / count / double(kFullLevel >> 16) : -1;
}

Sf2File InstrumentSet::Build() const
{
    Sf2File file;
    SampleCache cache(rom_, file);
    for (const auto& [preset, keys] : keys_)
    {
        const auto [bank, program] = preset;
        const uint32_t address = ProgramInstrument(rom_, info_, bank, program);
        const InstrumentRecord r = ReadInstrument(rom_, address);
        std::vector<sf2::Zone> zones;
        if (r.kind == InstrumentKind::kDrums || r.kind == InstrumentKind::kKeySplit)
        {
            // A kit's or a split's part gets a zone on each key that the songs play.
            const bool drums = r.kind == InstrumentKind::kDrums;
            for (int key : keys)
            {
                const InstrumentRecord part = PlayedInstrument(rom_, r, key);
                AddZones(rom_, info_, cache, part, key, key, drums ? part.key : -1, ReleaseFrom(bank, program, key),
                         zones);
            }
        }
        else
        {
            AddZones(rom_, info_, cache, r, 0, 127, -1, ReleaseFrom(bank, program, 0), zones);
        }
        if (zones.empty())
        {
            continue;
        }
        const std::string name = AddressName("Instrument", address);
        file.instruments.push_back(MakeInstrument(name, std::move(zones)));
        MoveSharedGenerators(file.instruments.back());
        file.presets.push_back(MakePreset(name, uint16_t(bank), uint16_t(program), int(file.instruments.size()) - 1));
    }
    return file;
}

} // namespace agbsf2::rd1
