#!/bin/sh
# Builds ChronoStretch's release zip from the last device build (the toolkit's build_port.sh), catalog-ready:
#   sh tools/release.sh <version>        (from ports/chrono, in a python:3 container with the repo mounted; tools/release.ps1)
set -e
V="$1"
[ -n "$V" ] || { echo "usage: sh tools/release.sh <version>"; exit 2; }
python3 ../../tools/release.py --so build/chrono.so --skin "build/skin/Milliano - VST - ChronoStretch" \
    --entry build/pluginlist-entry.xml --version "$V" \
    --extra User-README.txt:User/README.txt --extra LICENSE:LICENSE.txt \
    --extra THIRD-PARTY-NOTICES.md:THIRD-PARTY-NOTICES.md --user-data User \
    --repo guerrillamilliano/ChronoStretch --license MIT --id chronostretch \
    --about "A cue sampler: finds the BPM, key and hits of your WAVs, browses and previews your drives, chops a sound by transient, slices, beat or by hand across 64 pads in four banks on a zoomable waveform, plays them in time and in key with MPC, with per-pad controls, per-pad FX presets, undo and drum removal." \
    -o dist
python3 ../../tools/catalog_check.py "dist/ChronoStretch-$V-mpc-armv7.zip" --catalog --expect-id chronostretch \
    --expect-repo guerrillamilliano/ChronoStretch
