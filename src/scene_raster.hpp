#pragma once

// Software rasteriser for the title-screen actors.
//
// The GXP shader toolchain does not yet reproduce a GameCube material, so the
// title background is rasterised on the CPU from the same .mod geometry, .dck
// joints, .dsk camera and model textures the GXM path uses. That keeps the
// image real disc data rather than a stand-in, at the cost of being a still.
//
// Free of vita2d and GXM so the host preview in tools/scene_preview renders
// through exactly this code and a host image means the handheld agrees.

#include <cstdint>
#include <string>
#include <vector>

#include "cine/cine.h"
#include "gcn_texture.hpp"
#include "render/matrix.hpp"
#include "render/skeleton.hpp"

namespace raster {

// RGBA8, top-left origin, with a matching depth buffer.
struct Target {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
  std::vector<float> depth;

  void reset(int width, int height);
};

// One drawable model: its geometry, the images its materials sample, and the
// joint world matrices for the frame being drawn.
struct Actor {
  cine::ModInventory model;
  cine::Geometry geometry;
  cine::Dck animation;
  std::vector<gcn::Image> images;

  // Set for an idle that cycles, such as logo.anm's swaying flowers, so it is
  // sampled with wrap_frame rather than held on its last key.
  bool loops = false;

  // Decodes every texture the model carries. Returns false and sets error on
  // the first one that fails.
  bool load(const std::string& mod_path, const std::string& anm_path, const char* label,
            std::string& error);

  // Rasterises the model posed at a cinematic frame. Returns the number of
  // triangles that reached the framebuffer.
  //
  // Writes are restricted to rows [row_begin, row_end), which is what lets the
  // frame be split across cores: bands do not overlap, so threads sharing one
  // target never touch the same colour or depth pixel. row_end of -1 means the
  // whole target.
  long draw(Target& target, const render::Mat4& view_projection, float frame, int row_begin = 0,
            int row_end = -1) const;
};

// The camera opening.dsk describes at a cinematic frame, composed the way
// CamDataInfo::update and Matrix4f::makeLookat do, including the pillarbox that
// keeps the GameCube's 4:3 framing inside the Vita's panel.
render::Mat4 title_view_projection(const cine::Dsk& dsk, float frame, int target_width,
                                   int target_height);

// Reads the value of a named camera field, or the fallback when absent.
float camera_field(const cine::Dsk& dsk, const char* name, float frame, float fallback);

}  // namespace raster
