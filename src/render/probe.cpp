#include "render/probe.hpp"

#include <cstdint>
#include <vector>

#include "render/gxm3d.hpp"
#include "render/matrix.hpp"

namespace render {
namespace {

const int kScreenWidth = 960;
const int kScreenHeight = 544;

void push_face(std::vector<VertexGcn>& vertices, std::vector<uint32_t>& indices,
               const float origin[3], const float right[3], const float up[3],
               const float normal[3]) {
  const uint32_t base = static_cast<uint32_t>(vertices.size());
  const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

  for (int i = 0; i < 4; ++i) {
    VertexGcn vertex = {};
    for (int axis = 0; axis < 3; ++axis) {
      vertex.position[axis] =
          origin[axis] + right[axis] * corners[i][0] + up[axis] * corners[i][1];
      vertex.normal[axis] = normal[axis];
    }
    vertex.texcoord0[0] = corners[i][0];
    vertex.texcoord0[1] = corners[i][1];
    vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 0xff;
    vertices.push_back(vertex);
  }

  const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) {
    indices.push_back(base + order[i]);
  }
}

}  // namespace

class Probe::Impl {
 public:
  Renderer3D renderer;
  GpuBuffer vertices;
  GpuBuffer indices;
  uint32_t index_count = 0;
  vita2d_texture* texture = nullptr;
};

bool Probe::init(vita2d_texture* texture) {
  if (impl_ != nullptr) {
    return true;
  }

  Impl* impl = new Impl();
  if (!impl->renderer.init()) {
    delete impl;
    return false;
  }

  std::vector<VertexGcn> vertices;
  std::vector<uint32_t> indices;

  // Unit cube centred on the origin, one quad per face so each gets full UVs.
  // Each row is an origin corner, the two edge vectors, then the face normal.
  const float faces[6][12] = {
      {-1, -1, 1, 2, 0, 0, 0, 2, 0, 0, 0, 1},
      {1, -1, -1, -2, 0, 0, 0, 2, 0, 0, 0, -1},
      {-1, -1, -1, 0, 0, 2, 0, 2, 0, -1, 0, 0},
      {1, -1, 1, 0, 0, -2, 0, 2, 0, 1, 0, 0},
      {-1, 1, 1, 2, 0, 0, 0, 0, -2, 0, 1, 0},
      {-1, -1, -1, 2, 0, 0, 0, 0, 2, 0, -1, 0},
  };
  for (int i = 0; i < 6; ++i) {
    push_face(vertices, indices, &faces[i][0], &faces[i][3], &faces[i][6], &faces[i][9]);
  }

  if (!impl->vertices.upload(vertices.data(), vertices.size() * sizeof(VertexGcn)) ||
      !impl->indices.upload(indices.data(), indices.size() * sizeof(uint32_t))) {
    impl->renderer.shutdown();
    delete impl;
    return false;
  }

  impl->index_count = static_cast<uint32_t>(indices.size());
  impl->texture = texture;
  impl_ = impl;
  return true;
}

void Probe::shutdown() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->vertices.release();
  impl_->indices.release();
  impl_->renderer.shutdown();
  delete impl_;
  impl_ = nullptr;
}

void Probe::draw(float seconds) {
  if (impl_ == nullptr) {
    return;
  }

  MaterialUniforms uniforms = default_material_uniforms();
  uniforms.world = multiply(rotation_y(seconds), rotation_x(seconds * 0.7f));

  const float eye[3] = {0.0f, 0.0f, 6.0f};
  const float target[3] = {0.0f, 0.0f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};

  // GameCube output is 4:3, so keep that shape inside the Vita's panel.
  const Mat4 projection = perspective(1.0f, 4.0f / 3.0f, 0.1f, 100.0f);
  const Mat4 fit = pillarbox_ndc(kScreenWidth, kScreenHeight, 4, 3);
  uniforms.view_proj = multiply(fit, multiply(projection, look_at(eye, target, up)));

  // A single head-on light so the cube's faces read as distinct while the
  // lighting path is exercised.
  uniforms.ambient[0] = uniforms.ambient[1] = uniforms.ambient[2] = 0.25f;
  uniforms.lights[0].direction[0] = 0.0f;
  uniforms.lights[0].direction[1] = 0.0f;
  uniforms.lights[0].direction[2] = -1.0f;
  uniforms.lights[0].color[0] = uniforms.lights[0].color[1] = uniforms.lights[0].color[2] = 0.9f;

  impl_->renderer.begin_scene();
  impl_->renderer.draw(impl_->vertices, impl_->indices, 0, impl_->index_count, uniforms,
                       impl_->texture);
  impl_->renderer.end_scene();
}

}  // namespace render
