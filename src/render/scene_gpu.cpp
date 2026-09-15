#include "render/scene_gpu.hpp"

#include <psp2/gxm.h>

#include <cstdio>
#include <cstring>
#include <exception>

#include "gcn_texture.hpp"
#include "render/skeleton.hpp"
#include "scene_raster.hpp"

// vita2d uploads this as the world-view-projection uniform on every draw call,
// so replacing it per batch hands the GPU a real 3D transform while still going
// through vita2d's own shaders. Those shaders come from Sony's compiler and
// work; the ones tools/tev_shader_gen feeds through psp2spvc do not.
extern "C" float _vita2d_ortho_matrix[16];

namespace render {
namespace {

const int kScreenWidth = 960;
const int kScreenHeight = 544;

// vita2d_draw_array builds its index list as 16-bit, so a single call cannot
// exceed 65536 vertices. The terrain batches are larger than that, so they are
// split. Kept a multiple of three to avoid splitting a triangle.
const size_t kMaxVerticesPerCall = 65535 / 3 * 3;

unsigned int pack_unit_color(const float color[4]) {
  unsigned int channel[4];
  for (int i = 0; i < 4; ++i) {
    float value = color[i] * 255.0f;
    if (value < 0.0f) value = 0.0f;
    if (value > 255.0f) value = 255.0f;
    channel[i] = static_cast<unsigned int>(value);
  }
  return RGBA8(channel[0], channel[1], channel[2], channel[3]);
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
  vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
                             SCE_GXM_TEXTURE_FILTER_LINEAR);
  // GX wraps by default for every material in these two models, and vita2d
  // leaves a new texture clamped.
  sceGxmTextureSetUAddrMode(&texture->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
  sceGxmTextureSetVAddrMode(&texture->gxm_tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
  return texture;
}

vita2d_texture* material_texture(const cine::ModInventory& model,
                                 const std::vector<vita2d_texture*>& textures,
                                 const cine::Material& material, size_t texmap) {
  if (texmap >= material.textures.size()) {
    return nullptr;
  }
  const uint32_t attribute = material.textures[texmap].source_attribute;
  if (attribute >= model.texture_attributes.size()) {
    return nullptr;
  }
  const int index = model.texture_attributes[attribute].texture_index;
  if (index < 0 || static_cast<size_t>(index) >= textures.size()) {
    return nullptr;
  }
  return textures[static_cast<size_t>(index)];
}

// GXM clips depth to 0..w, while render::perspective follows OpenGL and spans
// -w..w. Without this the near half of every scene is clipped away.
Mat4 depth_zero_to_one() {
  Mat4 result = identity();
  result.m[10] = 0.5f;
  result.m[14] = 0.5f;
  return result;
}

}  // namespace

bool SceneGpu::Actor::load(const std::string& mod_path, const std::string& anm_path,
                           const char* label, std::string& error) {
  const std::vector<uint8_t> bytes = cine::read_file(mod_path);
  model = cine::parse_mod(bytes, mod_path);
  geometry = cine::build_geometry(bytes, model, mod_path);
  animation = load_dck(anm_path, label);

  textures.assign(model.textures.size(), nullptr);
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

  // Expand every batch into a triangle list once, in object space. The GPU
  // transforms it each frame, so nothing here is rebuilt per frame.
  for (size_t b = 0; b < geometry.batches.size(); ++b) {
    const cine::GeoBatch& source_batch = geometry.batches[b];
    if (source_batch.material_index < 0 ||
        static_cast<size_t>(source_batch.material_index) >= model.materials.size()) {
      continue;
    }
    const cine::Material& material =
        model.materials[static_cast<size_t>(source_batch.material_index)];

    // The raster colour is the vertex colour times the material colour, as a
    // GameCube vertex stage computes it. Most of logo.mod carries no texture --
    // its only texture is the copyright strip -- so the flowers' white petals
    // and yellow centres are this material colour alone.
    float material_color[4];
    for (int channel = 0; channel < 4; ++channel) {
      material_color[channel] = static_cast<float>(material.color[channel]) / 255.0f;
    }

    Batch batch;
    batch.texture = material_texture(model, textures, material, 0);
    batch.tint = pack_unit_color(material_color);
    batch.parent_joint = source_batch.parent_joint;

    for (uint32_t i = 0; i + 2 < source_batch.index_count; i += 3) {
      for (int k = 0; k < 3; ++k) {
        const uint32_t index = geometry.indices[source_batch.first_index + i + k];
        if (index >= geometry.vertices.size()) {
          continue;
        }
        const cine::GeoVertex& vertex = geometry.vertices[index];
        if (batch.texture != nullptr) {
          vita2d_texture_vertex out;
          out.x = vertex.position[0];
          out.y = vertex.position[1];
          out.z = vertex.position[2];
          out.u = vertex.texcoord[0][0];
          out.v = vertex.texcoord[0][1];
          batch.textured.push_back(out);
        } else {
          float color[4];
          for (int channel = 0; channel < 4; ++channel) {
            color[channel] =
                static_cast<float>(vertex.color[channel]) / 255.0f * material_color[channel];
          }
          vita2d_color_vertex out;
          out.x = vertex.position[0];
          out.y = vertex.position[1];
          out.z = vertex.position[2];
          out.color = pack_unit_color(color);
          batch.colored.push_back(out);
        }
      }
    }

    if (!batch.textured.empty() || !batch.colored.empty()) {
      batches.push_back(std::move(batch));
    }
  }
  return true;
}

void SceneGpu::Actor::release() {
  for (size_t i = 0; i < textures.size(); ++i) {
    if (textures[i] != nullptr) {
      vita2d_free_texture(textures[i]);
    }
  }
  textures.clear();
  batches.clear();
}

bool SceneGpu::init(const char* data_root) {
  if (ready_) {
    return true;
  }
  try {
    const std::string root = data_root;
    const std::string opening_base = root + "cinemas/opening/";
    const std::string logo_base = root + "cinemas/titles/";

    dsk_ = cine::parse_dsk(cine::read_text_file(opening_base + "opening.dsk"), "opening.dsk");
    if (!opening_.load(opening_base + "opening.mod", opening_base + "opening.anm", "opening",
                       error_)) {
      return false;
    }
    if (!logo_.load(logo_base + "logo.mod", logo_base + "logo.anm", "logo", error_)) {
      return false;
    }
  } catch (const std::exception& exception) {
    error_ = exception.what();
    return false;
  }

  std::memcpy(saved_ortho_, _vita2d_ortho_matrix, sizeof(saved_ortho_));
  ready_ = true;
  error_.clear();
  return true;
}

void SceneGpu::shutdown() {
  opening_.release();
  logo_.release();
  ready_ = false;
}

void SceneGpu::draw_actor(Actor& actor, const Mat4& view_projection, float frame) {
  const std::vector<Mat4> joints = animated_joint_world_matrices(
      actor.model, actor.animation, clamp_frame(frame, actor.animation));

  for (size_t b = 0; b < actor.batches.size(); ++b) {
    Batch& batch = actor.batches[b];

    Mat4 world = identity();
    if (batch.parent_joint >= 0 && static_cast<size_t>(batch.parent_joint) < joints.size()) {
      world = joints[static_cast<size_t>(batch.parent_joint)];
    }

    // Hand this batch's full transform to vita2d's vertex stage.
    const Mat4 mvp = multiply(view_projection, world);
    std::memcpy(_vita2d_ortho_matrix, mvp.m, sizeof(_vita2d_ortho_matrix));

    if (batch.texture != nullptr) {
      for (size_t start = 0; start < batch.textured.size(); start += kMaxVerticesPerCall) {
        size_t count = batch.textured.size() - start;
        if (count > kMaxVerticesPerCall) {
          count = kMaxVerticesPerCall;
        }
        vita2d_draw_array_textured(batch.texture, SCE_GXM_PRIMITIVE_TRIANGLES,
                                   batch.textured.data() + start, count, batch.tint);
        ++draw_calls_;
      }
    } else {
      for (size_t start = 0; start < batch.colored.size(); start += kMaxVerticesPerCall) {
        size_t count = batch.colored.size() - start;
        if (count > kMaxVerticesPerCall) {
          count = kMaxVerticesPerCall;
        }
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, batch.colored.data() + start, count);
        ++draw_calls_;
      }
    }
  }
}

void SceneGpu::draw(float cinematic_frame) {
  if (!ready_) {
    return;
  }

  SceGxmContext* context = vita2d_get_context();
  if (context != nullptr && depth_test_) {
    // vita2d draws 2D in submission order with depth off. The scene needs a
    // real depth test, and the menu drawn afterwards needs it back off.
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_LESS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_LESS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_ENABLED);
  }

  draw_calls_ = 0;
  const Mat4 view_projection =
      multiply(depth_zero_to_one(),
               raster::title_view_projection(dsk_, cinematic_frame, kScreenWidth, kScreenHeight));
  draw_actor(opening_, view_projection, cinematic_frame);
  draw_actor(logo_, view_projection, cinematic_frame);

  // Put back the screen-space projection the 2D menu and text are drawn with.
  std::memcpy(_vita2d_ortho_matrix, saved_ortho_, sizeof(saved_ortho_));

  if (context != nullptr && depth_test_) {
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
  }

  char line[128];
  std::snprintf(line, sizeof(line), "draws=%ld frame=%.0f", draw_calls_, cinematic_frame);
  stats_ = line;
}

}  // namespace render
