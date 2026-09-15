#include "cine/cine.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_strip(std::vector<std::uint8_t>& bytes,
                  const std::vector<std::uint16_t>& positions)
{
    bytes.push_back(0x98);
    append_u16(bytes, static_cast<std::uint16_t>(positions.size()));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        append_u16(bytes, positions[i]);
        append_u16(bytes, 0);
    }
}

cine::ModInventory geometry_inventory(const std::vector<std::uint8_t>& display_list,
                                      std::uint32_t faces)
{
    cine::ModInventory inventory = {};
    for (int i = 0; i < 5; ++i) {
        cine::ModVector3 position = {
            static_cast<float>(i), static_cast<float>(i * i), 0.0f
        };
        inventory.positions.push_back(position);
    }
    const cine::ModVector3 normal = {0.0f, 0.0f, 1.0f};
    inventory.normals.push_back(normal);
    cine::Material material = {};
    material.use_nbt = false;
    inventory.materials.push_back(material);
    cine::DisplayList list = {};
    list.face_count = faces;
    list.data_length = static_cast<std::uint32_t>(display_list.size());
    list.data_offset = 0;
    cine::MatrixGroup group;
    group.display_lists.push_back(list);
    cine::Mesh mesh = {};
    mesh.parent_joint = -1;
    mesh.material_index = 0;
    mesh.matrix_groups.push_back(group);
    inventory.meshes.push_back(mesh);
    return inventory;
}

bool exists(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return static_cast<bool>(file);
}

void synthetic_tests()
{
    const std::uint8_t raw[] = { 0x12, 0x34, 0x56, 0x78, 0x3f, 0x80, 0x00, 0x00 };
    cine::Reader reader(raw, sizeof(raw));
    check(reader.u16be() == 0x1234, "u16 endian");
    check(reader.u16be() == 0x5678, "second u16 endian");
    check(std::fabs(reader.f32be() - 1.0f) < 0.0001f, "float endian");
    bool bounded = false;
    try {
        reader.u8();
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "reader bounds error");

    std::vector<std::uint8_t> bundle;
    append_u32(bundle, 1);
    append_u32(bundle, 0);
    append_u32(bundle, 3);
    append_u32(bundle, 4);
    bundle.insert(bundle.end(), {'a', '.', 'b', '\0'});
    bundle.insert(bundle.end(), {1, 2, 3});
    const cine::Bundle parsed = cine::parse_anm(bundle);
    check(parsed.entries.size() == 1, "bundle count");
    check(parsed.entries[0].path == "a.b", "bundle path");
    check(parsed.entries[0].payload_offset == 20, "bundle payload offset");

    cine::AnimParam constant = {1, 0, 0};
    check(cine::evaluate(constant, std::vector<float>(1, 42.0f), 10.0f) == 42.0f,
          "constant evaluation");

    const std::uint8_t triangle[] = {
        0x90, 0x00, 0x03,
        0x00, 0x00, 0x01, 0x00, 0x02,
        0x00, 0x00, 0x03, 0x00, 0x04,
        0x00, 0x00, 0x05, 0x00, 0x06
    };
    const cine::DisplayListStats stats = cine::disassemble_display_list(
        triangle, sizeof(triangle), 1, false);
    check(stats.opcode_counts[1] == 1 && stats.vat_counts[0] == 1,
          "GX triangle opcode");
    check(stats.vertices == 3 && stats.primitives == 1, "GX triangle totals");
    check(stats.max_position_index == 5 && stats.max_normal_index == 6,
          "GX triangle indices");
    bounded = false;
    try {
        cine::disassemble_display_list(triangle, sizeof(triangle) - 1, 1, false);
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "truncated GX display list");
    const std::uint8_t unknown[] = {0x7f};
    bounded = false;
    try {
        cine::disassemble_display_list(unknown, sizeof(unknown), 1, false);
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "unknown GX opcode");

    std::vector<std::uint8_t> strips;
    append_strip(strips, {0, 1, 2, 3});
    append_strip(strips, {0, 1, 2, 3});
    const cine::Geometry strip_geometry
        = cine::build_geometry(strips, geometry_inventory(strips, 4));
    check(strip_geometry.vertices.size() == 4, "strip vertex deduplication");
    check(strip_geometry.indices.size() == 12, "strip index count");
    const std::uint32_t expected[] = {0, 1, 2, 2, 1, 3, 0, 1, 2, 2, 1, 3};
    for (std::size_t i = 0; i < strip_geometry.indices.size(); ++i)
        check(strip_geometry.indices[i] == expected[i], "strip winding");
    check(strip_geometry.batches.size() == 1
          && strip_geometry.batches[0].index_count == 12, "strip batch");

    std::vector<std::uint8_t> degenerate;
    append_strip(degenerate, {});
    append_strip(degenerate, {0});
    append_strip(degenerate, {0, 1});
    const cine::Geometry degenerate_geometry
        = cine::build_geometry(degenerate, geometry_inventory(degenerate, 0));
    check(degenerate_geometry.indices.empty(), "short strips emit no triangles");

    bounded = false;
    try {
        cine::build_geometry(
            std::vector<std::uint8_t>(strips.begin(), strips.end() - 1),
            geometry_inventory(strips, 4));
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "truncated geometry display list");

    std::vector<std::uint8_t> out_of_range;
    append_strip(out_of_range, {0, 1, 5});
    bounded = false;
    try {
        cine::build_geometry(
            out_of_range, geometry_inventory(out_of_range, 1));
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "geometry attribute bounds");

    std::vector<std::uint8_t> bad_primitive = {0x90, 0, 0};
    bounded = false;
    try {
        cine::build_geometry(
            bad_primitive, geometry_inventory(bad_primitive, 0));
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "geometry primitive invariant");

    std::vector<std::uint8_t> bad_vat = {0x99, 0, 0};
    bounded = false;
    try {
        cine::build_geometry(bad_vat, geometry_inventory(bad_vat, 0));
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "geometry VAT invariant");

    std::vector<std::uint8_t> state_command = {0x61, 0, 0, 0, 0};
    bounded = false;
    try {
        cine::build_geometry(
            state_command, geometry_inventory(state_command, 0));
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "geometry state-command invariant");

    std::vector<std::uint8_t> malformed_mod;
    append_u32(malformed_mod, 0x10);
    append_u32(malformed_mod, 24);
    append_u32(malformed_mod, 1);
    malformed_mod.resize(32);
    append_u32(malformed_mod, 0xffff);
    append_u32(malformed_mod, 24);
    malformed_mod.resize(64);
    bounded = false;
    try {
        cine::parse_mod(malformed_mod);
    } catch (const std::runtime_error&) {
        bounded = true;
    }
    check(bounded, "malformed attribute chunk");
}

void real_tests()
{
    const char* root = "runtime-data/dataDir/cinemas/";
    if (!exists("runtime-data/dataDir/cinemas/opening.cin")) {
        std::cout << "real assets absent; skipped\n";
        return;
    }

    const cine::Cin cin = cine::parse_cin(cine::read_text_file(std::string(root) + "opening.cin"));
    check(cin.scenes.size() == 1, "opening scene count");
    check(cin.actors.size() == 2, "opening actor count");
    check(cin.cuts.size() == 2, "opening cut count");
    check(cin.cuts[0].start == 0 && cin.cuts[0].end == 209 && cin.cuts[0].flags == 2,
          "opening first cut");
    check(cin.cuts[1].start == 207 && cin.cuts[1].end == 209 && cin.cuts[1].flags == 5,
          "opening second cut");
    check(cin.cuts[0].actors.size() == 2 && cin.cuts[0].keys.size() == 1,
          "opening cut actors and keys");

    const cine::Dsk dsk = cine::parse_dsk(
        cine::read_text_file(std::string(root) + "opening/opening.dsk"));
    check(dsk.frame_count == 209, "DSK frame count");
    check(dsk.camera_count == 1 && dsk.diffuse_light_count == 1, "DSK camera/light count");
    check(dsk.camera_values.size() == 23 && dsk.light_values.size() == 5, "DSK float pools");
    check(dsk.cameras[0].fields.size() == 10, "DSK camera fields");

    const cine::Bundle opening = cine::parse_anm(
        cine::read_file(std::string(root) + "opening/opening.anm"));
    check(opening.entries.size() == 1 && opening.entries[0].type == 3, "opening bundle DCK");
    const cine::BundleEntry& entry = opening.entries[0];
    const cine::Dck dck = cine::parse_dck(
        opening.bytes.data() + entry.payload_offset, entry.size);
    check(dck.joint_count == 8 && dck.frame_count == 209, "opening DCK dimensions");
    check(dck.scale.size() == 1 && dck.rotation.size() == 1 && dck.translation.size() == 1,
          "opening DCK pools");

    const cine::ModInventory mod = cine::parse_mod(
        cine::read_file(std::string(root) + "opening/opening.mod"));
    check(mod.has_end && mod.chunks.size() == 13, "opening MOD chunks");
    check(mod.chunks[6].id == 0x20 && mod.chunks[8].id == 0x30
          && mod.chunks[10].id == 0x50, "opening MOD key chunks");
    check(mod.tev_infos.size() == 5 && mod.materials.size() == 7, "opening materials");
    check(mod.tev_infos[0].stage_count == 5 && mod.tev_infos[3].stage_count == 7,
          "opening TEV stages");
    check(mod.materials[0].texgens.size() == 3
          && mod.materials[0].textures.size() == 3, "opening PVW textures");
    check(mod.meshes.size() == 7 && mod.meshes[0].feature_flags == 0x1d,
          "opening meshes");
    check(mod.positions.size() == 9776 && mod.normals.size() == 8425
          && mod.colors.size() == 1 && mod.texcoords[0].size() == 1274
          && mod.texcoords[1].size() == 9752, "opening attributes");
    check(mod.vertex_matrices.size() == 5 && mod.matrix_envelopes.empty()
          && mod.joints.size() == 8, "opening rig");
    std::uint64_t opening_faces = 0;
    std::uint64_t opening_vertices = 0;
    std::uint64_t opening_strips = 0;
    for (std::size_t m = 0; m < mod.meshes.size(); ++m)
        for (std::size_t g = 0; g < mod.meshes[m].matrix_groups.size(); ++g)
            for (std::size_t d = 0;
                 d < mod.meshes[m].matrix_groups[g].display_lists.size(); ++d) {
                const cine::DisplayList& list
                    = mod.meshes[m].matrix_groups[g].display_lists[d];
                opening_faces += list.face_count;
                opening_vertices += list.stats.vertices;
                opening_strips += list.stats.opcode_counts[2];
                check(list.stats.consumed_bytes == list.data_length,
                      "opening DL fully consumed");
            }
    check(opening_faces == 14617 && opening_vertices == 20823
          && opening_strips == 3103, "opening GX totals");
    const cine::Geometry opening_geometry = cine::build_geometry(
        cine::read_file(std::string(root) + "opening/opening.mod"), mod);
    check(opening_geometry.vertices.size() == 10275
          && opening_geometry.indices.size() == 43851
          && opening_geometry.batches.size() == 7, "opening geometry totals");
    check(opening_geometry.indices.size() / 3 == opening_faces,
          "opening geometry face reconciliation");

    const cine::ModInventory logo = cine::parse_mod(
        cine::read_file(std::string(root) + "titles/logo.mod"));
    check(logo.has_end && logo.chunks.size() == 11, "logo MOD chunks");
    check(logo.tev_infos.size() == 2 && logo.materials.size() == 599,
          "logo materials");
    check(logo.tev_infos[0].stage_count == 1 && logo.tev_infos[1].stage_count == 1,
          "logo TEV stages");
    check(logo.materials[3].texgens.size() == 1
          && logo.materials[3].textures.size() == 1, "logo PVW texture");
    check(logo.meshes.size() == 599, "logo meshes");
    check(logo.positions.size() == 20785 && logo.normals.size() == 123
          && logo.texcoords[0].size() == 4, "logo attributes");
    check(logo.vertex_matrices.size() == 201 && logo.matrix_envelopes.empty()
          && logo.joints.size() == 210, "logo rig");
    std::uint64_t logo_faces = 0;
    std::uint64_t logo_vertices = 0;
    std::uint64_t logo_strips = 0;
    for (std::size_t m = 0; m < logo.meshes.size(); ++m)
        for (std::size_t g = 0; g < logo.meshes[m].matrix_groups.size(); ++g)
            for (std::size_t d = 0;
                 d < logo.meshes[m].matrix_groups[g].display_lists.size(); ++d) {
                const cine::DisplayList& list
                    = logo.meshes[m].matrix_groups[g].display_lists[d];
                logo_faces += list.face_count;
                logo_vertices += list.stats.vertices;
                logo_strips += list.stats.opcode_counts[2];
                check(list.stats.consumed_bytes == list.data_length,
                      "logo DL fully consumed");
            }
    check(logo_faces == 30267 && logo_vertices == 50187
          && logo_strips == 9960, "logo GX totals");
    const cine::Geometry logo_geometry = cine::build_geometry(
        cine::read_file(std::string(root) + "titles/logo.mod"), logo);
    check(logo_geometry.vertices.size() == 28291
          && logo_geometry.indices.size() == 90801
          && logo_geometry.batches.size() == 599, "logo geometry totals");
    check(logo_geometry.indices.size() / 3 == logo_faces,
          "logo geometry face reconciliation");
}

} // namespace

int main()
{
    try {
        synthetic_tests();
        real_tests();
        std::cout << "cine tests passed\n";
    } catch (const std::exception& exception) {
        std::cerr << "cine test failed: " << exception.what() << "\n";
        return 1;
    }
    return 0;
}
