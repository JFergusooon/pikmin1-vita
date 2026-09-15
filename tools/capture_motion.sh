#!/bin/sh
# Installs a built eboot, runs it under Vita3K, and captures the game window
# twice a few seconds apart.
#
# One still cannot show whether an animation is running. Two stills taken from
# the settled title screen can: if the flowers are swaying the frames differ,
# and if playback has frozen on a held key they are identical.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
vita3k="$HOME/Library/Application Support/Vita3K/Vita3K"
app="$vita3k/fs/ux0/app/PIKMINVIT"

build=${1:-build}
out=${2:-/tmp/pikmin-motion}
settle=${3:-200}
gap=${4:-3}

vpk="$root/$build/pikmin_vita.vpk"
if [ ! -f "$vpk" ]; then
  echo "no vpk at $vpk" >&2
  exit 1
fi

pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
sleep 2

stage=$(mktemp -d)
unzip -o -q "$vpk" -d "$stage"
rm -rf "$app/shaders"
cp -R "$stage"/. "$app"/
rm -rf "$stage"

mkdir -p "$out"
open -na /Applications/Vita3K.app --args -r PIKMINVIT

# An early shot catches the boot screen while the strip is still being built,
# which is the only way to see the progress indicator.
early=${5:-20}
sleep "$early"
id=$(swift "$root/tools/window_id.swift" || true)
if [ -n "$id" ]; then
  screencapture -o -x -l"$id" "$out/loading.png"
fi

sleep $((settle - early))

id=$(swift "$root/tools/window_id.swift" || true)
if [ -z "$id" ]; then
  echo "game window not found; emulator may have exited" >&2
  pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
  exit 1
fi

screencapture -o -x -l"$id" "$out/a.png"
sleep "$gap"
screencapture -o -x -l"$id" "$out/b.png"
pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
echo "$out/a.png $out/b.png"
