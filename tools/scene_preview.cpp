// Host preview of the title-screen background.
//
//   scene_preview <data-root> <frame> <out.tga> [opening|logo|both] [width height]
//
// Renders through src/scene_raster.cpp, which is the same code the Vita build
// rasterises the background with, so this preview and the handheld agree.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "scene_raster.hpp"

namespace {

bool write_tga(const std::string& path, const raster::Target& target) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  uint8_t header[18] = {0};
  header[2] = 2;  // uncompressed true-colour
  header[12] = static_cast<uint8_t>(target.width & 0xFF);
  header[13] = static_cast<uint8_t>((target.width >> 8) & 0xFF);
  header[14] = static_cast<uint8_t>(target.height & 0xFF);
  header[15] = static_cast<uint8_t>((target.height >> 8) & 0xFF);
  header[16] = 32;
  header[17] = 0x28;  // 8 alpha bits, top-left origin
  std::fwrite(header, 1, sizeof(header), file);

  std::vector<uint8_t> row(static_cast<size_t>(target.width) * 4);
  for (int y = 0; y < target.height; ++y) {
    for (int x = 0; x < target.width; ++x) {
      const uint8_t* pixel = &target.rgba[(static_cast<size_t>(y) * target.width + x) * 4];
      row[x * 4 + 0] = pixel[2];
      row[x * 4 + 1] = pixel[1];
      row[x * 4 + 2] = pixel[0];
      row[x * 4 + 3] = pixel[3];
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: scene_preview <data-root> <frame> <out.tga> "
                 "[opening|logo|both] [width height]\n");
    return 2;
  }

  const std::string root = argv[1];
  const float frame = static_cast<float>(std::atof(argv[2]));
  const std::string out = argv[3];
  const std::string which = (argc >= 5) ? argv[4] : "both";
  const int width = (argc >= 7) ? std::atoi(argv[5]) : 960;
  const int height = (argc >= 7) ? std::atoi(argv[6]) : 544;

  try {
    const std::string opening_base = root + "cinemas/opening/";
    const cine::Dsk dsk =
        cine::parse_dsk(cine::read_text_file(opening_base + "opening.dsk"), "opening.dsk");

    std::printf("frame %.1f at %dx%d, eye=(%.1f, %.1f, %.1f) fovy=%.2fdeg\n", frame, width, height,
                raster::camera_field(dsk, "cam_pos_x", frame, 0.0f),
                raster::camera_field(dsk, "cam_pos_y", frame, 402.0f),
                raster::camera_field(dsk, "cam_pos_z", frame, 20.0f),
                raster::camera_field(dsk, "cam_fovy", frame, 41.5f));

    const render::Mat4 view_projection = raster::title_view_projection(dsk, frame, width, height);

    raster::Target target;
    target.reset(width, height);

    std::string error;
    if (which == "opening" || which == "both") {
      raster::Actor opening;
      if (!opening.load(opening_base + "opening.mod", opening_base + "opening.anm", "opening",
                        error)) {
        std::fprintf(stderr, "opening: %s\n", error.c_str());
        return 1;
      }
      std::printf("  opening: %ld triangles\n", opening.draw(target, view_projection, frame));
    }
    if (which == "logo" || which == "both") {
      const std::string logo_base = root + "cinemas/titles/";
      raster::Actor logo;
      logo.loops = true;
      if (!logo.load(logo_base + "logo.mod", logo_base + "logo.anm", "logo", error)) {
        std::fprintf(stderr, "logo: %s\n", error.c_str());
        return 1;
      }
      std::printf("  logo: %ld triangles\n", logo.draw(target, view_projection, frame));
    }

    if (!write_tga(out, target)) {
      std::fprintf(stderr, "could not write %s\n", out.c_str());
      return 1;
    }
    std::printf("wrote %s\n", out.c_str());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
