#!/bin/sh
# SPDX-License-Identifier: MIT

# Stands in for the program in droplet_test.applescript. For each file it prints what the program prints,
# picked by the file's name: good.gba converts, one of partial.gba's songs can't be written, bad.gba
# has no driver, set-01.minigsf converts its set, and set-02.minigsf holds the same music.
# Like the program, it exits with 1 if anything failed.

status=0
first=1
for path in "$@"; do
    name=$(basename "$path")
    folder=$(dirname "$path")
    [ "$first" = 1 ] || echo
    first=0
    case "$name" in
    good.gba)
        echo "$name: TEST (TEST), 2 songs"
        echo "  song table 0x08000100 (from play-song code)"
        echo "song  0: 0:01.00, 2 tracks, 120.0 BPM -> good_00.mid, good_00.sf2"
        echo "song  1: 0:01.00, 2 tracks, 120.0 BPM -> good_01.mid, good_01.sf2"
        echo "converted 2 songs"
        echo "output: $folder/good"
        ;;
    partial.gba)
        echo "$name: TEST (TEST), 2 songs"
        echo "song  0: 0:01.00, 2 tracks, 120.0 BPM -> partial_00.mid, partial_00.sf2"
        echo "  song 1: can't write $folder/partial/partial_01.mid" >&2
        echo "converted 1 song, 1 failed"
        echo "output: $folder/partial"
        status=1
        ;;
    set-01.minigsf)
        echo "$name: TEST (TEST), 2 songs"
        echo "song  0: 0:01.00, 2 tracks, 120.0 BPM -> set_00.mid, set_00.sf2"
        echo "song  1: 0:01.00, 2 tracks, 120.0 BPM -> set_01.mid, set_01.sf2"
        echo "converted 2 songs"
        echo "output: $folder/set"
        ;;
    set-02.minigsf)
        echo "$name: same music as set.gsflib, already converted"
        ;;
    *)
        echo "$name: no Konami, Rare, Quintet, Nintendo R&D2, Brownie Brown, Ubisoft Milan, Krawall or MP2K sound driver found: this game's music uses" \
             "another engine, or a driver version that agbsf2 doesn't support" >&2
        status=1
        ;;
    esac
done

exit $status
