// SPDX-License-Identifier: MIT

#include "midi.h"

#include <cstdint>

#include "song_banks.h"

namespace agbsf2
{

ChannelWriter::ChannelWriter(MidiTrack& track, int channel, int program, BankProgram preset, int range,
                             uint32_t loop_start, bool shared)
    : track_(track),
      channel_(channel),
      range_(range),
      loop_start_(loop_start),
      shared_(shared),
      before_loop_(loop_start > 0),
      program_(program),
      bank_(preset.bank)
{
    // A writer on a shared MIDI channel leaves the program, pan, expression and bend to each note.
    if (shared_)
    {
        track_.Control(0, channel_, cc::kVolume, 127);
        track_.Control(0, channel_, cc::kReverb, 0);
        track_.Control(0, channel_, cc::kChorus, 0);
        if (range_)
        {
            track_.BendRange(0, channel_, range_);
        }
        program_ = -1;
        pan_ = expression_ = bend_ = -1;
        return;
    }

    track_.Bank(0, channel_, preset.bank);
    track_.Program(0, channel_, preset.program);
    track_.Control(0, channel_, cc::kVolume, 127);
    track_.Control(0, channel_, cc::kPan, pan_);
    track_.Control(0, channel_, cc::kExpression, expression_);
    track_.Control(0, channel_, cc::kReverb, 0);
    track_.Control(0, channel_, cc::kChorus, 0);
    if (range_)
    {
        track_.BendRange(0, channel_, range_);
        track_.PitchBend(0, channel_, bend_);
    }
}

void ChannelWriter::Reach(uint32_t at)
{
    if (before_loop_ && at >= loop_start_)
    {
        before_loop_ = false;
        program_ = -1;
        pan_ = expression_ = bend_ = -1;
    }
}

void ChannelWriter::SetProgram(uint32_t at, int program, BankProgram preset)
{
    Reach(at);
    if (program == program_)
    {
        return;
    }

    if (program_ < 0 || preset.bank != bank_)
    {
        track_.Bank(at, channel_, preset.bank);
    }

    program_ = program;
    bank_ = preset.bank;
    track_.Program(at, channel_, preset.program);
}

void ChannelWriter::SetLevels(uint32_t at, double left, double right)
{
    Reach(at);
    int pan = pan_, expression = expression_;
    LevelsToControllers(left, right, pan, expression);
    if (pan != pan_)
    {
        track_.Control(at, channel_, cc::kPan, pan_ = pan);
    }
    if (expression != expression_)
    {
        track_.Control(at, channel_, cc::kExpression, expression_ = expression);
    }
}

void ChannelWriter::SetBend(uint32_t at, double semitones)
{
    Reach(at);
    if (!range_)
    {
        return;
    }

    const int value = BendValue(semitones, range_);
    if (value != bend_)
    {
        track_.PitchBend(at, channel_, bend_ = value);
    }
}

void ChannelWriter::Note(uint32_t on, uint32_t off, int key)
{
    track_.NoteOn(on, channel_, key, 127);
    track_.NoteOff(off, channel_, key);
    if (shared_)
    {
        program_ = -1;
        pan_ = expression_ = bend_ = -1;
    }
}

} // namespace agbsf2
