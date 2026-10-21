// SPDX-License-Identifier: MIT

// Converts the music of GBA games with Konami's, Rare's, Quintet's, Nintendo R&D2's, Brownie Brown's or Ubisoft Milan's
// sound driver, Nintendo's MP2K or Krawall, to MIDI files and SoundFonts.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/console.h"
#include "xsflib/text.h"

#include "files.h"
#include "music.h"
#include "program.h"
#include "rom.h"

#ifndef AGBSF2_VERSION
#define AGBSF2_VERSION "dev"
#endif

namespace fs = std::filesystem;
using namespace agbsf2;

namespace
{

// Prints the help text to `f`.
void Usage(FILE* f)
{
    std::fprintf(f,
                 "%s %s - music converter for GBA games\n"
                 "\n"
                 "usage: %s [options] <file> [<file> ...]\n"
                 "\n"
                 "Converts every song in each GBA ROM (.gba) or GSF rip (.gsflib, .minigsf) to\n"
                 "a MIDI file and a matching SoundFont, for games whose music uses Konami's,\n"
                 "Rare's, Quintet's, Nintendo R&D2's, Brownie Brown's or Ubisoft Milan's sound\n"
                 "driver, Nintendo's MP2K or Krawall. A game with more than one of them has each\n"
                 "one's songs converted. The results go in a folder beside the input file, with\n"
                 "the same name. You can also drop files on the program.\n"
                 "\n"
                 "options:\n"
                 "  -o, --output DIR      output directory (default: <file's folder>/<base name>)\n"
                 "  -n, --name NAME       base name of the output files (default: file name)\n"
                 "  -s, --songs LIST      songs to convert, e.g. 0,3,7-9 (default: all)\n"
                 "  -l, --loops N         play each song's loop N times (default: 2)\n"
                 "  -t, --tracks LIST     only convert these tracks, e.g. 4-15 (default: all; in\n"
                 "                        Konami's driver, 0-3 are the PSG channels and 4 and up\n"
                 "                        the sample voices, in Quintet's, 0-3 are the PSG\n"
                 "                        channels and 4 and 5 the PCM channels, in Brownie\n"
                 "                        Brown's, 0-3 are the music's PSG channels, 4-7 the sound\n"
                 "                        effects' and 8 and up the sample channels, in Ubisoft\n"
                 "                        Milan's, 0-2 are the PSG channels and 9 the kit, and in\n"
                 "                        Krawall, the module's channels, 0-19)\n"
                 "      --single-sf2      write one SoundFont for all songs\n"
                 "      --voice-channels  give each of the driver's sound channels a MIDI channel\n"
                 "                        (MP2K), so that notes stop where the game cuts them\n"
                 "                        off, though a track's notes then move between channels\n"
                 "      --psg-exclusive   end a PSG note's slow release where its track's next\n"
                 "                        note on the same PSG channel starts (MP2K). The game\n"
                 "                        cuts it off there. A note that another track cuts off\n"
                 "                        still fades out\n"
                 "      --frame-timing    put each event on the frame the game plays it in,\n"
                 "                        rather than on the song's beat\n"
                 "      --dump            also write a text listing of each song's sequence data\n"
                 "      --info            print each driver's detected tables and songs, then\n"
                 "                        exit\n"
                 "      --driver NAME     use only konami, rare, quintet, rd2, brownie, ubimilan,\n"
                 "                        krawall or mp2k, instead of every driver detection finds\n"
                 "      --song-table ADDR    use this song table address (hex; in Nintendo R&D2's\n"
                 "                           driver, the address of the game's settings for it,\n"
                 "                           in Ubisoft Milan's, the sound bank, and in Krawall,\n"
                 "                           the game's table of modules)\n"
                 "      --song-count N       use this many songs\n"
                 "      --sample-table ADDR  use this sample table address (hex; Konami's driver)\n"
                 "      --mix-rate HZ        use this DirectSound mixer rate (Konami's driver)\n"
                 "      --trace SONG      print the driver model's state after each frame of SONG:\n"
                 "                        in Konami's driver, each track's output (frame track\n"
                 "                        pitch b2 vol trig flags key, and 2 pan fields, which\n"
                 "                        depend on the revision); in Rare's, each note slot\n"
                 "                        that plays (frame slot state channel key pitch-key\n"
                 "                        velocity phase level instrument position fraction);\n"
                 "                        in Quintet's, each write to the sound, DMA and timer\n"
                 "                        registers (frame address size value, with frame -1\n"
                 "                        for the song's start); in MP2K, each sound channel\n"
                 "                        that plays (frame channel status track key pitch-key\n"
                 "                        velocity priority level right left, then rate sample\n"
                 "                        count position fraction, or a PSG channel's frequency\n"
                 "                        goal counter sustain pan wave); in Nintendo R&D2's,\n"
                 "                        each write to the PSG's registers (frame address size\n"
                 "                        value), each voice that plays (frame voice state type\n"
                 "                        track priority note velocity, and the rest of its\n"
                 "                        record) and the order the mixer takes them in; in\n"
                 "                        Brownie Brown's, each write to the sound registers\n"
                 "                        (frame w address size value), each sound channel's\n"
                 "                        record and each mixer voice's or FIFO's that isn't\n"
                 "                        clear (frame c0 or v0, then the record in hex), and the\n"
                 "                        driver's other variables when they change (frame g);\n"
                 "                        in Krawall, the mixer's variables (frame g), the\n"
                 "                        player's record (frame p), each of the module's\n"
                 "                        channels (frame c0) and each mixer channel (frame m0),\n"
                 "                        in hex, when they change; in Ubisoft Milan's, each\n"
                 "                        write to the PSG's registers (frame w address size\n"
                 "                        value), the track (frame t countdown command next\n"
                 "                        plays ending) and each mixer voice that plays (frame\n"
                 "                        v0 position loop-start end loop-end volume loops)\n"
                 "      --trace-frames N  frames to trace (default: 3000)\n"
                 "  -q, --quiet           only print warnings and errors\n"
                 "  -h, --help            show this help\n"
                 "      --version         show the version\n",
                 kProgramName, AGBSF2_VERSION, kProgramName);
}

struct Options
{
    std::vector<std::string> inputs;
    std::string out_dir, name;
    std::set<int> songs;
    ConvertSettings convert;
    Overrides overrides;
    bool info = false, dump = false, quiet = false, single_sf2 = false;
    bool tracks_given = false; // a -t has replaced the default of every track
    int trace_song = -1;
    long long trace_frames = 3000;
};

// Prints an error about the options, after the program's name.
void OptionError(const std::string& message)
{
    std::fprintf(stderr, "%s: %s\n", kProgramName, message.c_str());
}

// Parses the whole of `s` as a number in `base`, by C's rules (std::strtoll's).
bool ParseNumber(const std::string& s, long long& out, int base)
{
    const std::optional<int64_t> value = xsflib::ParseInteger(s, {base, true});
    if (!value)
    {
        return false;
    }

    out = *value;

    return true;
}

// Parses the whole of `s` as a decimal number, by C's rules (std::strtod's).
bool ParseNumber(const std::string& s, double& out)
{
    const std::optional<xsflib::DecimalNumber> value = xsflib::ReadDecimal(s, {true});
    if (!value || s.c_str()[value->length] != '\0')
    {
        return false;
    }

    out = value->value;

    return true;
}

// Parses a list such as "0,3,7-9" into `values`.
bool ParseList(const std::string& s, std::set<int>& values)
{
    size_t start = 0;
    while (start <= s.size())
    {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos)
        {
            comma = s.size();
        }

        // An item is a number or a range: "7" or "7-9".
        const std::string item = s.substr(start, comma - start);
        const size_t dash = item.find('-');
        const std::string first = item.substr(0, dash);
        const std::string last = dash == std::string::npos ? first : item.substr(dash + 1);
        long long a = 0, b = 0;
        if (!ParseNumber(first, a, 10) || !ParseNumber(last, b, 10) || a < 0 || b < a || b > 4096)
        {
            return false;
        }

        for (long long i = a; i <= b; i++)
        {
            values.insert(int(i));
        }

        start = comma + 1;
    }

    return true;
}

// Reads the value of the option at args[i] into `out`: the argument after it. Moves `i` on to the value. Returns false,
// after a message, if the option comes last.
bool NextValue(const std::vector<std::string>& args, size_t& i, const char* option, std::string& out)
{
    if (i + 1 >= args.size())
    {
        OptionError(std::string(option) + " needs a value");
        return false;
    }

    out = args[++i];

    return true;
}

// Adds the tracks of a --tracks list to the options. The first list replaces the default of every track, and each later
// one adds its tracks to those before it. Returns false, after a message, for a bad list.
bool AddTracks(const std::string& list, Options& o)
{
    // Krawall's modules have up to 20 channels, and the other drivers 16 tracks at most.
    std::set<int> tracks;
    if (!ParseList(list, tracks) || tracks.empty() || *tracks.rbegin() >= 20)
    {
        OptionError("bad track list (tracks are 0-19)");
        return false;
    }

    if (!o.tracks_given)
    {
        o.convert.track_mask = 0;
        o.tracks_given = true;
    }
    for (int t : tracks)
    {
        o.convert.track_mask |= 1u << t;
    }

    return true;
}

// Checks the options that depend on each other, once every argument is read. The overrides of Konami's tables also pick
// its driver. Returns false, after a message, if the options don't fit together.
bool CheckOptions(Options& o)
{
    if (o.inputs.empty())
    {
        Usage(stderr);
        return false;
    }
    if (o.inputs.size() > 1 && (!o.name.empty() || o.trace_song >= 0))
    {
        OptionError("--name and --trace take a single input file");
        return false;
    }

    // Only Konami's driver has a sample table and a mixer rate to override.
    if (o.overrides.sample_table || o.overrides.mix_rate > 0)
    {
        if (o.overrides.driver != Driver::kAny && o.overrides.driver != Driver::kKonami)
        {
            OptionError("--sample-table and --mix-rate are for Konami's driver");
            return false;
        }

        o.overrides.driver = Driver::kKonami;
    }

    return true;
}

// Reads the arguments into `o`. Returns the exit code when there's nothing more to do: after --help or --version, or
// after a mistake in the options. Returns nothing to go on.
std::optional<int> ParseArgs(const std::vector<std::string>& args, Options& o)
{
    for (size_t i = 1; i < args.size(); i++)
    {
        const std::string& a = args[i];
        std::string v;
        long long n;
        if (a == "-h" || a == "--help")
        {
            Usage(stdout);
            return 0;
        }
        else if (a == "--version")
        {
            std::printf("%s %s\n", kProgramName, AGBSF2_VERSION);
            return 0;
        }
        else if (a == "-o" || a == "--output")
        {
            if (!NextValue(args, i, "--output", o.out_dir))
            {
                return 2;
            }
        }
        else if (a == "-n" || a == "--name")
        {
            if (!NextValue(args, i, "--name", o.name))
            {
                return 2;
            }
        }
        else if (a == "-s" || a == "--songs")
        {
            if (!NextValue(args, i, "--songs", v))
            {
                return 2;
            }
            if (!ParseList(v, o.songs))
            {
                OptionError("bad song list");
                return 2;
            }
        }
        else if (a == "-l" || a == "--loops")
        {
            if (!NextValue(args, i, "--loops", v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 10) || n < 1 || n > 100)
            {
                OptionError("--loops must be between 1 and 100");
                return 2;
            }

            o.convert.loops = int(n);
        }
        else if (a == "-t" || a == "--tracks")
        {
            if (!NextValue(args, i, "--tracks", v) || !AddTracks(v, o))
            {
                return 2;
            }
        }
        else if (a == "--single-sf2")
        {
            o.single_sf2 = true;
        }
        else if (a == "--voice-channels")
        {
            o.convert.voice_channels = true;
        }
        else if (a == "--psg-exclusive")
        {
            o.convert.psg_exclusive = true;
        }
        else if (a == "--frame-timing")
        {
            o.convert.frame_timing = true;
        }
        else if (a == "--dump")
        {
            o.dump = true;
        }
        else if (a == "--info")
        {
            o.info = true;
        }
        else if (a == "--driver")
        {
            if (!NextValue(args, i, "--driver", v))
            {
                return 2;
            }

            o.overrides.driver = DriverNamed(v);
            if (o.overrides.driver == Driver::kAny)
            {
                OptionError("--driver needs konami, rare, quintet, rd2, brownie, ubimilan, krawall or mp2k");
                return 2;
            }
        }
        else if (a == "--song-table" || a == "--sample-table")
        {
            if (!NextValue(args, i, a.c_str(), v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 16) || n <= 0 || n > 0xFFFFFFFF)
            {
                OptionError(a + " needs a hex address");
                return 2;
            }

            (a == "--song-table" ? o.overrides.song_table : o.overrides.sample_table) = uint32_t(n);
        }
        else if (a == "--song-count")
        {
            if (!NextValue(args, i, "--song-count", v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 10) || n < 1 || n > 4096)
            {
                OptionError("--song-count needs a number from 1 to 4096");
                return 2;
            }

            o.overrides.song_count = int(n);
        }
        else if (a == "--mix-rate")
        {
            if (!NextValue(args, i, "--mix-rate", v))
            {
                return 2;
            }

            // The same range that Konami's driver detection accepts from the driver's timer setting.
            double& rate = o.overrides.mix_rate;
            if (!ParseNumber(v, rate) || !(rate >= 4000 && rate <= 65536))
            {
                OptionError("--mix-rate needs a rate from 4000 to 65536 Hz");
                return 2;
            }
        }
        else if (a == "--trace")
        {
            if (!NextValue(args, i, "--trace", v))
            {
                return 2;
            }
            if (!ParseNumber(v, n, 10) || n < 0 || n > std::numeric_limits<int>::max())
            {
                OptionError("--trace needs a song number");
                return 2;
            }

            o.trace_song = int(n);
        }
        else if (a == "--trace-frames")
        {
            if (!NextValue(args, i, "--trace-frames", v))
            {
                return 2;
            }
            if (!ParseNumber(v, o.trace_frames, 10) || o.trace_frames < 1)
            {
                OptionError("--trace-frames needs a positive number");
                return 2;
            }
        }
        else if (a == "-q" || a == "--quiet")
        {
            o.quiet = true;
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            OptionError("unknown option " + a + " (see --help)");
            return 2;
        }
        else
        {
            o.inputs.push_back(a);
        }
    }

    if (!CheckOptions(o))
    {
        return 2;
    }

    return std::nullopt;
}

// Returns a time as minutes and seconds, such as 1:35.23. It's rounded to hundredths before it's split, so that 59.999
// seconds reads 1:00.00 and not 0:60.00.
std::string TimeString(double seconds)
{
    const long long hundredths = std::llround(seconds * 100);
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%05.2f", hundredths / 6000, double(hundredths % 6000) / 100);

    return b;
}

// Prints the --info report: the ROM's title, then for each driver the game has, its detection log and warnings,
// followed by each song's data address, note-playing track count, length and loop points, calculated with the current
// conversion options. The drivers come in the order detection finds them. With --driver, --song-table or --song-count
// there's only one.
void PrintInfo(const Rom& rom, const std::vector<FoundMusic>& drivers, const std::vector<std::string>& unread,
               const Options& o)
{
    std::printf("ROM: %s (%s)%s\n", rom.Title().c_str(), rom.GameCode().c_str(), rom.FromGsf() ? ", from GSF" : "");
    for (const std::string& w : unread)
    {
        std::printf("  warning: %s; its songs are left out\n", w.c_str());
    }

    for (size_t d = 0; d < drivers.size(); d++)
    {
        if (d > 0)
        {
            std::printf("\n");
        }

        const Music& music = *drivers[d].music;
        for (const std::string& l : music.Log())
        {
            std::printf("  %s\n", l.c_str());
        }
        for (const std::string& w : music.Warnings())
        {
            std::printf("  warning: %s\n", w.c_str());
        }

        std::printf("\n song  address     tracks  length     loop\n");
        for (int s = 0; s < music.SongCount(); s++)
        {
            if (!music.HasSong(s))
            {
                std::printf("  %3d  (empty)\n", s);
                continue;
            }

            const SongReport r = music.InspectSong(s, o.convert);
            std::string loop = "-";
            if (r.loop_end >= 0)
            {
                loop = TimeString(r.loop_start) + " - " + TimeString(r.loop_end);
            }

            std::printf("  %3d  0x%08X  %6d  %-9s  %s\n", s, unsigned(r.address), r.tracks,
                        TimeString(r.seconds).c_str(), loop.c_str());
            for (const std::string& w : r.warnings)
            {
                std::printf("       warning: %s\n", w.c_str());
            }
        }
    }
}

// Prints the driver model's state after each frame of the song --trace names, which the driver's
// tools/*/compare_trace.py compares with the driver.
int Trace(const Music& music, const Options& o)
{
    std::vector<std::string> warnings;
    if (!music.Trace(o.trace_song, o.trace_frames, stdout, warnings))
    {
        std::fprintf(stderr, "%s: song %d is invalid\n", kProgramName, o.trace_song);
        return 1;
    }

    for (const std::string& w : music.Warnings())
    {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }
    for (const std::string& w : warnings)
    {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }

    return 0;
}

// Reports the conversion result, including songs skipped because they play no notes. `song_label` comes before the
// song's number. `tracks_chosen` is true when --tracks chose the tracks.
void ReportSong(const std::string& song_label, int song, const SongReport& r, bool tracks_chosen)
{
    if (r.silent)
    {
        std::printf("%s %2d: %s, skipped\n", song_label.c_str(), song,
                    tracks_chosen ? "no notes on the chosen tracks" : "plays no notes");
        return;
    }

    std::string loop;
    if (r.loop_end >= 0)
    {
        loop = ", loop " + TimeString(r.loop_start) + "-" + TimeString(r.loop_end);
    }

    const std::string midi = Utf8(PathFromUtf8(r.midi_path).filename());
    const std::string sf2 = r.sf2_path.empty() ? "" : ", " + Utf8(PathFromUtf8(r.sf2_path).filename());
    std::printf("%s %2d: %s%s, %d track%s, %.1f BPM -> %s%s\n", song_label.c_str(), song, TimeString(r.seconds).c_str(),
                loop.c_str(), r.tracks, r.tracks == 1 ? "" : "s", r.bpm, midi.c_str(), sf2.c_str());
}

// The songs that the conversion of a file has dealt with so far.
struct Tally
{
    int converted = 0;
    int silent = 0;   // songs without notes on the chosen tracks, which aren't written
    int failures = 0; // songs that failed, and files that couldn't be written
};

// Returns true if the options pick song `song` of `music`: --songs allows it, and the music has it.
bool Picked(const Options& o, const Music& music, int song)
{
    return (o.songs.empty() || o.songs.count(song)) && music.HasSong(song);
}

// Converts the songs of one of a game's drivers to the files that `settings` names, and adds them to `tally`. Messages
// put `song_label` before a song's number, and those on stderr start with `prefix`, apart from errors about the whole
// file, which start with its name, `label`.
void ConvertSongs(Music& music, const ConvertSettings& settings, const std::string& song_label,
                  const std::string& label, const std::string& prefix, const Options& o, Tally& tally)
{
    // Report what detection found. Its warnings are printed even with --quiet.
    if (!o.quiet)
    {
        for (const std::string& l : music.Log())
        {
            std::printf("  %s\n", l.c_str());
        }
    }
    for (const std::string& w : music.Warnings())
    {
        std::fprintf(stderr, "%swarning: %s\n", prefix.c_str(), w.c_str());
    }

    if (o.single_sf2)
    {
        music.ShareSoundfont(settings);
    }

    const fs::path out_dir = PathFromUtf8(settings.out_dir);
    const int converted_before = tally.converted;
    std::string error;
    for (int s = 0; s < music.SongCount(); s++)
    {
        if (!Picked(o, music, s))
        {
            continue;
        }

        const SongReport r = music.ConvertSong(s, settings);
        if (!r.ok)
        {
            tally.failures++;
        }
        else
        {
            (r.silent ? tally.silent : tally.converted)++;
            if (!o.quiet)
            {
                ReportSong(song_label, s, r, o.tracks_given);
            }
        }

        for (const std::string& w : r.warnings)
        {
            std::fprintf(stderr, "%s%s %d: %s\n", prefix.c_str(), song_label.c_str(), s, w.c_str());
        }

        if (o.dump)
        {
            if (!music.DumpSong(s, SongStem(settings, music.FileNumber(s)) + ".txt", error))
            {
                std::fprintf(stderr, "%s%s %d: %s\n", prefix.c_str(), song_label.c_str(), s, error.c_str());
                tally.failures++;
            }
        }
    }

    if (o.single_sf2 && tally.converted > converted_before)
    {
        const fs::path path = out_dir / PathFromUtf8(settings.base_name + ".sf2");
        if (!music.WriteSharedSoundfont(Utf8(path), error))
        {
            std::fprintf(stderr, "%s: %s\n", label.c_str(), error.c_str());
            tally.failures++;
        }
        else if (!o.quiet)
        {
            std::printf("soundfont -> %s\n", Utf8(path.filename()).c_str());
        }
    }
}

// Returns true if `source` is the same file as one of `files`. Links and differences in case then don't count.
bool Listed(const std::vector<fs::path>& files, const fs::path& source)
{
    std::error_code ec;
    return std::any_of(files.begin(), files.end(), [&](const fs::path& f) { return fs::equivalent(source, f, ec); });
}

// Returns the folder for a game's files: the one that --output names, or a folder called `name` beside the game. A file
// without an extension would share its path with that folder. The folder then gets a suffix.
fs::path OutputFolder(const fs::path& source, const std::string& name, const Options& o)
{
    if (!o.out_dir.empty())
    {
        return PathFromUtf8(o.out_dir);
    }

    fs::path out_dir = source.parent_path() / PathFromUtf8(name);
    if (out_dir.lexically_normal() == source.lexically_normal())
    {
        out_dir += "_output";
    }

    return out_dir;
}

// Prints the line that names a game: its title, its code, and the number of its songs, or of its drivers if it has more
// than one.
void PrintGame(const std::string& label, const Rom& rom, const std::vector<FoundMusic>& drivers)
{
    std::printf("%s: %s (%s), ", label.c_str(), rom.Title().c_str(), rom.GameCode().c_str());
    if (drivers.size() == 1)
    {
        const int songs = drivers.front().music->SongCount();
        std::printf("%d song%s\n", songs, songs == 1 ? "" : "s");
    }
    else
    {
        std::printf("%d sound drivers\n", int(drivers.size()));
    }
}

// Prints how many of a game's songs were converted, skipped and failed, and where the files went.
void PrintTally(const Tally& tally, const fs::path& out_dir)
{
    std::printf("converted %d song%s", tally.converted, tally.converted == 1 ? "" : "s");
    if (tally.silent)
    {
        std::printf(" (%d without notes skipped)", tally.silent);
    }
    if (tally.failures)
    {
        std::printf(", %d failed", tally.failures);
    }

    std::error_code ec;
    std::printf("\noutput: %s\n", Utf8(fs::absolute(out_dir, ec).lexically_normal()).c_str());
}

// Converts one input file, which messages call `label`. `done` holds the ROM images already converted or listed in this
// run, and `failed` those in which detection found no music it could read. A set of several mini-GSFs is then read
// once.
int ConvertFile(const std::string& input, const std::string& label, const Options& o, std::vector<fs::path>& done,
                std::vector<fs::path>& failed)
{
    Rom rom;
    std::string error;
    if (!rom.Load(input, error))
    {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), error.c_str());
        return 1;
    }

    // A mini-GSF only selects a song of its library, so the output is the library's music and is named after it.
    // Compare file identities so case differences and links don't cause duplicate conversions. A set's later mini-GSFs
    // skip detection, which can take seconds.
    const fs::path source = PathFromUtf8(rom.GsfLibrary().empty() ? input : rom.GsfLibrary());
    if (Listed(done, source))
    {
        if (!o.quiet)
        {
            std::printf("%s: same music as %s, already %s\n", label.c_str(), Utf8(source.filename()).c_str(),
                        o.info ? "listed" : "converted");
        }

        return 0;
    }
    if (Listed(failed, source))
    {
        std::fprintf(stderr, "%s: same music as %s, which can't be converted\n", label.c_str(),
                     Utf8(source.filename()).c_str());
        return 1;
    }

    // A game can have more than one driver. The report covers each, and a conversion converts each one's songs, but
    // --trace follows the first, and the table overrides go to the first alone.
    std::vector<std::string> unread;
    std::vector<FoundMusic> drivers = OpenAllMusic(rom, o.overrides, error, unread);
    if (drivers.empty())
    {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), error.c_str());
        failed.push_back(source);
        return 1;
    }
    if (o.overrides.song_table || o.overrides.song_count)
    {
        drivers.resize(1);
    }

    if (o.info)
    {
        PrintInfo(rom, drivers, unread, o);
        done.push_back(source);
        return 0;
    }
    if (o.trace_song >= 0)
    {
        return Trace(*drivers.front().music, o);
    }

    // Nothing is written, not even the output folder, when the options pick none of the songs.
    const auto picks = [&o](const FoundMusic& f)
    {
        for (int s = 0; s < f.music->SongCount(); s++)
        {
            if (Picked(o, *f.music, s))
            {
                return true;
            }
        }

        return false;
    };
    if (std::none_of(drivers.begin(), drivers.end(), picks))
    {
        std::fprintf(stderr, "%s: no songs selected\n", label.c_str());
        return 1;
    }

    done.push_back(source);

    const std::string name = SafeFileName(o.name.empty() ? Utf8(source.stem()) : o.name);
    const fs::path out_dir = OutputFolder(source, name, o);
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "%s: can't create %s: %s\n", label.c_str(), Utf8(out_dir).c_str(), ec.message().c_str());
        return 1;
    }

    // Report the game. Warnings are printed even with --quiet, which leaves out the line that names the file, so then
    // the warnings and errors about the file start with its name.
    const std::string prefix = o.quiet ? label + ": " : "  ";
    if (!o.quiet)
    {
        PrintGame(label, rom, drivers);
    }
    for (const std::string& w : unread)
    {
        std::fprintf(stderr, "%swarning: %s; its songs are left out\n", prefix.c_str(), w.c_str());
    }

    const auto voice_channels = [](const FoundMusic& f)
    {
        return f.music->SupportsVoiceChannels();
    };
    if (o.convert.voice_channels && std::none_of(drivers.begin(), drivers.end(), voice_channels))
    {
        std::fprintf(stderr, "%swarning: --voice-channels only changes MP2K's conversions, so it's ignored\n",
                     prefix.c_str());
    }

    const auto psg_exclusive = [](const FoundMusic& f)
    {
        return f.music->SupportsPsgExclusive();
    };
    if (o.convert.psg_exclusive && std::none_of(drivers.begin(), drivers.end(), psg_exclusive))
    {
        std::fprintf(stderr, "%swarning: --psg-exclusive only changes MP2K's SoundFonts and is ignored here\n",
                     prefix.c_str());
    }

    // The first driver's files are named after the game, and each other driver's after the game and the driver, whose
    // name messages put before its songs' numbers.
    Tally tally;
    for (size_t d = 0; d < drivers.size(); d++)
    {
        const std::string driver = DriverName(drivers[d].driver);
        ConvertSettings settings = o.convert;
        settings.out_dir = Utf8(out_dir);
        settings.base_name = d == 0 ? name : name + "_" + driver;
        ConvertSongs(*drivers[d].music, settings, d == 0 ? "song" : driver + " song", label, prefix, o, tally);
    }

    if (!o.quiet)
    {
        PrintTally(tally, out_dir);
    }

    return tally.failures ? 1 : 0;
}

// Converts every input the arguments name. Returns the exit code.
int Run(const std::vector<std::string>& args)
{
    // Keep progress and errors in order when both go to one pipe (the macOS droplet).
    xsflib::console::LineBufferOutput();

    Options o;
    if (const std::optional<int> exit_code = ParseArgs(args, o))
    {
        return *exit_code;
    }

    std::vector<fs::path> done, failed;
    int result = 0;
    for (size_t i = 0; i < o.inputs.size(); i++)
    {
        if (i > 0 && !o.quiet)
        {
            std::printf("\n");
        }

        // On Windows a path that isn't valid UTF-8, such as a GSF's _lib name in another encoding, throws, and so does
        // running out of memory. That ends this file's conversion, but not the others'. The message starts with the
        // file's name, like the first line about any file, which the macOS droplet relies on.
        std::string label = o.inputs[i];
        try
        {
            // A path that ends in a separator names the folder before it.
            fs::path path = PathFromUtf8(o.inputs[i]);
            if (path.filename().empty())
            {
                path = path.parent_path();
            }
            label = Utf8(path.filename());

            if (ConvertFile(o.inputs[i], label, o, done, failed))
            {
                result = 1;
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", label.c_str(), e.what());
            result = 1;
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    // On Windows: the arguments from the wide command line, UTF-8 output, and a window that waits after files are
    // dropped on the program, or after a double-click.
    xsflib::console::UseUtf8Output(false);
    const int result = Run(xsflib::console::Arguments(argc, argv));
    xsflib::console::WaitInOwnWindow(stdout, "\nPress Enter to close this window.");

    return result;
}
