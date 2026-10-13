// SPDX-License-Identifier: MIT

// A model of Krawall playing a module: the player, which runs a tick of the module each time the sound timer runs out,
// and the mixer, whose channels play the samples. The game calls the mixer's worker once a frame to mix the frame's
// samples, and the worker runs the player's ticks at the samples where they fall due.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "krawall/driver.h"
#include "rom.h"

namespace agbsf2::krawall
{

// The mixer's channels, and the most channels a module can have.
constexpr int kMixChannels = 32;
constexpr int kPlayerChannels = 20;

// The modes of krapPlay(): loop the module, play one of its songs, or play it as a jingle.
constexpr int kModeLoop = 1;
constexpr int kModeSong = 2;
constexpr int kModeJingle = 4;

// The sizes of the player's record, a player channel's and a mixer channel's, as the game keeps them in RAM.
constexpr size_t kPlayerRecordSize = 0x34;
constexpr size_t kChannelRecordSize = 0x60;
constexpr size_t kMixRecordSize = 0x2C;

// A mixer channel. The handle's low byte identifies the channel; the upper bytes identify the playback instance. This
// prevents a stale handle from changing a note after the mixer channel has been reassigned.
struct MixChannel
{
    uint8_t volume = 0; // 0-64 from the player, though a byte holds what it's given
    int8_t pan = 0;     // -64 to 64
    uint8_t status = 0; // 0 free, 1 playing
    uint8_t loop = 0;   // 0 none, 1 forwards, 2 back and forth
    uint32_t start = 0; // the address of the sample's first point
    uint32_t pos = 0;   // the address of the point it plays
    uint32_t end = 0;
    uint32_t loop_length = 0;
    int32_t inc = 0; // the step a sample of the mix, in 65536ths of a point; negative while a loop plays backwards
    uint32_t handle = 0;
    uint16_t frac = 0; // the position's fraction, in 65536ths of a point
    uint8_t left = 0;  // the volume of each side
    uint8_t right = 0;
    uint8_t hq = 0;         // 8 if the channel interpolates
    uint8_t mix = 0;        // the mixing routine: 0 or 2 both sides, 1 left, 3 right, 7 the ramp at a stop
    uint8_t hq_sample = 0;  // the sample's choice of interpolation
    uint8_t sfx = 0;        // 1 for a sound effect, or while ramping, the samples left of the ramp
    uint32_t loop_from = 0; // the loop's first point; while ramping, the ramp's level and step

    // Not part of the record: the plays and moves of the position that SetPos() has made, the sample's header, and its
    // number in the game's table of samples.
    uint32_t moves = 0;
    uint32_t sample = 0;
    int sample_index = -1;
};

// A channel of the module, as the player keeps it.
struct Channel
{
    // The channel's note: its mixer channel's handle, volume and pan, and its sample's rate as a note and a period.
    uint32_t handle = 0;
    int8_t volume = 0;         // 0-64
    int8_t channel_volume = 0; // the channel's volume, which the channel volume effect sets
    int8_t panning = 0;        // -64 to 64
    uint8_t start_point = 0;   // where the next note starts, in 256 points
    uint8_t effect = 0;        // the row's effect
    int16_t note = 0;          // the note from C-0, with the sample's relative note
    int16_t period = 0;        // the note's period, with the slides
    int16_t played_period = 0; // the period that the mixer plays, with the vibrato or the arpeggio
    uint32_t sample = 0;       // the sample's header, or 0
    uint32_t instrument = 0;
    uint8_t last_effect = 0;
    uint8_t effect_op = 0;

    // The effects' settings, which their operands set and an operand of 0 leaves as they are, and their places in their
    // waves.
    uint16_t porta_step = 0;
    int16_t porta_goal = 0; // the period that the slide to a note slides to
    int16_t gliss_note = 0; // the note that a slide with glissando plays
    uint8_t vibrato_pos = 0;
    uint8_t vibrato_speed = 0;
    uint8_t vibrato_depth = 0;
    uint8_t vibrato_reset = 0; // a new note starts the wave again
    uint32_t vibrato_wave = 0;
    uint8_t tremolo_pos = 0;
    uint8_t tremolo_speed = 0;
    uint8_t tremolo_depth = 0;
    uint8_t tremolo_reset = 0;
    uint32_t tremolo_wave = 0;
    uint8_t panbrello_pos = 0;
    uint8_t panbrello_speed = 0;
    uint8_t panbrello_depth = 0;
    uint8_t panbrello_reset = 0;
    uint32_t panbrello_wave = 0;
    uint8_t glissando = 0;
    uint8_t tremor_count = 0; // the ticks of the tremor so far, which mutes the note at `tremor_on`
    uint8_t tremor_on = 0;
    uint8_t tremor_cycle = 0;
    uint8_t volume_slide = 0;
    uint8_t fine_volume_slide = 0;
    uint8_t channel_volume_slide = 0;
    uint8_t volume_slide_once = 0; // the volume slide is fine, so it slides on the row alone
    uint8_t pan_slide = 0;
    uint8_t pan_slide_once = 0;
    uint8_t arpeggio = 0;
    uint8_t arpeggio_step = 0;
    uint8_t porta = 0;
    uint8_t fine_porta = 0;
    uint8_t extra_fine_porta = 0;
    uint8_t retrig = 0;

    // The row's state: the note's pitch, volume and pan go back to the note's when the effect that changed them ends.
    uint8_t restore_note = 0;
    uint8_t play_note = 0; // the row starts a note, after its effect
    uint8_t in_row = 0;    // the row has data for the channel

    // The instrument's envelopes: each one's state (0 off, 1 stopped, 2 held at its sustain point, 3 running, 4 running
    // past the sustain point after the release), level, point, tick and the tick of the next point, the fade after the
    // release, and the place in the instrument's vibrato.
    uint8_t envelopes_on = 0;
    uint8_t volume_env_state = 0;
    int8_t envelope_volume = 0; // 0-64
    uint16_t volume_env_level = 0;
    uint8_t volume_env_point = 0;
    uint8_t volume_env_tick = 0;
    uint8_t volume_env_target = 0;
    uint8_t fading = 0;
    int16_t fade = 0;
    uint8_t pan_env_state = 0;
    int8_t envelope_pan = 0; // -64 to 64, added to the channel's pan
    uint16_t pan_env_level = 0;
    uint8_t pan_env_point = 0;
    uint8_t pan_env_tick = 0;
    uint8_t pan_env_target = 0;
    uint8_t column_effect = 0; // the volume column's effect, less 0x60
    uint8_t instrument_vibrato_pos = 0;

    // Not part of the record: the sample's number in the game's table of samples, or -1.
    int sample_index = -1;
};

// A model of Krawall that plays a module a frame at a time, as the game's call to the mixer's worker does.
class Player
{
public:
    // Starts module `module` as krapPlay() does in `mode`, from song `song`. Keeps references to `rom` and `info`,
    // which have to outlive the player.
    Player(const Rom& rom, const DriverInfo& info, uint32_t module, int mode = kModeLoop, int song = 0);

    // Returns false if the module can't be played.
    bool Valid() const
    {
        return valid_;
    }

    // Runs one call of the mixer's worker: mixes a frame's samples, running the player's ticks as they fall due.
    void Frame();

    // Returns false once the player has stopped.
    bool Playing() const
    {
        return status_ != 0;
    }

    // Sets a function that runs after each tick, with the time of the tick in the mixer's samples from the start.
    void OnTick(std::function<void(uint64_t)> hook)
    {
        on_tick_ = std::move(hook);
    }

    const ModuleInfo& Module() const
    {
        return module_;
    }

    const std::array<MixChannel, kMixChannels>& Mixer() const
    {
        return mixer_;
    }

    const std::array<Channel, kPlayerChannels>& Channels() const
    {
        return channels_;
    }

    // Returns the speed, the tick within the row, and the order and row that the last row came from, with the state of
    // the pattern loop then, which together say where the module is.
    int Speed() const
    {
        return speed_;
    }

    int TickInRow() const
    {
        return tick_;
    }

    int RowOrder() const
    {
        return row_order_;
    }

    int RowNumber() const
    {
        return row_number_;
    }

    uint32_t LoopState() const
    {
        return row_loop_state_;
    }

    int Tempo() const
    {
        return tempo_;
    }

    // Returns the samples from one tick to the next, and the samples mixed so far.
    uint32_t TickSamples() const
    {
        return timer_speed_;
    }

    uint64_t Elapsed() const
    {
        return elapsed_;
    }

    // Returns the level of the mix's output for a channel volume of 1, where 128 is full scale: the game's master
    // volume and the mixer's shift.
    double OutputScale() const;

    // The player's record, a player channel's and a mixer channel's, laid out as the game keeps them, and the mixer's
    // other variables: the master volumes, the timer's step and count, the music volume and the count of plays. The
    // player's record leaves out its pointers to the routines that work out periods and frequencies.
    std::array<uint8_t, kPlayerRecordSize> PlayerRecord() const;
    static std::array<uint8_t, kChannelRecordSize> ChannelRecord(const Channel& c);
    static std::array<uint8_t, kMixRecordSize> MixRecord(const MixChannel& m);
    std::array<uint8_t, 16> Globals() const;

    // Returns the problems found while playing.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    void Warn(const std::string& text);

    // The mixer: kramPlayExt(), which starts a sample on the channel that `handle` names if it's still that play's, or
    // else on the first free channel, and returns the play's handle, or 0 if every channel is busy; kramStop(),
    // kramSetFreq(), kramSetVol(), kramSetPan() and kramSetPos(), which return false for a handle that's no longer its
    // channel's; and the rest of its state.
    uint32_t Play(uint32_t sample, int sample_index, int sfx, uint32_t handle, uint32_t freq, uint32_t volume,
                  int32_t pan);
    bool Stop(uint32_t handle);
    bool SetFreq(uint32_t handle, uint32_t freq);
    bool SetVol(uint32_t handle, uint32_t volume);
    bool SetPan(uint32_t handle, int32_t pan);
    bool SetPos(uint32_t handle, uint32_t pos);
    void SetSides(MixChannel& m) const;
    void SetPanning(MixChannel& m, int32_t pan) const;
    void ResetChannels();
    void SetMusicVolume(int32_t volume);
    void SetTimerBpm(int bpm);
    void SetTimer(bool on);

    // Mixes `amount` samples, moving each channel's position on, and in the mixing routines, by `samples`.
    void Mix(uint32_t amount);
    static void Advance(MixChannel& m, uint32_t samples);

    // The player: a tick, which plays a row or the effects between rows, and then the envelopes; the start of the row's
    // data; the move to the next row after a row; the note on; the stop; and the settings that go to the mixer.
    void Timer();
    void FindRow();
    void NextRow();
    void PlayRow();
    void PlayTickBetweenRows();
    void RunEnvelopes();
    void PlayNote(Channel& c);
    void Stop();
    void SetGlobalVolume();
    void SetChannelVolume(Channel& c);
    void SetChannelPan(Channel& c);
    void SetChannelFreq(Channel& c);
    void StopChannel(Channel& c);

    // Returns a note's period for a sample, the frequency of a period, a linear period's frequency in any module, and a
    // point of a wave.
    uint16_t PeriodOf(uint32_t note, uint32_t sample) const;
    uint32_t FrequencyOf(int32_t period) const;
    uint32_t LinearFreq(uint32_t period) const;
    int16_t Wave(uint32_t table, uint32_t index) const;

    // The effects: the ones of the effect column, by number, which run on the row's tick and on the ticks between, and
    // those of the volume column.
    void Effect(Channel& c, int effect, bool on_row, bool second);
    void VolumeColumnEffect(Channel& c, bool on_row);
    void VolumeSlideS3m(Channel& c, bool on_row);
    void VolumeSlideXm(Channel& c, bool on_row);
    void PortaS3m(Channel& c, bool on_row, bool up);
    void PortaNote(Channel& c, bool on_row);
    void Vibrato(Channel& c, bool on_row);
    void VibratoTick(Channel& c);
    void PanSlide(Channel& c, bool on_row);
    void Retrig(Channel& c, bool on_row);
    void Arpeggio(Channel& c, bool on_row);
    void PatternLoop(Channel& c);
    void VolumeDown(Channel& c, int by);
    void VolumeUp(Channel& c, int by);
    void PanLeft(Channel& c, int by);
    void PanRight(Channel& c, int by);

    const Rom& rom_;
    const DriverInfo& info_;
    bool valid_ = false;
    ModuleInfo module_;
    std::function<void(uint64_t)> on_tick_;
    std::vector<std::string> warnings_;

    // The player's record.
    uint32_t mode_ = 0;
    uint32_t status_ = 0; // 0 stopped, 2 playing
    uint16_t tempo_ = 0;
    uint8_t speed_ = 0;
    uint8_t tick_ = 0;
    uint8_t row_ = 0;
    uint8_t order_ = 0;
    uint32_t data_ = 0;     // the next byte of the pattern's data
    int8_t goto_order_ = 0; // the order that a jump goes to after the row, or -1
    int8_t goto_row_ = 0;   // the row of the next order that a break goes to, or -1
    int8_t loop_row_ = 0;   // the pattern loop's row, the times it has left to go back there, and whether it runs
    int8_t loop_count_ = 0;
    int8_t loop_active_ = 0;
    int8_t volume_ = 0; // the module's global volume, which the global volume effect sets
    uint8_t volume_slide_ = 0;
    uint8_t vibrato_offset_ = 0; // where a vibrato starts in its wave: 32 for modules with instruments
    uint8_t instrument_based_ = 0;
    uint8_t fast_slides_ = 0;
    uint32_t song_start_ = 0;
    uint32_t pattern_ = 0;
    std::array<Channel, kPlayerChannels> channels_;

    // The order and row that the last row came from, and the pattern loop's state then.
    int row_order_ = 0;
    int row_number_ = 0;
    uint32_t row_loop_state_ = 0;

    // The mixer.
    std::array<MixChannel, kMixChannels> mixer_;
    std::array<uint8_t, 2> master_ = {128, 128};
    uint8_t hq_mode_ = 0;
    uint8_t hq_ramp_ = 0;
    uint32_t plays_ = 0;
    int32_t music_volume_ = 128;
    bool timer_on_ = false;
    uint16_t timer_speed_ = 0x8000;
    uint16_t timer_count_ = 0x8000;
    uint16_t timer_backup_ = 0;
    uint64_t elapsed_ = 0;
};

} // namespace agbsf2::krawall
