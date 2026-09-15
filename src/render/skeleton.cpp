#include "render/skeleton.hpp"

#include <cmath>
#include <stdexcept>

namespace render {

Mat4 joint_matrix(const cine::ModJoint& joint) {
  const Mat4 scale = scaling(joint.scale.x, joint.scale.y, joint.scale.z);
  const Mat4 rotation =
      multiply(rotation_z(joint.rotation.z),
               multiply(rotation_y(joint.rotation.y), rotation_x(joint.rotation.x)));
  return multiply(translation(joint.translation.x, joint.translation.y, joint.translation.z),
                  multiply(rotation, scale));
}

cine::Dck load_dck(const std::string& path, const char* source) {
  const cine::Bundle bundle = cine::parse_anm(cine::read_file(path), source);
  if (bundle.entries.empty() || bundle.entries[0].type != 3) {
    throw std::runtime_error(std::string(source) + " has no skeletal animation");
  }
  const cine::BundleEntry& entry = bundle.entries[0];
  return cine::parse_dck(bundle.bytes.data() + entry.payload_offset, entry.size, source);
}

float clamp_frame(float frame, const cine::Dck& animation) {
  const float last =
      animation.frame_count > 0 ? static_cast<float>(animation.frame_count - 1) : 0.0f;
  return frame < last ? frame : last;
}

float wrap_frame(float frame, const cine::Dck& animation) {
  // The cycle is frame_count - 1 long because the last key repeats the first,
  // which is what lets the sway meet itself without a visible jump.
  if (animation.frame_count <= 1) {
    return 0.0f;
  }
  const float period = static_cast<float>(animation.frame_count - 1);
  if (frame <= 0.0f) {
    return 0.0f;
  }
  float wrapped = std::fmod(frame, period);
  if (wrapped < 0.0f) {
    wrapped += period;
  }
  return wrapped;
}

std::vector<Mat4> animated_joint_world_matrices(const cine::ModInventory& model,
                                                const cine::Dck& animation, float frame) {
  std::vector<Mat4> result(model.joints.size(), identity());
  const size_t count =
      model.joints.size() < animation.joints.size() ? model.joints.size() : animation.joints.size();
  for (size_t i = 0; i < count; ++i) {
    cine::ModJoint pose = model.joints[i];
    const cine::JointAnim& anim = animation.joints[i];
    float* scale_values[3] = {&pose.scale.x, &pose.scale.y, &pose.scale.z};
    float* rotation_values[3] = {&pose.rotation.x, &pose.rotation.y, &pose.rotation.z};
    float* translation_values[3] = {
        &pose.translation.x, &pose.translation.y, &pose.translation.z};
    for (int axis = 0; axis < 3; ++axis) {
      // AnimDck::extractSRT's defaults for tracks with no keys. Translation
      // defaulting to 1.0 rather than 0.0 looks like a retail slip, but the
      // shipped code is explicit about it.
      *scale_values[axis] = anim.scale[axis].entries == 0
                                ? 1.0f
                                : cine::evaluate(anim.scale[axis], animation.scale, frame);
      *rotation_values[axis] = anim.rotation[axis].entries == 0
                                   ? 0.0f
                                   : cine::evaluate(anim.rotation[axis], animation.rotation, frame);
      *translation_values[axis] =
          anim.translation[axis].entries == 0
              ? 1.0f
              : cine::evaluate(anim.translation[axis], animation.translation, frame);
    }
    // BaseShape::updateAnim walks the model's joint hierarchy, not the
    // animation's, so the parent comes from the MOD.
    const Mat4 local = joint_matrix(pose);
    const int parent = model.joints[i].parent_index;
    result[i] = parent >= 0 && static_cast<size_t>(parent) < i
                    ? multiply(result[static_cast<size_t>(parent)], local)
                    : local;
  }
  return result;
}

}  // namespace render
