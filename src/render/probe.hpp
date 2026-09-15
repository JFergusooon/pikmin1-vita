#pragma once

#include <vita2d.h>

namespace render {

// Bring-up probe for the GXM 3D path: a depth-tested, textured, rotating cube.
// It proves shader registration, 3D vertex formats, index buffers, uniform
// upload, depth testing and texture binding all work on the target before real
// model data depends on them. Compiled in only under PIKMIN_RENDER_PROBE.
class Probe {
 public:
  bool init(vita2d_texture* texture);
  void shutdown();
  void draw(float seconds);

 private:
  class Impl;
  Impl* impl_ = nullptr;
};

}  // namespace render
