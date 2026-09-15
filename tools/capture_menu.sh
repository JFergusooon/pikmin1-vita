#!/bin/sh
# Installs a built eboot, runs it under Vita3K, and walks the title sequence
# into the main menu, capturing along the way.
#
# Entering the menu needs a button press, so this one drives the emulator
# through System Events rather than only watching it. That needs an
# accessibility grant; without it the key presses are refused and only the
# title captures come out.
#
# Key names come from Vita3K's own config.yml: start is Enter, down is
# ArrowDown.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
vita3k="$HOME/Library/Application Support/Vita3K/Vita3K"
app="$vita3k/fs/ux0/app/PIKMINVIT"

build=${1:-build}
out=${2:-/tmp/pikmin-menu}
settle=${3:-150}

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

# Watch the loading bar go by rather than guessing blind.
sleep 25
id=$(swift "$root/tools/window_id.swift" || true)
[ -n "$id" ] && screencapture -o -x -l"$id" "$out/1-loading.png"

sleep $((settle - 25))
id=$(swift "$root/tools/window_id.swift" || true)
if [ -z "$id" ]; then
  echo "game window not found; emulator may have exited" >&2
  pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
  exit 1
fi

# The descent has landed and the press-start prompt is up.
screencapture -o -x -l"$id" "$out/2-title.png"

press() {
  osascript -e 'tell application "Vita3K" to activate' >/dev/null 2>&1 || true
  osascript -e "tell application \"System Events\" to key code $1" >/dev/null 2>&1 \
    || echo "key press refused (accessibility not granted)" >&2
}

# 36 is Return, which Vita3K maps to start.
press 36
sleep 2
screencapture -o -x -l"$id" "$out/3-menu-start.png"

# 125 is the down arrow. The blend runs for half a second, so one shot lands
# mid-fade and the next after it settles.
press 125
sleep 1
screencapture -o -x -l"$id" "$out/4-menu-options.png"

pkill -f 'Vita3K -r PIKMINVIT' 2>/dev/null || true
ls "$out"
