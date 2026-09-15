// Checks that rasterising a frame in horizontal bands matches rasterising it
// whole. The device splits the frame across cores that way, so if the two
// disagree the split is unsound rather than merely slower.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "scene_raster.hpp"

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "runtime-data/dataDir/";
  const float frame = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 209.0f;
  const int bands = argc > 3 ? std::atoi(argv[3]) : 3;
  const int width = 480;
  const int height = 272;

  std::string error;
  raster::Actor opening;
  if (!opening.load(root + "cinemas/opening/opening.mod", root + "cinemas/opening/opening.anm",
                    "opening", error)) {
    std::fprintf(stderr, "opening: %s\n", error.c_str());
    return 1;
  }
  raster::Actor logo;
  logo.loops = true;
  if (!logo.load(root + "cinemas/titles/logo.mod", root + "cinemas/titles/logo.anm", "logo",
                 error)) {
    std::fprintf(stderr, "logo: %s\n", error.c_str());
    return 1;
  }
  const cine::Dsk dsk = cine::parse_dsk(
      cine::read_text_file(root + "cinemas/opening/opening.dsk"), "opening.dsk");
  const render::Mat4 vp = raster::title_view_projection(dsk, frame, width, height);

  raster::Target whole;
  whole.reset(width, height);
  opening.draw(whole, vp, frame);
  logo.draw(whole, vp, frame);

  raster::Target banded;
  banded.reset(width, height);
  for (int i = 0; i < bands; ++i) {
    const int begin = height * i / bands;
    const int end = height * (i + 1) / bands;
    opening.draw(banded, vp, frame, begin, end);
    logo.draw(banded, vp, frame, begin, end);
  }

  long differing = 0;
  int worst = 0;
  for (std::size_t i = 0; i < whole.rgba.size(); ++i) {
    const int delta = std::abs(int(whole.rgba[i]) - int(banded.rgba[i]));
    if (delta != 0) {
      ++differing;
      if (delta > worst) worst = delta;
    }
  }
  std::printf("frame %.0f, %d bands: %ld differing bytes of %zu (worst %d)\n", frame, bands,
              differing, whole.rgba.size(), worst);
  std::printf("  => %s\n", differing == 0 ? "IDENTICAL, split is sound" : "MISMATCH");
  return differing == 0 ? 0 : 1;
}
