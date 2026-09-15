#pragma once

namespace render {

// Column-major 4x4, translation in m[12..14], matching the layout vita2d's
// shaders expect from sceGxmSetUniformDataF.
struct Mat4 {
  float m[16];
};

Mat4 identity();
Mat4 multiply(const Mat4& a, const Mat4& b);

// GameCube projections are a right-handed frustum with NDC depth in [-1, 1],
// which is also what GXM's default viewport maps.
Mat4 perspective(float fovy_radians, float aspect, float near_plane, float far_plane);
Mat4 orthographic(float left, float right, float bottom, float top, float near_plane,
                  float far_plane);
Mat4 look_at(const float eye[3], const float target[3], const float up[3]);

Mat4 translation(float x, float y, float z);
Mat4 scaling(float x, float y, float z);
Mat4 rotation_x(float radians);
Mat4 rotation_y(float radians);
Mat4 rotation_z(float radians);

void transform_point(const Mat4& m, const float in[3], float out[4]);

// Fits a 4:3 GameCube image into the Vita's wider panel without distortion,
// applied in normalised device coordinates after projection.
Mat4 pillarbox_ndc(int screen_width, int screen_height, int image_width, int image_height);

}  // namespace render
