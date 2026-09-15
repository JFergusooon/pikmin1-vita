// Reports how each MOD batch binds vertices to matrices, so the renderer's
// matrix handling can be checked against what the assets actually need.
//
// GX gives a matrix group up to ten matrix slots. A vertex names a slot, the
// group's dependency list maps the slot to a vertex-matrix, and the
// vertex-matrix is either a joint (rigid, used as-is) or an envelope (skinned,
// needs animatedWorld * inverseBind per joint). A renderer that keeps one world
// matrix per batch is only correct when every batch resolves to a single rigid
// joint, so that is what this prints.

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cine/cine.h"
#include "render/skeleton.hpp"

namespace {

float camera_field(const cine::Dsk& dsk, const char* name, float frame, float fallback) {
    if (dsk.cameras.empty()) {
        return fallback;
    }
    const cine::DskTable& camera = dsk.cameras[0];
    for (std::size_t i = 0; i < camera.fields.size(); ++i) {
        if (camera.fields[i].name == name) {
            return cine::evaluate(camera.fields[i].param, dsk.camera_values, frame);
        }
    }
    return fallback;
}

// Projects the posed model through the same camera the renderer builds and
// counts what lands on screen. A black frame is either nothing in the frustum
// or a shading problem, and this separates the two.
void frustum_coverage(const std::string& mod_path, const std::string& anm_path,
                      const std::string& dsk_path) {
    const std::vector<std::uint8_t> bytes = cine::read_file(mod_path);
    const cine::ModInventory model = cine::parse_mod(bytes, mod_path);
    const cine::Geometry geometry = cine::build_geometry(bytes, model, mod_path);
    const cine::Dck animation = render::load_dck(anm_path, "anm");
    const cine::Dsk dsk = cine::parse_dsk(cine::read_text_file(dsk_path), "dsk");

    std::printf("== frustum coverage %s\n", mod_path.c_str());
    const float frames[] = {0.0f, 20.0f, 60.0f, 100.0f, 150.0f, 209.0f};
    for (float frame : frames) {
        const float eye[3] = {
            camera_field(dsk, "cam_pos_x", frame, 0.0f),
            camera_field(dsk, "cam_pos_y", frame, 0.0f),
            camera_field(dsk, "cam_pos_z", frame, 0.0f),
        };
        const float target[3] = {
            camera_field(dsk, "cam_lat_x", frame, 0.0f),
            camera_field(dsk, "cam_lat_y", frame, 0.0f),
            camera_field(dsk, "cam_lat_z", frame, 0.0f),
        };
        const float up[3] = {0.0f, 1.0f, 0.0f};
        const float fovy = camera_field(dsk, "cam_fovy", frame, 41.5f) * 3.14159265358979f / 180.0f;
        const render::Mat4 view_projection = render::multiply(
            render::pillarbox_ndc(960, 544, 4, 3),
            render::multiply(render::perspective(fovy, 4.0f / 3.0f, 1.0f, 15000.0f),
                             render::look_at(eye, target, up)));
        const std::vector<render::Mat4> joints = render::animated_joint_world_matrices(
            model, animation, render::clamp_frame(frame, animation));

        std::size_t on_screen = 0;
        std::size_t behind = 0;
        std::size_t total = 0;
        // How far the projection spreads matters as much as whether it lands in
        // range: a transform that collapses the scene to a dot still counts as
        // on screen while drawing almost nothing.
        float ndc_lo[2] = {1e30f, 1e30f};
        float ndc_hi[2] = {-1e30f, -1e30f};
        for (const cine::GeoBatch& batch : geometry.batches) {
            if (batch.parent_joint < 0 ||
                static_cast<std::size_t>(batch.parent_joint) >= joints.size()) {
                continue;
            }
            const render::Mat4 mvp =
                render::multiply(view_projection, joints[static_cast<std::size_t>(batch.parent_joint)]);
            for (std::uint32_t i = 0; i < batch.index_count; ++i) {
                const cine::GeoVertex& v =
                    geometry.vertices[geometry.indices[batch.first_index + i]];
                float clip[4];
                for (int row = 0; row < 4; ++row) {
                    clip[row] = mvp.m[row] * v.position[0] + mvp.m[4 + row] * v.position[1] +
                                mvp.m[8 + row] * v.position[2] + mvp.m[12 + row];
                }
                ++total;
                if (clip[3] <= 0.0f) {
                    ++behind;
                    continue;
                }
                const float x = clip[0] / clip[3];
                const float y = clip[1] / clip[3];
                const float z = clip[2] / clip[3];
                if (x < ndc_lo[0]) { ndc_lo[0] = x; }
                if (x > ndc_hi[0]) { ndc_hi[0] = x; }
                if (y < ndc_lo[1]) { ndc_lo[1] = y; }
                if (y > ndc_hi[1]) { ndc_hi[1] = y; }
                if (x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f && z >= -1.0f && z <= 1.0f) {
                    ++on_screen;
                }
            }
        }
        std::printf("  frame %5.0f eye=(%7.1f,%7.1f,%6.1f) on_screen=%6zu/%6zu behind=%5zu"
                    " ndc_x[%7.2f %7.2f] ndc_y[%7.2f %7.2f]\n",
                    frame, eye[0], eye[1], eye[2], on_screen, total, behind, ndc_lo[0], ndc_hi[0],
                    ndc_lo[1], ndc_hi[1]);
    }
    std::printf("\n");
}

// Prints what actually feeds each material's raster colour, to explain a scene
// that draws geometry but comes out black.
void shading_inputs(const std::string& mod_path) {
    const std::vector<std::uint8_t> bytes = cine::read_file(mod_path);
    const cine::ModInventory model = cine::parse_mod(bytes, mod_path);
    const cine::Geometry geometry = cine::build_geometry(bytes, model, mod_path);

    std::printf("== shading inputs %s\n", mod_path.c_str());
    std::map<std::int32_t, std::pair<std::uint32_t, std::uint32_t>> vertex_color_range;
    std::map<std::int32_t, std::size_t> vertex_counts;
    for (const cine::GeoBatch& batch : geometry.batches) {
        for (std::uint32_t i = 0; i < batch.index_count; ++i) {
            const cine::GeoVertex& v =
                geometry.vertices[geometry.indices[batch.first_index + i]];
            const std::uint32_t luma = static_cast<std::uint32_t>(v.color[0]) +
                                       v.color[1] + v.color[2];
            auto it = vertex_color_range.find(batch.material_index);
            if (it == vertex_color_range.end()) {
                vertex_color_range[batch.material_index] = std::make_pair(luma, luma);
            } else {
                if (luma < it->second.first) { it->second.first = luma; }
                if (luma > it->second.second) { it->second.second = luma; }
            }
            ++vertex_counts[batch.material_index];
        }
    }

    for (std::size_t i = 0; i < model.materials.size() && i < 12; ++i) {
        const cine::Material& m = model.materials[i];
        const bool lit = (m.lighting_control & 1u) != 0;
        std::printf("  material %2zu color=(%3u,%3u,%3u,%3u) lighting=0x%-6x lit=%d"
                    " tex=%d texgens=%u texdata=%u",
                    i, m.color[0], m.color[1], m.color[2], m.color[3], m.lighting_control,
                    lit ? 1 : 0, m.texture_index, m.texgen_count, m.texture_data_count);
        auto it = vertex_color_range.find(static_cast<std::int32_t>(i));
        if (it != vertex_color_range.end()) {
            std::printf(" vtx_sum_rgb=[%u..%u] verts=%zu", it->second.first, it->second.second,
                        vertex_counts[static_cast<std::int32_t>(i)]);
        } else {
            std::printf(" (no batch)");
        }
        std::printf("\n");
    }
    std::printf("  textures in model: %zu, texture attributes: %zu\n", model.textures.size(),
                model.texture_attributes.size());
    std::printf("\n");
}

// Transforms the model's vertices by the joint each batch binds to and prints
// the resulting bounds per frame. A correct pose keeps the model inside a box
// roughly the size of its bind bounds; geometry flying apart shows up as bounds
// that grow without limit.
void pose_bounds(const std::string& mod_path, const std::string& anm_path) {
    const std::vector<std::uint8_t> mod_bytes = cine::read_file(mod_path);
    const cine::ModInventory model = cine::parse_mod(mod_bytes, mod_path);
    const cine::Geometry geometry = cine::build_geometry(mod_bytes, model, mod_path);
    const cine::Dck animation = render::load_dck(anm_path, "anm");

    std::printf("== pose bounds %s\n", mod_path.c_str());
    const float frames[] = {0.0f, 1.0f, 20.0f, 40.0f, 60.0f, 78.0f, 104.0f, 208.0f};
    for (float requested : frames) {
        const float frame = render::clamp_frame(requested, animation);
        const std::vector<render::Mat4> joints =
            render::animated_joint_world_matrices(model, animation, frame);

        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {-1e30f, -1e30f, -1e30f};
        bool bad = false;
        for (const cine::GeoBatch& batch : geometry.batches) {
            if (batch.parent_joint < 0 ||
                static_cast<std::size_t>(batch.parent_joint) >= joints.size()) {
                continue;
            }
            const render::Mat4& world = joints[static_cast<std::size_t>(batch.parent_joint)];
            for (std::uint32_t i = 0; i < batch.index_count; ++i) {
                const cine::GeoVertex& v = geometry.vertices[geometry.indices[batch.first_index + i]];
                for (int axis = 0; axis < 3; ++axis) {
                    const float value = world.m[axis] * v.position[0] +
                                        world.m[4 + axis] * v.position[1] +
                                        world.m[8 + axis] * v.position[2] + world.m[12 + axis];
                    if (value != value) {
                        bad = true;
                        continue;
                    }
                    if (value < lo[axis]) { lo[axis] = value; }
                    if (value > hi[axis]) { hi[axis] = value; }
                }
            }
        }
        std::printf("  frame %6.1f  x[%10.1f %10.1f] y[%10.1f %10.1f] z[%10.1f %10.1f]%s\n",
                    frame, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], bad ? "  HAS NaN" : "");
    }
    std::printf("\n");
}

void report(const std::string& path) {
    const std::vector<std::uint8_t> bytes = cine::read_file(path);
    const cine::ModInventory model = cine::parse_mod(bytes, path);
    const cine::Geometry geometry = cine::build_geometry(bytes, model, path);

    std::printf("== %s\n", path.c_str());
    std::printf("joints=%zu vertex_matrices=%zu envelopes=%zu batches=%zu\n",
                model.joints.size(), model.vertex_matrices.size(),
                model.matrix_envelopes.size(), geometry.batches.size());

    std::size_t rigid = 0;
    std::size_t multi_slot = 0;
    std::size_t enveloped = 0;
    std::size_t tri_spans_slots = 0;
    std::size_t parent_mismatch = 0;

    for (const cine::GeoBatch& batch : geometry.batches) {
        std::set<std::uint16_t> slots;
        for (std::uint32_t i = 0; i < batch.index_count; ++i) {
            slots.insert(geometry.vertices[geometry.indices[batch.first_index + i]].matrix_index);
        }
        // Triangles are emitted as an index list, so any triangle whose three
        // corners disagree on a slot cannot be drawn with one world matrix.
        for (std::uint32_t i = 0; i + 2 < batch.index_count; i += 3) {
            const std::uint16_t a = geometry.vertices[geometry.indices[batch.first_index + i]].matrix_index;
            const std::uint16_t b = geometry.vertices[geometry.indices[batch.first_index + i + 1]].matrix_index;
            const std::uint16_t c = geometry.vertices[geometry.indices[batch.first_index + i + 2]].matrix_index;
            if (a != b || b != c) {
                ++tri_spans_slots;
            }
        }

        if (slots.size() > 1) {
            ++multi_slot;
        }

        // Resolve each slot through the group's dependency list.
        const cine::Mesh& mesh = model.meshes[batch.mesh_index];
        const cine::MatrixGroup& group = mesh.matrix_groups[batch.matrix_group_index];
        bool batch_enveloped = false;
        bool batch_rigid = true;
        for (std::uint16_t slot : slots) {
            if (slot >= group.dependencies.size()) {
                std::printf("  batch: slot %u outside dependency list (%zu)\n",
                            static_cast<unsigned>(slot), group.dependencies.size());
                batch_rigid = false;
                continue;
            }
            const std::int16_t dependency = group.dependencies[slot];
            if (dependency < 0) {
                batch_rigid = false;
                continue;
            }
            const cine::VertexMatrix& vm =
                model.vertex_matrices[static_cast<std::size_t>(dependency)];
            if (!vm.has_partial_weights) {
                batch_enveloped = true;
                batch_rigid = false;
            } else if (static_cast<std::int32_t>(vm.index) != batch.parent_joint) {
                ++parent_mismatch;
            }
        }
        if (batch_enveloped) {
            ++enveloped;
        }
        if (batch_rigid && slots.size() == 1) {
            ++rigid;
        }
    }

    // Upstream drives the hierarchy from the model's parent indices, not the
    // animation's, so the two have to agree for either to be usable.
    std::printf("batches single rigid joint : %zu\n", rigid);
    std::printf("batches using >1 slot      : %zu\n", multi_slot);
    std::printf("batches using an envelope  : %zu\n", enveloped);
    std::printf("triangles spanning slots   : %zu\n", tri_spans_slots);
    std::printf("slots whose joint != parent: %zu\n", parent_mismatch);
    std::printf("\n");
}

void compare_hierarchy(const std::string& mod_path, const std::string& anm_path,
                       const std::string& track) {
    const std::vector<std::uint8_t> mod_bytes = cine::read_file(mod_path);
    const cine::ModInventory model = cine::parse_mod(mod_bytes, mod_path);
    const cine::Bundle bundle = cine::parse_anm(cine::read_file(anm_path), anm_path);

    const cine::BundleEntry* entry = nullptr;
    for (const cine::BundleEntry& candidate : bundle.entries) {
        if (candidate.path == track) {
            entry = &candidate;
        }
    }
    if (entry == nullptr) {
        std::printf("== %s: no track named %s\n", anm_path.c_str(), track.c_str());
        for (const cine::BundleEntry& candidate : bundle.entries) {
            std::printf("  track: %s\n", candidate.path.c_str());
        }
        return;
    }

    const cine::Dck dck = cine::parse_dck(bundle.bytes.data() + entry->payload_offset, entry->size, track);
    std::printf("== %s [%s]\n", anm_path.c_str(), track.c_str());
    std::printf("model joints=%zu anim joints=%zu frames=%u\n", model.joints.size(),
                dck.joints.size(), dck.frame_count);

    std::size_t parent_disagreements = 0;
    std::size_t parent_after_self = 0;
    const std::size_t count =
        model.joints.size() < dck.joints.size() ? model.joints.size() : dck.joints.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (model.joints[i].parent_index != dck.joints[i].parent) {
            if (parent_disagreements < 8) {
                std::printf("  joint %zu: model parent %d, anim parent %d\n", i,
                            model.joints[i].parent_index, dck.joints[i].parent);
            }
            ++parent_disagreements;
        }
        if (dck.joints[i].parent >= static_cast<std::int32_t>(i)) {
            ++parent_after_self;
        }
    }
    std::printf("parent disagreements       : %zu\n", parent_disagreements);
    std::printf("anim parents not before it : %zu\n", parent_after_self);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4) {
        try {
            report(argv[1]);
            compare_hierarchy(argv[1], argv[2], argv[3]);
            pose_bounds(argv[1], argv[2]);
            shading_inputs(argv[1]);
            frustum_coverage(argv[1], argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: skin_probe <model.mod>...\n");
        std::fprintf(stderr, "       skin_probe <model.mod> <anims.anm> <track>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        try {
            report(argv[i]);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", argv[i], error.what());
            return 1;
        }
    }
    return 0;
}
