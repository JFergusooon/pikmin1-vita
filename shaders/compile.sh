#!/bin/sh
# Compiles the port's GLSL material shaders to Vita GXP binaries.
#
# The Vita consumes only precompiled GXP. Sony's psp2cgc is unavailable and
# Vita3K stubs out the runtime SceShaccCg compiler, so shaders go through
# glslangValidator into SPIR-V and then through psp2spvc into GXP, offline.
# The resulting .gxp files ship inside the VPK.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
psp2spvc="$root/.toolchain/psp2spvc/build/src/psp2spvc"

if [ ! -x "$psp2spvc" ]; then
  echo "psp2spvc not built; see README" >&2
  exit 1
fi

out="$here/gxp"
mkdir -p "$out"

# shaders/gen holds the per-material TEV programs emitted by
# tools/tev_shader_gen, which are compiled the same way as the handwritten ones.
for source in "$here"/*.vert "$here"/*.frag "$here"/gen/*.vert "$here"/gen/*.frag; do
  [ -e "$source" ] || continue
  name=$(basename "$source")
  spv="$out/$name.spv"
  gxp="$out/${name%.*}.$(echo "${name##*.}" | cut -c1)gxp"
  glslangValidator "$source" -V -o "$spv" >/dev/null
  "$psp2spvc" "$spv" -o "$gxp" -Oreg-space
  rm -f "$spv"
  printf '%-28s %6d bytes\n' "$(basename "$gxp")" "$(wc -c < "$gxp")"
done
