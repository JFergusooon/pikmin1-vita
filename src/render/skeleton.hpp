#pragma once

#include <string>
#include <vector>

#include "cine/cine.h"
#include "render/matrix.hpp"

namespace render {

// Joint posing for MOD models driven by DCK animation. Kept apart from the
// renderer so host tools can check the matrices without a GXM context.

// Composes one joint's local matrix as translate * rotate(ZYX) * scale, which
// is what Matrix4f::makeSRT builds.
Mat4 joint_matrix(const cine::ModJoint& joint);

// Reads the first skeletal entry of an .anm bundle.
cine::Dck load_dck(const std::string& path, const char* source);

// Holds on the last key once the animation is shorter than the cut.
float clamp_frame(float frame, const cine::Dck& animation);

// Wraps into the track instead of holding, for animations that idle on a loop.
// logo.anm's flowers sway on a 78-frame cycle while the opening camera takes
// 209 frames to land, so clamping freezes them for most of the descent and all
// of the settled title screen.
float wrap_frame(float frame, const cine::Dck& animation);

// World matrix per joint at a frame. Every channel comes from the animation:
// AnimDck::extractSRT defaults absent tracks rather than falling back to the
// MOD's rest pose, so the model's own scale/rotation/translation go unused.
//
// These are the matrices meshes are drawn with directly. An inverse bind matrix
// belongs only to envelope-skinned vertices, which are folded into a separate
// matrix in BaseShape::calcWeightedMatrices.
std::vector<Mat4> animated_joint_world_matrices(const cine::ModInventory& model,
                                                const cine::Dck& animation, float frame);

}  // namespace render
