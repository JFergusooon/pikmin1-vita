#pragma once

#include <vita2d.h>

#include <string>
#include <vector>

#include "scene_raster.hpp"

namespace render {

// The title screen's opening descent, rasterised on the CPU.
//
// The offline GXP toolchain does not reproduce a GameCube material yet, so the
// background comes from src/scene_raster.cpp rather than the GXM path. The
// rasteriser is far too slow to run per displayed frame, so the descent is
// rendered once into a frame strip at the Vita panel's native 960x544 and then
// played back.
//
// The strip is built a frame at a time rather than in one call. Rendering it
// all at once wedges the application for minutes behind an unchanging boot
// logo, with no way to tell progress from a hang; stepping it lets the logo
// screen stay live and report how far along it is.
//
// This is the real opening.mod terrain and logo.mod flower logo moving through
// the real opening.dsk camera, shaded through the materials' own TEV chains.
class TitleBackground {
 public:
  // Parses the models, animations and camera and decodes their textures. Must
  // be called before step().
  bool begin(const char* data_root);

  // Rasterises and uploads the next strip frame. Returns true while frames
  // remain, so it can drive a loop directly.
  bool step();

  bool done() const { return source_ == nullptr && cache_read_fd_ < 0; }

  // True while frames are being rasterised rather than read back from the
  // cache, so the loading screen can say which of the two is happening.
  bool building() const { return source_ != nullptr; }

  // How much of the strip exists, as 0..1, for a progress indicator.
  float progress() const;

  bool ready() const { return !frames_.empty(); }
  const char* error() const { return error_.c_str(); }

  void release();

  // Draws the strip frame nearest a cinematic frame, stretched to the panel.
  void draw(float cinematic_frame) const;

 private:
  // The geometry, textures and camera the strip is rendered from. Held only
  // while building, then dropped: together they outweigh the strip itself.
  struct Source {
    cine::Dsk dsk;
    raster::Actor opening;
    raster::Actor logo;
    raster::Target target;
    float frame;
    float sway_period;
    bool in_sway;
  };

  bool append(const raster::Target& target);

  // Rasterising the strip costs minutes and produces the same images on every
  // boot, so the first run writes it to disk and later runs just read it back.
  bool open_cache();
  bool read_cached_frames(int count);
  void finish_cache(bool complete);

  std::vector<vita2d_texture*> frames_;
  // Scratch for one frame as packed RGB565, shared by the texture upload and
  // the cache file since both want exactly that layout.
  std::vector<uint8_t> packed_;
  int cache_read_fd_ = -1;
  int cache_write_fd_ = -1;
  size_t cache_remaining_ = 0;
  Source* source_ = nullptr;
  // Where the descent ends and the settled-camera sway cycle begins. Playback
  // repeats everything from here on, so the flowers never stop moving.
  size_t loop_start_ = 0;
  size_t expected_frames_ = 0;
  int width_ = 0;
  int height_ = 0;
  float frame_stride_ = 1.0f;
  std::string error_;
};

}  // namespace render
