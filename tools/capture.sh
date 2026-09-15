#!/bin/sh
# Installs a built eboot, runs it under Vita3K, and captures the game window.
#
# The capture goes through `screencapture -l <window id>`, which reads a window
# even while it sits behind others. Grabbing the screen instead would need the
# window focused, and raising it needs an accessibility grant this script does
# not have. Launching through `open` detaches the emulator from the calling
# shell, which otherwise kills it when the shell exits.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
vita3k="$HOME/Library/Application Support/Vita3K/Vita3K"
app="$vita3k/fs/ux0/app/PIKMINVIT"

build=${1:-build}
out=${2:-/tmp/pikmin-capture}
settle=${3:-18}

vpk="$root/$build/pikmin_vita.vpk"
if [ ! -f "$vpk" ]; then
  echo "no vpk at $vpk" >&2
  exit 1
fi

pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
sleep 2

# The whole package is installed, not just eboot.bin: the generated TEV shaders
# ship as VPK files, and copying the executable alone leaves the app directory
# holding a stale shader set.
stage=$(mktemp -d)
unzip -o -q "$vpk" -d "$stage"
rm -rf "$app/shaders"
cp -R "$stage"/. "$app"/
rm -rf "$stage"

mkdir -p "$out"
open -na /Applications/Vita3K.app --args -r PIKMINVIT
sleep "$settle"

id=$(swift "$root/tools/window_id.swift" || true)
if [ -z "$id" ]; then
  echo "game window not found; emulator may have exited" >&2
  pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
  exit 1
fi

screencapture -o -x -l"$id" "$out/frame.png"
pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
echo "$out/frame.png"
