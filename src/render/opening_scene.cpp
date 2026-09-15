#include "render/opening_scene.hpp"

#include <vita2d.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "cine/cine.h"
#include "gcn_texture.hpp"
#include "render/gxm3d.hpp"
#include "render/matrix.hpp"
#include "render/skeleton.hpp"

namespace render {
namespace {

const int kScreenWidth = 960;
const int kScreenHeight = 544;
const float kPi = 3.14159265358979323846f;

float camera_field(const cine::Dsk& dsk, const char* name, float frame, float fallback) {
  if (dsk.cameras.empty()) {
    return fallback;
  }
  const cine::DskTable& camera = dsk.cameras[0];
  for (size_t i = 0; i < camera.fields.size(); ++i) {
    if (camera.fields[i].name == name) {
      return cine::evaluate(camera.fields[i].param, dsk.camera_values, frame);
    }
  }
  return fallback;
}

vita2d_texture* create_white_texture() {
  vita2d_texture* texture = vita2d_create_empty_texture(1, 1);
  if (texture != nullptr) {
    *static_cast<uint32_t*>(vita2d_texture_get_datap(texture)) = RGBA8(255, 255, 255, 255);
  }
  return texture;
}

vita2d_texture* upload_texture(const gcn::Image& image) {
  vita2d_texture* texture = vita2d_create_empty_texture_format(
      static_cast<unsigned>(image.width), static_cast<unsigned>(image.height),
      SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
  if (texture == nullptr) {
    return nullptr;
  }
  uint8_t* destination = static_cast<uint8_t*>(vita2d_texture_get_datap(texture));
  const unsigned stride = vita2d_texture_get_stride(texture);
  const size_t row_bytes = static_cast<size_t>(image.width) * 4;
  for (int y = 0; y < image.height; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * stride,
                image.rgba.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
  }
  return texture;
}

std::vector<VertexGcn> convert_vertices(const cine::Geometry& geometry) {
  std::vector<VertexGcn> vertices(geometry.vertices.size());
  for (size_t i = 0; i < vertices.size(); ++i) {
    const cine::GeoVertex& source = geometry.vertices[i];
    VertexGcn& destination = vertices[i];
    for (int axis = 0; axis < 3; ++axis) {
      destination.position[axis] = source.position[axis];
      destination.normal[axis] = source.normal[axis];
    }
    for (int tex = 0; tex < 2; ++tex) {
      destination.texcoord0[tex] = source.texcoord[0][tex];
      destination.texcoord1[tex] = source.texcoord[1][tex];
    }
    for (int channel = 0; channel < 4; ++channel) {
      destination.color[channel] = source.color[channel];
    }
  }
  return vertices;
}

bool upload_model_textures(const cine::ModInventory& model,
                           std::vector<vita2d_texture*>& textures, std::string& error) {
  textures.resize(model.textures.size(), nullptr);
  for (size_t i = 0; i < model.textures.size(); ++i) {
    const cine::ModTexture& source = model.textures[i];
    gcn::Image image;
    if (!gcn::decode_texture_payload(static_cast<gcn::TexFormat>(source.format), source.width,
                                     source.height, source.data.data(), source.data.size(), image,
                                     error)) {
      return false;
    }
    textures[i] = upload_texture(image);
    if (textures[i] == nullptr) {
      error = "model texture upload failed";
      return false;
    }
  }
  return true;
}

}  // namespace

class OpeningScene::Impl {
 public:
  ~Impl() {
    for (size_t i = 0; i < textures.size(); ++i) {
      if (textures[i] != nullptr) {
        vita2d_free_texture(textures[i]);
      }
    }
    for (size_t i = 0; i < logo_textures.size(); ++i) {
      if (logo_textures[i] != nullptr) {
        vita2d_free_texture(logo_textures[i]);
      }
    }
    if (white != nullptr) {
      vita2d_free_texture(white);
    }
    if (renderer_initialized) {
      renderer.shutdown();
    }
  }

  Renderer3D renderer;
  bool renderer_initialized = false;
  GpuBuffer vertices;
  GpuBuffer indices;
  cine::ModInventory model;
  cine::Geometry geometry;
  cine::Dsk camera;
  cine::Dck animation;
  std::vector<vita2d_texture*> textures;
  GpuBuffer logo_vertices;
  GpuBuffer logo_indices;
  cine::ModInventory logo_model;
  cine::Geometry logo_geometry;
  cine::Dck logo_animation;
  std::vector<vita2d_texture*> logo_textures;
  bool logo_ready = false;
  vita2d_texture* white = nullptr;
};

// A TEV stage names a texture map, which selects one of the material's own
// texture entries. That entry points at a texture attribute, which finally
// names an image in the model. Resolving the whole chain is what makes each
// stage sample the layer the material intended.
vita2d_texture* material_texture(const cine::ModInventory& model,
                                 const std::vector<vita2d_texture*>& textures,
                                 const cine::Material& material, size_t texmap,
                                 vita2d_texture* fallback) {
  if (texmap >= material.textures.size()) {
    return fallback;
  }
  const uint32_t attribute_index = material.textures[texmap].source_attribute;
  if (attribute_index >= model.texture_attributes.size()) {
    return fallback;
  }
  const int texture_index =
      model.texture_attributes[static_cast<size_t>(attribute_index)].texture_index;
  if (texture_index < 0 || static_cast<size_t>(texture_index) >= textures.size()) {
    return fallback;
  }
  return textures[static_cast<size_t>(texture_index)];
}

Mat4 texgen_matrix(const cine::Material& material, size_t index, float frame) {
  if (index >= material.textures.size()) {
    return identity();
  }
  float rows[16];
  cine::texture_matrix(material.textures[index], frame, rows);
  // cine writes the matrix the way GX loads it, row-major; Mat4 is column-major.
  Mat4 result;
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      result.m[col * 4 + row] = rows[row * 4 + col];
    }
  }
  return result;
}

void draw_model(Renderer3D& renderer, const GpuBuffer& vertices, const GpuBuffer& indices,
                const cine::ModInventory& model, const cine::Geometry& geometry,
                const std::vector<Mat4>& joints, const Mat4& view_projection,
                const std::vector<vita2d_texture*>& textures, vita2d_texture* white,
                float frame) {
  for (size_t i = 0; i < geometry.batches.size(); ++i) {
    const cine::GeoBatch& batch = geometry.batches[i];
    if (batch.material_index < 0 ||
        static_cast<size_t>(batch.material_index) >= model.materials.size()) {
      continue;
    }
    const cine::Material& material =
        model.materials[static_cast<size_t>(batch.material_index)];
    if (material.tev_info_index < 0 ||
        static_cast<size_t>(material.tev_info_index) >= model.tev_infos.size()) {
      continue;
    }
    const cine::TevInfo& tev = model.tev_infos[static_cast<size_t>(material.tev_info_index)];

    // Skip rather than fall back to a stand-in shader: drawing a material with
    // the wrong combiner chain looks plausible and hides the gap.
    const int program = renderer.program_for(cine::tev_key(tev));
    if (program < 0) {
      continue;
    }

    MaterialUniforms uniforms = default_material_uniforms();
    uniforms.view_proj = view_projection;
    if (batch.parent_joint >= 0 && static_cast<size_t>(batch.parent_joint) < joints.size()) {
      uniforms.world = joints[static_cast<size_t>(batch.parent_joint)];
    }
    for (int channel = 0; channel < 4; ++channel) {
      uniforms.material_color[channel] = static_cast<float>(material.color[channel]) / 255.0f;
    }
    for (int texgen = 0; texgen < kMaxMaterialTextures; ++texgen) {
      uniforms.tex_matrix[texgen] =
          texgen_matrix(material, static_cast<size_t>(texgen), frame);
    }

    // Full ambient and no directional lights, so the raster colour reduces to
    // the material colour times the vertex colour.
    //
    // Materials with PVWLightingInfo::EnableColor0 clear are unlit on hardware
    // and reduce to exactly this. The lit ones need DayMgr's light set, which is
    // not ported; fabricating directional lights tinted the whole scene instead
    // of approximating it, so they stay off until the real lights are read.
    uniforms.ambient[0] = uniforms.ambient[1] = uniforms.ambient[2] = 1.0f;
    uniforms.ambient[3] = 1.0f;
    for (int light = 0; light < 3; ++light) {
      for (int channel = 0; channel < 4; ++channel) {
        uniforms.lights[light].color[channel] = 0.0f;
      }
    }

    const int texture_count = cine::tev_max_texmap(tev) + 1;
    vita2d_texture* bound[kMaxMaterialTextures] = {nullptr, nullptr, nullptr, nullptr};
    for (int texmap = 0; texmap < texture_count && texmap < kMaxMaterialTextures; ++texmap) {
      bound[texmap] =
          material_texture(model, textures, material, static_cast<size_t>(texmap), white);
    }

    renderer.draw(program, vertices, indices, batch.first_index, batch.index_count, uniforms,
                  bound, texture_count);
  }
}

bool OpeningScene::init(const char* data_root) {
  if (impl_ != nullptr) {
    return true;
  }

  Impl* next = new Impl();
  try {
    const std::string base = std::string(data_root) + "cinemas/opening/";
    const std::vector<uint8_t> bytes = cine::read_file(base + "opening.mod");
    next->model = cine::parse_mod(bytes, "opening.mod");
    next->geometry = cine::build_geometry(bytes, next->model, "opening.mod");
    next->camera = cine::parse_dsk(cine::read_text_file(base + "opening.dsk"), "opening.dsk");
    next->animation = load_dck(base + "opening.anm", "opening.anm");

    const std::vector<VertexGcn> vertices = convert_vertices(next->geometry);

    if (!next->renderer.init()) {
      error_ = next->renderer.error();
      delete next;
      return false;
    }
    next->renderer_initialized = true;
    if (!next->vertices.upload(vertices.data(), vertices.size() * sizeof(VertexGcn)) ||
        !next->indices.upload(next->geometry.indices.data(),
                              next->geometry.indices.size() * sizeof(uint32_t))) {
      error_ = "opening geometry upload failed";
      delete next;
      return false;
    }
    next->white = create_white_texture();
    if (next->white == nullptr) {
      error_ = "opening white texture allocation failed";
      delete next;
      return false;
    }
    if (!upload_model_textures(next->model, next->textures, error_)) {
      delete next;
      return false;
    }

    const std::string logo_base = std::string(data_root) + "cinemas/titles/";
    const std::vector<uint8_t> logo_bytes = cine::read_file(logo_base + "logo.mod");
    next->logo_model = cine::parse_mod(logo_bytes, "logo.mod");
    next->logo_geometry = cine::build_geometry(logo_bytes, next->logo_model, "logo.mod");
    next->logo_animation = load_dck(logo_base + "logo.anm", "logo.anm");
    const std::vector<VertexGcn> logo_vertices = convert_vertices(next->logo_geometry);
    if (!next->logo_vertices.upload(logo_vertices.data(),
                                    logo_vertices.size() * sizeof(VertexGcn)) ||
        !next->logo_indices.upload(next->logo_geometry.indices.data(),
                                   next->logo_geometry.indices.size() * sizeof(uint32_t))) {
      error_ = "logo geometry upload failed";
      delete next;
      return false;
    }
    if (!upload_model_textures(next->logo_model, next->logo_textures, error_)) {
      delete next;
      return false;
    }
    next->logo_ready = true;
  } catch (const std::exception& exception) {
    error_ = exception.what();
    delete next;
    return false;
  }

  impl_ = next;
  error_.clear();
  return true;
}

void OpeningScene::shutdown() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->vertices.release();
  impl_->indices.release();
  delete impl_;
  impl_ = nullptr;
}

void OpeningScene::draw(float frame) {
  if (impl_ == nullptr) {
    return;
  }

  const float eye[3] = {
      camera_field(impl_->camera, "cam_pos_x", frame, 0.0f),
      camera_field(impl_->camera, "cam_pos_y", frame, 6390.0f),
      camera_field(impl_->camera, "cam_pos_z", frame, 20.0f),
  };
  const float target[3] = {
      camera_field(impl_->camera, "cam_lat_x", frame, 0.0f),
      camera_field(impl_->camera, "cam_lat_y", frame, 0.0f),
      camera_field(impl_->camera, "cam_lat_z", frame, 0.0f),
  };
  const float up[3] = {0.0f, 1.0f, 0.0f};
  // cam_fovy is the full vertical angle in degrees. CamDataInfo never samples
  // the file's cam_near and cam_far, keeping its constructor's planes instead,
  // and the .dsk asks for 0.1 to 32768, a ratio that wrecks depth precision.
  const float fovy = camera_field(impl_->camera, "cam_fovy", frame, 41.5f) * kPi / 180.0f;
  const Mat4 fit = pillarbox_ndc(kScreenWidth, kScreenHeight, 4, 3);
  const Mat4 projection = perspective(fovy, 4.0f / 3.0f, 1.0f, 15000.0f);
  const Mat4 view_projection = multiply(fit, multiply(projection, look_at(eye, target, up)));

  // Every batch in both actors binds to a single joint with no envelope
  // weights, so the joint's animated world matrix is used directly. Inverse
  // bind matrices only apply to envelope-skinned vertices, which these models
  // do not have; multiplying by one here moved the geometry off its joint.
  impl_->renderer.begin_scene();
  const std::vector<Mat4> joints = animated_joint_world_matrices(
      impl_->model, impl_->animation, clamp_frame(frame, impl_->animation));
  draw_model(impl_->renderer, impl_->vertices, impl_->indices, impl_->model, impl_->geometry,
             joints, view_projection, impl_->textures, impl_->white, frame);
  if (impl_->logo_ready) {
    const std::vector<Mat4> logo_joints = animated_joint_world_matrices(
        impl_->logo_model, impl_->logo_animation, clamp_frame(frame, impl_->logo_animation));
    draw_model(impl_->renderer, impl_->logo_vertices, impl_->logo_indices, impl_->logo_model,
               impl_->logo_geometry, logo_joints, view_projection, impl_->logo_textures,
               impl_->white, frame);
  }
  impl_->renderer.end_scene();
}

}  // namespace render
