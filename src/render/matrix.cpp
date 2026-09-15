#include "render/matrix.hpp"

#include <cmath>

namespace render {
namespace {

float length3(const float v[3]) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void normalize3(float v[3]) {
  const float len = length3(v);
  if (len <= 0.0f) {
    return;
  }
  v[0] /= len;
  v[1] /= len;
  v[2] /= len;
}

void cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot3(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

}  // namespace

Mat4 identity() {
  Mat4 r = {};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 r = {};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[k * 4 + row] * b.m[col * 4 + k];
      }
      r.m[col * 4 + row] = sum;
    }
  }
  return r;
}

Mat4 perspective(float fovy_radians, float aspect, float near_plane, float far_plane) {
  Mat4 r = {};
  const float f = 1.0f / std::tan(fovy_radians * 0.5f);
  r.m[0] = f / aspect;
  r.m[5] = f;
  r.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
  r.m[11] = -1.0f;
  r.m[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
  return r;
}

Mat4 orthographic(float left, float right, float bottom, float top, float near_plane,
                  float far_plane) {
  Mat4 r = identity();
  r.m[0] = 2.0f / (right - left);
  r.m[5] = 2.0f / (top - bottom);
  r.m[10] = -2.0f / (far_plane - near_plane);
  r.m[12] = -(right + left) / (right - left);
  r.m[13] = -(top + bottom) / (top - bottom);
  r.m[14] = -(far_plane + near_plane) / (far_plane - near_plane);
  return r;
}

Mat4 look_at(const float eye[3], const float target[3], const float up[3]) {
  float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  normalize3(forward);

  float side[3];
  cross3(forward, up, side);
  normalize3(side);

  float true_up[3];
  cross3(side, forward, true_up);

  Mat4 r = identity();
  r.m[0] = side[0];
  r.m[4] = side[1];
  r.m[8] = side[2];
  r.m[1] = true_up[0];
  r.m[5] = true_up[1];
  r.m[9] = true_up[2];
  r.m[2] = -forward[0];
  r.m[6] = -forward[1];
  r.m[10] = -forward[2];
  r.m[12] = -dot3(side, eye);
  r.m[13] = -dot3(true_up, eye);
  r.m[14] = dot3(forward, eye);
  return r;
}

Mat4 translation(float x, float y, float z) {
  Mat4 r = identity();
  r.m[12] = x;
  r.m[13] = y;
  r.m[14] = z;
  return r;
}

Mat4 scaling(float x, float y, float z) {
  Mat4 r = identity();
  r.m[0] = x;
  r.m[5] = y;
  r.m[10] = z;
  return r;
}

Mat4 rotation_x(float radians) {
  Mat4 r = identity();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  r.m[5] = c;
  r.m[9] = -s;
  r.m[6] = s;
  r.m[10] = c;
  return r;
}

Mat4 rotation_y(float radians) {
  Mat4 r = identity();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  r.m[0] = c;
  r.m[8] = s;
  r.m[2] = -s;
  r.m[10] = c;
  return r;
}

Mat4 rotation_z(float radians) {
  Mat4 r = identity();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  r.m[0] = c;
  r.m[4] = -s;
  r.m[1] = s;
  r.m[5] = c;
  return r;
}

void transform_point(const Mat4& m, const float in[3], float out[4]) {
  for (int row = 0; row < 4; ++row) {
    out[row] = m.m[0 * 4 + row] * in[0] + m.m[1 * 4 + row] * in[1] + m.m[2 * 4 + row] * in[2] +
               m.m[3 * 4 + row];
  }
}

Mat4 pillarbox_ndc(int screen_width, int screen_height, int image_width, int image_height) {
  Mat4 result = identity();
  if (screen_width <= 0 || screen_height <= 0 || image_width <= 0 || image_height <= 0) {
    return result;
  }

  const float screen_aspect = static_cast<float>(screen_width) / static_cast<float>(screen_height);
  const float image_aspect = static_cast<float>(image_width) / static_cast<float>(image_height);

  if (image_aspect < screen_aspect) {
    result.m[0] = image_aspect / screen_aspect;  // narrower: bars left and right
  } else {
    result.m[5] = screen_aspect / image_aspect;  // wider: bars top and bottom
  }
  return result;
}

}  // namespace render
