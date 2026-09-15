#include "render/gxm3d.hpp"

#include <psp2/kernel/sysmem.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "render/tev_programs.hpp"

namespace render {
namespace {

size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

void* gpu_alloc(size_t size, SceUID* uid) {
  const size_t aligned = align_up(size, 4096);
  const SceUID block = sceKernelAllocMemBlock(
      "pikmin_gpu", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, static_cast<SceSize>(aligned),
      nullptr);
  if (block < 0) {
    return nullptr;
  }

  void* memory = nullptr;
  if (sceKernelGetMemBlockBase(block, &memory) < 0) {
    sceKernelFreeMemBlock(block);
    return nullptr;
  }
  if (sceGxmMapMemory(memory, static_cast<SceSize>(aligned), SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
    sceKernelFreeMemBlock(block);
    return nullptr;
  }

  *uid = block;
  return memory;
}

void set_matrix(void* uniforms, const SceGxmProgramParameter* parameter, const Mat4& value) {
  if (parameter != nullptr) {
    // The generated shaders transform with `mat4 * vec4` under standard GLSL
    // rules, so the uniform payload is read as four consecutive column
    // vectors. Mat4 already stores columns, so it uploads unchanged.
    //
    // Transposing here instead makes the projection's w row land in a column,
    // so clip w comes from the vertex x and y rather than its depth. Every
    // triangle then divides by a different, position-dependent w and the scene
    // collapses into a thin band of slivers mirrored about the screen centre.
    sceGxmSetUniformDataF(uniforms, parameter, 0, 16, value.m);
  }
}

void set_vec4(void* uniforms, const SceGxmProgramParameter* parameter, const float value[4]) {
  if (parameter != nullptr) {
    sceGxmSetUniformDataF(uniforms, parameter, 0, 4, value);
  }
}

std::string shader_path(const char* name, const char* extension) {
  return std::string("app0:shaders/") + name + extension;
}

}  // namespace

MaterialUniforms default_material_uniforms() {
  MaterialUniforms u = {};
  u.view_proj = identity();
  u.world = identity();
  for (int i = 0; i < kMaxMaterialTextures; ++i) {
    u.tex_matrix[i] = identity();
  }
  for (int i = 0; i < 3; ++i) {
    u.lights[i].direction[0] = 0.0f;
    u.lights[i].direction[1] = -1.0f;
    u.lights[i].direction[2] = 0.0f;
    u.lights[i].direction[3] = 0.0f;
  }
  u.ambient[0] = u.ambient[1] = u.ambient[2] = 1.0f;
  u.ambient[3] = 1.0f;
  u.material_color[0] = u.material_color[1] = u.material_color[2] = 1.0f;
  u.material_color[3] = 1.0f;
  return u;
}

GpuBuffer::~GpuBuffer() {
  release();
}

bool GpuBuffer::upload(const void* data, size_t size) {
  release();
  if (size == 0) {
    return false;
  }

  SceUID uid = -1;
  void* memory = gpu_alloc(size, &uid);
  if (memory == nullptr) {
    return false;
  }
  std::memcpy(memory, data, size);

  uid_ = uid;
  data_ = memory;
  size_ = size;
  return true;
}

void GpuBuffer::release() {
  if (data_ != nullptr) {
    sceGxmUnmapMemory(data_);
    data_ = nullptr;
  }
  if (uid_ >= 0) {
    sceKernelFreeMemBlock(uid_);
    uid_ = -1;
  }
  size_ = 0;
}

bool Renderer3D::load_program(const char* path, std::vector<uint8_t>& out) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long length = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (length <= 0) {
    std::fclose(file);
    return false;
  }
  out.resize(static_cast<size_t>(length));
  const size_t read = std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  return read == out.size();
}

int Renderer3D::ensure_vertex_variant(SceGxmShaderPatcher* patcher, int texcoords) {
  for (size_t i = 0; i < vertex_variants_.size(); ++i) {
    if (vertex_variants_[i].texcoords == texcoords) {
      return static_cast<int>(i);
    }
  }

  VertexVariant variant;
  variant.texcoords = texcoords;

  char name[32];
  std::snprintf(name, sizeof(name), "tev_vs%d", texcoords);
  if (!load_program(shader_path(name, ".vgxp").c_str(), variant.gxp)) {
    error_ = "cannot read a generated vertex program";
    return -1;
  }

  const SceGxmProgram* gxp = reinterpret_cast<const SceGxmProgram*>(variant.gxp.data());
  if (sceGxmShaderPatcherRegisterProgram(patcher, gxp, &variant.id) < 0) {
    error_ = "generated vertex program rejected";
    return -1;
  }

  const SceGxmProgramParameter* position =
      sceGxmProgramFindParameterByName(gxp, "in_position");
  const SceGxmProgramParameter* normal = sceGxmProgramFindParameterByName(gxp, "in_normal");
  const SceGxmProgramParameter* texcoord0 =
      sceGxmProgramFindParameterByName(gxp, "in_texcoord0");
  const SceGxmProgramParameter* texcoord1 =
      sceGxmProgramFindParameterByName(gxp, "in_texcoord1");
  const SceGxmProgramParameter* color = sceGxmProgramFindParameterByName(gxp, "in_color");
  if (position == nullptr || normal == nullptr || texcoord0 == nullptr || texcoord1 == nullptr ||
      color == nullptr) {
    error_ = "vertex attributes missing";
    return -1;
  }

  // Every uniform is required. Uploading through a name the compiler did not
  // emit is a silent no-op that leaves the register at whatever the buffer
  // already held, which renders as an empty scene with nothing to explain it,
  // so a missing name is reported by name instead.
  std::string missing;
  const auto find_uniform = [&](const char* name) {
    const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(gxp, name);
    if (parameter == nullptr) {
      if (!missing.empty()) {
        missing += ", ";
      }
      missing += name;
    }
    return parameter;
  };

  variant.view_proj = find_uniform("SceneBlock.view_proj");
  variant.world = find_uniform("SceneBlock.world");
  for (int i = 0; i < kMaxMaterialTextures; ++i) {
    char uniform[40];
    std::snprintf(uniform, sizeof(uniform), "SceneBlock.tex_matrix%d", i);
    variant.tex_matrix[i] = find_uniform(uniform);
  }
  for (int i = 0; i < 3; ++i) {
    char direction[40];
    char color_name[40];
    std::snprintf(direction, sizeof(direction), "SceneBlock.light_dir%d", i);
    std::snprintf(color_name, sizeof(color_name), "SceneBlock.light_color%d", i);
    variant.light_dir[i] = find_uniform(direction);
    variant.light_color[i] = find_uniform(color_name);
  }
  variant.ambient = find_uniform("SceneBlock.ambient");
  variant.material_color = find_uniform("SceneBlock.material_color");
  if (!missing.empty()) {
    error_ = "vertex uniforms missing: " + missing;
    return -1;
  }

  SceGxmVertexAttribute attributes[5] = {};
  attributes[0].streamIndex = 0;
  attributes[0].offset = offsetof(VertexGcn, position);
  attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  attributes[0].componentCount = 3;
  attributes[0].regIndex = sceGxmProgramParameterGetResourceIndex(position);

  attributes[1].streamIndex = 0;
  attributes[1].offset = offsetof(VertexGcn, normal);
  attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  attributes[1].componentCount = 3;
  attributes[1].regIndex = sceGxmProgramParameterGetResourceIndex(normal);

  attributes[2].streamIndex = 0;
  attributes[2].offset = offsetof(VertexGcn, texcoord0);
  attributes[2].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  attributes[2].componentCount = 2;
  attributes[2].regIndex = sceGxmProgramParameterGetResourceIndex(texcoord0);

  attributes[3].streamIndex = 0;
  attributes[3].offset = offsetof(VertexGcn, texcoord1);
  attributes[3].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  attributes[3].componentCount = 2;
  attributes[3].regIndex = sceGxmProgramParameterGetResourceIndex(texcoord1);

  attributes[4].streamIndex = 0;
  attributes[4].offset = offsetof(VertexGcn, color);
  attributes[4].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
  attributes[4].componentCount = 4;
  attributes[4].regIndex = sceGxmProgramParameterGetResourceIndex(color);

  SceGxmVertexStream stream = {};
  stream.stride = sizeof(VertexGcn);
  stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_32BIT;

  if (sceGxmShaderPatcherCreateVertexProgram(patcher, variant.id, attributes, 5, &stream, 1,
                                             &variant.program) < 0) {
    error_ = "vertex program creation failed";
    return -1;
  }

  vertex_variants_.push_back(std::move(variant));
  return static_cast<int>(vertex_variants_.size() - 1);
}

bool Renderer3D::init() {
  if (ready_) {
    return true;
  }

  SceGxmShaderPatcher* patcher = vita2d_get_shader_patcher();
  if (patcher == nullptr) {
    error_ = "no shader patcher";
    return false;
  }

  size_t count = 0;
  const TevProgramEntry* table = tev_program_table(&count);
  if (count == 0) {
    error_ = "no generated TEV programs";
    return false;
  }

  for (size_t i = 0; i < count; ++i) {
    const int variant = ensure_vertex_variant(patcher, table[i].texcoords);
    if (variant < 0) {
      return false;
    }

    Program program;
    program.key = table[i].key;
    program.vertex_variant = variant;
    if (!load_program(shader_path(table[i].name, ".fgxp").c_str(), program.gxp)) {
      error_ = "cannot read a generated fragment program";
      return false;
    }

    const SceGxmProgram* gxp = reinterpret_cast<const SceGxmProgram*>(program.gxp.data());
    if (sceGxmShaderPatcherRegisterProgram(patcher, gxp, &program.id) < 0) {
      error_ = "generated fragment program rejected";
      return false;
    }

    // The pair is matched here, which is why the generator emits a vertex stage
    // per texture coordinate count: the varyings have to line up exactly.
    const SceGxmProgram* vertex_gxp =
        reinterpret_cast<const SceGxmProgram*>(vertex_variants_[variant].gxp.data());
    if (sceGxmShaderPatcherCreateFragmentProgram(
            patcher, program.id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
            nullptr, vertex_gxp, &program.program) < 0) {
      error_ = "fragment program creation failed";
      return false;
    }

    programs_.push_back(std::move(program));
  }

  ready_ = true;
  return true;
}

void Renderer3D::shutdown() {
  SceGxmShaderPatcher* patcher = vita2d_get_shader_patcher();
  if (patcher != nullptr) {
    for (size_t i = 0; i < programs_.size(); ++i) {
      if (programs_[i].program != nullptr) {
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, programs_[i].program);
      }
      sceGxmShaderPatcherUnregisterProgram(patcher, programs_[i].id);
    }
    for (size_t i = 0; i < vertex_variants_.size(); ++i) {
      if (vertex_variants_[i].program != nullptr) {
        sceGxmShaderPatcherReleaseVertexProgram(patcher, vertex_variants_[i].program);
      }
      sceGxmShaderPatcherUnregisterProgram(patcher, vertex_variants_[i].id);
    }
  }
  programs_.clear();
  vertex_variants_.clear();
  ready_ = false;
}

int Renderer3D::program_for(uint64_t tev_key) const {
  for (size_t i = 0; i < programs_.size(); ++i) {
    if (programs_[i].key == tev_key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void Renderer3D::begin_scene() {
  SceGxmContext* context = vita2d_get_context();
  if (context == nullptr) {
    return;
  }

  sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_LESS);
  sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_LESS);
  sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
  sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);

  // GX treats clockwise triangles as front facing, the opposite of GXM, and the
  // engine's default is GX_CULL_BACK. Culling clockwise here removed the front
  // faces instead, which left the scene empty from outside and showed only
  // backfaces from within it.
  sceGxmSetCullMode(context, SCE_GXM_CULL_CCW);
}

void Renderer3D::end_scene() {
  SceGxmContext* context = vita2d_get_context();
  if (context == nullptr) {
    return;
  }

  // vita2d draws the menu on top afterwards, so hand the depth state back.
  sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
  sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
  sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
  sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
  sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
}

void Renderer3D::draw(int program_index, const GpuBuffer& vertices, const GpuBuffer& indices,
                      uint32_t first_index, uint32_t index_count,
                      const MaterialUniforms& uniforms, vita2d_texture* const* textures,
                      int texture_count) {
  if (!ready_ || index_count == 0 || vertices.data() == nullptr || indices.data() == nullptr) {
    return;
  }
  if (program_index < 0 || program_index >= static_cast<int>(programs_.size())) {
    return;
  }

  SceGxmContext* context = vita2d_get_context();
  if (context == nullptr) {
    return;
  }

  const Program& program = programs_[static_cast<size_t>(program_index)];
  const VertexVariant& variant = vertex_variants_[static_cast<size_t>(program.vertex_variant)];

  sceGxmSetVertexProgram(context, variant.program);
  sceGxmSetFragmentProgram(context, program.program);

  void* buffer = nullptr;
  if (sceGxmReserveVertexDefaultUniformBuffer(context, &buffer) < 0 || buffer == nullptr) {
    return;
  }
  set_matrix(buffer, variant.view_proj, uniforms.view_proj);
  set_matrix(buffer, variant.world, uniforms.world);
  for (int i = 0; i < kMaxMaterialTextures; ++i) {
    set_matrix(buffer, variant.tex_matrix[i], uniforms.tex_matrix[i]);
  }
  for (int i = 0; i < 3; ++i) {
    set_vec4(buffer, variant.light_dir[i], uniforms.lights[i].direction);
    set_vec4(buffer, variant.light_color[i], uniforms.lights[i].color);
  }
  set_vec4(buffer, variant.ambient, uniforms.ambient);
  set_vec4(buffer, variant.material_color, uniforms.material_color);

  for (int i = 0; i < texture_count && i < kMaxMaterialTextures; ++i) {
    if (textures[i] != nullptr) {
      sceGxmSetFragmentTexture(context, static_cast<unsigned>(i), &textures[i]->gxm_tex);
    }
  }

  sceGxmSetVertexStream(context, 0, vertices.data());
  const uint32_t* index_base = static_cast<const uint32_t*>(indices.data()) + first_index;
  sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U32, index_base,
             index_count);
}

}  // namespace render
