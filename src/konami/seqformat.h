// SPDX-License-Identifier: MIT

// Konami GBA sound driver: sequence data format.
//
// A song has 16 tracks, or 12 in the Rave Master revision, 10 in the Eternal Duelist revision and 8 in the Dungeon Dice
// Monsters revision. Tracks 0-3 drive the Game Boy PSG channels (square 1, square 2, wave, noise), and each of the
// others drives one DirectSound sample voice. Every track is a byte stream of the form
//
//     <delay> { <command> <delay> }*
//
// where <delay> is a frame count (see ReadDelay) and <command> is one of the opcodes in the Op enum below. The
// revisions of the driver read different commands and delays. See docs/konami.md for the full description.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "rom.h"

namespace agbsf2::konami
{

constexpr int kTracks = 16;   // maximum tracks per song
constexpr int kPsgTracks = 4; // tracks 0..3

inline bool IsPsgTrack(int t)
{
    return t < kPsgTracks;
}

// The revisions of the driver that agbsf2 reads. They differ in their commands and delays, and in how they play them.
enum class Revision
{
    kUltimateMasters,     // Yu-Gi-Oh! Ultimate Masters Edition, and the games whose driver reads its commands
    kWct2004,             // Yu-Gi-Oh! World Championship Tournament 2004
    kRaveMaster,          // Rave Master: Special Attack Force
    kEternalDuelist,      // Yu-Gi-Oh! The Eternal Duelist Soul and Worldwide Edition
    kDungeonDiceMonsters, // Yu-Gi-Oh! Dungeon Dice Monsters
};

// Returns the revision's name, as --info prints it.
const char* RevisionName(Revision r);

// Returns the number of tracks in a song of revision `r`: 16, or 12 in the Rave Master revision, which has 8 sample
// voices, 10 in the Eternal Duelist revision, which has 6, and 8 in the Dungeon Dice Monsters revision, which has 4.
int TrackCount(Revision r);

// The first note that the Dungeon Dice Monsters revision plays one entry of its PSG frequency table after the note
// before it, where the table holds the noise channel's settings. The notes below it are 16 entries apart.
constexpr int kDungeonDiceNoiseNote = 72;

// The frequency of a square channel's note 0, C2, in Hz: 440 Hz × 2^(-33/12).
constexpr double kC2 = 65.40639132514966;

// Returns the size of a song table entry in revision `r`: the song's base address, and the offset of each track.
uint32_t SongEntrySize(Revision r);

// The sequence commands. The comments give their opcodes and arguments in the Ultimate Masters revision, and in the WCT
// 2004 revision where it differs. The Rave Master and Eternal Duelist revisions read the WCT 2004 revision's commands,
// less a few, and Eternal Duelist has an F0 of its own. The Dungeon Dice Monsters revision has commands of its own, and
// shares only some of the others (see DecodeCommand).
enum class Op
{
    kDelay,        // not a command: the delay that follows every command
    kDuty,         // 00-8F  the PSG duty/length byte (square) or wave number (wave)
    kNote,         // A0-BF  DirectSound note: [vol] sample [semitone]
    kVolume,       // C0-CF  a volume change (DS: a live volume change, PSG: a retrigger)
    kPsgNote,      // D0-DF  PSG note: [vol] note
    kRest,         // E0-EF  a rest, which silences the track
    kPan,          // F0 xx
    kVibrato,      // F1 xx  vibrato depth (Eternal Duelist: F4-FC xx as well)
    kPitchBend,    // F2 xx or F2 8x xx  bend in 1/32 semitones (see DecodeCommand)
    kLoopPoint,    // F3     loop start marker (DS tracks skip 2 extra bytes)
    kEchoFeedback, // F7 xx
    kEchoDelay,    // F8 xx  (WCT 2004: bit 7 picks echo bus 1)
    kEchoRoute,    // F9 xx  high nibble = DS voice 0-11, low nibble = echo bus + 1 (0 = off buses 0 and 1)
    kEndTrack,     // FE     end of track, restarted by a song loop (WCT 2004: FD)
    kEndTrackHard, // FD     end of track, never restarted
    kJump,         // FF 00 = song stop, FF nn = song loop, back to each track's loop point (WCT 2004: FF, FE)
    kPanLevels,    // WCT 2004 F0 xy: left level x, right level y (0-F each)
    kAttack,       // WCT 2004 F4 xx: the attack rate of PSG notes
    kDecay,        // WCT 2004 F5 xx: the decay rate of PSG notes
    kInstrument,   // WCT 2004 F6 xx: instrument from a table that the game gives the driver
    kVolumeScale,  // WCT 2004 FA xx: volume scale from a setting that the game gives the driver
    kNop,          // WCT 2004 FB, FC
    kWave,         // WCT 2004 00-8F with a low nibble of 4 or more: wave (nibble - 4), loaded at once
    kCall,         // WCT 2004 9x lo hi nn: a call of nn commands from offset hilo of the track's data
    kPsgPan,       // Eternal Duelist F0 xx on a PSG track: the track's NR51 bits
    kPairVolumes,  // Eternal Duelist F0 xy on a sample track: volume y, and x for the next track's voice
    kSampleAtNote, // Dungeon Dice Monsters 9x nn: the track's sample at note nn, at volume x
    kNextNote,     // Dungeon Dice Monsters Ax nn: kNote, played by the next track
    kSetVolume,    // Dungeon Dice Monsters Dx: volume x, without starting a note
    kPsgVolume,    // Dungeon Dice Monsters F0 xx: the PSG's volume on each side (NR50)
    kSampleBend,   // Dungeon Dice Monsters F1 xx (the next track) or F2 xx: a bend of the note by xx - 32 sixteenths
    kFade,         // Dungeon Dice Monsters F8 (this track) or F9 (the next track): a fade-out over 4 frames
    kRelease,      // Dungeon Dice Monsters FB: kFade, with the square channels' envelope (a rest during vibrato)
    kUnknown,
};

// A decoded command, or the delay after one.
struct Command
{
    Op op = Op::kUnknown;
    uint32_t addr = 0;   // address of the opcode byte
    uint32_t length = 0; // bytes including arguments
    uint8_t opcode = 0;
    int vol = -1;       // kNote/kVolume/kPsgNote, and Dungeon Dice Monsters' kNextNote/kSampleAtNote/kSetVolume
    int sample = -1;    // kNote: sample index (Dungeon Dice Monsters: sample map entry, also for kNextNote)
    int semitone = 0;   // kNote: signed semitone offset
    int note = -1;      // kPsgNote/kSampleAtNote: note number
    int value = 0;      // generic argument (kCall: offset of the commands); kPitchBend: 1/32 semitones, or 1/16 in
                        // Dungeon Dice Monsters
    int count = 0;      // kCall: commands to play
    uint32_t delay = 0; // kDelay: frames
};

// Reads a delay at `addr`. Returns the number of bytes used.
//   Ultimate Masters:
//                  00-DF          delay = byte
//                  Ex yy          delay = (x << 8) | yy
//                  Fx lo hi       delay = hi << 8 | lo
//   WCT 2004 and Rave Master:
//                  00-DF          delay = byte
//                  bb yy          delay = (bb & 1F) << 8 | yy for bb from E0 up
//   Eternal Duelist and Dungeon Dice Monsters:
//                  00-EF          delay = byte
//                  Fx yy          delay = (x << 8) | yy
uint32_t ReadDelay(const Rom& rom, uint32_t addr, Revision revision, uint32_t& delay);

// Decodes the command at `addr` for the given track. Never reads past the ROM.
Command DecodeCommand(const Rom& rom, uint32_t addr, int track, Revision revision);

// Returns the value that command `opcode` (00-8F) stores in the track's duty byte in the Ultimate Masters revision.
inline uint8_t DutyByteFor(uint8_t opcode)
{
    return (opcode & 0x80) ? uint8_t((opcode & 0x3F) << 6) : uint8_t(opcode - 4);
}

// The square channels' duty cycles, selected by bits 6-7 of the duty byte.
inline constexpr const char* kDutyNames[4] = {"12.5%", "25%", "50%", "75%"};

// A song table entry: the start of the song's data, and the start of each track in it.
struct SongHeader
{
    uint32_t base = 0;
    int tracks = kTracks;           // the song's tracks
    uint16_t offsets[kTracks] = {}; // 0 past `tracks`
};

// Reads entry `song` of a song table of revision `revision`. Returns false if the entry isn't in the ROM.
bool ReadSongHeader(const Rom& rom, uint32_t song_table, int song, Revision revision, SongHeader& out);

// The most commands that WalkTrack reads unless it's told otherwise.
inline constexpr uint32_t kMaxWalkCommands = 200000;

// Walks one track from its start until an end/jump command, an unknown opcode, or `max_commands`, and follows the F3
// rebasing rule. Calls `visit(cmd, frame)` for each command (and each delay). The walk passes over a call (kCall). With
// `follow_calls` set, the walk plays the call's commands with their delays and goes back after the last one. The
// driver plays a call the same way. The walk leaves out a quirk of the Eternal Duelist and Dungeon Dice Monsters
// revisions: they go back to the same place every 256 commands after that. Returns true if the track reached an end
// command.
bool WalkTrack(const Rom& rom, const SongHeader& song, int track, Revision revision,
               const std::function<void(const Command&, uint32_t frame)>& visit,
               uint32_t max_commands = kMaxWalkCommands, bool follow_calls = false);

// Returns a human-readable form of a command on the given track of a song of revision `revision`.
std::string Describe(const Command& c, int track, Revision revision);

} // namespace agbsf2::konami
