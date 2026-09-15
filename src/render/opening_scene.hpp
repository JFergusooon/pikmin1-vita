#pragma once

#include <string>

namespace render {

// The real 3D actor behind Pikmin's title screen. Geometry, materials and the
// camera all come from the retail opening.mod/opening.dsk files.
class OpeningScene {
 public:
  bool init(const char* data_root);
  void shutdown();
  void draw(float frame);

  bool ready() const { return impl_ != nullptr; }
  const char* error() const { return error_.c_str(); }

 private:
  class Impl;
  Impl* impl_ = nullptr;
  std::string error_;
};

}  // namespace render
