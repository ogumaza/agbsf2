// SPDX-License-Identifier: MIT

#include "krawall/driver.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "krawall/unpack.h"
#include "program.h"
#include "rom.h"
#include "thumb.h"

namespace agbsf2::krawall
{
namespace
{

// The most modules detection lists, so that unrelated ROM data can't make the list endless.
constexpr size_t kMaxModules = 1024;

// The mixer's rate that the model's rates and ticks are for: the library's default, which sets the step of a sample's
// points as 4 times its frequency and the sound timer's ticks from 15 seconds of the mix.
constexpr int kModelRate = 16384;

// The starts of the version strings that the library's builds hold.
constexpr const char* kVersionTexts[] = {"$Id: Krawall", "$Id: player.c"};

// The settings of games whose code sets them in a way that detection can't read: from the game's options, in ARM code,
// or each frame. The values come from the game's code.
struct GameSettings
{
    const char* game_code;
    int stereo = -1;             // kragInit()'s argument, or -1 for the one that detection finds
    uint32_t music_volume = 128; // krapSetMusicVol()'s volume
    uint32_t music_fade_in = 0;  // the volume's step a frame from 0 after each start, or 0 for none
    int quality = -1;            // kramQualityMode()'s argument, or -1 for the one that detection finds
    uint32_t once[4] = {};       // modules that the game plays once, in mode 0, through routines of its own
};

constexpr GameSettings kGameSettings[] = {
    // Eragon: its options' default music level, 7 of 10, at 0x08063A41. The table at 0x08063C8C gives level 7 a volume
    // of 161.
    {.game_code = "BENE", .music_volume = 161},
    // Eragon's European release: its options' default music level, 7 of 10, at 0x0806398D. The table at 0x08063BD8
    // gives level 7 a volume of 161.
    {.game_code = "BENP", .music_volume = 161},
    // Harry Potter and the Prisoner of Azkaban: its options' default music level, 10 of 10, at 0x0806B816. The routine
    // at 0x0803FF98 gives the library 12 times the level.
    {.game_code = "BHTE", .music_volume = 120},
    // Its Japanese release: the default, 10 of 10, at 0x0806B7A2. The routine at 0x08040000 gives the library 12 times
    // the level.
    {.game_code = "BHTJ", .music_volume = 120},
    // Juka and the Monophonic Menace: the routine at 0x08023A84 runs before the mixer's worker each frame. It sets the
    // music volume, which it raises from 0 by 2 a frame after each start.
    {.game_code = "BJKE", .music_fade_in = 2},
    // Its European release: the routine at 0x08023A38 does the same.
    {.game_code = "BJKP", .music_fade_in = 2},
    // Racing Fever: the routine at 0x080042A4 sets half the volume that the game gives it, 127 of 0xFF, before it
    // plays a module.
    {.game_code = "BRWP", .music_volume = 127},
    // Puppy Luv: Spa and Resort: the routine at 0x0801A548 gives the options' music volume a default of 64. The game
    // sets that volume after its call of krapSetMusicVol() with 60 at 0x08024BF8.
    {.game_code = "BYXE", .music_volume = 64},
    // The Koala Brothers: Outback Adventures calls krapSetMusicVol() with 64 before its title's song, at 0x0800097C.
    // Each level's routine starts its song at 0 and fades it to 64, as at 0x080019BA. The routine at 0x08003AAC fades
    // song 2 to 32 instead. Five other levels play song 2 at 64.
    {.game_code = "BAKE", .music_volume = 64},
    // Its European release makes the same call at the same address.
    {.game_code = "BAKP", .music_volume = 64},
    // The Lord of the Rings: The Third Age: at boot, the routine at 0x0803DA8C passes the music volume of the game's
    // settings to krapSetMusicVol(). Its default, at 0x08054849, is 153. The handler of a script's command 9, at
    // 0x080104F4, can set another volume for a scene.
    {.game_code = "B3AE", .music_volume = 153},
    // Its Japanese release: the routine at 0x0803DB28 does the same. The default, at 0x08054BF9, is 153.
    {.game_code = "B3AJ", .music_volume = 153},
    // Crash of the Titans: at boot, the routine at 0x08036A10 passes the music level of the game's settings to the
    // routine at 0x08036AC0, which calls krapSetMusicVol() with the level's volume from the table at 0x08063DBC. The
    // default level, at 0x08063A25, is 2, a volume of 96.
    {.game_code = "BQCE", .music_volume = 96},
    // Its European release: the routine at 0x08036A1C does the same, with the table at 0x08063E08. The default level,
    // at 0x08063A71, is 2, a volume of 96.
    {.game_code = "BQCP", .music_volume = 96},
    // Digimon Racing: after its language select, the game passes the music level of its settings to the routine at
    // 0x0800806C, which calls krapSetMusicVol() through 0x08013134 with the level's volume from the table at
    // 0x08040E88. The routine at 0x08007DB4 gives the level a default of 6, a volume of 180.
    {.game_code = "BDGE", .music_volume = 180},
    // Its European release has the same routines at 0x0800806C, 0x08013134 and 0x08007DB4, and the table at 0x08040EB8.
    {.game_code = "BDGP", .music_volume = 180},
    // Digimon Racing's Japanese release has the same code at other addresses. It has no language select. At boot, the
    // routine at 0x080080F0 calls krapSetMusicVol() through 0x080131B8 with the level's volume from the table at
    // 0x080401C8. The routine at 0x08007E38 gives the level a default of 6, a volume of 180.
    {.game_code = "BDJJ", .music_volume = 180},
    // The games with the modified build call kragInit() with their options' choice of stereo, 1 by default, and
    // kramQualityMode() with 0x21 in their sound init. Their start-up code then sets the options' music volume: Big
    // Mutha Truckers 100 at 0x08000684, Street Racing Syndicate 64 at 0x080003DA, M&M's Break 'Em 48 at 0x08000418 and
    // Princess Natasha 64 at 0x080008F4. Each of the first two games has another release that makes the call at the
    // same address.
    {.game_code = "B63E", .stereo = 1, .music_volume = 100, .quality = 0x21},
    {.game_code = "B63P", .stereo = 1, .music_volume = 100, .quality = 0x21},
    {.game_code = "BCZE", .stereo = 1, .music_volume = 64, .quality = 0x21},
    {.game_code = "BCZP", .stereo = 1, .music_volume = 64, .quality = 0x21},
    {.game_code = "BEME", .stereo = 1, .music_volume = 48, .quality = 0x21},
    {.game_code = "BNPE", .stereo = 1, .music_volume = 64, .quality = 0x21},
    // Cocoto Platform Jumper plays its modules through its routine at 0x08008C1C, in the mode that the caller gives, or
    // through its routine at 0x08008B88, which starts the module later in mode 1 or 0 by its third argument on the
    // stack. Mode 0 plays four modules once: from 0x0801375A, 0x08016B80 and 0x0801357E through the first, and from
    // 0x08013E4A through the second.
    {.game_code = "BC8P", .once = {0x08032C94, 0x0803520C, 0x0804EC30, 0x080565D4}},
    // Games that call krapSetMusicVol() once at boot or before their first module, with no options behind it. The
    // addresses are those of the calls: The Game of Life, Yahtzee and Payday 96 at 0x0801F51C, Candy Land 96 at
    // 0x0800CD58, Dora the Explorer: Dora's World Adventure 77 at 0x0800FDC0, and Shamu's Deep Sea Adventures 60 in ARM
    // code at 0x08000488.
    {.game_code = "B3UE", .music_volume = 96},
    {.game_code = "BC4E", .music_volume = 96},
    {.game_code = "BXPE", .music_volume = 77},
    {.game_code = "BBAE", .music_volume = 60},
    // Shamu's Deep Sea Adventures' European release is the same program, byte for byte, apart from the header.
    {.game_code = "BBAP", .music_volume = 60},
    // Marvel: Ultimate Alliance starts its modules at 0 and fades them to 64, at 0x0803AB06 and 0x0803AB30.
    // Harry Potter and the Order of the Phoenix starts them at 0, at 0x08032FFA. It fades them to 16 times the byte at
    // 0x02004F10, 5 by default, at 0x08033046.
    {.game_code = "B4ME", .music_volume = 64},
    {.game_code = "BJXE", .music_volume = 80},
    // Marvel: Ultimate Alliance's European release makes the same calls at 0x0803AADE and 0x0803AB08.
    {.game_code = "B4MP", .music_volume = 64},
    // The Bee Game calls krapSetMusicVol(64) in ARM code at 0x08028064.
    {.game_code = "BKFE", .music_volume = 64},
    // The Bee Game's European release, Die Biene Maja: Klatschmohnwiese in Gefahr, makes the same call at 0x08027FC8.
    {.game_code = "BKFX", .music_volume = 64},
    // The three Uno games call krapSetMusicVol(96) at boot, then their wrappers set the level from a table of the
    // game's: Uno 52 80 at 0x080290C8, Uno and Skip-Bo 80 at 0x0802CBD0, and Uno Free Fall 48 at 0x080162F4.
    {.game_code = "BU5E", .music_volume = 80},
    {.game_code = "BUQE", .music_volume = 80},
    {.game_code = "BUIE", .music_volume = 48},
    // Their European releases have the same tables and make the same calls at other addresses: Uno 52 at 0x0802941C,
    // Uno and Skip-Bo at 0x0802D654, and Uno Free Fall at 0x080169C0.
    {.game_code = "BU5P", .music_volume = 80},
    {.game_code = "BUQP", .music_volume = 80},
    {.game_code = "BUIP", .music_volume = 48},
    // Three games share a routine that passes a level from 0 to 9 through a table to krapSetMusicVol(), and keeps the
    // level in their settings. Another routine in two of them maps its argument to the argument of kramQualityMode().
    // The Legend of Spyro: The Eternal Night's table is at 0x0806F9E8. Its routine at 0x0804972C sets level 9 at boot,
    // a volume of 85. Its routine at 0x08049A40 gets 2 from 0x0804967C, a quality of 1.
    // Pirates of the Caribbean: Dead Man's Chest's table is at 0x08080710. Its routine at 0x080350F8 sets level 5, a
    // volume of 140. Its routine at 0x080353B8 gets 2 from 0x08035050, a quality of 1.
    // The Chronicles of Narnia's table at 0x0805449C gives level 9 a volume of 254.
    {.game_code = "BU7E", .music_volume = 85, .quality = 1},
    // The Legend of Spyro: The Eternal Night's European release has the same table at 0x0806FCB4. The routine at
    // 0x080499F8 sets level 9 at boot, and the routine at 0x08049D0C gets 2 from 0x08049948.
    {.game_code = "BU7P", .music_volume = 85, .quality = 1},
    {.game_code = "B8QE", .music_volume = 140, .quality = 1},
    {.game_code = "B2WE", .music_volume = 254},
    // The Sims 2 calls krapSetMusicVol(180) from the routine at 0x0804DFD0. That routine keeps the volume in the game's
    // settings.
    {.game_code = "B46E", .music_volume = 180},
};

// The pieces that a game plays in a mode or at a music volume that detection can't read. A row names a module, and one
// of the songs that its orders separate with +++, or -1 for a module without markers. The values come from the game's
// code.
struct SongSettings
{
    const char* game_code;
    uint32_t module;
    int song = -1;         // the song's number in the module, or -1 for a module without markers
    int mode = -1;         // krapPlay()'s mode, or -1 for the one that the song and jingle give
    int music_volume = -1; // krapSetMusicVol()'s volume after krapPlay(), or -1 for the game's
};

constexpr SongSettings kSongSettings[] = {
    // Care Bears: Care Quest plays songs 0 and 8 at 128. Each level's routine starts its song at 0 and fades it to
    // 64: song 1 at 0x08003400, song 4 at 0x08004BCC, song 7 at 0x08007218 and song 6 at 0x08008524. The routine at
    // 0x080038BC plays song 2 once, in the song mode, at 128. The routine at 0x0800A1AC plays songs 4 and 5 as jingles
    // in a level. Song 5 then plays at the level's 64.
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 1, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 2, .mode = 2},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 4, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 5, .mode = 6, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 6, .music_volume = 64},
    {.game_code = "BEAE", .module = 0x08360B0C, .song = 7, .music_volume = 64},
    // Its European release fades its levels' songs to 64 at other addresses: song 1 at 0x080035A4, song 4 at
    // 0x08004D44, song 7 at 0x08007380 and song 6 at 0x08008722. The routine at 0x08003B70 plays song 3 once, not
    // song 2. The routine at 0x0800A428 plays the jingles.
    {.game_code = "BEAP", .module = 0x08361B28, .song = 1, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 3, .mode = 2},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 4, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 5, .mode = 6, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 6, .music_volume = 64},
    {.game_code = "BEAP", .module = 0x08361B28, .song = 7, .music_volume = 64},
    // The Koala Brothers: Outback Adventures plays songs 4 and 5 only as jingles, as at 0x0800171A and 0x0800172E.
    {.game_code = "BAKE", .module = 0x0858C284, .song = 4, .mode = 6},
    {.game_code = "BAKE", .module = 0x0858C284, .song = 5, .mode = 6},
    // Its European release makes the same calls at the same addresses, with the module elsewhere.
    {.game_code = "BAKP", .module = 0x0858BF70, .song = 4, .mode = 6},
    {.game_code = "BAKP", .module = 0x0858BF70, .song = 5, .mode = 6},
    // Franklin the Turtle's ARM code plays nine of its modules only as jingles, at 41 calls of krapPlay(), as at
    // 0x08008848, 0x080088F8 and 0x08013AB8. Each of its 20 mini-games calls krapSetMusicVol(64) and then plays piece
    // 1, 2 or 3, as at 0x08008290 and 0x08008400. krapPlay() keeps the music volume. The title plays piece 0 at 128.
    {.game_code = "BFKE", .module = 0x083321A8, .music_volume = 64},
    {.game_code = "BFKE", .module = 0x08334368, .music_volume = 64},
    {.game_code = "BFKE", .module = 0x08335A18, .music_volume = 64},
    {.game_code = "BFKE", .module = 0x08335F60, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x083361F4, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x0833647C, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x083366F0, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x0833697C, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x08336C28, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x08336E84, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x08337104, .mode = kModeJingle},
    {.game_code = "BFKE", .module = 0x0833737C, .mode = kModeJingle},
    // Its European release has three revisions under one game code. Each has the code and the modules at other
    // addresses. The revisions make these calls in the same order, with the modules in the same order.
    {.game_code = "BFKP", .module = 0x0832D3C0, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x0832F580, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08330C30, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08331178, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833140C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331694, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331908, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331B94, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08331E40, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833209C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833231C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08332594, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08332FAC, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x0833516C, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x0833681C, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08336D64, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08336FF8, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337280, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083374F4, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337780, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337A2C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337C88, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337F08, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08338180, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083321A8, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08334368, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08335A18, .music_volume = 64},
    {.game_code = "BFKP", .module = 0x08335F60, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083361F4, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833647C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x083366F0, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833697C, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08336C28, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08336E84, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x08337104, .mode = kModeJingle},
    {.game_code = "BFKP", .module = 0x0833737C, .mode = kModeJingle},
    // Cocoto Kart Racer's routine at 0x0800BC70 plays a race's module and sets the music volume: 100 for the first four
    // tracks at 0x0800C936, and 90 for the fifth at 0x0800C94E. On the last lap, 0x08005426 plays a jingle and
    // 0x0800542E sets 90. krapPlay() keeps the music volume. 90 therefore holds for the last lap's module, from
    // 0x08005638, for the jingle at the race's end, from 0x080053BA to 0x080053E6, and for the results' module, from
    // 0x0800B400 and 0x0800BB3C. The cup's end plays the race's jingles again at 128, at 0x08009692 to 0x080096D6. The
    // menus set 128. The countdown's jingle, from 0x0800BC9C, plays at 128 in a single race and a cup's first race, and
    // at 90 in the cup's other races. It has no row.
    {.game_code = "BC5P", .module = 0x082415F0, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x082405FC, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x0823F658, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x0822A254, .music_volume = 100},
    {.game_code = "BC5P", .module = 0x08230458, .music_volume = 100},
    {.game_code = "BC5P", .module = 0x082344B8, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x08253D18, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x08256C0C, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x0826181C, .music_volume = 100},
    {.game_code = "BC5P", .module = 0x08261D7C, .music_volume = 90},
    {.game_code = "BC5P", .module = 0x082726E8, .music_volume = 100},
    // Cocoto Platform Jumper's routine at 0x08008C1C sets the music volume after krapPlay(). Its caller gives a factor,
    // and says whether the options' music level scales it. The level's default is 8 of 10, at 0x08000506. A level's
    // module has a factor of 110, from 0x08003A8C. The routine then sets 88. In the first four worlds, a tip stops the
    // music 51 frames after the start. When the tip ends, 0x08016EB2 sets the factor times the level over 12: 73. The
    // pause's end, at 0x08016790, sets 73 too. A restart after a lost life plays at 88 without a tip. The fifth world's
    // levels have no tips. Its module plays at 88, and at 120 at the game's end, from 0x08006118. The flood when the
    // clock runs out plays at 80, from 0x080165A8 with a factor of 100. The module from 0x08013E4A plays at 88. Those
    // from 0x0801357E and 0x0801375A play at 110, and those from 0x0800588C, 0x08005D20, 0x08008AAC and 0x08016B80 at
    // 120, without the level.
    {.game_code = "BC8P", .module = 0x0802DE90, .music_volume = 73},
    {.game_code = "BC8P", .module = 0x080322F0, .music_volume = 73},
    {.game_code = "BC8P", .module = 0x08032C94, .music_volume = 110},
    {.game_code = "BC8P", .module = 0x080349C0, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x0803520C, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x08041C38, .music_volume = 88},
    {.game_code = "BC8P", .module = 0x080439E4, .music_volume = 80},
    {.game_code = "BC8P", .module = 0x0804EC30, .music_volume = 88},
    {.game_code = "BC8P", .module = 0x08056334, .music_volume = 73},
    {.game_code = "BC8P", .module = 0x080565D4, .music_volume = 110},
    {.game_code = "BC8P", .module = 0x0813D328, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x081448FC, .music_volume = 120},
    {.game_code = "BC8P", .module = 0x081499C8, .music_volume = 73},
    // The Bee Game's ARM routine at 0x08027CDC plays each entry of its table of modules at 0x08027C1C in the song mode.
    // That mode goes back to a module's first order rather than its restart order.
    {.game_code = "BKFE", .module = 0x08380B3C, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08389078, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x0837DA44, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08377CB8, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08374BBC, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08374F9C, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08385BF8, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x0837ADE4, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08375378, .mode = kModeSong},
    {.game_code = "BKFE", .module = 0x08374658, .mode = kModeSong},
    // The Bee Game's European release has the same routine at 0x08027C40 and the same modules, 0x5A68 bytes lower.
    // The routine plays the entries of the table at 0x08027B80.
    {.game_code = "BKFX", .module = 0x0837B0D4, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08383610, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08377FDC, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08372250, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836F154, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836F534, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x08380190, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0837537C, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836F910, .mode = kModeSong},
    {.game_code = "BKFX", .module = 0x0836EBF0, .mode = kModeSong},
};

// The tables with a music volume for each entry of a game's table of modules.
struct SongLevels
{
    const char* game_code;
    uint32_t levels;
};

constexpr SongLevels kSongLevels[] = {
    // Postman Pat and the Greendale Rocket: the routine at 0x08003B54 plays entry n of its table of modules and then
    // calls krapSetMusicVol() at 0x08003B76 with byte n of this table, times the options' level over 128. The level's
    // default is 128.
    {"BROP", 0x08032C48},
};

// The start of the player's tables, which are the same in every build.
constexpr int16_t kSine[] = {0, 25, 50, 74, 98, 120, 142, 162, 180, 197, 212, 225, 236, 244, 250, 254};
constexpr int16_t kRamp[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120};
constexpr int16_t kSquare[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
constexpr int16_t kRandom[] = {-28, 185, 162, -32, -161, 170, -222, -186, -177, -193, -2, 232, -252, -133, 121, -127};
constexpr uint16_t kPeriods[] = {27392, 25855, 24403, 23034, 21741, 20521, 19369, 18282, 17256, 16287, 15373, 14510};
constexpr uint16_t kFineTunes[] = {34716, 34654, 34591, 34529, 34467, 34405, 34343, 34281};
constexpr uint32_t kLinear[] = {1096155136, 1097144902, 1098135561, 1099127114, 1100119563, 1101112908};

// A game's call of krapPlay() for an entry of a table of modules: ldr r1, =table; lsls r0, rN, #2; adds r0, r0, r1;
// ldr r0, [r0]; movs r1, #mode; movs r2, #song; bl krapPlay. Digimon Racing's routine at 0x080132D0 plays its jingle
// that way. The routine starts with push {..., lr}; lsls r0, r0, #24; lsrs rN, r0, #24. The routine's first argument
// then picks the entry. The game's calls of the routine give the entry's number with movs r0, #number.
constexpr const char* kPlayEntryPattern = "49xx 00xx 1840 6800 21xx 22xx Fxxx Fxxx";

// A game's direct call of krapPlay(): ldr r0, =module; movs r1, #mode, then movs r2, #song or not, and bl krapPlay.
// Cocoto Kart Racer plays some of its modules once that way, in mode 0 or 4.
constexpr const char* kPlayCallPatterns[] = {"48xx 21xx 22xx Fxxx Fxxx", "48xx 21xx Fxxx Fxxx"};

// A game's call of krapPlay() for an entry of a table of what it plays: ldr r1, =table; lsls r0, rN, #3; adds r0, r0,
// r1; ldr r3, [r0]; ldr r4, [r0, #4]; ldrb r1, [r0, #4]; lsrs r2, r4, #8; lsls r2, r2, #24; lsrs r2, r2, #24; adds r0,
// r3, #0; bl krapPlay. An entry has 8 bytes: the module's address, krapPlay()'s mode and song, and 2 bytes of 0.
// Eragon's routine at 0x0803FD04 plays its entries that way.
constexpr const char* kPlayTablePattern = "49xx 00xx 1840 6803 6844 7901 0A22 0612 0E12 1C18 Fxxx Fxxx";

// A game's routine that plays entry n of its table of modules in the mode of its table of modes, a word an entry. It
// then calls krapSetMusicVol() with the volume of its table of volumes, a byte an entry: push {r4, lr}; adds r4, r0,
// #0; ldr r0, =modules; lsls r1, r4, #2; adds r0, r1, r0; ldr r0, [r0]; ldr r3, =modes; adds r1, r1, r3; ldr r1, [r1];
// bl krapPlay; ldr r0, =volumes; adds r0, r4, r0; ldrb r0, [r0]; movs r1, #0; bl krapSetMusicVol. Franklin's Great
// Adventures' routine at 0x0800362C plays its modules that way. Its start-up code copies the table of modules to RAM.
constexpr const char* kPlayVolumePattern =
    "B510 1C04 48xx 00A1 1808 6800 4Bxx 18C9 6809 Fxxx Fxxx 48xx 1820 7800 2100 Fxxx Fxxx";

// The bytes of an entry of such a table, and the most entries that detection reads.
constexpr uint32_t kPlayEntrySize = 8;
constexpr uint32_t kMaxPlayEntries = 256;

// The most bytes from the start of such a routine to its call of krapPlay().
constexpr uint32_t kMaxPlayEntryOffset = 64;

// The code of a build that detection looks for. Each pattern is Thumb code, and an index is a halfword's in it.
struct BuildCode
{
    Build build;
    const char* version;   // the start of the build's version string, or nullptr if its code alone tells it apart
    const char* play;      // the start of krapPlay()
    const char* tables;    // the note on of the row's routine, whose ldr literals give the instruments and the samples
    int instruments_index; // or -1 for a build whose modules hold their instruments
    int samples_index;
    const char* timer; // the timers' setup in kragInit(), whose constant gives timer 0's reload: the mixer's rate
    int timer_index;
    const char* frame; // getDmaAddress()'s movs of half the samples of a frame
    int frame_index;
    int frame_shift_index; // the lsls whose shift scales the movs to the samples of a frame, or -1 for a shift of 1

    // The starts of kragInit(), which the game calls with its choice of stereo, and of kramQualityMode() and
    // kramSetMasterVol(). kragInit() calls kramSetMasterVol() itself, kInitMasterCall bytes in, before the game can.
    const char* init;
    const char* quality;
    const char* master;
    const char* mixer; // code that tells the build apart from others with the same player, or nullptr

    // Code whose ldr literal at samples_index gives the samples, where the row's note on doesn't load them, or nullptr.
    const char* samples = nullptr;
};

constexpr uint32_t kInitMasterCall = 6;

// The game's sound init calls kramSetMasterVol() within this many bytes after its call of kragInit(). A game can call
// it elsewhere too, as Shrek SuperSlam's pause does at 0x0801A24E.
constexpr uint32_t kInitMasterWindow = 0x80;

// The code of the build of 2003/09/01. The builds of player.c, 1.2.2.10 to 1.4, have the same krapPlay(), timers,
// getDmaAddress() and kragInit(). Their compiler gave the row's note on other registers. Build 1.4 has the
// kramQualityMode() and kramSetMasterVol() of 2003/09/01. Those of the builds before it have no ramping and no shift.
constexpr const char* kPlay2003 = "B5F0 4657 464E 4645 B4E0 1C0F 4680 4692 1C38 2204 4010 2800";
constexpr const char* kTables2003 = "0407 49xx 0BBA 588F 0071 616F 3902 5A78 4Fxx 0082 58B8 6128";
constexpr const char* kTimer2003 = "48xx 49xx 4Bxx 8008 3102 48xx 800B 3906 8008";
constexpr const char* kFrame2003 = "49xx 23xx 0058 680B 181A 600A";
constexpr const char* kInit2003 = "B510 1C04 2080 F000 F9xx 1C20 F000 F8xx F000 F8xx";
constexpr const char* kQuality2003 = "B530 2210 4002 2A00 D0xx 49xx 2301 700B";
constexpr const char* kTablesPlayerC = "0407 49xx 0BBA 588F 00x0 616F 3802 5A39 4Fxx 008A 58B8 6128";
constexpr const char* kQualityPlayerC = "B510 4Axx 2802 DCxx 7010 4Axx 4Cxx 2018 1C91 780A 2A00";
constexpr const char* kMasterPlayerC = "B5F0 4Cxx 4Exx 1C05 2100 2000 11C3 1C1A 2B7F DD00";

// The player of the builds from 2004/09/17 on, which the three share: krapPlay(), and the row's note on.
constexpr const char* kPlay2004 = "B5F0 4657 464E 4645 B4E0 4680 2002 1C0F 4692 Fxxx Fxxx 2003 Fxxx Fxxx 2204";
constexpr const char* kTables2004 = "0431 48xx 0B8A 5886 007C 61AE 3C02 5B31 48xx 008E 5984 616C";
constexpr const char* kTimer2004 = "4Bxx 49xx 48xx 800B 3102 4Bxx 8008 3906 800B";

// getDmaAddress()'s step in the builds from 2004/09/17 on: movs r3, #half and lsls r5, r3, #shift. The shift is 1 for a
// frame of 276 samples. Cocoto Platform Jumper's build of 2005-04-21 shifts by 2, for 552 samples a frame.
constexpr const char* kFrame2004 = "4Bxx 18D5 23xx 6035 00xD";

constexpr const char* kInit2004 = "B510 1C04 2080 F000 Fxxx 1C20 F000 F8xx";
constexpr const char* kQuality2004 = "B530 2120 4001 2900 D0xx 49xx 2300 700B";
constexpr const char* kMaster = "B5F0 1C04 0C20 2800 D0xx 49xx";

// The code of the unsigned build in Puppy Luv: Spa and Resort. Another compiler built it. The row's note on loads the
// samples at the start of the row, and the instruments where it plays a note.
constexpr const char* kPlayUnsigned = "B5F0 465F 4656 464D 4644 B4F0 4681 2002 468A 4693 Fxxx Fxxx 2003 Fxxx Fxxx 2204";
constexpr const char* kTablesUnsigned = "4Axx 0083 589A 4640 0043 61AA 189B 3B02 881B 9A04 009B 5899";
constexpr const char* kSamplesUnsigned = "48xx 49xx 2300 7473 9004 9102 469B 6973 7818 3301 6173 2800";
constexpr const char* kTimerUnsigned = "4Axx 4Bxx 801A 22C4 3302 801A 4Axx 3B06 801A";
constexpr const char* kFrameUnsigned = "49xx 4Bxx 680A 18D3 602B 4Bxx 18D3 6033 23xx 005B";
constexpr const char* kInitUnsigned = "B510 1C04 2080 F000 Fxxx 1C20 Fxxx Fxxx 2001 F000 Fxxx";
constexpr const char* kQualityUnsigned = "B510 0683 D5xx 4Axx 2300 7013 230F 4018 2802 DCxx";
constexpr const char* kMasterUnsigned = "B5F0 4647 B480 1C04 0C00 2800 D1xx 2005";

// The code of the modified build. Another compiler built it too. Its kragInit() copies the game's table of samples to
// RAM. Its routine that loads a music file reads the table, at the literal that kLoaderModified starts with. The
// timers' setup loads timer 0's reload with movs and lsls, and the setup stores half the samples of a frame with movs.
constexpr const char* kPlayModified = "B5F0 4647 B480 1C07 1C0E 4690 2002 Fxxx Fxxx 2003 Fxxx Fxxx 2104";
constexpr const char* kLoaderModified = "48xx 4641 0089 4688 4440 6805 686C 3446 2002 4240 4004";
constexpr const char* kTimerModified = "49xx 4Axx 1C10 8008 3102 20C4 8008 3906 23xx 021B 1C18 8008";
constexpr const char* kFrameModified = "2002 7008 49xx 20xx 0080 6008";
constexpr const char* kInitModified =
    "B530 1C04 2080 Fxxx Fxxx 1C20 Fxxx Fxxx 2001 Fxxx Fxxx Fxxx Fxxx Fxxx Fxxx 48xx 8800";
constexpr const char* kQualityModified = "B530 1C02 2020 4010 2800 D0xx 49xx 2000";
constexpr const char* kMasterModified = "B5F0 1C07 0C38 2800 D1xx 2005";

// The builds' code, in the order detection tries them. The builds of player.c come first. Their version strings tell
// them apart, and their krapPlay() is the one of 2003/09/01. The builds from 2004/09/17 on differ only in their mixer's
// code. From 2004/11/14 on, kramSetMusicVol() calls a routine that sets each channel's mixing routine again, and in
// 2005-04-21 that routine gives a channel in the centre a routine of its own.
constexpr BuildCode kBuildCode[] = {
    {Build::kV1_2_2_10, "$Id: player.c,v 1.2.2.10 ", kPlay2003, kTablesPlayerC, 1, 8, kTimer2003, 5, kFrame2003, 1, -1,
     kInit2003, kQualityPlayerC, kMasterPlayerC, nullptr},
    {Build::kV1_2_2_14, "$Id: player.c,v 1.2.2.14 ", kPlay2003, kTablesPlayerC, 1, 8, kTimer2003, 5, kFrame2003, 1, -1,
     kInit2003, kQualityPlayerC, kMasterPlayerC, nullptr},
    {Build::kV1_2_2_15, "$Id: player.c,v 1.2.2.15 ", kPlay2003, kTablesPlayerC, 1, 8, kTimer2003, 5, kFrame2003, 1, -1,
     kInit2003, kQualityPlayerC, kMasterPlayerC, nullptr},
    {Build::kV1_2_2_17, "$Id: player.c,v 1.2.2.17 ", kPlay2003, kTablesPlayerC, 1, 8, kTimer2003, 5, kFrame2003, 1, -1,
     kInit2003, kQualityPlayerC, kMasterPlayerC, nullptr},
    {Build::kV1_4, "$Id: player.c,v 1.4 ", kPlay2003, kTablesPlayerC, 1, 8, kTimer2003, 5, kFrame2003, 1, -1, kInit2003,
     kQuality2003, kMaster, nullptr},
    {Build::k2003_09_01, nullptr, kPlay2003, kTables2003, 1, 8, kTimer2003, 5, kFrame2003, 1, -1, kInit2003,
     kQuality2003, kMaster, nullptr},
    {Build::k2005_04_21, nullptr, kPlay2004, kTables2004, 1, 8, kTimer2004, 5, kFrame2004, 2, 4, kInit2004,
     kQuality2004, kMaster, "425A 0993 E7xx 2304 E7xx 2301 7023 7042"},
    {Build::k2004_11_14, nullptr, kPlay2004, kTables2004, 1, 8, kTimer2004, 5, kFrame2004, 2, 4, kInit2004,
     kQuality2004, kMaster, "425A 0993 E7xx 2300 E7xx 2201 2340 7022 425A 1C13 7043"},
    {Build::k2004_09_17, nullptr, kPlay2004, kTables2004, 1, 8, kTimer2004, 5, kFrame2004, 2, 4, kInit2004,
     kQuality2004, kMaster, "B570 4Bxx 28FF D9xx 20FF 7018 48xx 2420 1C1E 2540 7881 2900"},
    {Build::kUnsigned, nullptr, kPlayUnsigned, kTablesUnsigned, 0, 0, kTimerUnsigned, 6, kFrameUnsigned, 8, -1,
     kInitUnsigned, kQualityUnsigned, kMasterUnsigned, nullptr, kSamplesUnsigned},
    {Build::kModified, nullptr, kPlayModified, kLoaderModified, -1, 0, kTimerModified, 8, kFrameModified, 3, -1,
     kInitModified, kQualityModified, kMasterModified, nullptr},
};

// Returns the address of the first copy of `bytes` in the ROM at a multiple of `align`, from `from` on, or 0.
uint32_t FindBytes(const Rom& rom, const std::vector<uint8_t>& bytes, uint32_t align, uint32_t from = kRomBase)
{
    const uint8_t* data = rom.Ptr(kRomBase);
    if (!data || bytes.empty() || from < kRomBase || from - kRomBase >= rom.Size())
    {
        return 0;
    }

    const uint8_t* end = data + rom.Size();
    for (const uint8_t* at = data + (from - kRomBase); (at = std::search(at, end, bytes.begin(), bytes.end())) != end;
         at++)
    {
        if ((at - data) % align == 0)
        {
            return kRomBase + uint32_t(at - data);
        }
    }

    return 0;
}

// Returns the address of the first copy of `text` in the ROM, or 0.
uint32_t FindText(const Rom& rom, const char* text)
{
    const std::string bytes = text;
    return FindBytes(rom, std::vector<uint8_t>(bytes.begin(), bytes.end()), 1);
}

// Returns the address of the first copy of a table's values in the ROM, at a multiple of their size, from `from` on, or
// 0.
template <typename T, size_t N>
uint32_t FindTable(const Rom& rom, const T (&values)[N], uint32_t from = kRomBase)
{
    std::vector<uint8_t> bytes;
    for (T v : values)
    {
        for (size_t i = 0; i < sizeof(T); i++)
        {
            bytes.push_back(uint8_t(uint32_t(v) >> (8 * i)));
        }
    }

    return FindBytes(rom, bytes, uint32_t(sizeof(T)), from);
}

// Returns `at` if the ROM's halfwords there start with `values`, or 0.
template <size_t N>
uint32_t HalfwordsAt(const Rom& rom, uint32_t at, const int16_t (&values)[N])
{
    if (!rom.Contains(at, uint32_t(2 * N)))
    {
        return 0;
    }
    for (size_t i = 0; i < N; i++)
    {
        if (int16_t(rom.U16(at + 2 * uint32_t(i))) != values[i])
        {
            return 0;
        }
    }

    return at;
}

// ARM code that calls a Thumb routine of the library. A game whose code is ARM calls the routine through a veneer,
// ldr ip, [pc]; bx ip, followed by the routine's address with the Thumb bit set. Or it loads that address itself:
// ldr rX, =routine|1; mov lr, pc; bx rX. BFKE's start-up code sets the argument after the load: ldr rX,
// =routine|1; mov r0, #value; mov lr, pc; bx rX.
constexpr uint32_t kVeneerLoad = 0xE59FC000;
constexpr uint32_t kVeneerBranch = 0xE12FFF1C;
constexpr uint32_t kMovLrPc = 0xE1A0E00F;
constexpr uint32_t kBx = 0xE12FFF10;
constexpr uint32_t kMovR0 = 0xE3A00000;
constexpr uint32_t kMovR0Mask = 0xFFFFF000;

// The most ARM instructions that ArmArgument() reads back from a call.
constexpr int kMaxArgumentSteps = 16;

// Returns the target of an ARM ldr of a word at a pc-relative address, or 0 if `word` isn't one. Sets `reg` to the
// register that it loads.
uint32_t ArmLiteral(uint32_t word, uint32_t at, uint32_t& reg)
{
    if ((word & 0xFF7F0000) != 0xE51F0000)
    {
        return 0;
    }

    reg = (word >> 12) & 0xF;
    const uint32_t offset = word & 0xFFF;
    return (word & 0x00800000) ? at + 8 + offset : at + 8 - offset;
}

// Returns the starts of the game's ARM calls of `routine`: each bl of a veneer of it, and each ldr of the second form.
std::vector<uint32_t> FindArmCalls(const Rom& rom, uint32_t routine)
{
    std::vector<uint32_t> veneers;
    std::vector<uint32_t> calls;
    for (uint32_t at = kRomBase; rom.Contains(at, 12); at += 4)
    {
        const uint32_t word = rom.U32(at);
        if (word == kVeneerLoad && rom.U32(at + 4) == kVeneerBranch && rom.U32(at + 8) == (routine | 1))
        {
            veneers.push_back(at);
        }

        uint32_t reg = 0;
        const uint32_t literal = ArmLiteral(word, at, reg);
        if (!literal || !rom.Contains(literal, 4) || rom.U32(literal) != (routine | 1))
        {
            continue;
        }
        if (rom.U32(at + 4) == kMovLrPc && rom.U32(at + 8) == (kBx | reg))
        {
            calls.push_back(at);
        }
        else if (reg != 0 && rom.Contains(at, 16) && (rom.U32(at + 4) & kMovR0Mask) == kMovR0 &&
                 rom.U32(at + 8) == kMovLrPc && rom.U32(at + 12) == (kBx | reg))
        {
            // The call starts after the move, so that ArmArgument() reads it.
            calls.push_back(at + 8);
        }
    }

    for (uint32_t at = kRomBase; !veneers.empty() && rom.Contains(at, 4); at += 4)
    {
        const uint32_t word = rom.U32(at);
        const uint32_t target = at + 8 + uint32_t(int32_t(word << 8) >> 6);
        if ((word >> 24) == 0xEB && std::find(veneers.begin(), veneers.end(), target) != veneers.end())
        {
            calls.push_back(at);
        }
    }

    return calls;
}

// Returns the value that the ARM code before `call` leaves in r0, or -1 if it can't tell. It reads back to the last
// write of r0: a mov of an immediate or an ldr of a literal gives the value, and a mov from another register goes on
// back to that register's last write. It stops at a push, a branch or a call, at any other write of the register, and
// after kMaxArgumentSteps instructions.
int64_t ArmArgument(const Rom& rom, uint32_t call)
{
    uint32_t reg = 0;
    for (int step = 1; step <= kMaxArgumentSteps; step++)
    {
        const uint32_t at = call - 4 * uint32_t(step);
        const uint32_t word = rom.U32(at);
        if (!rom.Contains(at, 4) || (word >> 28) != 0xE || (word & 0x0E000000) == 0x0A000000 ||
            (word & 0x0FFFFFF0) == 0x012FFF10 || (word & 0x0F000000) == 0x0F000000 || (word & 0xFFFF0000) == 0xE92D0000)
        {
            return -1;
        }

        uint32_t loaded = 0;
        const uint32_t literal = ArmLiteral(word, at, loaded);
        if (literal && loaded == reg)
        {
            return rom.Contains(literal, 4) ? int64_t(rom.U32(literal)) : -1;
        }

        // A data processing instruction other than a compare writes its Rd, and a multiply its Rn field. A load
        // writes its Rd, and a load or store with writeback its base. A block load writes the registers it lists.
        const uint32_t rd = (word >> 12) & 0xF;
        const uint32_t rn = (word >> 16) & 0xF;
        if ((word & 0x0C000000) == 0)
        {
            if ((word & 0x0FC000F0) == 0x00000090)
            {
                if (rn == reg)
                {
                    return -1;
                }
                continue;
            }

            const uint32_t opcode = (word >> 21) & 0xF;
            if ((opcode >= 8 && opcode <= 11) || rd != reg)
            {
                continue;
            }

            if ((word & 0xFFEF0000) == 0xE3A00000)
            {
                const uint32_t rotate = 2 * ((word >> 8) & 0xF);
                const uint32_t imm = word & 0xFF;
                return rotate ? int64_t((imm >> rotate) | (imm << (32 - rotate))) & 0xFFFFFFFF : int64_t(imm);
            }
            if ((word & 0xFFEF0FF0) == 0xE1A00000)
            {
                reg = word & 0xF;
                continue;
            }
            return -1;
        }

        if ((word & 0x0C000000) == 0x04000000)
        {
            const bool load = word & (1u << 20);
            const bool writeback = (word & (1u << 21)) || !(word & (1u << 24));
            if ((load && rd == reg) || (writeback && rn == reg))
            {
                return -1;
            }
            continue;
        }

        if ((word & 0x0E000000) == 0x08000000)
        {
            const bool load = word & (1u << 20);
            if ((load && (word & (1u << reg))) || ((word & (1u << 21)) && rn == reg))
            {
                return -1;
            }
        }
    }

    return -1;
}

// Returns the immediate of a `movs r0, #imm` just before each of the game's calls to `routine`, the first one found, or
// -1 if no call has one. Without a Thumb call, the value that ARM code leaves in r0 before a call counts.
int CallArgument(const Rom& rom, uint32_t routine)
{
    for (uint32_t call : FindCalls(rom, routine))
    {
        const uint16_t before = rom.U16(call - 2);
        if ((before & 0xFF00) == 0x2000)
        {
            return before & 0xFF;
        }
    }
    for (uint32_t call : FindArmCalls(rom, routine))
    {
        const int64_t value = ArmArgument(rom, call);
        if (value >= 0 && value <= 0xFF)
        {
            return int(value);
        }
    }

    return -1;
}

// Returns the argument that the Thumb code loads into r0, with `movs` or `ldr` from a literal, just before the call at
// `call`, or -1.
int64_t ThumbCallLiteral(const Rom& rom, uint32_t call)
{
    const uint16_t before = rom.U16(call - 2);
    if ((before & 0xFF00) == 0x2000)
    {
        return before & 0xFF;
    }
    if ((before & 0xFF00) == 0x4800)
    {
        return ThumbLiteral(rom, call - 2);
    }

    return -1;
}

// Returns the argument that the game loads into r0, with `movs` or `ldr` from a literal, just before its first call to
// `routine` other than the one at `skip`, or -1 if no call has one. Without a Thumb call, the value that ARM code
// leaves in r0 before a call counts.
int64_t CallLiteral(const Rom& rom, uint32_t routine, uint32_t skip)
{
    for (uint32_t call : FindCalls(rom, routine))
    {
        const int64_t value = call == skip ? -1 : ThumbCallLiteral(rom, call);
        if (value >= 0)
        {
            return value;
        }
    }
    for (uint32_t call : FindArmCalls(rom, routine))
    {
        const int64_t value = ArmArgument(rom, call);
        if (value >= 0)
        {
            return value;
        }
    }

    return -1;
}

// Returns the constant that the Thumb code at `at` loads: an ldr from a literal, or a movs and an lsls of the same
// register.
uint32_t ThumbConstant(const Rom& rom, uint32_t at)
{
    const uint16_t op = rom.U16(at);
    const uint16_t next = rom.U16(at + 2);
    const uint16_t reg = (op >> 8) & 7;
    if ((op & 0xF800) == 0x2000 && (next & 0xF800) == 0 && (next & 7) == reg && ((next >> 3) & 7) == reg)
    {
        return uint32_t(op & 0xFF) << ((next >> 6) & 0x1F);
    }

    return (op & 0xF800) == 0x4800 ? ThumbLiteral(rom, at) : 0;
}

// Returns the library's version string at `at`: its printable characters.
std::string VersionText(const Rom& rom, uint32_t at)
{
    std::string text;
    for (; rom.Contains(at) && rom.U8(at) >= 0x20 && rom.U8(at) < 0x7F && text.size() < 80; at++)
    {
        text += char(rom.U8(at));
    }

    return text;
}

// Returns true if the ROM has the build's code, and its version string if the build is told apart by that. Sets `play`
// to krapPlay() and `tables` to the row's note on.
bool MatchesBuild(const Rom& rom, const BuildCode& code, uint32_t version, uint32_t& play, uint32_t& tables)
{
    play = FindFirstThumb(rom, code.play);
    tables = play ? FindFirstThumb(rom, code.tables) : 0;
    if (!tables)
    {
        return false;
    }

    const bool versioned = !code.version || (version && VersionText(rom, version).rfind(code.version, 0) == 0);
    return versioned && (!code.mixer || FindFirstThumb(rom, code.mixer));
}

// Returns true if a pattern's header is one the player can read: a row count, and an index of every 4th row's data that
// starts at the data and doesn't go back.
bool PlausiblePattern(const Rom& rom, Build build, uint32_t at)
{
    const uint32_t data = PatternData(build, at);
    if (!rom.Contains(at, data - at))
    {
        return false;
    }

    const uint32_t rows = PatternRows(rom, build, at);
    if (rows == 0 || rom.U16(at) != 0)
    {
        return false;
    }

    uint16_t last = 0;
    for (uint32_t i = 1; i < kPatternIndexEntries && 4 * i < rows; i++)
    {
        const uint16_t offset = rom.U16(at + 2 * i);
        if (offset < last || !rom.Contains(data + offset))
        {
            return false;
        }

        last = offset;
    }

    return true;
}

// Finds the modules by their headers, in the order they're in the ROM.
std::vector<uint32_t> ScanModules(const Rom& rom, Build build)
{
    std::vector<uint32_t> found;
    for (uint32_t at = kRomBase; rom.Contains(at, kModulePatterns + 4) && found.size() < kMaxModules; at += 4)
    {
        // The cheap checks first: the channels, the orders and the speed.
        const uint8_t channels = rom.U8(at + kModuleChannels);
        const uint8_t orders = rom.U8(at + kModuleOrderCount);
        if (channels == 0 || channels > 32 || orders == 0 || rom.U8(at + kModuleRestart) >= orders)
        {
            continue;
        }
        if (rom.U8(at + kModuleSpeed) == 0 || rom.U8(at + kModuleTempo) < 0x20)
        {
            continue;
        }

        ModuleInfo module;
        if (ReadModule(rom, build, at, module))
        {
            found.push_back(at);
        }
    }

    return found;
}

// Finds the game's table of modules: the longest run of at least two pointers to modules found by the scan. Sets the
// table's address and length, or leaves both at 0 if no table is found.
void FindModuleTable(const Rom& rom, const std::vector<uint32_t>& modules, uint32_t& table, int& count)
{
    std::vector<uint32_t> sorted = modules;
    std::sort(sorted.begin(), sorted.end());
    uint32_t run_start = 0;
    int run = 0;
    for (uint32_t at = kRomBase; rom.Contains(at, 4); at += 4)
    {
        if (!std::binary_search(sorted.begin(), sorted.end(), rom.U32(at)))
        {
            run = 0;
            continue;
        }

        if (run++ == 0)
        {
            run_start = at;
        }
        if (run > count && run >= 2)
        {
            table = run_start;
            count = run;
        }
    }
}

// Reads the tables that the player's code at `table_code` names, and finds the player's other tables. Returns false and
// sets `error` if one isn't in the ROM.
bool ReadTables(const Rom& rom, const BuildCode& code, uint32_t table_code, DriverInfo& info, std::string& error)
{
    const uint32_t samples_code = code.samples ? FindFirstThumb(rom, code.samples) : table_code;
    info.instruments =
        code.instruments_index >= 0 ? ThumbLiteral(rom, table_code + 2 * uint32_t(code.instruments_index)) : 0;
    info.samples = samples_code ? ThumbLiteral(rom, samples_code + 2 * uint32_t(code.samples_index)) : 0;

    // The player's tables.
    info.sine = FindTable(rom, kSine);
    // The ramp's first values turn up in other data too, as in Franklin's Great Adventures. So do the square wave's,
    // all 255. The ramp follows the sine, and the square wave follows the ramp. Where no ramp follows the sine, the
    // ramp is the first copy of its values that the square wave follows. Cocoto Kart Racer's data hold another copy of
    // the ramp's first values before the player's tables.
    info.ramp = info.sine ? HalfwordsAt(rom, info.sine + 128, kRamp) : 0;
    if (!info.ramp)
    {
        info.ramp = FindTable(rom, kRamp);
        for (uint32_t at = info.ramp; at; at = FindTable(rom, kRamp, at + 2))
        {
            if (HalfwordsAt(rom, at + 128, kSquare))
            {
                info.ramp = at;
                break;
            }
        }
    }
    info.square = info.ramp ? HalfwordsAt(rom, info.ramp + 128, kSquare) : 0;
    info.random = FindTable(rom, kRandom);
    info.periods = FindTable(rom, kPeriods);
    info.fine_tunes = FindTable(rom, kFineTunes);
    info.linear = FindTable(rom, kLinear);
    const RomTable tables[] = {
        {info.sine, 128, "vibrato's sine"},
        {info.ramp, 128, "vibrato's ramp"},
        {info.square, 128, "vibrato's square"},
        {info.random, 128, "vibrato's random wave"},
        {info.periods, 240, "table of periods"},
        {info.fine_tunes, 128, "table of fine tunes"},
        {info.linear, 3072, "table of linear frequencies"},
        {info.samples, 4, "table of samples"},
    };
    if (const char* missing = MissingTable(rom, tables))
    {
        error = std::string("found Krawall, but not its ") + missing;
        return false;
    }

    return true;
}

// Reads the mixer's rate and the samples it mixes a frame, which the build of the library sets. Returns false and sets
// `error` if the code that sets them isn't found.
bool ReadMixRate(const Rom& rom, const BuildCode& code, DriverInfo& info, std::string& error)
{
    const uint32_t timer = FindFirstThumb(rom, code.timer);
    const uint32_t reload = timer ? ThumbConstant(rom, timer + 2 * uint32_t(code.timer_index)) & 0xFFFF : 0;
    const uint32_t frame = FindFirstThumb(rom, code.frame);
    if (!reload || !frame)
    {
        error = "found Krawall, but not its mixer's rate";
        return false;
    }
    info.mix_rate = int((1u << 24) / (0x10000 - reload));
    const int shift =
        code.frame_shift_index >= 0 ? (rom.U16(frame + 2 * uint32_t(code.frame_shift_index)) >> 6) & 0x1F : 1;
    info.frame_samples = (rom.U16(frame + 2 * uint32_t(code.frame_index)) & 0xFF) << shift;
    if (info.mix_rate != kModelRate)
    {
        info.warnings.push_back("the mixer runs at " + std::to_string(info.mix_rate) +
                                " Hz, but the model plays notes "
                                "and ticks as the library does at " +
                                std::to_string(kModelRate) + " Hz");
    }

    return true;
}

// Reads the game's settings for stereo and the mixer's quality, its master and music volumes, and its fade-in.
void ReadGameSettings(const Rom& rom, const BuildCode& code, DriverInfo& info)
{
    const uint32_t init = FindFirstThumb(rom, code.init);
    const uint32_t quality = FindFirstThumb(rom, code.quality);
    int stereo = init ? CallArgument(rom, init) : -1;
    const int mode = quality ? CallArgument(rom, quality) : -1;
    info.stereo = stereo != 0;
    info.quality = uint8_t(std::max(mode, 0));

    // The game's master volume, from its call to kramSetMasterVol() rather than kragInit()'s. The sound init's call,
    // which follows the game's call of kragInit(), comes first. Without one, the first call with a literal counts.
    const uint32_t master = FindFirstThumb(rom, code.master);
    int64_t master_volume = -1;
    if (init && master)
    {
        const std::vector<uint32_t> inits = FindCalls(rom, init);
        for (uint32_t call : FindCalls(rom, master))
        {
            const auto follows = [call](uint32_t c)
            {
                return call > c && call - c <= kInitMasterWindow;
            };
            if (call != init + kInitMasterCall && std::any_of(inits.begin(), inits.end(), follows))
            {
                master_volume = ThumbCallLiteral(rom, call);
            }
            if (master_volume >= 0)
            {
                break;
            }
        }
    }
    if (master_volume < 0 && master)
    {
        master_volume = CallLiteral(rom, master, init ? init + kInitMasterCall : 0);
    }
    if (master_volume >= 0)
    {
        info.master_volume = uint32_t(master_volume);
    }

    for (const GameSettings& game : kGameSettings)
    {
        if (rom.GameCode() != game.game_code)
        {
            continue;
        }

        if (game.stereo >= 0)
        {
            stereo = game.stereo;
            info.stereo = stereo != 0;
        }
        info.music_volume = game.music_volume;
        info.music_fade_in = game.music_fade_in;
        if (game.quality >= 0)
        {
            info.quality = uint8_t(game.quality);
        }
    }

    if (stereo < 0)
    {
        info.warnings.push_back("found no call to kragInit(), so the mixer is taken to be stereo");
    }
}

// Lists the modules: from the given table, or the headers that a scan of the ROM finds. A count given for the modules
// applies to a given table and to the game's table that the scan finds. The count leaves out the modules that the table
// doesn't list. Returns the table that the list comes from, or 0 for a list from the scan.
uint32_t ListModules(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info)
{
    std::vector<uint32_t> found;
    uint32_t table = overrides.module_table;
    if (!table)
    {
        found = ScanModules(rom, info.build);
        FindModuleTable(rom, found, info.module_table, info.table_count);
        table = overrides.module_count > 0 ? info.module_table : 0;
    }

    if (table)
    {
        const int count = overrides.module_count > 0 ? overrides.module_count : int(kMaxModules);
        for (int i = 0; i < count && rom.Contains(table + 4 * uint32_t(i), 4); i++)
        {
            const uint32_t at = rom.U32(table + 4 * uint32_t(i));
            ModuleInfo module;
            if (!ReadModule(rom, info.build, at, module))
            {
                if (overrides.module_count > 0)
                {
                    info.warnings.push_back("entry " + std::to_string(i) + " of the module table, " + Hex(at) +
                                            ", isn't a module the player can read");
                }
                else
                {
                    break;
                }
            }
            info.modules.push_back(at);
        }

        return table;
    }

    // The modules in the order of the game's table, and then any that it doesn't list, in the ROM's order. Without a
    // table, a count given for the modules keeps that many of them.
    for (int i = 0; i < info.table_count; i++)
    {
        info.modules.push_back(rom.U32(info.module_table + 4 * uint32_t(i)));
    }
    for (uint32_t at : found)
    {
        if (std::find(info.modules.begin(), info.modules.end(), at) == info.modules.end())
        {
            info.modules.push_back(at);
        }
    }
    if (overrides.module_count > 0 && info.modules.size() > size_t(overrides.module_count))
    {
        info.modules.resize(size_t(overrides.module_count));
    }

    return 0;
}

// Logs what detection found, with the modules that come from `table`, or from a scan if it's 0.
void LogDetection(const Rom& rom, uint32_t table, DriverInfo& info)
{
    char text[160];
    if (info.version)
    {
        info.log.push_back("version: " + VersionText(rom, info.version));
    }
    info.log.push_back(std::string("build: ") + BuildName(info.build));
    std::snprintf(text, sizeof text, "krapPlay(): %s, samples: %s, instruments: %s", Hex(info.play).c_str(),
                  Hex(info.samples).c_str(), info.instruments ? Hex(info.instruments).c_str() : "in each module");
    info.log.push_back(text);
    std::snprintf(text, sizeof text, "mixer: %d Hz, %d samples a frame, %s, quality %u, master volume 0x%X",
                  info.mix_rate, info.frame_samples, info.stereo ? "stereo" : "mono", unsigned(info.quality),
                  unsigned(info.master_volume));
    info.log.push_back(text);

    if (info.music_volume != 128 || info.music_fade_in)
    {
        std::snprintf(text, sizeof text, "music volume: %u", unsigned(info.music_volume));
        info.log.push_back(text);
    }
    if (info.music_fade_in)
    {
        std::snprintf(text, sizeof text, "the game fades the music in from 0 by %u a frame",
                      unsigned(info.music_fade_in));
        info.log.push_back(text);
    }

    if (info.table_count)
    {
        std::snprintf(text, sizeof text, "module table: %s (%d)", Hex(info.module_table).c_str(), info.table_count);
        info.log.push_back(text);
    }

    const size_t unlisted = info.modules.size() - std::min(info.modules.size(), size_t(info.table_count));
    if (!table && unlisted && info.files.empty())
    {
        info.log.push_back(std::to_string(unlisted) + (unlisted == 1 ? " module" : " modules") +
                           (info.table_count ? " that the table doesn't list" : ", which no table of modules lists"));
    }
}

// Returns the start of the routine whose call of krapPlay() kPlayEntryPattern matches at `at`. The routine has to pass
// its first argument to the register that picks the table's entry. Returns 0 otherwise.
uint32_t PlayEntryRoutine(const Rom& rom, uint32_t at)
{
    const uint16_t entry_register = (rom.U16(at + 2) >> 3) & 7;
    for (uint32_t back = 2; back <= kMaxPlayEntryOffset && at - back >= kRomBase; back += 2)
    {
        const uint32_t start = at - back;
        if ((rom.U16(start) & 0xFF00) == 0xB500)
        {
            const bool argument = rom.U16(start + 2) == 0x0600 && rom.U16(start + 4) == (0x0E00 | entry_register);
            return argument ? start : 0;
        }
    }

    return 0;
}

// Returns the modules that the game plays once, as jingles: the entries of a table of modules that a routine of the
// game plays with krapPlay() in the jingle mode (kPlayEntryPattern), by the number that each call of the routine gives.
// The call's mode has to leave the loop bit clear. With that bit, krapPlay() loops the module in the jingle mode too.
// Logs each such routine, and warns of a call whose number detection can't read.
std::vector<uint32_t> FindJingles(const Rom& rom, DriverInfo& info)
{
    std::vector<uint32_t> jingles;
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kPlayEntryPattern)))
    {
        const bool lsls = (rom.U16(at + 2) & 0xFFC7) == 0x0080;
        const int mode = rom.U16(at + 8) & 0xFF;
        if (!lsls || (mode & (kModeJingle | kModeLoop)) != kModeJingle || BlTarget(rom, at + 12) != info.play)
        {
            continue;
        }
        const uint32_t routine = PlayEntryRoutine(rom, at);
        if (!routine)
        {
            continue;
        }

        const uint32_t table = ThumbLiteral(rom, at);
        for (uint32_t call : FindCalls(rom, routine))
        {
            const uint16_t before = rom.U16(call - 2);
            const uint32_t entry = table + 4 * uint32_t(before & 0xFF);
            if ((before & 0xFF00) != 0x2000 || !rom.Contains(entry, 4))
            {
                info.warnings.push_back("the game plays a module once from " + Hex(call) +
                                        ", by a number that detection can't read. That module converts as a loop");
                continue;
            }

            jingles.push_back(rom.U32(entry));
        }

        info.log.push_back("the game's routine that plays a module once, as a jingle: " + Hex(routine));
    }

    return jingles;
}

// Returns the modules that the game plays once through its direct calls of krapPlay() (kPlayCallPatterns): the listed
// modules that no such call plays with the loop bit. Each has the mode of its first such call in the ROM. The game's
// row of kGameSettings adds the modules that it plays once through routines of its own, in mode 0. Logs them.
std::vector<std::pair<uint32_t, int>> FindPlaysOnce(const Rom& rom, DriverInfo& info)
{
    // Each call's module and mode, in the ROM's order, and the modules that a call loops.
    std::map<uint32_t, std::pair<uint32_t, int>> calls;
    std::set<uint32_t> looped;
    for (const char* text : kPlayCallPatterns)
    {
        const std::vector<ThumbPattern> pattern = ParseThumbPattern(text);
        const uint32_t bl = 2 * uint32_t(pattern.size()) - 4;
        for (uint32_t at : FindThumb(rom, pattern))
        {
            const uint32_t module = ThumbLiteral(rom, at);
            const int mode = rom.U16(at + 2) & 0xFF;
            const bool listed = std::find(info.modules.begin(), info.modules.end(), module) != info.modules.end();
            if (!listed || BlTarget(rom, at + bl) != info.play)
            {
                continue;
            }

            calls[at] = {module, mode};
            if (mode & kModeLoop)
            {
                looped.insert(module);
            }
        }
    }

    std::vector<std::pair<uint32_t, int>> once;
    std::set<uint32_t> seen;
    std::string listed;
    for (const auto& [at, call] : calls)
    {
        const auto [module, mode] = call;
        if (looped.contains(module) || !seen.insert(module).second)
        {
            continue;
        }

        once.emplace_back(module, mode);
        listed += (listed.empty() ? "" : ", ") + Hex(module) + " (mode " + std::to_string(mode) + ")";
    }
    // The modules that the game plays once through routines of its own.
    for (const GameSettings& game : kGameSettings)
    {
        if (rom.GameCode() != game.game_code)
        {
            continue;
        }

        for (uint32_t module : game.once)
        {
            const bool listed_module =
                std::find(info.modules.begin(), info.modules.end(), module) != info.modules.end();
            if (module && listed_module && seen.insert(module).second)
            {
                once.emplace_back(module, 0);
                listed += (listed.empty() ? "" : ", ") + Hex(module) + " (mode 0)";
            }
        }
    }

    if (!listed.empty())
    {
        info.log.push_back("modules that the game plays once: " + listed);
    }

    return once;
}

// Returns the entries of the game's table of what it plays (kPlayTablePattern), up to the first that isn't one: an
// entry needs a module that the list has, a mode of 0 to 7 and 2 bytes of 0. An entry that repeats an earlier one is
// left out. Sets the table's address in `info`, or leaves it 0 if the game has no such table, and logs the table.
std::vector<ModuleSong> ReadPlayTable(const Rom& rom, DriverInfo& info)
{
    std::vector<ModuleSong> entries;
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kPlayTablePattern)))
    {
        if (BlTarget(rom, at + 20) == info.play)
        {
            info.play_table = ThumbLiteral(rom, at);
            break;
        }
    }
    if (!info.play_table)
    {
        return entries;
    }

    uint32_t n = 0;
    for (; n < kMaxPlayEntries; n++)
    {
        const uint32_t entry = info.play_table + kPlayEntrySize * n;
        if (!rom.Contains(entry, kPlayEntrySize))
        {
            break;
        }
        const uint32_t module = rom.U32(entry);
        const int mode = rom.U8(entry + 4);
        const int song = rom.U8(entry + 5);
        const bool listed = std::find(info.modules.begin(), info.modules.end(), module) != info.modules.end();
        if (!listed || mode > 7 || rom.U16(entry + 6) != 0)
        {
            break;
        }

        // The song counts for a module with markers alone. Without them, every song starts at the first order.
        ModuleInfo header;
        const bool markers = ReadModule(rom, info.build, module, header) && SongsInModule(header) > 0;
        const ModuleSong piece{module, markers ? song : -1, false, mode};
        if (std::find(entries.begin(), entries.end(), piece) == entries.end())
        {
            entries.push_back(piece);
        }
    }

    info.log.push_back("the game's table of what it plays: " + Hex(info.play_table) + " (" + std::to_string(n) +
                       (n == 1 ? " entry)" : " entries)"));

    return entries;
}

// Returns the pieces that the game's routine of kPlayVolumePattern plays: each entry of the table of modules that the
// scan found, in the entry's mode and at its music volume. An entry that repeats an earlier one is left out. Returns
// none if the game has no such routine or a mode isn't one of krapPlay()'s, and logs the tables.
std::vector<ModuleSong> ReadModuleSettings(const Rom& rom, DriverInfo& info)
{
    std::vector<ModuleSong> entries;
    const uint32_t count = uint32_t(info.table_count);
    for (uint32_t at : FindThumb(rom, ParseThumbPattern(kPlayVolumePattern)))
    {
        // The routine's table of modules is the one that the scan found, or a copy of it in RAM.
        const uint32_t modules = ThumbLiteral(rom, at + 4);
        const uint32_t modes = ThumbLiteral(rom, at + 12);
        const uint32_t volumes = ThumbLiteral(rom, at + 22);
        if (BlTarget(rom, at + 18) != info.play || count == 0 ||
            (modules != info.module_table && rom.Contains(modules, 4)) || !rom.Contains(modes, 4 * count) ||
            !rom.Contains(volumes, count))
        {
            continue;
        }

        for (uint32_t n = 0; n < count; n++)
        {
            const uint32_t mode = rom.U32(modes + 4 * n);
            if (mode > 7)
            {
                return {};
            }
            const ModuleSong piece{info.modules[n], -1, false, int(mode), rom.U8(volumes + n)};
            if (std::find(entries.begin(), entries.end(), piece) == entries.end())
            {
                entries.push_back(piece);
            }
        }

        info.log.push_back("the game's routine at " + Hex(at) + " plays each module in the mode of the table at " +
                           Hex(modes) + " and at the music volume of the table at " + Hex(volumes));
        break;
    }

    return entries;
}

// Lists the songs. The entries of the game's table of what it plays come first, each in the entry's mode. A module that
// no entry plays follows. One without markers converts as a whole, and one with markers song by song, each in the
// player's song mode. A whole module that the game plays once converts in the jingle mode. Logs the songs that play
// once and the jingles.
void ListSongs(const Rom& rom, DriverInfo& info, const std::vector<uint32_t>& more_jingles = {})
{
    info.songs = ReadPlayTable(rom, info);
    if (info.songs.empty())
    {
        info.songs = ReadModuleSettings(rom, info);
    }

    std::string once;
    for (size_t n = 0; n < info.songs.size(); n++)
    {
        if (!(info.songs[n].mode & kModeLoop))
        {
            once += (once.empty() ? "" : ", ") + std::to_string(n);
        }
    }
    if (!once.empty())
    {
        info.log.push_back("songs whose mode doesn't loop: " + once);
    }

    std::vector<uint32_t> jingles = FindJingles(rom, info);
    jingles.insert(jingles.end(), more_jingles.begin(), more_jingles.end());
    const std::vector<std::pair<uint32_t, int>> direct_once = FindPlaysOnce(rom, info);
    std::string listed;
    for (uint32_t at : info.modules)
    {
        const auto played = [at](const ModuleSong& s)
        {
            return s.module == at && s.mode >= 0;
        };
        if (std::any_of(info.songs.begin(), info.songs.end(), played))
        {
            continue;
        }

        ModuleInfo module;
        const int songs = ReadModule(rom, info.build, at, module) ? SongsInModule(module) : 0;
        const auto this_module = [at](const std::pair<uint32_t, int>& p)
        {
            return p.first == at;
        };
        const auto direct = std::find_if(direct_once.begin(), direct_once.end(), this_module);
        if (songs == 0 && direct != direct_once.end())
        {
            info.songs.push_back({at, -1, false, direct->second});
            continue;
        }
        if (songs == 0)
        {
            const bool jingle = std::find(jingles.begin(), jingles.end(), at) != jingles.end();
            if (jingle)
            {
                listed += (listed.empty() ? "" : ", ") + std::to_string(info.songs.size());
            }
            info.songs.push_back({at, -1, jingle});
            continue;
        }

        for (int s = 0; s < songs; s++)
        {
            info.songs.push_back({at, s});
        }

        char text[160];
        std::snprintf(text, sizeof text, "the module at %s separates %d %s with +++", Hex(at).c_str(), songs,
                      songs == 1 ? "song" : "songs");
        info.log.push_back(text);
    }

    if (!listed.empty())
    {
        info.log.push_back("jingles, which play once: " + listed);
    }

    // The rows give the pieces whose mode or music volume detection can't read from the game's code.
    for (const SongSettings& row : kSongSettings)
    {
        const auto piece = std::find_if(info.songs.begin(), info.songs.end(), [&row](const ModuleSong& s)
                                        { return s.module == row.module && s.song == row.song; });
        if (rom.GameCode() != row.game_code || piece == info.songs.end())
        {
            continue;
        }

        std::string text = "the game plays song " + std::to_string(piece - info.songs.begin());
        if (row.mode >= 0)
        {
            piece->mode = row.mode;
            text += " in mode " + std::to_string(row.mode);
        }
        if (row.music_volume >= 0)
        {
            piece->music_volume = row.music_volume;
            text += " at a music volume of " + std::to_string(row.music_volume);
        }
        info.log.push_back(text);
    }

    for (const SongLevels& table : kSongLevels)
    {
        if (rom.GameCode() != table.game_code || !rom.Contains(table.levels, uint32_t(info.songs.size())))
        {
            continue;
        }

        for (size_t i = 0; i < info.songs.size(); i++)
        {
            info.songs[i].music_volume = rom.U8(table.levels + uint32_t(i));
        }
        info.log.push_back("the game gives each song a music volume, from the table at " + Hex(table.levels));
    }
}

// A music file of the modified build lists the samples that the game unpacks into RAM before it plays the module: a
// byte of their count, then a byte for each sample's number in the game's table. The module's data follow, packed, at
// the next multiple of 4 (see Unpack()). They end with a byte of the instruments' count and one of the patterns'.
// Before them come the patterns' data, the module's header, the patterns' offsets in the data and the instruments.
// The game's loader turns the offsets into addresses.
struct MusicFile
{
    uint32_t file = 0;
    uint32_t samples = 0;      // the samples that it lists
    std::vector<uint8_t> data; // its module's data, unpacked
    uint32_t module = 0;       // the module's offset in `data`
};

// The most bytes of a module's data: EWRAM holds no more.
constexpr uint32_t kMaxModuleData = 0x40000;

// The counts at the end of a module's data.
constexpr uint32_t kModuleDataCounts = 2;

// Where detection maps the unpacked modules: at the next multiple of this after the ROM's end.
constexpr uint32_t kUnpackedAlign = 0x10000;

// Unpacks the music file at `file`. Returns false if it isn't one that the game's loader reads.
bool ReadMusicFile(const Rom& rom, uint32_t file, MusicFile& music)
{
    const uint32_t samples = rom.U8(file);
    const uint32_t packed = (file + 1 + samples + 3) & ~3u;
    const uint32_t size = rom.U8(packed) | (rom.U8(packed + 1) << 8) | (rom.U8(packed + 2) << 16);
    if (!rom.Contains(file, packed + 4 - file) || size < kModifiedPatterns + kModuleDataCounts || size > kMaxModuleData)
    {
        return false;
    }

    std::vector<uint8_t> data;
    if (!Unpack(rom, packed, data) || data.size() != size)
    {
        return false;
    }

    const uint32_t instruments = data[size - 2];
    const uint32_t patterns = data[size - 1];
    const uint32_t tail = 4 * patterns + kModifiedPatterns + kModuleDataCounts + kInstrumentSize * instruments;
    if (tail > size)
    {
        return false;
    }

    const uint32_t module = size - tail;
    for (uint32_t p = 0; p < patterns; p++)
    {
        if (xsflib::LoadLe32(&data[module + kModifiedPatterns + 4 * p]) >= module)
        {
            return false;
        }
    }

    music.file = file;
    music.samples = samples;
    music.data = std::move(data);
    music.module = module;

    return true;
}

// Lays out the module of `music` at `base`. The game's loader does the same at the address that its malloc() gives.
// The patterns' offsets become addresses, and the header gets the address of the instruments.
void PlaceModule(MusicFile& music, uint32_t base)
{
    uint8_t* data = music.data.data();
    const uint32_t size = uint32_t(music.data.size());
    const uint32_t patterns = data[size - 1];
    for (uint32_t p = 0; p < patterns; p++)
    {
        uint8_t* entry = data + music.module + kModifiedPatterns + 4 * p;
        xsflib::StoreLe32(entry, base + xsflib::LoadLe32(entry));
    }

    xsflib::StoreLe32(data + music.module + kModuleInstruments,
                      base + size - kModuleDataCounts - kInstrumentSize * data[size - 2]);
}

// Finds the music files that words in the ROM point to: the literals of the game's code and the entries of its tables.
// The entries of tables, runs of 2 or more such words, come first, in their order. The rest follow in the order of the
// first word that points to each. Without such words, a scan of the ROM finds the files, and `scanned` is set.
std::vector<MusicFile> FindMusicFiles(const Rom& rom, bool& scanned)
{
    std::vector<MusicFile> files;
    std::unordered_map<uint32_t, size_t> found;
    std::unordered_set<uint32_t> rejected;

    // Returns the index in `files` of the file at `at`, or -1 if there's none.
    const auto lookup = [&](uint32_t at) -> int
    {
        if (!rom.Contains(at, 16) || rejected.count(at))
        {
            return -1;
        }
        if (const auto it = found.find(at); it != found.end())
        {
            return int(it->second);
        }

        MusicFile music;
        if (!ReadMusicFile(rom, at, music))
        {
            rejected.insert(at);
            return -1;
        }

        found[at] = files.size();
        files.push_back(std::move(music));

        return int(files.size() - 1);
    };

    // Puts the file at index `i` of `files` next in the order, unless it's there already or `i` is -1.
    std::vector<size_t> order;
    std::vector<bool> placed;
    const auto place = [&](int i)
    {
        placed.resize(files.size());
        if (i >= 0 && !placed[size_t(i)])
        {
            placed[size_t(i)] = true;
            order.push_back(size_t(i));
        }
    };

    // The entries of tables first, then the other words, then a scan if no word points to a file.
    for (uint32_t at = kRomBase; rom.Contains(at, 8); at += 4)
    {
        const int first = lookup(rom.U32(at));
        const int second = first >= 0 ? lookup(rom.U32(at + 4)) : -1;
        if (second >= 0)
        {
            place(first);
            place(second);
        }
    }
    for (uint32_t at = kRomBase; rom.Contains(at, 4); at += 4)
    {
        place(lookup(rom.U32(at)));
    }
    scanned = order.empty();
    for (uint32_t at = kRomBase; scanned && rom.Contains(at, 16) && order.size() < kMaxModules; at += 4)
    {
        place(lookup(at));
    }

    std::vector<MusicFile> listed;
    for (size_t i = 0; i < order.size() && i < kMaxModules; i++)
    {
        listed.push_back(std::move(files[order[i]]));
    }

    return listed;
}

// Returns the music files that the game plays once. The game's routines that play a file call the loader, directly or
// through a routine that only calls it, and then krapPlay(). A call of such a routine with ldr r0, =file; movs r1, #0
// plays the file in the jingle mode.
std::vector<uint32_t> MusicJingles(const Rom& rom, uint32_t loader_code)
{
    // The loader starts with push {r4-r7, lr}.
    uint32_t loader = 0;
    for (uint32_t back = 2; back <= 0x80 && !loader; back += 2)
    {
        loader = rom.U16(loader_code - back) == 0xB5F0 ? loader_code - back : 0;
    }
    if (!loader)
    {
        return {};
    }

    std::vector<uint32_t> calls = FindCalls(rom, loader);
    for (uint32_t call : std::vector<uint32_t>(calls))
    {
        // A routine of push {lr}; bl loader; pop {r1}; bx r1.
        if (rom.U16(call - 2) == 0xB500 && rom.U16(call + 4) == 0xBC02 && rom.U16(call + 6) == 0x4708)
        {
            const std::vector<uint32_t> outer = FindCalls(rom, call - 2);
            calls.insert(calls.end(), outer.begin(), outer.end());
        }
    }

    std::vector<uint32_t> jingles;
    for (uint32_t call : calls)
    {
        uint32_t routine = 0;
        for (uint32_t back = 2; back <= 0x40 && !routine; back += 2)
        {
            routine = (rom.U16(call - back) & 0xFF00) == 0xB500 ? call - back : 0;
        }
        for (uint32_t play : routine ? FindCalls(rom, routine) : std::vector<uint32_t>())
        {
            if (rom.U16(play - 2) == 0x2100 && (rom.U16(play - 4) & 0xF800) == 0x4800)
            {
                jingles.push_back(ThumbLiteral(rom, play - 4));
            }
        }
    }

    return jingles;
}

// Lists the modified build's modules: unpacks the music files and maps their modules after the ROM's end, in a copy of
// the ROM that `info.unpacked` holds. Returns the modules of the files that the game plays once. Returns false and sets
// `error` if there are no modules.
bool ListMusicFiles(const Rom& rom, uint32_t loader_code, DriverInfo& info, std::vector<uint32_t>& jingles,
                    std::string& error)
{
    bool scanned = false;
    std::vector<MusicFile> files = FindMusicFiles(rom, scanned);

    // Each file's module follows the ROM's end, laid out as the game's loader does.
    std::vector<uint8_t> image(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    const uint32_t start = (rom.End() + kUnpackedAlign - 1) & ~(kUnpackedAlign - 1);
    image.resize(start - kRomBase);
    for (MusicFile& music : files)
    {
        const uint32_t base = kRomBase + uint32_t(image.size());
        PlaceModule(music, base);
        image.insert(image.end(), music.data.begin(), music.data.end());
        image.resize((image.size() + 3) & ~size_t(3));
        info.modules.push_back(base + music.module);
        info.files.push_back(music.file);
    }
    if (info.modules.empty())
    {
        error = "found Krawall, but no music files";
        return false;
    }

    auto unpacked = std::make_shared<Rom>();
    unpacked->Assign(std::move(image));
    std::vector<uint32_t> keep_modules, keep_files;
    for (size_t i = 0; i < info.modules.size(); i++)
    {
        ModuleInfo module;
        if (!ReadModule(*unpacked, info.build, info.modules[i], module))
        {
            continue;
        }

        // The game unpacks a file's samples with a routine that detection doesn't read.
        if (files[i].samples)
        {
            info.warnings.push_back("the music file at " + Hex(files[i].file) + " lists " +
                                    std::to_string(files[i].samples) + " samples to unpack. " + kProgramName +
                                    " can't read them and leaves the file out");
            continue;
        }

        keep_modules.push_back(info.modules[i]);
        keep_files.push_back(info.files[i]);
    }

    info.modules = std::move(keep_modules);
    info.files = std::move(keep_files);
    info.unpacked = std::move(unpacked);
    if (info.modules.empty())
    {
        error = "found Krawall, but no music files";
        return false;
    }

    for (uint32_t file : MusicJingles(rom, loader_code))
    {
        const auto it = std::find(info.files.begin(), info.files.end(), file);
        if (it != info.files.end())
        {
            jingles.push_back(info.modules[size_t(it - info.files.begin())]);
        }
    }

    info.log.push_back(std::string("music files, from ") +
                       (scanned ? "a scan of the ROM" : "the game's code and tables") + ", and their modules:");
    for (size_t i = 0; i < info.files.size(); i++)
    {
        info.log.push_back("  " + Hex(info.files[i]) + ": " + Hex(info.modules[i]));
    }

    return true;
}

} // namespace

bool ReadModule(const Rom& rom, Build build, uint32_t address, ModuleInfo& module)
{
    module = ModuleInfo();
    if (!rom.Contains(address, kModulePatterns))
    {
        return false;
    }

    module.address = address;
    module.channels = rom.U8(address + kModuleChannels);
    module.order_count = rom.U8(address + kModuleOrderCount);
    module.restart = rom.U8(address + kModuleRestart);
    for (uint32_t i = 0; i < 256; i++)
    {
        module.orders[i] = rom.U8(address + kModuleOrders + i);
    }
    for (uint32_t i = 0; i < 32; i++)
    {
        module.channel_pan[i] = rom.S8(address + kModuleChannelPans + i);
    }
    for (uint32_t i = 0; i < 64; i++)
    {
        module.song_starts[i] = rom.U8(address + kModuleSongStarts + i);
    }
    module.global_volume = rom.U8(address + kModuleGlobalVolume);
    module.speed = rom.U8(address + kModuleSpeed);
    module.tempo = rom.U8(address + kModuleTempo);
    const uint8_t flags[3] = {rom.U8(address + kModuleFlags), rom.U8(address + kModuleFlags + 1),
                              rom.U8(address + kModuleFlags + 2)};
    module.instruments = flags[0] != 0;
    module.linear = flags[1] != 0;
    module.fast_slides = flags[2] != 0;
    module.volume_opt = rom.U8(address + kModuleFlags + 3);
    module.amiga_limits = rom.U8(address + kModuleFlags + 4);

    // The header's flags are bytes of 0 or 1, and the pans are within the player's range.
    if (module.channels == 0 || module.channels > 32 || module.order_count == 0 || module.speed == 0)
    {
        return false;
    }
    for (uint8_t f : {flags[0], flags[1], flags[2], module.volume_opt, module.amiga_limits})
    {
        if (f > 1)
        {
            return false;
        }
    }
    for (int c = 0; c < module.channels; c++)
    {
        if (module.channel_pan[size_t(c)] < -64 || module.channel_pan[size_t(c)] > 64)
        {
            return false;
        }
    }

    // The orders name the patterns. Their table follows the header. In the modified build, the address of the
    // module's instruments comes before it.
    const uint32_t table = address + ModulePatterns(build);
    if (build == Build::kModified)
    {
        module.instruments_at = rom.U32(address + kModuleInstruments);
    }

    int patterns = 0;
    for (int i = 0; i < module.order_count; i++)
    {
        const uint8_t o = module.orders[size_t(i)];
        if (o < kOrderSkip)
        {
            patterns = std::max(patterns, o + 1);
        }
    }
    if (patterns == 0 || !rom.Contains(table, 4 * uint32_t(patterns)))
    {
        return false;
    }
    for (int p = 0; p < patterns; p++)
    {
        const uint32_t pattern = rom.U32(table + 4 * uint32_t(p));
        if (!PlausiblePattern(rom, build, pattern))
        {
            return false;
        }

        module.patterns.push_back(pattern);
    }

    return true;
}

uint32_t SampleEnd(const Rom& rom, Build build, uint32_t sample)
{
    // The modified build's header gives the points' count where the others give the address after them.
    const uint32_t end = rom.U32(sample + kSampleEnd);
    return build == Build::kModified ? sample + kSamplePoints + end : end;
}

uint32_t ModulePatterns(Build build)
{
    return build == Build::kModified ? kModifiedPatterns : kModulePatterns;
}

const char* BuildName(Build build)
{
    switch (build)
    {
    case Build::kV1_2_2_10:
        return "player.c 1.2.2.10 of 2002/08/24";
    case Build::kV1_2_2_14:
        return "player.c 1.2.2.14 of 2002/09/12";
    case Build::kV1_2_2_15:
        return "player.c 1.2.2.15 of 2002/12/19";
    case Build::kV1_2_2_17:
        return "player.c 1.2.2.17 of 2003/01/13";
    case Build::kV1_4:
        return "player.c 1.4 of 2003/03/19";
    case Build::k2003_09_01:
        return "2003/09/01";
    case Build::k2004_09_17:
        return "2004/09/17";
    case Build::k2004_11_14:
        return "2004/11/14";
    case Build::k2005_04_21:
        return "2005-04-21";
    case Build::kUnsigned:
        return "unsigned";
    case Build::kModified:
        return "modified";
    }

    return "unknown";
}

uint32_t PatternRows(const Rom& rom, Build build, uint32_t pattern)
{
    return build < Build::k2004_09_17 ? rom.U8(pattern + kPatternRows) : rom.U16(pattern + kPatternRows);
}

uint32_t PatternData(Build build, uint32_t pattern)
{
    return pattern + kPatternRows + (build < Build::k2004_09_17 ? 1 : 2);
}

RowEntry ReadEntry(const Rom& rom, Build build, uint8_t flags, uint32_t& at)
{
    // Up to 2003/09/01, the note is in the first byte's top 7 bits. The second byte holds the instrument's low 8 bits,
    // and the first byte's low bit holds its 9th. From 2004/09/17 on, the note is in the first byte's low 7 bits, the
    // second byte holds the instrument's low 8 bits, and where the first byte's top bit is set, a third byte holds its
    // top 8.
    RowEntry entry;
    entry.channel = flags & kEntryChannel;
    if ((flags & kEntryNote) && build < Build::k2004_09_17)
    {
        const uint8_t b0 = rom.U8(at++);
        entry.instrument = uint16_t(rom.U8(at++) | ((b0 & 1) << 8));
        entry.note = uint8_t(b0 >> 1);
    }
    else if (flags & kEntryNote)
    {
        const uint8_t b0 = rom.U8(at++);
        entry.instrument = rom.U8(at++);
        if (b0 & 0x80)
        {
            entry.instrument = uint16_t(entry.instrument | (rom.U8(at++) << 8));
        }
        entry.note = uint8_t(b0 & 0x7F);
    }
    if (flags & kEntryVolume)
    {
        entry.volume = rom.U8(at++);
    }
    if (flags & kEntryEffect)
    {
        entry.effect = rom.U8(at++);
        entry.effect_op = rom.U8(at++);
    }

    return entry;
}

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();
    error.clear();

    // The library's version string, which names a signed build, and the player's code, which each build compiled the
    // same in every game.
    for (const char* text : kVersionTexts)
    {
        info.version = info.version ? info.version : FindText(rom, text);
    }

    const BuildCode* code = nullptr;
    uint32_t table_code = 0;
    for (const BuildCode& c : kBuildCode)
    {
        if (MatchesBuild(rom, c, info.version, info.play, table_code))
        {
            code = &c;
            break;
        }
    }
    if (!code)
    {
        if (info.version)
        {
            error = "found Krawall, but not a build that " + std::string(kProgramName) + " supports (" +
                    VersionText(rom, info.version) + ")";
        }
        return false;
    }

    info.build = code->build;
    if (!ReadTables(rom, *code, table_code, info, error) || !ReadMixRate(rom, *code, info, error))
    {
        return false;
    }

    ReadGameSettings(rom, *code, info);

    // The modified build's modules are packed in music files.
    if (info.build == Build::kModified)
    {
        std::vector<uint32_t> jingles;
        if (!ListMusicFiles(rom, table_code, info, jingles, error))
        {
            return false;
        }

        LogDetection(rom, 0, info);
        ListSongs(*info.unpacked, info, jingles);

        return true;
    }

    const uint32_t table = ListModules(rom, overrides, info);
    if (info.modules.empty())
    {
        error = "found Krawall, but no modules";
        return false;
    }

    LogDetection(rom, table, info);
    ListSongs(rom, info);

    return true;
}

int SongsInModule(const ModuleInfo& module)
{
    int songs = 0;
    bool markers = false;
    bool in_song = false;
    for (int i = 0; i < module.order_count; i++)
    {
        const uint8_t o = module.orders[size_t(i)];
        markers = markers || o == kOrderSkip;
        if (o < kOrderSkip && !in_song)
        {
            songs++;
        }
        in_song = o < kOrderSkip;
    }

    return markers ? std::min(songs, int(module.song_starts.size())) : 0;
}

} // namespace agbsf2::krawall
