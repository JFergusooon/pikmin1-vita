#pragma once

#include <vita2d.h>

#include <string>
#include <vector>

#include "cine/cine.h"
#include "render/matrix.hpp"

#ifndef PIKMIN_SCENE_DEPTH
#define PIKMIN_SCENE_DEPTH 1
#endif

namespace render {

// The title cinematic drawn through vita2d's own shaders.
//
// The offline GXP toolchain emits fragment programs whose varying block does
// not match what Sony's compiler produces, so the generated per-material TEV
// shaders sample garbage. vita2d's shaders are built by that same Sony
// toolchain and are known good, so this path uses them instead: the vertex
// transform runs on the CPU, which is cheap, and the GPU does the fill, which
// is what actually costs.
//
// Two shapes of draw cover both models. logo.mod's 599 flower batches carry no
// texture at all -- its only texture is the copyright strip -- so they go
// through the per-vertex-colour array. opening.mod's terrain is seven textured
// batches, so those go through the textured array with the material colour as
// the tint.
class SceneGpu {
 public:
  bool init(const char* data_root);
  void shutdown();

  bool ready() const { return ready_; }
  const char* error() const { return error_.c_str(); }

  // Call inside vita2d_start_drawing()/vita2d_end_drawing().
  void draw(float cinematic_frame);

  // What the last draw actually produced, so an empty screen can be told apart
  // from geometry landing somewhere unexpected.
  const char* stats() const { return stats_.c_str(); }

 private:
  // A model plus the GPU textures and scratch buffers it draws through.
  struct Actor {
    cine::ModInventory model;
    cine::Geometry geometry;
    cine::Dck animation;
    std::vector<vita2d_texture*> textures;

    bool load(const std::string& mod_path, const std::string& anm_path, const char* label,
              std::string& error);
    void release();
  };

  void draw_actor(Actor& actor, const Mat4& view_projection, float frame);

  Actor opening_;
  Actor logo_;
  cine::Dsk dsk_;
  bool ready_ = false;
  // vita2d never clears the depth buffer, so whether a depth test can be used
  // at all has to be established on hardware rather than assumed.
  bool depth_test_ = PIKMIN_SCENE_DEPTH;
  std::string error_;
  std::string stats_;

  // Accumulated over a frame's batches to build stats_.
  long emitted_triangles_ = 0;
  long textured_calls_ = 0;
  long colored_calls_ = 0;
  float min_x_ = 0.0f;
  float max_x_ = 0.0f;
  float min_y_ = 0.0f;
  float max_y_ = 0.0f;

  // Reused across batches and frames so a frame costs no allocation.
  std::vector<vita2d_texture_vertex> textured_;
  std::vector<vita2d_color_vertex> colored_;
};

}  // namespace render
