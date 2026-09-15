// Prints an .anm's skeletal track length and how much each joint actually
// moves, to tell a looping idle apart from a one-shot pose.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "cine/cine.h"
#include "render/skeleton.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: anim_probe <model.mod> <anim.anm>\n");
    return 2;
  }
  const std::string mod_path = argv[1];
  const std::string anm_path = argv[2];

  const std::vector<std::uint8_t> bytes = cine::read_file(mod_path);
  const cine::ModInventory model = cine::parse_mod(bytes, mod_path);
  const cine::Dck animation = render::load_dck(anm_path, "probe");

  std::printf("%s\n  model joints=%zu  anim joints=%zu  frame_count=%u\n", anm_path.c_str(),
              model.joints.size(), animation.joints.size(), animation.frame_count);

  // Compare the first and last frame, plus the midpoint, against frame 0. A
  // looping idle returns near its start; a one-shot ends somewhere else.
  const float last = animation.frame_count > 0 ? float(animation.frame_count - 1) : 0.0f;
  const std::vector<render::Mat4> a =
      render::animated_joint_world_matrices(model, animation, 0.0f);
  const std::vector<render::Mat4> mid =
      render::animated_joint_world_matrices(model, animation, last * 0.5f);
  const std::vector<render::Mat4> b =
      render::animated_joint_world_matrices(model, animation, last);

  double max_mid = 0.0;
  double max_end = 0.0;
  for (std::size_t j = 0; j < a.size(); ++j) {
    double dm = 0.0;
    double de = 0.0;
    for (int i = 0; i < 16; ++i) {
      dm += std::fabs(double(mid[j].m[i]) - double(a[j].m[i]));
      de += std::fabs(double(b[j].m[i]) - double(a[j].m[i]));
    }
    if (dm > max_mid) max_mid = dm;
    if (de > max_end) max_end = de;
  }
  std::printf("  max joint delta: frame0->mid=%.4f  frame0->last=%.4f\n", max_mid, max_end);
  std::printf("  => %s\n", (max_mid > 0.01 && max_end < max_mid * 0.35)
                               ? "LOOPING idle (returns to start): must wrap, not clamp"
                               : "one-shot or static");
  return 0;
}
