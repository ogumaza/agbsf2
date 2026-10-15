// SPDX-License-Identifier: MIT

#include "rare/soundfont.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "seqlib/sf2/units.h"

namespace agbsf2::rare
{
namespace
{

// The shortest time a SoundFont envelope can give a phase, in timecents: 1 ms.
constexpr int kInstant = -12000;

// The greatest attenuation, in centibels.
constexpr int kSilent = 1440;

// A modulator that mutes a note while its key's polyphonic key pressure is 64 or more. Its 96 dB of attenuation takes
// a 16-bit sample below its smallest step.
constexpr sf2::Modulator kMute = {sf2::kPolyPressure | sf2::kSwitch, sf2::kInitialAttenuation, 960, 0, 0};

// The shortest loop a sample gets, in points. A shorter one is repeated until it's at least this long, since some
// players can't play a very short loop.
constexpr uint32_t kMinLoop = 32;

// The points after a loop's end that repeat its start, for players that read past the end when they interpolate.
constexpr uint32_t kLoopGuard = 8;

// Returns a time in timecents, the unit of SoundFont envelope times.
int Timecents(double seconds)
{
    return sf2::ToTimecents(seconds * 1000.0, kInstant, 8000);
}

// Returns the SoundFont duration for a decay or release that falls to silence over `frames` frames. The driver's level
// falls linearly, so its fade stays loud for most of its length, where a SoundFont's level falls linearly in decibels
// and becomes quiet sooner. The SoundFont fade is lengthened to keep their loudness close while the fade is audible.
// Short fades fall in larger steps and need less stretching.
double FadeSeconds(int frames)
{
    const double stretch = std::clamp(3.0 + 3.0 * (frames - 5) / 11.0, 3.0, 6.0);
    return stretch * frames / kFrameRate;
}

// Returns the sustain level of an instrument with sustain value 0-99 as the driver works it out, from 0 to 0x8000.
uint32_t SustainLevel(uint32_t sustain)
{
    const uint32_t value = std::min<uint32_t>(sustain, 99);
    const uint32_t whole = (value << 7) / 99;
    const uint32_t part = (((value << 7) % 99) << 8) / 99;
    return (whole << 8) | part;
}

std::string Name(const char* kind, uint32_t address)
{
    char b[32];
    std::snprintf(b, sizeof b, "%s %08X", kind, unsigned(address));

    return b;
}

// Returns the name of the SoundFont instrument for the driver's instrument at `address` played with the envelope
// settings `settings`: "Instrument 08700154" without any, and otherwise the address and each setting given, such as
// "08700154 d0s80r0" for a decay of 0, a sustain level of 80 and a release of 0.
std::string InstrumentName(uint32_t address, const EnvelopeSettings& settings)
{
    if (settings.None())
    {
        return Name("Instrument", address);
    }

    char b[16];
    std::snprintf(b, sizeof b, "%08X ", unsigned(address));
    std::string name = b;
    const std::pair<char, uint8_t> parts[] = {
        {'a', settings.attack}, {'d', settings.decay}, {'s', settings.sustain}, {'r', settings.release}};
    for (const auto& [letter, value] : parts)
    {
        if (value < 0x80)
        {
            name += letter + std::to_string(value);
        }
    }

    return name;
}

} // namespace

Sf2Envelope EnvelopeFor(const DriverInfo& info, const Instrument& inst, const EnvelopeSettings& settings)
{
    Sf2Envelope env;

    // The attack rises in a straight line, as a SoundFont's does, and reaches the full level after `steps` + 1 frames.
    const uint32_t attack = std::min<uint32_t>(inst.attack, 99);
    const uint32_t steps = settings.attack < 0x80 ? settings.attack : ((99 - attack) << 5) / 99;
    if (steps > 0)
    {
        env.attack = Timecents((steps + 1) / kFrameRate);
    }

    // The decay falls to the sustain level. At a sustain of 0, the driver stops the note when the decay ends.
    const uint32_t sustain = settings.sustain < 0x80 ? uint32_t(settings.sustain) << 8 : SustainLevel(inst.sustain);
    const int decay = settings.decay < 0x80 ? FadeEntryFrames(settings.decay) : FadeFrames(info, inst.decay);
    if (sustain < 0x8000)
    {
        const double level = sustain / 32768.0;
        if (sustain << 4 < 0x1000)
        {
            env.sustain = kSilent;
            env.decay = decay ? Timecents(FadeSeconds(decay)) : kInstant;
        }
        else
        {
            // A SoundFont's decay time is for a fall of 100 dB, so it reaches the sustain level in proportion.
            env.sustain = std::min(kSilent, int(std::lround(-200.0 * std::log10(level))));
            env.decay = decay && env.sustain > 0 ? Timecents(1000.0 / env.sustain * decay / kFrameRate) : kInstant;
        }
    }

    const int release = settings.release < 0x80 ? FadeEntryFrames(settings.release) : FadeFrames(info, inst.release);
    if (release)
    {
        env.release = Timecents(FadeSeconds(release));
    }

    return env;
}

SoundfontBuilder::SoundfontBuilder(const Rom& rom, const DriverInfo& info, int first_free_bank)
    : rom_(rom), info_(info), next_bank_(first_free_bank)
{
}

int SoundfontBuilder::InstrumentFor(uint32_t address, const EnvelopeSettings& settings)
{
    const auto id = std::make_pair(address, settings);
    const auto cached = instruments_.find(id);
    if (cached != instruments_.end())
    {
        return cached->second;
    }

    instruments_[id] = -1;
    Instrument top;
    if (!ReadInstrument(rom_, address, top))
    {
        return -1;
    }

    // The global zone makes velocity and controller 7 scale the level in a straight line, as the driver does. The
    // default curves square them, and these take the place of the defaults.
    sf2::Instrument si = MakeInstrument(InstrumentName(address, settings));
    si.zones.front().modulators.push_back(
        {sf2::kNoteOnVelocity | sf2::kNegative | sf2::kConcave, sf2::kInitialAttenuation, 480, 0, 0});
    si.zones.front().modulators.push_back(
        {uint16_t(sf2::kController | 7) | sf2::kNegative | sf2::kConcave, sf2::kInitialAttenuation, 480, 0, 0});
    if (mutes_)
    {
        si.zones.front().modulators.push_back(kMute);
    }

    if (top.IsSample())
    {
        const int sample = SampleFor(top);
        if (sample >= 0)
        {
            si.zones.push_back(ZoneFor(top, settings, sample, 0, 127, false));
        }
    }
    else
    {
        // A drum kit or key split gets a zone for each run of keys that play the same instrument. The driver plays any
        // other type of instrument as a drum kit.
        const bool drums = top.type != kInstKeySplit;
        int key = 0;
        while (key < 128)
        {
            const uint32_t sub_address = SplitInstrument(rom_, top, key);
            int last = key;
            while (last + 1 < 128 && SplitInstrument(rom_, top, last + 1) == sub_address)
            {
                last++;
            }

            Instrument sub;
            if (sub_address && ReadInstrument(rom_, sub_address, sub) && sub.IsSample())
            {
                const int sample = SampleFor(sub);
                if (sample >= 0)
                {
                    si.zones.push_back(ZoneFor(sub, settings, sample, key, last, drums));
                }
            }

            key = last + 1;
        }
    }

    if (si.zones.size() < 2)
    {
        return -1;
    }

    file_.instruments.push_back(si);
    instruments_[id] = int(file_.instruments.size()) - 1;

    return instruments_[id];
}

int SoundfontBuilder::BankFor(int bank, const EnvelopeSettings& settings)
{
    if (settings.None())
    {
        return bank;
    }

    const auto key = std::make_pair(bank, settings);
    const auto found = banks_.find(key);
    if (found != banks_.end())
    {
        return found->second;
    }

    // The drum bank is left for the drum channel's presets.
    if (next_bank_ == kDrumBank)
    {
        next_bank_++;
    }
    banks_[key] = next_bank_;

    return next_bank_++;
}

void SoundfontBuilder::AddPreset(int bank, int program, int instrument)
{
    for (const sf2::Preset& p : file_.presets)
    {
        if (p.bank == bank && p.program == program)
        {
            return;
        }
    }

    file_.presets.push_back(
        MakePreset("Program " + std::to_string(program), uint16_t(bank), uint16_t(program), instrument));
}

void SoundfontBuilder::AddMutes()
{
    if (mutes_)
    {
        return;
    }

    mutes_ = true;
    for (sf2::Instrument& si : file_.instruments)
    {
        si.zones.front().modulators.push_back(kMute);
    }
}

int SoundfontBuilder::SampleFor(const Instrument& inst)
{
    if (!SampleValid(rom_, inst))
    {
        return -1;
    }

    const bool loops = inst.Loops();
    const auto key = std::make_tuple(inst.start, inst.end, loops ? inst.loop_length : 0, uint32_t(loops), inst.rate,
                                     std::min<uint32_t>(inst.root_key, 127));
    const auto cached = samples_.find(key);
    if (cached != samples_.end())
    {
        return cached->second;
    }

    // The driver's samples are signed 8-bit.
    sf2::Sample s;
    s.name = Name("Sample", inst.start);
    s.sample_rate = inst.rate;
    s.original_key = uint8_t(std::min<uint32_t>(inst.root_key, 127));
    const uint32_t length = inst.end - inst.start;
    for (uint32_t i = 0; i < length; i++)
    {
        s.data.push_back(int16_t(rom_.S8(inst.start + i) * 256));
    }

    // A loop is the sample's last loop_length points. A short one is repeated after the end until it's long enough.
    if (loops)
    {
        s.loop_start = length - inst.loop_length;
        s.loop_end = length;
        while (s.loop_end - s.loop_start < kMinLoop)
        {
            for (uint32_t i = 0; i < inst.loop_length; i++)
            {
                s.data.push_back(s.data[s.loop_start + i]);
            }

            s.loop_end += inst.loop_length;
        }

        for (uint32_t i = 0; i < kLoopGuard; i++)
        {
            s.data.push_back(s.data[s.loop_start + i % (s.loop_end - s.loop_start)]);
        }
    }

    file_.samples.push_back(s);
    samples_[key] = int(file_.samples.size()) - 1;

    return samples_[key];
}

sf2::Zone SoundfontBuilder::ZoneFor(const Instrument& voice, const EnvelopeSettings& settings, int sample, int low,
                                    int high, bool fixed_pitch) const
{
    sf2::Zone z;
    z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, low, high));

    // Envelope phases at their defaults are left out.
    const Sf2Envelope env = EnvelopeFor(info_, voice, settings);
    const std::pair<uint16_t, int> phases[] = {
        {sf2::kAttackVolEnv, env.attack}, {sf2::kDecayVolEnv, env.decay}, {sf2::kReleaseVolEnv, env.release}};
    for (const auto& [op, value] : phases)
    {
        if (value != kInstant)
        {
            z.generators.push_back(sf2::Generator::Value(op, value));
        }
    }
    if (env.sustain)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kSustainVolEnv, env.sustain));
    }

    // A fine tune of a semitone or more goes partly into the coarse tune, since the fine tune only reaches 99 cents.
    if (voice.fine_tune / 100)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kCoarseTune, std::clamp(voice.fine_tune / 100, -120, 120)));
    }
    if (voice.fine_tune % 100)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kFineTune, voice.fine_tune % 100));
    }

    if (fixed_pitch)
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kScaleTuning, 0));
    }
    if (voice.Loops())
    {
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, 1));
    }
    z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

    return z;
}

} // namespace agbsf2::rare
