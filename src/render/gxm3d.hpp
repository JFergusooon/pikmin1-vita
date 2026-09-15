#pragma once

#include <psp2/gxm.h>
#include <vita2d.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "render/matrix.hpp"

namespace render {

// One GameCube model vertex, in the order the .mod attribute chunks supply.
// Colours stay 8-bit and are normalised by the vertex attribute format, as GX
// did, rather than being widened on the CPU.
struct VertexGcn {
  float position[3];
  float normal[3];
  float texcoord0[2];
  float texcoord1[2];
  uint8_t color[4];
};

struct DirectionalLight {
  float direction[4];
  float color[4];
};

// The most texture maps and coordinates any title material uses.
const int kMaxMaterialTextures = 4;

// Everything a material vertex stage reads for one draw. Each texture matrix
// carries the corresponding texgen's transform for this frame, which is how the
// scrolling layers in the opening move.
struct MaterialUniforms {
  Mat4 view_proj;
  Mat4 world;
  Mat4 tex_matrix[kMaxMaterialTextures];
  DirectionalLight lights[3];
  float ambient[4];
  float material_color[4];
};

MaterialUniforms default_material_uniforms();

// GPU-mapped storage for geometry that outlives a frame. vita2d's pool is
// per-frame and reset on every swap, so model data needs its own allocation.
class GpuBuffer {
 public:
  GpuBuffer() = default;
  ~GpuBuffer();

  GpuBuffer(const GpuBuffer&) = delete;
  GpuBuffer& operator=(const GpuBuffer&) = delete;

  bool upload(const void* data, size_t size);
  void release();

  void* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  SceUID uid_ = -1;
  void* data_ = nullptr;
  size_t size_ = 0;
};

// Draws indexed 3D geometry through vita2d's GXM context, so the 2D menu and
// the 3D background share one scene and one depth buffer.
//
// A GameCube material combines textures and lighting in up to eight TEV stages
// configured by the model file, which no single shader can reproduce. The Vita
// runs only precompiled shaders and Vita3K stubs out the runtime compiler, so
// tools/tev_shader_gen emits one shader pair per distinct configuration at
// build time and this class loads the whole set, selecting per draw.
class Renderer3D {
 public:
  bool init();
  void shutdown();

  bool ready() const { return ready_; }
  const char* error() const { return error_.c_str(); }

  // Both must be called inside vita2d_start_drawing()/vita2d_end_drawing().
  // begin_scene turns on depth testing; end_scene puts the context back the
  // way vita2d's 2D drawing expects to find it.
  void begin_scene();
  void end_scene();

  // Index of the program pair reproducing a TEV configuration, or -1 when the
  // configuration was not present in the models the generator ran over.
  int program_for(uint64_t tev_key) const;

  void draw(int program, const GpuBuffer& vertices, const GpuBuffer& indices,
            uint32_t first_index, uint32_t index_count, const MaterialUniforms& uniforms,
            vita2d_texture* const* textures, int texture_count);

 private:
  // A vertex stage, shared by every fragment program exchanging the same number
  // of texture coordinates. The scene uniforms live here.
  struct VertexVariant {
    int texcoords = 0;
    std::vector<uint8_t> gxp;
    SceGxmShaderPatcherId id = {};
    SceGxmVertexProgram* program = nullptr;

    const SceGxmProgramParameter* view_proj = nullptr;
    const SceGxmProgramParameter* world = nullptr;
    const SceGxmProgramParameter* tex_matrix[kMaxMaterialTextures] = {nullptr, nullptr, nullptr,
                                                                      nullptr};
    const SceGxmProgramParameter* light_dir[3] = {nullptr, nullptr, nullptr};
    const SceGxmProgramParameter* light_color[3] = {nullptr, nullptr, nullptr};
    const SceGxmProgramParameter* ambient = nullptr;
    const SceGxmProgramParameter* material_color = nullptr;
  };

  struct Program {
    uint64_t key = 0;
    int vertex_variant = -1;
    std::vector<uint8_t> gxp;
    SceGxmShaderPatcherId id = {};
    SceGxmFragmentProgram* program = nullptr;
  };

  static bool load_program(const char* path, std::vector<uint8_t>& out);
  int ensure_vertex_variant(SceGxmShaderPatcher* patcher, int texcoords);

  bool ready_ = false;
  std::string error_;

  std::vector<VertexVariant> vertex_variants_;
  std::vector<Program> programs_;
};

}  // namespace render
