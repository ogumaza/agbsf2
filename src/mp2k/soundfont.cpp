// SPDX-License-Identifier: MIT

#include "mp2k/soundfont.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "mp2k/sequencer.h"
#include "psg.h"

namespace agbsf2::mp2k
{
namespace
{

// The kinds of sample, which with a source make the cache's keys.
enum Kind
{
    kDirectKind,
    kReversedKind,
    kSquareKind,
    kWaveKind,
    kNoiseKind,
    kTriangleKind
};

// The level of a PSG channel at its full volume against a sample channel at its full level, in centibels. On each side,
// the hardware turns a PSG channel at volume v into 0 to 16v, out of a full scale of -512 to 512, and a sample
// channel's full level of 125/256 into 4 times that much of the sample.
constexpr int kPsgAttenuation = 63;

// The PSG's envelope steps: one for each count of its counter, which counts 16 times every 15 frames.
constexpr double kPsgStepsPerSecond = kFrameRate * 16 / 15;

// Returns the level of a PSG channel at its full volume against a full-scale sample. Camelot's mixer plays a sample
// channel at 9/8 of the driver's full level, so the PSG is that much quieter against it. The samples hold the levels,
// rather than the zones' attenuation, which FluidSynth applies at 0.4 of its value.
double PsgLevel(const DriverInfo& info)
{
    const double level = std::pow(10.0, -kPsgAttenuation / 200.0);
    return info.camelot_mixer ? level * 8 / 9 : level;
}

// Returns the level of a sample channel at its full level against a full-scale sample: the driver's master volume
// scales it, where Camelot's mixer leaves the master volume out.
double DirectLevel(const DriverInfo& info)
{
    return info.camelot_mixer ? 1.0 : (info.master_volume + 1) / 16.0;
}

// Returns the frames a sample channel's envelope takes to fall from `from` to `to` or below, multiplied by `rate`/256
// each frame and rounded down, or 0 if it doesn't fall.
int SampleFadeFrames(int from, int to, int rate)
{
    int frames = 0;
    for (int level = from; level > to; frames++)
    {
        const int next = (level * rate) >> 8;
        if (next >= level)
        {
            return 0;
        }

        level = next;
    }

    return frames;
}

// Returns the SoundFont time for a fall of 100 dB at the pace of a sample channel's fade from `from` to `to`. The
// driver's fade is a straight line in decibels, like a SoundFont's, until rounding down makes it faster at low levels.
int SampleFadeTime(int from, int to, int rate)
{
    const int frames = SampleFadeFrames(from, to, rate);
    const double decibels = 20.0 * std::log10(double(from) / std::max(to, 1));
    if (!frames || decibels <= 0)
    {
        return kInstant;
    }

    return Timecents(100.0 / decibels * frames / kFrameRate);
}

// Returns the SoundFont duration for a fade to silence lasting `seconds` that falls linearly in level, as the PSG's
// fades and Camelot's mixer's releases do. Such a fade stays loud for most of its length, where a SoundFont's falls
// linearly in decibels and becomes quiet sooner, so the SoundFont fade is lengthened to keep their loudness close.
// Rare's driver fades linearly too, and its conversion lengthens its fades the same way.
int StraightFadeTime(double seconds)
{
    return Timecents(FadeStretch(seconds * kFrameRate) * seconds);
}

// Returns the frequency in Hz of a PSG square wave at `key`, from the driver's frequency setting.
double SquareFrequency(int key)
{
    return SquareHz(int(KeyToPsgFrequency(1, key, 0)));
}

// Returns the rate in Hz of the noise generator's steps at a noise setting: 524288 Hz divided by the ratio (0 counts as
// 0.5) and by 2 to the power of the shift + 1.
double NoiseClock(uint32_t setting)
{
    const uint32_t ratio = setting & 7;
    const uint32_t shift = setting >> 4;
    return 524288.0 / (ratio ? double(ratio) : 0.5) / double(uint32_t(2) << shift);
}

// A part of a zone: its keys, and its tuning in cents from the sample at its root key. A part that plays one pitch
// whatever the key has a scale tuning of 0.
struct Part
{
    int low, high;
    double cents;
    bool fixed;
};

// Adds a zone for each part that plays `voice` with SoundFont sample `sample`, with the drum kit's pan `rhythm_pan`.
void AddParts(const DriverInfo& info, const Voice& voice, int sample, bool loops, const std::vector<Part>& parts,
              int rhythm_pan, sf2::Instrument& instrument)
{
    // The samples hold the levels of the sample and PSG channels (see DirectLevel() and PsgLevel()).
    const int psg = voice.PsgChannel();
    const VolumeEnvelope env = EnvelopeFor(info, voice);
    for (const Part& part : parts)
    {
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, part.low, part.high));
        if (rhythm_pan)
        {
            z.generators.push_back(sf2::Generator::Value(sf2::kPan, std::clamp(rhythm_pan * 500 / 127, -500, 500)));
        }
        AddVolumeEnvelope(z, env);
        AddTuning(z, int(std::lround(part.cents)));
        if (part.fixed)
        {
            z.generators.push_back(sf2::Generator::Value(sf2::kScaleTuning, 0));
        }
        if (loops)
        {
            z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, 1));
        }
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));

        // The pitch wheel doesn't move a fixed-pitch sample. This takes the place of the default modulator from it.
        if (psg == 0 && (voice.type & kVoiceFixed))
        {
            z.modulators.push_back({sf2::kSrcPitchWheel, sf2::kFineTune, 0, sf2::kSrcPitchWheelSensitivity, 0});
        }

        instrument.zones.push_back(z);
    }
}

// Returns the index of the sample that `cache` holds under `key`, or -1 if it holds none.
template <typename Key>
int CachedSample(const std::map<Key, int>& cache, const Key& key)
{
    const auto found = cache.find(key);
    return found != cache.end() ? found->second : -1;
}

// Adds `sample` to `file`, and to `cache` under `key`. Returns the sample's index.
template <typename Key>
int AddSample(Sf2File& file, std::map<Key, int>& cache, const Key& key, sf2::Sample sample)
{
    file.samples.push_back(std::move(sample));
    const int index = int(file.samples.size()) - 1;
    cache[key] = index;

    return index;
}

} // namespace

VolumeEnvelope EnvelopeFor(const DriverInfo& info, const Voice& voice)
{
    VolumeEnvelope env;

    // A PSG voice's envelope moves through its 16 levels in steps, one every `attack`, `decay` or `release` counts of
    // its counter, with the sustain level from 0 to 15. The full volume goes with the full level of 15.
    if (voice.PsgChannel())
    {
        const int attack = voice.attack & 7, decay = voice.decay & 7, release = voice.release & 7;
        const int sustain = voice.sustain & 15;
        if (attack)
        {
            env.attack = Timecents(15.0 * attack / kPsgStepsPerSecond);
        }

        if (sustain < 15 && decay)
        {
            // A decay to a sustain level reaches it in the same time, and one to silence is a fade.
            const double seconds = (15.0 - sustain) * decay / kPsgStepsPerSecond;
            env.sustain = sustain ? Centibels(sustain / 15.0) : kSilent;
            env.decay = sustain ? Timecents(seconds * 1000.0 / env.sustain) : StraightFadeTime(seconds);
        }
        else if (sustain < 15)
        {
            env.sustain = sustain ? Centibels(sustain / 15.0) : kSilent;
        }

        if (release)
        {
            env.release = StraightFadeTime((sustain ? sustain : 15) * double(release) / kPsgStepsPerSecond);
        }

        return env;
    }

    // A sample voice's level starts at its attack and gains that much each frame, up to 255. An attack of 255 is
    // instant.
    if (voice.attack < 255)
    {
        env.attack = Timecents((255.0 / voice.attack - 1) / kFrameRate);
    }

    // The decay falls to the sustain level, and at a sustain of 0, the note stops when it gets there.
    const int sustain = voice.sustain;
    if (sustain < 255)
    {
        env.sustain = Centibels(sustain / 255.0);
        env.decay = voice.decay ? SampleFadeTime(255, sustain ? sustain : 2, voice.decay) : kInstant;
    }

    // The release falls from the sustain level, or from the full level for a voice that doesn't sustain. Camelot's
    // mixer takes 256 less the release off the level each frame, in a straight line.
    const int from = sustain ? sustain : 255;
    if (info.camelot_mixer)
    {
        const int per_frame = 256 - voice.release;
        env.release = StraightFadeTime((from + per_frame - 1) / per_frame / kFrameRate);
    }
    else if (voice.release)
    {
        env.release = SampleFadeTime(from, std::max(1, from / 8), voice.release);
    }

    return env;
}

Synth SynthOf(const Rom& rom, const DriverInfo& info, const Voice& voice)
{
    Wave wave;
    if (!info.camelot_mixer || voice.PsgChannel() || (voice.type & kVoiceFixed) || !ReadWave(rom, voice.wave, wave) ||
        wave.size != 0 || !rom.Contains(wave.Data(), 6))
    {
        return Synth::kNone;
    }

    // The data's second byte is the type: 0 for a pulse wave, 1 for a saw wave, and anything else for a triangle wave.
    switch (rom.S8(wave.Data() + 1))
    {
    case 0:
        return Synth::kPulse;
    case 1:
        return Synth::kSaw;
    default:
        return Synth::kTriangle;
    }
}

std::vector<int16_t> RenderSynth(const Rom& rom, const DriverInfo& info, Synth synth, uint32_t data, uint32_t frequency,
                                 uint32_t count)
{
    const uint32_t step = SynthStep(frequency, info.step_scale);
    std::vector<int16_t> out;
    uint32_t phase = 0;
    uint32_t lfo = 0;
    uint32_t duty = 0;
    int32_t filter = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        // A pulse wave is at half the full level for the part of each cycle that the duty gives. At the start of each
        // frame, the duty's triangle wave moves on by the data's fourth byte, from its sixth, and sets the duty from
        // the third byte and as far up again as the fifth byte gives.
        if (synth == Synth::kPulse)
        {
            if (i % uint32_t(info.samples_per_frame) == 0)
            {
                lfo += DutyStep(rom, data);
                uint32_t shape = lfo + (uint32_t(rom.U8(data + 5)) << 24);
                if (shape & 0x80000000u)
                {
                    shape = ~shape;
                }

                duty = (shape >> 8) * rom.U8(data + 4) + (uint32_t(rom.U8(data + 2)) << 24);
            }

            out.push_back(int16_t(phase < duty ? 64 * 256 : -64 * 256));
            phase += step;
            continue;
        }

        // A saw wave goes through its filter. The mixer plays the filter's output at half the level.
        phase += step;
        filter = FilterSaw(SawPoint(phase), filter);
        out.push_back(int16_t(std::clamp(filter * 128, -32768, 32767)));
    }

    return out;
}

SoundfontBuilder::SoundfontBuilder(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

int SoundfontBuilder::InstrumentFor(uint32_t address, int key)
{
    Voice top;
    if (!ReadVoice(rom_, address, top))
    {
        return -1;
    }

    // Voice groups often hold the same voice, so the instruments are kept by the voice's 12 bytes.
    std::array<uint8_t, 12> bytes;
    for (uint32_t i = 0; i < 12; i++)
    {
        bytes[i] = rom_.U8(address + i);
    }
    const auto cached = instruments_.find(bytes);
    if (cached != instruments_.end())
    {
        AddSynthKey(cached->second, address, key);
        return cached->second;
    }

    instruments_[bytes] = -1;

    // The global zone makes velocity and controller 7 scale the level in a straight line, as the driver does. The
    // default curves square them, and these take the place of the defaults.
    sf2::Instrument si = MakeInstrument(AddressName("Voice", address));
    si.zones.front().modulators.push_back(kLinearVelocity);
    si.zones.front().modulators.push_back(kLinearVolume);

    bool plays = false;
    if (!top.IsSplit())
    {
        plays = AddZone(si, top, 0, 127, -1, 0);
    }
    else if (top.type & kVoiceDrumKit)
    {
        // Each key of a drum kit plays its own voice, at that voice's key and pan.
        for (int k = 0; k < 128; k++)
        {
            Voice sub;
            const uint32_t sub_address = SplitVoice(rom_, top, k);
            if (sub_address && ReadVoice(rom_, sub_address, sub) && !sub.IsSplit())
            {
                plays = AddZone(si, sub, k, k, sub.key, sub.RhythmPan()) || plays;
            }
        }
    }
    else
    {
        // A key split gets a zone for each run of keys that play the same voice.
        int k = 0;
        while (k < 128)
        {
            const uint32_t sub_address = SplitVoice(rom_, top, k);
            int last = k;
            while (last + 1 < 128 && SplitVoice(rom_, top, last + 1) == sub_address)
            {
                last++;
            }

            Voice sub;
            if (sub_address && ReadVoice(rom_, sub_address, sub) && !sub.IsSplit())
            {
                plays = AddZone(si, sub, k, last, -1, 0) || plays;
            }

            k = last + 1;
        }
    }

    if (!plays)
    {
        return -1;
    }

    file_.instruments.push_back(si);
    instruments_[bytes] = int(file_.instruments.size()) - 1;
    AddSynthKey(instruments_[bytes], address, key);

    return instruments_[bytes];
}

void SoundfontBuilder::AddSynthKey(int instrument, uint32_t address, int key)
{
    if (instrument < 0 || key < 0 || key > 127 || !synth_keys_.insert({instrument, key}).second)
    {
        return;
    }

    // The key's voice: the voice itself, or a key split's or a drum kit's for the key, which plays at its own key and
    // pan in a drum kit.
    Voice voice;
    if (!ReadVoice(rom_, address, voice))
    {
        return;
    }

    int fixed_key = -1;
    int pan = 0;
    if (voice.IsSplit())
    {
        const bool drums = voice.type & kVoiceDrumKit;
        const uint32_t sub_address = SplitVoice(rom_, voice, key);
        if (!sub_address || !ReadVoice(rom_, sub_address, voice) || voice.IsSplit())
        {
            return;
        }
        if (drums)
        {
            fixed_key = voice.key;
            pan = voice.RhythmPan();
        }
    }

    // The sample plays the key at its own pitch, so the zone needs no tuning.
    const Synth synth = SynthOf(rom_, info_, voice);
    if ((synth == Synth::kPulse || synth == Synth::kSaw) && voice.attack)
    {
        const int sample = SynthSample(voice, fixed_key >= 0 ? fixed_key : key);
        AddParts(info_, voice, sample, Looped(file_.samples[size_t(sample)]), {{key, key, 0.0, fixed_key >= 0}}, pan,
                 file_.instruments[size_t(instrument)]);
    }
}

bool SoundfontBuilder::AddPreset(int bank, int program, int instrument)
{
    for (const sf2::Preset& p : file_.presets)
    {
        if (p.bank == bank && p.program == program)
        {
            return PresetInstrument(p) == instrument;
        }
    }

    file_.presets.push_back(
        MakePreset("Program " + std::to_string(program), uint16_t(bank), uint16_t(program), instrument));

    return true;
}

BankProgram SoundfontBuilder::SharedPreset(int program, int instrument)
{
    const auto key = std::make_pair(program, instrument);
    const auto found = slots_.find(key);
    if (found != slots_.end())
    {
        return found->second;
    }

    std::vector<bool> taken(128 * 128);
    for (const sf2::Preset& p : file_.presets)
    {
        if (p.bank < 128 && p.program < 128)
        {
            taken[size_t(p.bank * 128 + p.program)] = true;
        }
    }

    BankProgram slot = {-1, -1};
    for (int bank = 0; bank < 128 && slot.bank < 0; bank++)
    {
        if (!taken[size_t(bank * 128 + program)])
        {
            slot = {bank, program};
        }
    }
    for (int i = 0; i < 128 * 128 && slot.bank < 0; i++)
    {
        if (!taken[size_t(i)])
        {
            slot = {i / 128, i % 128};
        }
    }

    if (slot.bank >= 0)
    {
        AddPreset(slot.bank, slot.program, instrument);
        slots_[key] = slot;
    }

    return slot;
}

int SoundfontBuilder::SampleFor(const Voice& voice)
{
    switch (voice.PsgChannel())
    {
    case 0:
        switch (SynthOf(rom_, info_, voice))
        {
        case Synth::kNone:
            return DirectSample(voice.address);
        case Synth::kTriangle:
            return TriangleSample(voice.wave);
        default:
            return -1;
        }

    case 1:
    case 2:
        return SquareSample(int(voice.wave & 3));

    case 3:
        return WaveSample(voice.wave);

    case 4:
        return NoiseSample(int(voice.wave & 1));

    default:
        return -1;
    }
}

int SoundfontBuilder::DirectSample(uint32_t address)
{
    Voice voice;
    Wave wave;
    if (!ReadVoice(rom_, address, voice) || !ReadWave(rom_, voice.wave, wave) || wave.size == 0)
    {
        return -1;
    }

    // A newer driver plays a reversed voice's sample backwards, without its loop, and decodes compressed samples. It
    // doesn't play a compressed voice whose sample isn't compressed.
    const bool reverse = info_.special_samples && (voice.type & kVoiceReverse);
    const bool compressed = info_.special_samples && wave.type != 0;
    if (info_.special_samples && (voice.type & kVoiceCompressed) && !compressed && !reverse)
    {
        return -1;
    }

    const auto key = std::make_pair(int(reverse ? kReversedKind : kDirectKind), wave.address);
    const int cached = CachedSample(samples_, key);
    if (cached >= 0)
    {
        return cached;
    }

    // The driver's samples are signed 8-bit. A compressed sample keeps each block of 64 points in 33 bytes: the first
    // point, then a step for each of the others, from a table of 16, in the low half of the second byte and in each
    // half of the rest, high half first.
    static const int8_t kSteps[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};
    sf2::Sample s;
    s.name = AddressName(reverse ? "Reversed" : "Sample", wave.address);
    const double level = DirectLevel(info_);
    int8_t point = 0;
    for (uint32_t i = 0; i < wave.size; i++)
    {
        const uint32_t n = i % 64;
        const uint32_t block = wave.Data() + i / 64 * 33;
        if (!compressed)
        {
            point = rom_.S8(wave.Data() + i);
        }
        else if (n == 0)
        {
            point = rom_.S8(block);
        }
        else
        {
            const uint8_t byte = rom_.U8(block + 1 + n / 2);
            point = int8_t(uint8_t(point + kSteps[n % 2 ? byte & 0xF : byte >> 4]));
        }

        s.data.push_back(int16_t(std::lround(point * 256 * level)));
    }
    if (reverse)
    {
        std::reverse(s.data.begin(), s.data.end());
    }

    // The sample's rate for key 60 is its header's frequency over 1024, which the zones fine-tune to.
    s.sample_rate = std::max<uint32_t>((wave.frequency + 512) / 1024, 1);
    s.original_key = 60;

    // A loop runs from its start to the sample's end. A short one is repeated after the end until it's long enough.
    if (wave.loops && !reverse && wave.loop_start < wave.size)
    {
        s.loop_start = wave.loop_start;
        s.loop_end = wave.size;
        LengthenLoop(s);
        AddLoopGuard(s);
    }

    return AddSample(file_, samples_, key, std::move(s));
}

int SoundfontBuilder::SquareSample(int duty)
{
    const auto key = std::make_pair(int(kSquareKind), uint32_t(duty));
    const int cached = CachedSample(samples_, key);
    if (cached >= 0)
    {
        return cached;
    }

    // 64 points a cycle, at C4.
    sf2::Sample s;
    s.name = SquareName(duty);
    SetCycle(s, SquareCycle(duty, int16_t(std::lround(32767 * PsgLevel(info_)))));
    s.sample_rate = uint32_t(std::lround(64 * kMiddleC));
    s.original_key = 60;

    return AddSample(file_, samples_, key, std::move(s));
}

int SoundfontBuilder::WaveSample(uint32_t address)
{
    const auto key = std::make_pair(int(kWaveKind), address);
    const int cached = CachedSample(samples_, key);
    if (cached >= 0)
    {
        return cached;
    }
    if (!rom_.Contains(address, 16))
    {
        return -1;
    }

    // 32 points of 4 bits, high half first, at C3: an octave below a square wave with the same setting.
    sf2::Sample s;
    s.name = AddressName("Wave", address);
    std::vector<int16_t> cycle;
    for (uint32_t i = 0; i < 32; i++)
    {
        const uint8_t byte = rom_.U8(address + i / 2);
        const int point = i % 2 ? byte & 0xF : byte >> 4;
        cycle.push_back(int16_t(std::lround((point - 7.5) * 32767 / 7.5 * PsgLevel(info_))));
    }
    SetCycle(s, cycle);
    s.sample_rate = uint32_t(std::lround(16 * kMiddleC));
    s.original_key = 60;

    return AddSample(file_, samples_, key, std::move(s));
}

int SoundfontBuilder::NoiseSample(int narrow)
{
    const auto key = std::make_pair(int(kNoiseKind), uint32_t(narrow));
    const int cached = CachedSample(samples_, key);
    if (cached >= 0)
    {
        return cached;
    }

    return AddSample(file_, samples_, key, NoiseSequence(narrow != 0, PsgLevel(info_)));
}

int SoundfontBuilder::TriangleSample(uint32_t wave)
{
    const auto key = std::make_pair(int(kTriangleKind), wave);
    const int cached = CachedSample(samples_, key);
    if (cached >= 0)
    {
        return cached;
    }

    // The wave rises from -128 to 127 over the first half of the phase's 512 steps a cycle, and falls from 128 to -127
    // over the second. A cycle lasts 64 of a sample's points, and plays at the rate the header gives a sample.
    Wave header;
    ReadWave(rom_, wave, header);
    sf2::Sample s;
    s.name = AddressName("Triangle", wave);
    std::vector<int16_t> cycle;
    for (int i = 0; i < 64; i++)
    {
        const int step = 8 * i;
        cycle.push_back(int16_t(std::min((step < 256 ? step - 128 : 384 - step) * 256, 32767)));
    }
    SetCycle(s, cycle);
    s.sample_rate = std::max<uint32_t>((header.frequency + 512) / 1024, 1);
    s.original_key = 60;

    return AddSample(file_, samples_, key, std::move(s));
}

int SoundfontBuilder::SynthSample(const Voice& voice, int key)
{
    const auto cache_key = std::make_pair(voice.wave, key);
    const int cached = CachedSample(synth_samples_, cache_key);
    if (cached >= 0)
    {
        return cached;
    }

    // The sample plays the key at its pitch in the game, at the mixer's rate.
    Wave wave;
    ReadWave(rom_, voice.wave, wave);
    const Synth synth = SynthOf(rom_, info_, voice);
    const uint32_t frequency = SampleFrequency(info_, wave.frequency, key, 0);
    const double cycle = frequency ? 64.0 * info_.mix_rate / frequency : 0.0;

    // A pulse wave's duty goes through a cycle in the frames it takes the data's fourth byte to add up to a multiple of
    // 256, and the loop lasts that long, to the nearest whole cycle of the wave. A saw wave's filter takes a few points
    // to settle, and its loop starts after a cycle that's at least 64 points long and lasts 2048 points or so.
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    if (synth == Synth::kPulse)
    {
        const int speed = rom_.U8(wave.Data() + 3);
        const uint32_t frames = speed ? 256 / uint32_t(std::gcd(speed, 256)) : 1;
        const double target = double(frames) * info_.samples_per_frame;
        loop_end =
            cycle > 0 ? uint32_t(std::lround(std::max(1.0, std::round(target / cycle)) * cycle)) : uint32_t(target);
    }
    else
    {
        const double settle = cycle > 0 ? std::ceil(64.0 / cycle) * cycle : 64.0;
        const double length = cycle > 0 ? std::max(1.0, std::round(2048.0 / cycle)) * cycle : 2048.0;
        loop_start = uint32_t(std::lround(settle));
        loop_end = uint32_t(std::lround(settle + length));
    }

    sf2::Sample s;
    s.name = AddressName(synth == Synth::kPulse ? "Pulse" : "Saw", voice.wave) + " " + std::to_string(key);
    s.data = RenderSynth(rom_, info_, synth, wave.Data(), frequency, loop_end);
    s.loop_start = loop_start;
    s.loop_end = loop_end;
    AddLoopGuard(s);
    s.sample_rate = uint32_t(info_.mix_rate);
    s.original_key = uint8_t(key);

    return AddSample(file_, synth_samples_, cache_key, std::move(s));
}

bool SoundfontBuilder::AddZone(sf2::Instrument& instrument, const Voice& voice, int low, int high, int fixed_key,
                               int rhythm_pan)
{
    // A sample voice with no attack never gets louder than silence. A pulse or saw synth voice gets a zone for each key
    // it plays, as notes play them.
    const int psg = voice.PsgChannel();
    const Synth synth = SynthOf(rom_, info_, voice);
    if (psg == 0 && voice.attack == 0)
    {
        return false;
    }
    if (synth == Synth::kPulse || synth == Synth::kSaw)
    {
        return true;
    }

    const int sample = SampleFor(voice);
    if (sample < 0)
    {
        return false;
    }

    const sf2::Sample& s = file_.samples[size_t(sample)];
    std::vector<Part> parts;
    if (psg == 0 && (voice.type & kVoiceFixed))
    {
        // A fixed-pitch sample plays at the mixer's rate.
        const double rate = info_.samples_per_frame * kFrameRate;
        parts.push_back({low, high, 1200.0 * std::log2(rate / s.sample_rate), true});
    }
    else if (psg == 0)
    {
        Wave wave;
        ReadWave(rom_, voice.wave, wave);
        const double offset = 1200.0 * std::log2(wave.frequency / 1024.0 / s.sample_rate);
        parts.push_back({low, high, offset + (fixed_key >= 0 ? 100.0 * (fixed_key - 60) : 0.0), fixed_key >= 0});
    }
    else if (psg == 4)
    {
        // Each key has a noise setting, which keys 21 to 80 run through from slow to fast. A run of keys with the same
        // setting shares a part.
        for (int key = low; key <= high;)
        {
            const uint32_t setting = KeyToPsgFrequency(4, fixed_key >= 0 ? fixed_key : key, 0);
            int last = key;
            while (fixed_key < 0 && last < high && KeyToPsgFrequency(4, last + 1, 0) == setting)
            {
                last++;
            }

            parts.push_back({key, last, 1200.0 * std::log2(NoiseClock(setting) / kNoiseSequenceRate), true});
            key = last + 1;
        }
    }
    else
    {
        // The square and wave channels play the keys below 36 at the pitch of key 36. The others follow the driver's
        // frequency settings, which are within a few cents of equal temperament.
        auto cents_for = [](int key)
        {
            return 1200.0 * std::log2(SquareFrequency(key) / kMiddleC);
        };

        if (fixed_key >= 0)
        {
            parts.push_back({low, high, cents_for(std::max(fixed_key, 36)), true});
        }
        else
        {
            if (low < 36)
            {
                parts.push_back({low, std::min(high, 35), cents_for(36), true});
            }
            if (high >= 36)
            {
                const int first = std::max(low, 36);
                parts.push_back({first, high, cents_for(first) - 100.0 * (first - 60), false});
            }
        }
    }

    AddParts(info_, voice, sample, Looped(s), parts, rhythm_pan, instrument);

    return true;
}

} // namespace agbsf2::mp2k
