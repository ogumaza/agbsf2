// SPDX-License-Identifier: MIT

#include "rd2/sequencer.h"

#include <algorithm>
#include <string>
#include <vector>

namespace agbsf2::rd2
{
namespace
{

// The sound registers the driver writes.
constexpr uint32_t kRegisters = 0x04000060;
constexpr uint32_t kNr10 = 0x04000060;
constexpr uint32_t kNr11 = 0x04000062;
constexpr uint32_t kNr12 = 0x04000063;
constexpr uint32_t kNr13 = 0x04000064;
constexpr uint32_t kNr14 = 0x04000065;
constexpr uint32_t kNr21 = 0x04000068;
constexpr uint32_t kNr22 = 0x04000069;
constexpr uint32_t kNr23 = 0x0400006C;
constexpr uint32_t kNr24 = 0x0400006D;
constexpr uint32_t kNr30 = 0x04000070;
constexpr uint32_t kNr31 = 0x04000072;
constexpr uint32_t kNr32 = 0x04000073;
constexpr uint32_t kNr33 = 0x04000074;
constexpr uint32_t kNr41 = 0x04000078;
constexpr uint32_t kNr42 = 0x04000079;
constexpr uint32_t kNr43 = 0x0400007C;
constexpr uint32_t kNr44 = 0x0400007D;
constexpr uint32_t kNr51 = 0x04000081;
constexpr uint32_t kWaveRam = 0x04000090;

// The NRx2 value that tells the PSG voices to leave a channel's envelope as it is.
constexpr uint8_t kNoEnvelope = 8;

// The time of a track that F8 has started and that hasn't run yet.
constexpr uint64_t kFresh = ~uint64_t(0);

// The highest pitch index in the driver's tables.
constexpr int kTopIndex = 0x77;

// The most commands a track reads in a frame before the model gives up on it, so that a loop without a wait can't hang
// the conversion. The driver would hang there.
constexpr int kMaxCommands = 100000;

// The tuning of the region the driver keeps in RAM for instruments with a sample for each key, which plays a note of
// 0x30 at the sample's rate. The region is a sample voice's, with no flags and no release.
constexpr uint8_t kKeySampleTuning = 0x30;

// Returns a / b as the driver's division does: libgcc's, which gives 0 for a divisor of 0.
uint32_t UnsignedDiv(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}

// Returns a / b, rounded towards 0, as the driver's signed divisions do, or 0 for a divisor of 0, as libgcc's division
// does. The driver's mixer uses the BIOS's Div instead, which never returns for a divisor of 0.
int32_t SignedDiv(int32_t a, int32_t b)
{
    return b ? int32_t(int64_t(a) / int64_t(b)) : 0;
}

// Returns an event without a note's settings.
Event MakeEvent(Event::Kind kind, int track, int voice, uint32_t id, uint64_t units, uint32_t value,
                Event::Phase phase = Event::kCommands)
{
    Event e;
    e.kind = kind;
    e.phase = phase;
    e.track = track;
    e.voice = voice;
    e.id = id;
    e.units = units;
    e.value = value;

    return e;
}

} // namespace

// An instrument gives a note a region to play: its own, a drum kit's for the key, a key split's, or the driver's region
// in RAM with the instrument's sample for the key.
Lookup LookUpInstrument(const Rom& rom, const DriverInfo& info, uint16_t bank, uint16_t instrument, uint8_t note)
{
    Lookup lk;
    const uint32_t base = info.banks + rom.U32(info.banks + 4 * uint32_t(bank));
    const uint32_t inst = base + rom.U16(base + 2 * uint32_t(instrument));
    const uint8_t kind = rom.U8(inst);
    if ((kind & 0xF0) == 0)
    {
        lk.region = inst;
        lk.envelope = base + rom.U16(inst + 4);
        lk.found = true;
    }
    else if (kind == 0x10)
    {
        const uint32_t entry = base + rom.U16(inst + 2) + 4 * uint32_t(uint8_t(note - rom.U8(inst + 4)));
        lk.drum = true;
        lk.drum_pan = rom.U8(entry + 2);
        lk.region = base + rom.U16(entry);
        lk.envelope = base + rom.U16(lk.region + 4);
        lk.found = true;
    }
    else if (kind == 0x11)
    {
        lk.key_sample = true;
        lk.key_sample_index = rom.U16(base + rom.U16(inst + 2) + 2 * uint32_t(note));
        lk.region = 0;
        lk.envelope = info.key_envelope;
        lk.found = true;
    }
    else if (kind == 0x12)
    {
        uint32_t entry = base + rom.U16(inst + 2);
        for (int i = 0; i < 256 && note > rom.U8(entry); i++)
        {
            entry += 4;
        }
        lk.region = base + rom.U16(entry + 2);
        lk.envelope = base + rom.U16(lk.region + 4);
        lk.found = true;
    }

    lk.missing = lk.region == base;
    if (!lk.found || !lk.region)
    {
        return lk;
    }

    if (rom.U8(lk.region) == kWaveType)
    {
        lk.wave = base + rom.U16(inst + 2);
    }
    if (rom.U8(lk.region + 1) & 1)
    {
        lk.psg_table = base + rom.U16(lk.region + 2);
    }

    return lk;
}

uint32_t SampleAddress(const Rom& rom, const DriverInfo& info, uint16_t bank, uint32_t index)
{
    const uint16_t set = rom.U16(info.bank_sample_sets + 2 * uint32_t(bank));
    const uint32_t samples = info.sample_sets + rom.U32(info.sample_sets + 4 * uint32_t(set));
    return samples + rom.U32(samples + 4 * index);
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int sequence) : rom_(rom), info_(info), sequence_(sequence)
{
    valid_ = sequence >= 0 && sequence < int(info.sequence_addresses.size());

    // The voices as the driver's init leaves them: the sample voices in the free list, and a PSG voice for each of the
    // PSG's channels.
    for (int v = 0; v < kSampleVoices; v++)
    {
        voices_[size_t(v)].prev = v - 1;
        voices_[size_t(v)].next = v + 1 < kSampleVoices ? v + 1 : -1;
    }
    for (int i = 0; i < kPsgVoices; i++)
    {
        voices_[size_t(kSampleVoices + i)].type = uint8_t(kSquare1Type + i);
    }
    free_ = 0;

    // The registers that init writes and the PSG voices read back.
    registers_[kNr10 - kRegisters] = 0x08;
    registers_[kNr12 - kRegisters] = 0xF0;
    registers_[kNr51 - kRegisters] = 0xFF;
}

void Sequencer::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

std::vector<int> Sequencer::ActiveOrder() const
{
    std::vector<int> order;
    for (int v = active_; v >= 0 && order.size() < kSampleVoices; v = voices_[size_t(v)].next)
    {
        order.push_back(v);
    }

    return order;
}

void Sequencer::Write(uint32_t address, uint8_t size, uint32_t value)
{
    value &= size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    writes_.push_back({address, size, value});
    for (uint8_t i = 0; i < size; i++)
    {
        const uint32_t at = address + i - kRegisters;
        if (at < registers_.size())
        {
            registers_[at] = uint8_t(value >> (8 * i));
        }
    }
}

uint32_t Sequencer::ReadBack(uint32_t address, uint8_t size) const
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++)
    {
        const uint32_t at = address + i - kRegisters;
        value |= uint32_t(at < registers_.size() ? registers_[at] : 0) << (8 * i);
    }

    return value;
}

uint8_t Sequencer::RegionType(uint32_t region) const
{
    return region ? rom_.U8(region) : kSampleType;
}

uint8_t Sequencer::RegionFlags(uint32_t region) const
{
    return region ? rom_.U8(region + 1) : 0;
}

uint8_t Sequencer::RegionRelease(uint32_t region) const
{
    return region ? rom_.U8(region + 6) : 0;
}

uint8_t Sequencer::RegionTuning(uint32_t region) const
{
    return region ? rom_.U8(region + 7) : kKeySampleTuning;
}

uint8_t Sequencer::RegionSweep(uint32_t region) const
{
    return region ? rom_.U8(region + 8) : 0;
}

// Carries out the play request: stops what the player was playing and starts each of the sequence's tracks.
void Sequencer::Start()
{
    started_ = true;
    if (!valid_)
    {
        return;
    }

    // The game's play-music routine requests the sample voices that the sequence plays with, then the sequence, then
    // the player's second volume. The driver handles the requests in that order.
    const MusicSettings settings =
        size_t(sequence_) < info_.music_settings.size() ? info_.music_settings[size_t(sequence_)] : MusicSettings();
    SetSampleVoices(settings.sample_voices);

    StopPlayer();
    player_ = Player();
    player_.volume2 = uint8_t(settings.volume2);
    player_.sequence = info_.sequence_addresses[size_t(sequence_)];
    player_.banks = info_.bank_list_addresses[size_t(sequence_)];

    int count = rom_.S8(player_.sequence);
    if (count > kPlayerTracks)
    {
        Warn("the sequence has " + std::to_string(count) + " tracks, more than a player can have");
        count = kPlayerTracks;
    }
    for (int i = 0; i < count; i++)
    {
        const uint32_t offset = rom_.U16(player_.sequence + 2 + 2 * uint32_t(i));
        if (offset == 0)
        {
            continue;
        }

        const int slot = AllocateTrack();
        player_.tracks[size_t(i)] = slot;
        if (slot >= 0)
        {
            StartTrack(slot, player_.sequence + offset, i, 0);
        }
    }

    player_.active = true;
}

int Sequencer::AllocateTrack() const
{
    for (int slot = 0; slot < kTrackSlots; slot++)
    {
        if (!tracks_[size_t(slot)].active)
        {
            return slot;
        }
    }

    return -1;
}

// Starts a track in `slot` at `position`, after stopping the track the slot had. It starts with the player's first
// bank, centred, at full volume, with the notes' default length and velocity.
void Sequencer::StartTrack(int slot, uint32_t position, int number, uint64_t units)
{
    if (tracks_[size_t(slot)].active)
    {
        FreeTrack(slot);
    }

    Track& t = tracks_[size_t(slot)];
    t = Track();
    t.position = position;
    t.active = true;
    t.number = number;
    t.units = units;
    SetBank(t, 0);
}

void Sequencer::FreeTrack(int slot)
{
    ReleaseTrackVoices(slot);
    tracks_[size_t(slot)].active = false;
}

void Sequencer::ReleaseTrackVoices(int slot)
{
    Track& t = tracks_[size_t(slot)];
    const uint8_t legato = t.legato;
    t.legato = 0;
    for (int v = t.voices; v >= 0;)
    {
        const int next = voices_[size_t(v)].track_next;
        Release(v);
        v = next;
    }
    t.legato = legato;
}

void Sequencer::StopPlayer()
{
    if (!player_.active)
    {
        return;
    }

    for (int& slot : player_.tracks)
    {
        if (slot >= 0)
        {
            FreeTrack(slot);
        }
        slot = -1;
    }
    player_.active = false;
}

// Sets a track's bank to entry `bank` of the player's list of banks, with the bank's first instrument and the sample
// set it plays from, as C7 and a track's start do.
void Sequencer::SetBank(Track& t, uint16_t bank)
{
    t.bank = bank;
    t.instrument = 0;
    const uint16_t index = rom_.U16(player_.banks + 2 * uint32_t(bank));
    const uint16_t set = rom_.U16(info_.bank_sample_sets + 2 * uint32_t(index));
    t.samples = info_.sample_sets + rom_.U32(info_.sample_sets + 4 * uint32_t(set));
}

uint32_t Sequencer::ReadVarLength(Track& t)
{
    uint32_t value = rom_.U8(t.position++);
    if (value & 0x80)
    {
        value = ((value & 0x7F) << 8) | rom_.U8(t.position++);
    }

    return value;
}

void Sequencer::Step()
{
    events_.clear();
    writes_.clear();
    if (!started_)
    {
        Start();
    }

    // Each of the player's tracks reads its commands until its count of time is in the future, then takes the frame's
    // tempo off the count. A player stops when none of its tracks is left.
    phase_ = Event::kCommands;
    if (player_.active)
    {
        bool running = false;
        for (size_t i = 0; i < player_.tracks.size(); i++)
        {
            const int slot = player_.tracks[i];
            if (slot < 0)
            {
                continue;
            }

            if (RunTrack(slot) == 0)
            {
                running = true;
            }
            else
            {
                player_.tracks[i] = -1;
            }
        }
        if (!running)
        {
            StopPlayer();
        }
    }

    phase_ = Event::kPsgVoices;
    UpdatePsgVoices();
    phase_ = Event::kMixer;
    UpdateSampleVoices();
    elapsed_ += uint64_t(std::max(0, int(player_.tempo) + player_.tempo_adjust));
}

// Runs a track's commands for a frame. Returns 0 if it's still playing, 1 if it isn't active, or 2 if it has ended.
int Sequencer::RunTrack(int slot)
{
    Track& t = tracks_[size_t(slot)];
    if (!t.active)
    {
        return 1;
    }

    if (t.units == kFresh)
    {
        t.units = elapsed_;
    }

    for (int n = 0; t.counter <= 0; n++)
    {
        if (n == kMaxCommands)
        {
            Warn("track " + std::to_string(t.number) + " runs without waiting at " + Hex(t.position) +
                 ", which would hang the driver");
            FreeTrack(slot);
            return 2;
        }

        const uint32_t at = t.position;
        t.seen.emplace(at, t.units);
        command_units_ = t.units;
        const uint8_t op = rom_.U8(t.position++);
        if (op <= 0xBF)
        {
            // A note, with the track's length and velocity or new ones.
            uint8_t note = op;
            if (op >= 0x60)
            {
                t.length = uint16_t(ReadVarLength(t));
                t.velocity = rom_.U8(t.position++);
                note = uint8_t(op - 0x60);
            }

            const uint32_t length = uint32_t(t.length) * kTickUnits;
            NoteOn(slot, note, t.velocity, length);
            if (t.timed_notes == 1)
            {
                t.counter += int32_t(length);
                t.units += length;
            }
            continue;
        }
        if (op == 0xC0 || op == 0xC1)
        {
            if (op == 0xC1)
            {
                t.wait = uint16_t(ReadVarLength(t));
            }

            const uint32_t wait = uint32_t(t.wait) * kTickUnits;
            t.counter += int32_t(wait);
            t.units += wait;
            continue;
        }
        if ((op & 0xF0) == 0xD0)
        {
            // A slide for the next note, from or to another note.
            t.slide_flags = op & 0x0F;
            t.slide_note = uint8_t(rom_.U8(t.position) + t.transpose);
            t.slide_length = rom_.U8(t.position + 1);
            t.position += 2;
            t.slide_delay = 0;
            if (op & 1)
            {
                t.slide_delay = rom_.U8(t.position++);
            }
            t.slide = 1;
            continue;
        }

        switch (op)
        {
        case 0xC2:
            t.instrument = rom_.U8(t.position++);
            break;

        case 0xC3:
            t.pan = rom_.U8(t.position++);
            events_.push_back(MakeEvent(Event::kPan, t.number, -1, 0, t.units, t.pan));
            break;

        case 0xC4:
            t.priority = rom_.U8(t.position++);
            break;

        case 0xC5:
        case 0xC6:
            ReleaseTrackVoices(slot);
            t.legato = op == 0xC5 ? 1 : 0;
            break;

        case 0xC7:
            SetBank(t, rom_.U8(t.position++));
            break;

        case 0xC8:
            t.timed_notes = 1;
            break;

        case 0xC9:
            t.timed_notes = 0;
            break;

        case 0xCA:
            // A call to the game's code, which the game may not have given the driver.
            Warn("the sequence calls the game's code with CA, which the model skips");
            t.position++;
            break;

        case 0xE0:
            t.volume = rom_.U8(t.position++);
            events_.push_back(MakeEvent(Event::kTrackVolume, t.number, -1, 0, t.units, t.volume));
            break;

        case 0xE1:
            t.bend = rom_.S8(t.position++);
            break;

        case 0xE2:
            t.bend_range = rom_.U8(t.position++);
            break;

        case 0xE3:
            t.echo = rom_.U8(t.position++);
            break;

        case 0xE4:
            // The Super Mario Advance 2 revision's tempo is a byte.
            player_.tempo =
                info_.revision == Revision::kSuperMarioAdvance2 ? rom_.U8(t.position++) : uint16_t(ReadVarLength(t));
            break;

        case 0xE5:
            t.lfo_delay = rom_.U8(t.position++);
            break;

        case 0xE6:
            t.lfo_rate = rom_.U8(t.position++);
            break;

        case 0xE7:
            t.lfo_depth = rom_.U8(t.position++);
            break;

        case 0xE8:
            t.slide = 0;
            break;

        case 0xE9:
            t.transpose = rom_.U8(t.position++);
            break;

        case 0xEA:
            // The Super Mario Advance 2 revision has no player volume, and EA is one of the commands that do nothing.
            if (info_.revision != Revision::kSuperMarioAdvance2)
            {
                player_.volume = rom_.U8(t.position++);
                events_.push_back(MakeEvent(Event::kVolume, t.number, -1, 0, t.units, player_.volume));
            }
            break;

        case 0xF0:
            {
                t.position = player_.sequence + rom_.U16(t.position);
                Event e = MakeEvent(Event::kJump, t.number, -1, 0, t.units, t.position);
                const auto seen = t.seen.find(t.position);
                e.target_units = seen != t.seen.end() ? seen->second : kFresh;
                events_.push_back(e);
                break;
            }

        case 0xF4:
            {
                const uint32_t to = player_.sequence + rom_.U16(t.position);
                if (t.depth == kMaxDepth)
                {
                    Warn("F4 at " + Hex(at) + " nests deeper than the driver has room for");
                    FreeTrack(slot);
                    return 2;
                }

                t.stack[size_t(t.depth++)] = t.position + 2;
                t.position = to;
                break;
            }

        case 0xF8:
            {
                // Starts one of the player's tracks with this one's settings.
                const int number = rom_.U8(t.position);
                const uint32_t to = player_.sequence + rom_.U16(t.position + 1);
                t.position += 3;
                if (number >= kPlayerTracks)
                {
                    Warn("F8 at " + Hex(at) + " starts track " + std::to_string(number) + ", past the player's 10");
                    break;
                }

                int other = player_.tracks[size_t(number)];
                if (other < 0)
                {
                    other = AllocateTrack();
                    player_.tracks[size_t(number)] = other;
                    if (other < 0)
                    {
                        Warn("F8 at " + Hex(at) + " finds no free track");
                        break;
                    }
                }

                // The new track's time starts with the first frame it runs in: this one if it comes later in the
                // player's order, or is this one.
                StartTrack(other, to, number, other == slot ? elapsed_ : kFresh);
                Track& o = tracks_[size_t(other)];
                o.samples = t.samples;
                o.bank = t.bank;
                o.instrument = t.instrument;
                o.pan = t.pan;
                o.echo = t.echo;
                o.volume = t.volume;
                o.volume2 = t.volume2;
                o.priority = t.priority;
                o.bend = t.bend;
                o.bend_range = t.bend_range;
                o.transpose = t.transpose;
                events_.push_back(MakeEvent(Event::kTrackStart, t.number, -1, 0, t.units, uint32_t(number)));
                break;
            }

        case 0xFF:
            if (t.depth == 0)
            {
                events_.push_back(MakeEvent(Event::kTrackEnd, t.number, -1, 0, t.units, 0));
                FreeTrack(slot);
                return 2;
            }

            t.position = t.stack[size_t(--t.depth)];
            break;

        default:
            break;
        }
    }

    t.counter -= int32_t(player_.tempo);
    t.counter -= int32_t(player_.tempo_adjust);

    return 0;
}

void Sequencer::NoteOn(int slot, uint8_t note, uint8_t velocity, uint32_t length)
{
    Track& t = tracks_[size_t(slot)];
    const uint8_t given = note;
    if (t.mute)
    {
        return;
    }

    note = uint8_t(note + t.transpose);
    Event e;
    e.kind = Event::kNote;
    e.track = t.number;
    e.units = t.units;
    e.key = note;
    e.velocity = velocity;
    e.bank = rom_.U16(player_.banks + 2 * uint32_t(t.bank));
    e.instrument = t.instrument;
    e.length = length;
    e.lookup = LookUpInstrument(rom_, info_, e.bank, t.instrument, note);
    const Lookup& lk = e.lookup;
    if (!lk.found)
    {
        Warn("instrument " + std::to_string(t.instrument) + " of bank " + std::to_string(e.bank) +
             " isn't one the driver can play, so its notes are left out");
        events_.push_back(e);
        return;
    }

    if (lk.key_sample)
    {
        key_sample_ = lk.key_sample_index;
    }

    // A note lasts its length at the tempo it starts at: with the game's change to the tempo unless the region opts out
    // of it. The Super Mario Advance 2 revision keeps the length in units, and the frames, in 16 bits.
    const uint32_t region = lk.region;
    const int tempo = (RegionFlags(region) & 0x10) ? player_.tempo : player_.tempo + player_.tempo_adjust;
    const bool mario = info_.revision == Revision::kSuperMarioAdvance2;
    const uint32_t frames =
        mario ? uint16_t(UnsignedDiv(uint16_t(length), uint32_t(tempo))) : UnsignedDiv(length, uint32_t(tempo));

    // In legato, the track's voice plays the note; otherwise it gets a voice of its own, which starts its envelope.
    int v = t.voices;
    if (!t.legato || v < 0)
    {
        v = AllocateVoice(rom_.U8(info_.voice_classes + RegionType(region)), t.priority);
        if (v < 0)
        {
            events_.push_back(e);
            return;
        }

        LinkToTrack(slot, v);
        Voice& x = voices_[size_t(v)];
        x.note = mario ? 0 : given;
        x.position = 0;
        x.lfo_phase = 0;
        x.lfo_delay = t.lfo_delay;
        x.envelope = lk.envelope;
        x.level = 0;
        x.target = 0;
        x.segment = 0;
        x.point = 0xFF;
        x.release = RegionRelease(region);
        x.region = region;
    }

    Voice& x = voices_[size_t(v)];
    x.id = next_id_++;
    e.voice = v;
    e.id = x.id;

    // Drums and instruments with a sample for each key play at their region's pitch.
    x.own_pan = lk.drum ? 1 : 0;
    if (lk.drum)
    {
        note = 0x30;
        x.pan = lk.drum_pan;
    }
    else if (lk.key_sample)
    {
        note = 0x30;
    }

    x.velocity = velocity;
    x.volume = 0;
    x.frames = uint16_t(frames);
    x.echo = t.echo;
    x.base_pitch = PitchStep(x.type, note, RegionTuning(region));
    x.note_pitch = x.base_pitch;

    // A slide moves the pitch between the note and the slide's note over a share of the note's frames.
    if (!t.slide)
    {
        x.slide_delay = 0;
        x.slide_frames = 0;
        x.slide = 0;
        x.slide_end = 0;
        x.slide_step = 0;
    }
    else
    {
        const uint32_t target = PitchStep(x.type, t.slide_note, RegionTuning(region));
        x.slide_delay = t.slide_delay;
        x.slide_frames = (uint32_t(t.slide_length) * frames) >> 8;
        if (t.slide_flags & 2)
        {
            x.slide_end = int32_t(target - x.base_pitch);
        }
        else
        {
            x.slide_end = int32_t(x.base_pitch - target);
            x.base_pitch = target;
        }
        x.slide_step = SignedDiv(x.slide_end, int32_t(x.slide_frames));
        if (t.slide_flags & 4)
        {
            t.slide_note = note;
        }
        else
        {
            t.slide = 0;
        }
        x.slide = 0;
    }

    if (x.type == kSampleType)
    {
        const uint32_t index = region ? rom_.U16(region + 2) : key_sample_;
        x.sample = t.samples + rom_.U32(t.samples + 4 * index);
    }
    else if (x.type == kWaveType)
    {
        x.psg = lk.wave;
    }
    else if (RegionFlags(region) & 1)
    {
        x.psg = lk.psg_table;
    }
    else
    {
        x.psg = (x.psg & ~0xFFu) | (rom_.U16(region + 2) & 0xFFu);
    }

    e.type = x.type;
    e.sample = x.sample;
    e.psg = x.psg;
    events_.push_back(e);

    if (frames == 0)
    {
        Release(v);
    }
}

// Returns a voice for a note of the class, or -1 if none can be had: a free sample voice, or the first of the voices
// that play, if it's released or its priority isn't above the note's; or the PSG voice of the class, on the same terms.
int Sequencer::AllocateVoice(uint8_t voice_class, uint8_t priority)
{
    int v = -1;
    if (voice_class == 0)
    {
        if (free_ >= 0)
        {
            v = free_;
        }
        else
        {
            v = active_;
            if (v < 0 || (voices_[size_t(v)].state == 1 && priority < voices_[size_t(v)].priority))
            {
                return -1;
            }

            StopVoice(v);
        }

        Unlink(v, free_);
        voices_[size_t(v)].state = 1;
        voices_[size_t(v)].priority = priority;
        InsertActive(v);

        return v;
    }

    if (voice_class > kPsgVoices)
    {
        Warn("a region has a type the driver has no voice for");
        return -1;
    }

    v = kSampleVoices + voice_class - 1;
    Voice& x = voices_[size_t(v)];
    if (x.state == 1 && priority < x.priority)
    {
        return -1;
    }

    if (x.state != 0)
    {
        StopVoice(v);
    }
    x.state = 1;
    x.priority = priority;

    return v;
}

// Takes sample voices out of use until `count` are left, as the driver's request 0x0304 does. The driver allocates
// each one as a note of the highest priority would. That takes the first free voice, or else the first voice that
// plays. The driver then takes the voice off the list of voices that play and keeps it on a separate list. Neither the
// notes nor the mixer use that list.
void Sequencer::SetSampleVoices(int count)
{
    for (int left = kSampleVoices; left > count; left--)
    {
        const int v = AllocateVoice(0, 0xFF);
        if (v < 0)
        {
            break;
        }

        Unlink(v, active_);
        voices_[size_t(v)].state = 0;
    }
}

void Sequencer::Unlink(int v, int& head)
{
    const Voice& x = voices_[size_t(v)];
    if (x.prev >= 0)
    {
        voices_[size_t(x.prev)].next = x.next;
    }
    else
    {
        head = x.next;
    }
    if (x.next >= 0)
    {
        voices_[size_t(x.next)].prev = x.prev;
    }
}

void Sequencer::PushFront(int v, int& head)
{
    Voice& x = voices_[size_t(v)];
    x.next = head;
    x.prev = -1;
    if (head >= 0)
    {
        voices_[size_t(head)].prev = v;
    }
    head = v;
}

// Puts a sample voice in the list the mixer takes them in. Released voices come first, then those that play, each
// ordered by priority and then by age, so that a new note takes the first.
void Sequencer::InsertActive(int v)
{
    Voice& x = voices_[size_t(v)];
    int before = -1;
    int at = active_;
    if (x.state == 1)
    {
        if (at >= 0 && !(voices_[size_t(at)].state == 1 && x.priority < voices_[size_t(at)].priority))
        {
            do
            {
                before = at;
                at = voices_[size_t(at)].next;
            } while (at >= 0 && (voices_[size_t(at)].state != 1 || x.priority >= voices_[size_t(at)].priority));
        }
    }
    else if (x.state == 2)
    {
        if (at >= 0 && voices_[size_t(at)].state != 1 && x.priority >= voices_[size_t(at)].priority)
        {
            do
            {
                before = at;
                at = voices_[size_t(at)].next;
            } while (at >= 0 && voices_[size_t(at)].state != 1 && x.priority >= voices_[size_t(at)].priority);
        }
    }
    else
    {
        return;
    }

    x.next = at;
    if (at >= 0)
    {
        voices_[size_t(at)].prev = v;
    }
    x.prev = before;
    if (before >= 0)
    {
        voices_[size_t(before)].next = v;
    }
    else
    {
        active_ = v;
    }
}

void Sequencer::LinkToTrack(int slot, int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.track >= 0)
    {
        return;
    }

    Track& t = tracks_[size_t(slot)];
    x.track = slot;
    x.track_prev = -1;
    x.track_next = t.voices;
    t.voices = v;
    if (x.track_next >= 0)
    {
        voices_[size_t(x.track_next)].track_prev = v;
    }
}

void Sequencer::UnlinkFromTrack(int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.track < 0)
    {
        return;
    }

    const int slot = x.track;
    x.track = -1;
    if (x.track_next >= 0)
    {
        voices_[size_t(x.track_next)].track_prev = x.track_prev;
    }
    if (x.track_prev >= 0)
    {
        voices_[size_t(x.track_prev)].track_next = x.track_next;
    }
    else
    {
        tracks_[size_t(slot)].voices = x.track_next;
    }
}

// Releases a voice's note: a sample or wave voice fades out by itself, and a square or noise channel fades with its
// envelope, which frees the voice at once. A track in legato holds its notes.
void Sequencer::Release(int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.state != 1 || x.track < 0 || tracks_[size_t(x.track)].legato)
    {
        return;
    }

    if (x.type == kSampleType)
    {
        Unlink(v, active_);
        x.state = 2;
        InsertActive(v);
    }
    else if (x.type == kWaveType)
    {
        x.state = 2;
    }
    else
    {
        const uint32_t frequency = x.pitch & 0xFFFF;
        const uint32_t step = x.release >> 5u;
        const uint8_t envelope = step ? uint8_t((x.volume << 4) | step) : 0;
        if (x.type == kSquare1Type)
        {
            Write(kNr12, 1, envelope);
            Write(kNr13, 2, frequency | 0x8000);
            Write(kNr11, 1, ReadBack(kNr11, 1) & 0xC0);
        }
        else if (x.type == kSquare2Type)
        {
            Write(kNr22, 1, envelope);
            Write(kNr23, 2, frequency | 0x8000);
            Write(kNr21, 1, ReadBack(kNr21, 1) & 0xC0);
        }
        else if (x.type == kNoiseType)
        {
            Write(kNr42, 1, envelope);
            Write(kNr44, 1, 0x80);
        }
        x.state = 0;
    }

    events_.push_back(MakeEvent(Event::kRelease, tracks_[size_t(x.track)].number, v, x.id,
                                phase_ == Event::kCommands ? command_units_ : 0, 0, phase_));
    if (!x.own_pan)
    {
        x.pan = tracks_[size_t(x.track)].pan;
    }
    UnlinkFromTrack(v);
}

// Stops a voice at once and frees it.
void Sequencer::StopVoice(int v)
{
    Voice& x = voices_[size_t(v)];
    if (x.state == 0)
    {
        return;
    }

    switch (x.type)
    {
    case kSampleType:
        Unlink(v, active_);
        PushFront(v, free_);
        break;

    case kSquare1Type:
        Write(kNr12, 1, kNoEnvelope);
        Write(kNr14, 1, 0xC0);
        break;

    case kSquare2Type:
        Write(kNr22, 1, kNoEnvelope);
        Write(kNr24, 1, 0xC0);
        break;

    case kWaveType:
        Write(kNr30, 1, 0);
        break;

    case kNoiseType:
        Write(kNr42, 1, kNoEnvelope);
        Write(kNr44, 1, 0xC0);
        break;

    default:
        break;
    }

    events_.push_back(MakeEvent(Event::kStop, x.track >= 0 ? tracks_[size_t(x.track)].number : -1, v, x.id,
                                phase_ == Event::kCommands ? command_units_ : 0, 0, phase_));
    UnlinkFromTrack(v);
    x.state = 0;
}

// Returns a voice's pitch for a note: a sample's step from the pitch table, a square or wave's frequency setting, or a
// noise voice's index into the noise table. The index is the note less the region's tuning, plus 0x30.
uint32_t Sequencer::PitchStep(uint8_t type, uint8_t note, uint8_t tuning) const
{
    const int16_t raw = int16_t(uint16_t(note + 0x30 - tuning));
    const int index = raw < 0 ? 0 : raw > kTopIndex ? kTopIndex + 1 : raw;
    if (type == kSampleType)
    {
        return rom_.U32(info_.pitch_table + 4 * uint32_t(index));
    }
    if (type == kNoiseType)
    {
        return uint32_t(index);
    }

    return rom_.U16(info_.frequency_table + 2 * uint32_t(index));
}

// Moves a voice's envelope on by a frame, starting its next segment when one ends. A point with a negative length holds
// the level of the point before it. Returns the new level.
int32_t Sequencer::EnvelopeStep(Voice& v)
{
    if (v.segment == 0)
    {
        v.level = v.target;
        const uint8_t last = v.point;
        v.point = uint8_t(last + 1);
        if (rom_.U16(v.envelope + 4 * uint32_t(int32_t(int8_t(v.point)))) & 0x8000)
        {
            v.point = last;
        }

        const uint32_t at = v.envelope + 4 * uint32_t(int32_t(int8_t(v.point)));
        v.target = int16_t(rom_.U16(at + 2));
        v.segment = rom_.U16(at);
        v.level_step = int16_t(SignedDiv(int16_t(uint16_t(v.target - v.level)), v.segment));
    }

    v.level += v.level_step;
    v.segment--;

    return v.level;
}

// Returns a voice's pitch for the frame: its note with the slide, the track's bend and the LFO. A sample's pitch is a
// step that rises with the pitch; a square or wave's is a frequency setting, which the bend and LFO work on as 2048
// less the setting, the period.
uint32_t Sequencer::FramePitch(Voice& v)
{
    const Track& t = tracks_[size_t(std::max(v.track, 0))];
    uint32_t pitch = v.base_pitch;
    if (v.slide_delay != 0)
    {
        v.slide_delay--;
    }
    else if (v.slide_frames != 0)
    {
        v.slide += v.slide_step;
        if (--v.slide_frames == 0)
        {
            v.slide = v.slide_end;
        }
    }
    pitch += uint32_t(v.slide);

    const int bend = t.bend;
    if (bend != 0 && info_.revision == Revision::kSuperMarioAdvance2)
    {
        // The Super Mario Advance 2 revision scales the pitch by a factor in 256ths, which goes in a straight line from
        // 1 at no bend to the range's step at a bend of 128, both ways.
        const uint32_t range = rom_.U32(info_.pitch_table + 4 * (uint32_t(t.bend_range) + 0x30));
        const uint32_t factor = uint32_t(int32_t((range - 0x8000) * uint32_t(bend) + 0x400000) >> 14);
        pitch = v.type == kSampleType ? (pitch * factor) >> 8 : 0x800 - UnsignedDiv((0x800 - pitch) << 8, factor);
    }
    else if (bend != 0)
    {
        const uint32_t range = rom_.U32(info_.pitch_table + 4 * (uint32_t(t.bend_range) + 0x30));
        const uint32_t scale = uint32_t(bend > 0 ? bend : -bend) * range + 0x400000;
        if (bend > 0)
        {
            pitch = v.type == kSampleType ? (pitch * (scale >> 7)) >> 15
                                          : 0x800 - UnsignedDiv((0x800 - pitch) << 22, scale);
        }
        else if (v.type == kSampleType)
        {
            pitch = UnsignedDiv(pitch << 15, scale >> 7);
        }
        else
        {
            pitch = ((0x800 - pitch) * (scale >> 7)) >> 15;
            pitch = pitch > 0x7FF ? 0 : 0x800 - pitch;
        }
    }

    // The LFO waits out its delay, then follows its sine at its rate.
    const uint32_t depth = t.lfo_depth;
    if (depth == 0)
    {
        return pitch;
    }
    if (v.lfo_delay != 0)
    {
        v.lfo_delay--;
        return pitch;
    }

    const int sine = rom_.S8(info_.lfo_table + (v.lfo_phase >> 1));
    if (v.type == kSampleType)
    {
        if (sine >= 0)
        {
            pitch += (uint32_t(sine) * pitch * depth) >> 19;
        }
        else
        {
            pitch = UnsignedDiv(pitch << 12, ((uint32_t(-sine) * depth) >> 3) + 0x10000) << 4;
        }
    }
    else
    {
        uint32_t period;
        if (sine >= 0)
        {
            period = UnsignedDiv((0x800 - pitch) << 19, uint32_t(sine) * depth + 0x80000);
        }
        else
        {
            period = ((0x800 - pitch) * (uint32_t(-sine) * depth + 0x80000)) >> 19;
        }
        pitch = 0x800 - period;
    }

    v.lfo_phase += t.lfo_rate;
    if ((v.lfo_phase >> 1) > 0xFF)
    {
        v.lfo_phase -= 0x200;
    }

    return pitch;
}

// Returns a sample voice's level for the frame, and keeps its loudness for its release: the velocity, the player's and
// the track's volumes and the envelope while it plays, and a fade at the release's rate after it.
uint32_t Sequencer::SampleLevel(Voice& v)
{
    const bool mario = info_.revision == Revision::kSuperMarioAdvance2;
    uint32_t level;
    if (v.state == 1 && mario)
    {
        // The Super Mario Advance 2 revision has no second volume for the player, and scales the level to 127 at most
        // rather than 190.
        const Track& t = tracks_[size_t(std::max(v.track, 0))];
        level = uint32_t(v.velocity) << 8;
        level = (level * player_.fade) >> 7;
        level = (level * player_.volume) >> 7;
        level = (level * t.volume) >> 15;
        level = (level * t.volume2) >> 7;
        level = (level * uint32_t(EnvelopeStep(v))) >> 15;
        v.volume = level;
    }
    else if (v.state == 1)
    {
        const Track& t = tracks_[size_t(std::max(v.track, 0))];
        level = uint32_t(v.velocity) << 7;
        level = (level * player_.fade) >> 8;
        level = (level * player_.volume) >> 7;
        level = (level * player_.volume2) >> 8;
        level = (level * t.volume) >> 8;
        level = (level * t.volume2) >> 15;
        level = (level * uint32_t(EnvelopeStep(v))) >> 11;
        v.volume = level;
    }
    else
    {
        level = ((uint32_t(v.release) + 0xE6) * v.volume) >> 9;
        v.volume = level;
    }

    // A Link to the Past's revision multiplies the level by 3 and shifts it right by 9. The other two shift it right
    // by 8.
    return info_.revision == Revision::kLinkToThePast ? (level * 3) >> 9 : level >> 8;
}

// Returns a PSG voice's NRx2 for the frame, or for the wave voice its volume (0-4), or 8 to leave the channel as it is.
// The PSG's envelope runs each segment of the voice's: from the level at its start to the level at its end, stepping at
// the rate that covers the difference in the segment's frames. A voice panned to one side is twice as loud, since it's
// heard on that side only.
uint8_t Sequencer::PsgEnvelope(Voice& v, bool panned)
{
    const bool starts = v.segment == 0;
    uint32_t level = v.velocity;
    EnvelopeStep(v);
    if (!starts)
    {
        return kNoEnvelope;
    }

    // A released wave voice has no track, and the driver reads its settings from address 0, where the emulator reads 0.
    Track none;
    none.volume = 0;
    none.volume2 = 0;
    const Track& t = v.track >= 0 ? tracks_[size_t(v.track)] : none;
    const bool player = v.track >= 0;

    if (panned)
    {
        level <<= 1;
    }
    level <<= 15;
    level = (level * t.volume) >> 14;
    level = (level * t.volume2) >> 7;
    if (info_.revision == Revision::kSuperMarioAdvance2)
    {
        // The Super Mario Advance 2 revision has no second volume for the player.
        level = (level * (player ? player_.volume : 0)) >> 8;
    }
    else
    {
        level = (level * (player ? player_.volume : 0)) >> 7;
        level = (level * (player ? player_.volume2 : 0)) >> 8;
    }
    level = level * (player ? player_.fade : 0);
    if (v.type == kWaveType)
    {
        level >>= 22;
        v.volume = level;

        return uint8_t(std::min<uint32_t>((level * 5) >> 7, 4));
    }

    level >>= 15;
    v.volume = level;
    uint32_t start = (level * uint32_t(v.level)) >> 25;
    if (start & ~0xFu)
    {
        start = 0xF;
    }
    v.volume = (v.volume * uint32_t(v.target)) >> 25;
    if (v.volume & ~0xFu)
    {
        v.volume = 0xF;
    }

    const uint32_t end = v.volume;
    if (end == start)
    {
        return uint8_t((start << 4) | 8);
    }

    const uint32_t difference = end > start ? end - start : start - end;
    uint32_t period = uint16_t(SignedDiv(int32_t(uint16_t(v.segment + 15)), int32_t(difference)));
    if (period == 0)
    {
        return uint8_t((start << 4) | 8);
    }
    if (period & 0xFFF8)
    {
        period = 7;
    }

    uint8_t value = uint8_t((start << 4) | period);
    if (start < end)
    {
        value |= 8;
    }

    return value;
}

// Runs the PSG voices' frame: each voice releases when its frames run out, starts its channel on its first frame, and
// then writes its pitch, envelope and duty each frame.
void Sequencer::UpdatePsgVoices()
{
    for (int i = 0; i < kPsgVoices; i++)
    {
        const int index = kSampleVoices + i;
        Voice& v = voices_[size_t(index)];
        if (v.state == 1 && v.frames == 0)
        {
            Release(index);
        }
        if (v.state == 0)
        {
            continue;
        }

        uint32_t pitch;
        uint8_t pan;
        if (v.state == 1)
        {
            pitch = FramePitch(v);
            v.pitch = pitch;
            pan = v.own_pan ? v.pan : tracks_[size_t(std::max(v.track, 0))].pan;
        }
        else
        {
            pitch = v.pitch;
            pan = v.pan;
        }

        const uint8_t envelope = PsgEnvelope(v, pan != 0x40);
        const int channel = v.type - 1;
        const uint8_t both = uint8_t(0x11 << channel);
        const uint8_t kept = uint8_t(ReadBack(kNr51, 1) & ~both);
        if (pan == 0x40)
        {
            Write(kNr51, 1, kept | both);
        }
        else if (pan < 0x40)
        {
            Write(kNr51, 1, kept | uint8_t(0x10 << channel));
        }
        else
        {
            Write(kNr51, 1, kept | uint8_t(1 << channel));
        }

        if (v.state == 1)
        {
            if (v.position == 0)
            {
                StartPsgVoice(v, envelope);
                v.position = 1;
                v.frames--;
                continue;
            }

            v.position++;
            v.frames--;
        }
        else if (v.type == kWaveType)
        {
            // A released wave voice fades by itself, through the wave channel's four volumes.
            v.volume = ((uint32_t(v.release) + 0xE6) * v.volume) >> 9;
            uint32_t volume = pan != 0x40 ? v.volume << 1 : v.volume;
            volume = (volume * 5) >> 7;
            if (volume == 0)
            {
                StopVoice(index);
                continue;
            }

            Write(kNr32, 1, rom_.U8(info_.wave_volumes + std::min<uint32_t>(volume, 4)));
            continue;
        }

        // The duty or noise width for the frame, from the region's table of them.
        uint8_t duty = 0xFF;
        if (RegionFlags(v.region) & 1)
        {
            const uint32_t count = rom_.U16(v.psg);
            duty = v.position < count ? rom_.U8(v.psg + 2 + v.position) : rom_.U8(v.psg + 1 + count);
        }

        switch (v.type)
        {
        case kSquare1Type:
            if (envelope != kNoEnvelope)
            {
                Write(kNr12, 1, envelope);
                Write(kNr13, 2, pitch | 0x8000);
            }
            else if (RegionSweep(v.region) == 8)
            {
                Write(kNr13, 2, pitch);
            }
            Write(kNr11, 1, ReadBack(kNr11, 1) & 0xC0);
            if (duty != 0xFF)
            {
                Write(kNr11, 1, uint32_t(duty) << 6);
            }
            break;

        case kSquare2Type:
            if (envelope != kNoEnvelope)
            {
                Write(kNr22, 1, envelope);
                Write(kNr23, 2, pitch | 0x8000);
            }
            else
            {
                Write(kNr23, 2, pitch);
            }
            Write(kNr21, 1, ReadBack(kNr21, 1) & 0xC0);
            if (duty != 0xFF)
            {
                Write(kNr21, 1, uint32_t(duty) << 6);
            }
            break;

        case kWaveType:
            Write(kNr33, 2, (ReadBack(kNr33, 2) & 0x4000) | (pitch & 0xFFFF));
            if (envelope != kNoEnvelope)
            {
                Write(kNr32, 1, rom_.U8(info_.wave_volumes + envelope));
            }
            break;

        case kNoiseType:
            {
                if (envelope != kNoEnvelope)
                {
                    Write(kNr42, 1, envelope);
                    Write(kNr44, 1, 0x80);
                }

                const uint8_t shape = rom_.U8(info_.noise_table + std::min<uint32_t>(pitch & 0xFFFF, kTopIndex));
                if (duty != 0xFF)
                {
                    Write(kNr43, 1, shape | (duty ? 8 : 0));
                }
                else
                {
                    Write(kNr43, 1, (ReadBack(kNr43, 1) & 8) | shape);
                }
                break;
            }

        default:
            break;
        }
    }
}

// Starts a PSG voice's channel in the voice's first frame, at the note's pitch, before the frame's slide, bend and LFO.
void Sequencer::StartPsgVoice(Voice& v, uint8_t envelope)
{
    const bool table = (RegionFlags(v.region) & 1) != 0;
    switch (v.type)
    {
    case kSquare1Type:
        Write(kNr10, 1, RegionSweep(v.region));
        Write(kNr13, 2, v.base_pitch | 0x8000);
        Write(kNr12, 1, envelope);
        Write(kNr11, 1, uint32_t(table ? rom_.U8(v.psg + 2) : uint8_t(v.psg)) << 6);
        Write(kNr13, 2, v.base_pitch | 0x8000);
        break;

    case kSquare2Type:
        Write(kNr22, 1, envelope);
        Write(kNr23, 2, v.base_pitch | 0x8000);
        Write(kNr21, 1, uint32_t(uint8_t(v.psg)) << 6);
        break;

    case kWaveType:
        if (v.psg != current_wave_)
        {
            Write(kNr30, 1, 0);
            for (uint32_t i = 0; i < 16; i += 2)
            {
                Write(kWaveRam + i, 2, rom_.U16(v.psg + i));
            }
            current_wave_ = v.psg;
        }
        Write(kNr30, 1, 0xC0);
        Write(kNr33, 2, v.base_pitch | 0x8000);
        Write(kNr32, 1, rom_.U8(info_.wave_volumes + envelope));
        Write(kNr31, 1, 0);
        break;

    case kNoiseType:
        {
            Write(kNr42, 1, envelope);
            uint8_t shape = rom_.U8(info_.noise_table + std::min<uint32_t>(v.base_pitch & 0xFFFF, kTopIndex));
            if ((table ? rom_.U8(v.psg + 2) : uint8_t(v.psg)) != 0)
            {
                shape |= 8;
            }
            Write(kNr43, 1, shape);
            Write(kNr44, 1, 0x80);
            Write(kNr41, 1, 0);
            break;
        }

    default:
        break;
    }
}

// Runs the mixer's frame: each sample voice that plays, in the list's order, works out its level, pitch and pan and
// mixes its frame. A released voice stops when its level reaches 0, and any voice when its sample ends. Then the voices
// whose frames have run out are released.
void Sequencer::UpdateSampleVoices()
{
    for (int index = active_; index >= 0;)
    {
        Voice& v = voices_[size_t(index)];
        const int next = v.next;
        const uint32_t level = SampleLevel(v);
        uint32_t pitch;
        if (v.state == 1)
        {
            v.frames--;
            pitch = FramePitch(v);
            v.pitch = pitch;
            v.echo = tracks_[size_t(std::max(v.track, 0))].echo;
        }
        else if (level == 0)
        {
            StopVoice(index);
            index = next;
            continue;
        }
        else
        {
            pitch = v.pitch;
        }

        // The Super Mario Advance 2 revision multiplies the step by the frame's points and divides it by them again,
        // which changes it only where the product wraps.
        uint32_t step = UnsignedDiv((pitch >> 2) * rom_.U32(v.sample + 4), kMixRate);
        if (info_.revision == Revision::kSuperMarioAdvance2)
        {
            step = UnsignedDiv(step * kFrameSamples, kFrameSamples);
        }
        step >>= 5;
        if (MixVoice(v, step))
        {
            StopVoice(index);
        }
        index = next;
    }

    for (int index = 0; index < kSampleVoices; index++)
    {
        if (voices_[size_t(index)].state == 1 && voices_[size_t(index)].frames == 0)
        {
            Release(index);
        }
    }
}

// Moves a sample voice's position on by the frame's 176 points, going back by the loop's length each time it reaches
// the loop's end. Returns true if a sample without a loop has reached its end.
bool Sequencer::MixVoice(Voice& v, uint32_t step)
{
    // The mixer's inner loop mixes 4 points at a time when it's asked for a multiple of 4, and at least one pass.
    auto mixed = [](int32_t count)
    {
        return count > 0 ? count : (count % 4 == 0 ? 4 : 1);
    };

    const uint32_t loop_end = rom_.U32(v.sample + 12);
    const uint32_t end = loop_end ? loop_end : rom_.U32(v.sample);
    uint32_t position = v.position;
    int32_t count = int32_t(kFrameSamples);
    bool ends = false;
    if (((position + step * kFrameSamples) >> 8) >= end)
    {
        count = SignedDiv(int32_t((end << 8) - position - 1 + step), int32_t(step));
        ends = true;
    }

    position += step * uint32_t(mixed(count));
    if (!loop_end || !ends)
    {
        if (ends)
        {
            return true;
        }

        v.position = position;
        return false;
    }

    // Each time the mix reaches the loop's end, it goes back by the loop's length.
    const uint32_t loop_length = (loop_end - rom_.U32(v.sample + 8)) << 8;
    position -= loop_length;
    int32_t left = int32_t(kFrameSamples) - count;
    for (int pass = 0; left != 0; pass++)
    {
        if (pass == 1000 || left < 0)
        {
            Warn("a sample's loop is too short for the driver's mixer, which would hang");
            break;
        }

        int32_t part = left;
        ends = false;
        if (((position + step * uint32_t(left)) >> 8) >= end)
        {
            part = SignedDiv(int32_t((end << 8) - position - 1 + step), int32_t(step));
            ends = true;
        }

        position += step * uint32_t(mixed(part));
        if (ends)
        {
            position -= loop_length;
        }
        left -= part;
    }

    v.position = position;

    return false;
}

} // namespace agbsf2::rd2
