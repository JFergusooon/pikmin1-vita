#include "render/matrix.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

void check_near(float actual, float expected, const char* what, float tolerance = 1e-5f) {
  if (std::fabs(actual - expected) > tolerance) {
    std::printf("FAIL %s (expected %f, got %f)\n", what, expected, actual);
    ++failures;
  }
}

// vita2d's ortho_matrix, reproduced so the port's projection convention can be
// pinned against the one its shaders already render correctly with.
void vita2d_ortho(float* m, float left, float right, float bottom, float top, float near_plane,
                  float far_plane) {
  m[0x0] = 2.0f / (right - left);
  m[0x4] = 0.0f;
  m[0x8] = 0.0f;
  m[0xC] = -(right + left) / (right - left);

  m[0x1] = 0.0f;
  m[0x5] = 2.0f / (top - bottom);
  m[0x9] = 0.0f;
  m[0xD] = -(top + bottom) / (top - bottom);

  m[0x2] = 0.0f;
  m[0x6] = 0.0f;
  m[0xA] = -2.0f / (far_plane - near_plane);
  m[0xE] = -(far_plane + near_plane) / (far_plane - near_plane);

  m[0x3] = 0.0f;
  m[0x7] = 0.0f;
  m[0xB] = 0.0f;
  m[0xF] = 1.0f;
}

void test_identity() {
  const render::Mat4 i = render::identity();
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      check_near(i.m[col * 4 + row], col == row ? 1.0f : 0.0f, "identity element");
    }
  }

  const float p[3] = {3.0f, -4.0f, 5.0f};
  float out[4];
  render::transform_point(i, p, out);
  check_near(out[0], 3.0f, "identity transform x");
  check_near(out[1], -4.0f, "identity transform y");
  check_near(out[2], 5.0f, "identity transform z");
  check_near(out[3], 1.0f, "identity transform w");
}

void test_multiply_identity() {
  const render::Mat4 a =
      render::multiply(render::rotation_y(0.7f), render::translation(1.0f, 2.0f, 3.0f));
  const render::Mat4 b = render::multiply(a, render::identity());
  for (int i = 0; i < 16; ++i) {
    check_near(b.m[i], a.m[i], "multiply by identity");
  }
}

void test_translation_slots() {
  // The translation must live in m[12..14]; vita2d's shaders depend on it.
  const render::Mat4 t = render::translation(7.0f, 8.0f, 9.0f);
  check_near(t.m[12], 7.0f, "translation x slot");
  check_near(t.m[13], 8.0f, "translation y slot");
  check_near(t.m[14], 9.0f, "translation z slot");

  const float p[3] = {1.0f, 1.0f, 1.0f};
  float out[4];
  render::transform_point(t, p, out);
  check_near(out[0], 8.0f, "translated x");
  check_near(out[1], 9.0f, "translated y");
  check_near(out[2], 10.0f, "translated z");
}

void test_ortho_matches_vita2d() {
  float expected[16];
  vita2d_ortho(expected, 0.0f, 960.0f, 544.0f, 0.0f, -1.0f, 1.0f);
  const render::Mat4 actual = render::orthographic(0.0f, 960.0f, 544.0f, 0.0f, -1.0f, 1.0f);
  for (int i = 0; i < 16; ++i) {
    check_near(actual.m[i], expected[i], "ortho matches vita2d");
  }

  // Screen-space corners must land on the NDC corners.
  const float top_left[3] = {0.0f, 0.0f, 0.0f};
  const float bottom_right[3] = {960.0f, 544.0f, 0.0f};
  float out[4];
  render::transform_point(actual, top_left, out);
  check_near(out[0], -1.0f, "ortho top-left x");
  check_near(out[1], 1.0f, "ortho top-left y");
  render::transform_point(actual, bottom_right, out);
  check_near(out[0], 1.0f, "ortho bottom-right x");
  check_near(out[1], -1.0f, "ortho bottom-right y");
}

void test_perspective_depth_range() {
  const float near_plane = 1.0f;
  const float far_plane = 100.0f;
  const render::Mat4 p = render::perspective(1.0f, 4.0f / 3.0f, near_plane, far_plane);

  // A point on the near plane maps to NDC z -1, on the far plane to +1.
  const float on_near[3] = {0.0f, 0.0f, -near_plane};
  const float on_far[3] = {0.0f, 0.0f, -far_plane};
  float out[4];
  render::transform_point(p, on_near, out);
  check(out[3] > 0.0f, "near plane w positive");
  check_near(out[2] / out[3], -1.0f, "near plane maps to NDC -1");
  render::transform_point(p, on_far, out);
  check(out[3] > 0.0f, "far plane w positive");
  check_near(out[2] / out[3], 1.0f, "far plane maps to NDC +1", 1e-4f);

  // w must carry view-space depth so the perspective divide is correct.
  const float mid[3] = {0.0f, 0.0f, -10.0f};
  render::transform_point(p, mid, out);
  check_near(out[3], 10.0f, "w equals view depth");
}

void test_perspective_aspect() {
  const float aspect = 4.0f / 3.0f;
  const float fovy = 1.0f;
  const render::Mat4 p = render::perspective(fovy, aspect, 1.0f, 100.0f);

  // At view depth d the vertical half-extent is d*tan(fovy/2) and the
  // horizontal is that times the aspect; both must land on the edge.
  const float depth = 10.0f;
  const float half_h = depth * std::tan(fovy * 0.5f);
  const float half_w = half_h * aspect;

  const float top_edge[3] = {0.0f, half_h, -depth};
  const float right_edge[3] = {half_w, 0.0f, -depth};
  float out[4];
  render::transform_point(p, top_edge, out);
  check_near(out[1] / out[3], 1.0f, "fov vertical edge");
  render::transform_point(p, right_edge, out);
  check_near(out[0] / out[3], 1.0f, "aspect horizontal edge");
}

void test_look_at() {
  const float eye[3] = {0.0f, 0.0f, 10.0f};
  const float target[3] = {0.0f, 0.0f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const render::Mat4 v = render::look_at(eye, target, up);

  // The target lands straight ahead at the eye's distance.
  float out[4];
  render::transform_point(v, target, out);
  check_near(out[0], 0.0f, "look_at target x");
  check_near(out[1], 0.0f, "look_at target y");
  check_near(out[2], -10.0f, "look_at target z");

  // The eye itself maps to the view-space origin.
  render::transform_point(v, eye, out);
  check_near(out[0], 0.0f, "look_at eye x");
  check_near(out[1], 0.0f, "look_at eye y");
  check_near(out[2], 0.0f, "look_at eye z");

  // A point at world +x stays to the right when looking down -z.
  const float right[3] = {2.0f, 0.0f, 0.0f};
  render::transform_point(v, right, out);
  check_near(out[0], 2.0f, "look_at preserves right");
}

void test_rotation_handedness() {
  float out[4];
  const float x_axis[3] = {1.0f, 0.0f, 0.0f};

  // A +90 degree rotation about Y sends +x to -z in a right-handed frame.
  const render::Mat4 ry = render::rotation_y(static_cast<float>(M_PI) * 0.5f);
  render::transform_point(ry, x_axis, out);
  check_near(out[0], 0.0f, "rot_y x");
  check_near(out[2], -1.0f, "rot_y sends +x to -z");

  // A +90 degree rotation about Z sends +x to +y.
  const render::Mat4 rz = render::rotation_z(static_cast<float>(M_PI) * 0.5f);
  render::transform_point(rz, x_axis, out);
  check_near(out[0], 0.0f, "rot_z x");
  check_near(out[1], 1.0f, "rot_z sends +x to +y");

  // A +90 degree rotation about X sends +y to +z.
  const render::Mat4 rx = render::rotation_x(static_cast<float>(M_PI) * 0.5f);
  const float y_axis[3] = {0.0f, 1.0f, 0.0f};
  render::transform_point(rx, y_axis, out);
  check_near(out[1], 0.0f, "rot_x y");
  check_near(out[2], 1.0f, "rot_x sends +y to +z");
}

void test_compose_order() {
  // multiply(a, b) applies b first, then a.
  const render::Mat4 t = render::translation(10.0f, 0.0f, 0.0f);
  const render::Mat4 s = render::scaling(2.0f, 2.0f, 2.0f);

  const float p[3] = {1.0f, 0.0f, 0.0f};
  float out[4];

  render::transform_point(render::multiply(t, s), p, out);
  check_near(out[0], 12.0f, "translate after scale");

  render::transform_point(render::multiply(s, t), p, out);
  check_near(out[0], 22.0f, "scale after translate");
}

void test_pillarbox() {
  // A 4:3 image on the Vita's 960x544 panel keeps full height, loses width.
  const render::Mat4 p = render::pillarbox_ndc(960, 544, 4, 3);
  const float screen_aspect = 960.0f / 544.0f;
  check_near(p.m[0], (4.0f / 3.0f) / screen_aspect, "pillarbox x scale");
  check_near(p.m[5], 1.0f, "pillarbox keeps height");

  // The scaled width in pixels matches fitting 4:3 into 544 of height.
  check_near(p.m[0] * 960.0f, 544.0f * (4.0f / 3.0f), "pillarbox fitted width", 1e-2f);

  // A source wider than the panel letterboxes instead.
  const render::Mat4 l = render::pillarbox_ndc(960, 544, 21, 9);
  check_near(l.m[0], 1.0f, "letterbox keeps width");
  check(l.m[5] < 1.0f, "letterbox reduces height");

  // Matching aspects are an exact no-op.
  const render::Mat4 exact = render::pillarbox_ndc(960, 540, 16, 9);
  check_near(exact.m[0], 1.0f, "matching aspect x");
  check_near(exact.m[5], 1.0f, "matching aspect y");

  // Degenerate inputs must not divide by zero.
  const render::Mat4 degenerate = render::pillarbox_ndc(0, 0, 0, 0);
  for (int i = 0; i < 16; ++i) {
    check_near(degenerate.m[i], render::identity().m[i], "degenerate pillarbox is identity");
  }
}

}  // namespace

int main() {
  test_identity();
  test_multiply_identity();
  test_translation_slots();
  test_ortho_matches_vita2d();
  test_perspective_depth_range();
  test_perspective_aspect();
  test_look_at();
  test_rotation_handedness();
  test_compose_order();
  test_pillarbox();

  if (failures != 0) {
    std::printf("%d render test failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all render tests passed\n");
  return EXIT_SUCCESS;
}
