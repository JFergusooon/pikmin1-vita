#include "cine/cine.h"

#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {

std::string extension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string() : path.substr(dot);
}

void dump_dck(const std::uint8_t* data, std::size_t size, const std::string& path)
{
    const cine::Dck dck = cine::parse_dck(data, size, path);
    std::cout << "DCK joints=" << dck.joint_count << " frames=" << dck.frame_count
              << " pools(S/R/T)=" << dck.scale.size() << "/" << dck.rotation.size()
              << "/" << dck.translation.size() << "\n";
    std::map<std::int32_t, std::uint32_t> scale_entries;
    std::map<std::int32_t, std::uint32_t> rotation_entries;
    std::map<std::int32_t, std::uint32_t> translation_entries;
    for (std::size_t joint = 0; joint < dck.joints.size(); ++joint) {
        for (int axis = 0; axis < 3; ++axis) {
            ++scale_entries[dck.joints[joint].scale[axis].entries];
            ++rotation_entries[dck.joints[joint].rotation[axis].entries];
            ++translation_entries[dck.joints[joint].translation[axis].entries];
        }
    }
    const auto print_entries = [](const char* name,
                                  const std::map<std::int32_t, std::uint32_t>& entries) {
        std::cout << " entries(" << name << ")=";
        for (const auto& entry : entries)
            std::cout << entry.first << ":" << entry.second << " ";
    };
    print_entries("S", scale_entries);
    print_entries("R", rotation_entries);
    print_entries("T", translation_entries);
    std::cout << "\n";
}

void dump(const std::string& path)
{
    const std::string ext = extension(path);
    std::cout << path << ":\n";
    if (ext == ".anm") {
        const cine::Bundle bundle = cine::parse_anm(cine::read_file(path), path);
        std::cout << "  ANM entries=" << bundle.entries.size() << "\n";
        for (std::size_t i = 0; i < bundle.entries.size(); ++i) {
            const cine::BundleEntry& entry = bundle.entries[i];
            std::cout << "    [" << i << "] type=" << entry.type << " size=" << entry.size
                      << " offset=0x" << std::hex << entry.payload_offset << std::dec
                      << " path=" << entry.path << "\n";
            if (entry.type == 3) {
                std::cout << "      ";
                dump_dck(bundle.bytes.data() + entry.payload_offset, entry.size, path);
            }
        }
    } else if (ext == ".dck") {
        const std::vector<std::uint8_t> bytes = cine::read_file(path);
        std::cout << "  ";
        dump_dck(bytes.data(), bytes.size(), path);
    } else if (ext == ".cin") {
        const cine::Cin cin = cine::parse_cin(cine::read_text_file(path), path);
        std::cout << "  CIN type=" << cin.type << " flags=" << cin.flags
                  << " scenes=" << cin.scenes.size() << " actors=" << cin.actors.size()
                  << " cuts=" << cin.cuts.size() << "\n";
        for (std::size_t i = 0; i < cin.actors.size(); ++i)
            std::cout << "    actor " << cin.actors[i].shape << " anims=" << cin.actors[i].anims
                      << " bundle=" << cin.actors[i].bundle << "\n";
        for (std::size_t i = 0; i < cin.cuts.size(); ++i)
            std::cout << "    cut scene=" << cin.cuts[i].scene << " [" << cin.cuts[i].start
                      << "," << cin.cuts[i].end << "] flags=" << cin.cuts[i].flags
                      << " actors=" << cin.cuts[i].actors.size()
                      << " keys=" << cin.cuts[i].keys.size() << "\n";
    } else if (ext == ".dsk") {
        const cine::Dsk dsk = cine::parse_dsk(cine::read_text_file(path), path);
        std::cout << "  DSK frames=" << dsk.frame_count << " cameras=" << dsk.camera_count
                  << " diffuse_lights=" << dsk.diffuse_light_count
                  << " pools(camera/light)=" << dsk.camera_values.size() << "/"
                  << dsk.light_values.size() << "\n";
        for (std::size_t i = 0; i < dsk.cameras.size(); ++i) {
            std::cout << "    camera[" << dsk.cameras[i].index << "] " << dsk.cameras[i].name << "\n";
            for (std::size_t j = 0; j < dsk.cameras[i].fields.size(); ++j) {
                const cine::NamedAnimParam& field = dsk.cameras[i].fields[j];
                std::cout << "      " << field.name << " = (" << field.param.entries << ","
                          << field.param.offset << "," << field.param.flags << ")"
                          << " first=" << cine::evaluate(field.param, dsk.camera_values, 0.0f)
                          << "\n";
            }
        }
    } else if (ext == ".mod") {
        const std::vector<std::uint8_t> mod_bytes = cine::read_file(path);
        const cine::ModInventory mod = cine::parse_mod(mod_bytes, path);
        std::cout << "  MOD chunks=" << mod.chunks.size() << "\n";
        for (std::size_t i = 0; i < mod.chunks.size(); ++i) {
            const cine::ModChunk& chunk = mod.chunks[i];
            std::cout << "    0x" << std::hex << std::setw(8) << std::setfill('0') << chunk.offset
                      << " id=0x" << std::setw(4) << chunk.id << std::dec << std::setfill(' ')
                      << " " << cine::mod_chunk_name(chunk.id) << " length=" << chunk.length;
            if (chunk.primary_count >= 0)
                std::cout << " count=" << chunk.primary_count;
            if (chunk.secondary_count >= 0)
                std::cout << " tev=" << chunk.secondary_count;
            std::cout << "\n";
        }
        std::cout << "  TEV variants=" << mod.tev_infos.size()
                  << " materials=" << mod.materials.size() << "\n";
        for (std::size_t i = 0; i < mod.tev_infos.size(); ++i) {
            const cine::TevInfo& info = mod.tev_infos[i];
            std::cout << "    tev[" << i << "] stages=" << info.stage_count << "\n";
            for (int r = 0; r < 3; ++r)
                std::cout << "      reg" << r << "=(" << info.registers[r][0] << ","
                          << info.registers[r][1] << "," << info.registers[r][2] << ","
                          << info.registers[r][3] << ")\n";
            for (int k = 0; k < 4; ++k)
                std::cout << "      konst" << k << "=("
                          << static_cast<unsigned>(info.konst[k][0]) << ","
                          << static_cast<unsigned>(info.konst[k][1]) << ","
                          << static_cast<unsigned>(info.konst[k][2]) << ","
                          << static_cast<unsigned>(info.konst[k][3]) << ")\n";
            for (std::size_t s = 0; s < info.stages.size(); ++s) {
                const cine::TevStage& stage = info.stages[s];
                std::cout << "      stage[" << s << "] coord=" << static_cast<unsigned>(stage.texcoord)
                          << " map=" << static_cast<unsigned>(stage.texmap)
                          << " chan=" << static_cast<unsigned>(stage.channel)
                          << " konstC=" << static_cast<unsigned>(stage.konst_color)
                          << " konstA=" << static_cast<unsigned>(stage.konst_alpha)
                          << "\n        color:";
                for (int j = 0; j < 12; ++j)
                    std::cout << " " << static_cast<unsigned>(stage.color_combiner[j]);
                std::cout << "\n        alpha:";
                for (int j = 0; j < 12; ++j)
                    std::cout << " " << static_cast<unsigned>(stage.alpha_combiner[j]);
                std::cout << "\n";
            }
        }
        std::map<std::uint32_t, std::uint64_t> material_flags;
        std::map<std::int32_t, std::uint64_t> material_tevs;
        for (std::size_t i = 0; i < mod.materials.size(); ++i) {
            const cine::Material& material = mod.materials[i];
            ++material_flags[material.flags];
            ++material_tevs[material.tev_info_index];
            const std::uint32_t stages = material.tev_info_index >= 0
                ? mod.tev_infos[static_cast<std::size_t>(material.tev_info_index)].stage_count : 0;
            std::cout << "    material[" << i << "] flags=0x" << std::hex << material.flags
                      << std::dec << " texture=" << material.texture_index
                      << " tev=" << material.tev_info_index << "/" << stages
                      << " color=(" << static_cast<unsigned>(material.color[0]) << ","
                      << static_cast<unsigned>(material.color[1]) << ","
                      << static_cast<unsigned>(material.color[2]) << ","
                      << static_cast<unsigned>(material.color[3]) << ")"
                      << " lighting=0x" << std::hex << material.lighting_control
                      << " pe=0x" << material.pe_control << std::dec
                      << " texgens=" << material.texgen_count
                      << " textures=" << material.texture_data_count
                      << " normal=" << (material.use_nbt ? "NBT" : "NRM") << "\n";
            for (std::size_t g = 0; g < material.texgens.size(); ++g)
                std::cout << "      texgen[" << g << "] coord="
                          << static_cast<unsigned>(material.texgens[g].coord)
                          << " type=" << static_cast<unsigned>(material.texgens[g].type)
                          << " source=" << static_cast<unsigned>(material.texgens[g].source)
                          << " matrix=" << static_cast<unsigned>(material.texgens[g].matrix) << "\n";
            for (std::size_t t = 0; t < material.textures.size(); ++t) {
                const cine::MaterialTexture& tex = material.textures[t];
                std::cout << "      texdata[" << t << "] attr=" << tex.source_attribute
                          << " anim_factor=" << tex.animation_factor
                          << " frames=" << tex.frame_count << " speed=" << tex.speed
                          << " xform=[";
                for (int j = 0; j < 7; ++j)
                    std::cout << (j ? "," : "") << tex.transform[j];
                std::cout << "] keys(S/R/T)=" << tex.scale.size() << "/"
                          << tex.rotation.size() << "/" << tex.translation.size() << "\n";
                if (tex.animation_factor != 0xff) {
                    // Sample the transform across the loop to show it moves.
                    for (int s = 0; s < 3; ++s) {
                        const float frame =
                            static_cast<float>(tex.frame_count) * static_cast<float>(s) / 3.0f;
                        float matrix[16];
                        cine::texture_matrix(tex, frame, matrix);
                        std::cout << "        frame " << frame << " offset=("
                                  << matrix[3] << "," << matrix[7] << ") scale=("
                                  << matrix[0] << "," << matrix[5] << ")\n";
                    }
                }
            }
        }
        std::cout << "  Material variants: flags";
        for (std::map<std::uint32_t, std::uint64_t>::const_iterator it = material_flags.begin();
             it != material_flags.end(); ++it)
            std::cout << " 0x" << std::hex << it->first << std::dec << "=" << it->second;
        std::cout << " TEV";
        for (std::map<std::int32_t, std::uint64_t>::const_iterator it = material_tevs.begin();
             it != material_tevs.end(); ++it)
            std::cout << " " << it->first << "=" << it->second;
        std::cout << "\n";
        std::uint64_t groups = 0;
        std::uint64_t lists = 0;
        std::uint64_t faces = 0;
        std::uint64_t bytes = 0;
        cine::DisplayListStats total = {};
        total.max_matrix_index = total.max_position_index = total.max_normal_index
            = total.max_color_index = -1;
        std::map<std::uint32_t, std::uint64_t> mesh_features;
        for (int tex = 0; tex < 8; ++tex)
            total.max_texcoord_index[tex] = -1;
        for (std::size_t i = 0; i < mod.meshes.size(); ++i) {
            const cine::Mesh& mesh = mod.meshes[i];
            ++mesh_features[mesh.feature_flags];
            std::uint64_t mesh_lists = 0;
            std::uint64_t mesh_faces = 0;
            std::uint64_t mesh_bytes = 0;
            groups += mesh.matrix_groups.size();
            for (std::size_t g = 0; g < mesh.matrix_groups.size(); ++g) {
                const cine::MatrixGroup& group = mesh.matrix_groups[g];
                std::cout << "      mesh[" << i << "].group[" << g << "] dependencies="
                          << group.dependencies.size() << " dls="
                          << group.display_lists.size() << "\n";
                mesh_lists += group.display_lists.size();
                for (std::size_t d = 0; d < group.display_lists.size(); ++d) {
                    const cine::DisplayList& list = group.display_lists[d];
                    std::cout << "        dl[" << d << "] flags=0x" << std::hex
                              << list.flags << std::dec << " faces=" << list.face_count
                              << " bytes=" << list.data_length << " offset=0x" << std::hex
                              << list.data_offset << std::dec << " vertices="
                              << list.stats.vertices << " primitives="
                              << list.stats.primitives << "\n";
                    mesh_faces += list.face_count;
                    mesh_bytes += list.data_length;
                    total.vertices += list.stats.vertices;
                    total.primitives += list.stats.primitives;
                    total.state_commands += list.stats.state_commands;
                    for (int op = 0; op < 7; ++op)
                        total.opcode_counts[op] += list.stats.opcode_counts[op];
                    for (int vat = 0; vat < 8; ++vat)
                        total.vat_counts[vat] += list.stats.vat_counts[vat];
                    if (list.stats.max_matrix_index > total.max_matrix_index)
                        total.max_matrix_index = list.stats.max_matrix_index;
                    if (list.stats.max_position_index > total.max_position_index)
                        total.max_position_index = list.stats.max_position_index;
                    if (list.stats.max_normal_index > total.max_normal_index)
                        total.max_normal_index = list.stats.max_normal_index;
                    if (list.stats.max_color_index > total.max_color_index)
                        total.max_color_index = list.stats.max_color_index;
                    for (int tex = 0; tex < 8; ++tex)
                        if (list.stats.max_texcoord_index[tex] > total.max_texcoord_index[tex])
                            total.max_texcoord_index[tex] = list.stats.max_texcoord_index[tex];
                }
            }
            lists += mesh_lists;
            faces += mesh_faces;
            bytes += mesh_bytes;
            std::cout << "    mesh[" << i << "] joint=" << mesh.parent_joint
                      << " material=" << mesh.material_index << " features=0x" << std::hex
                      << mesh.feature_flags << std::dec << " groups=" << mesh.matrix_groups.size()
                      << " dls=" << mesh_lists << " faces=" << mesh_faces
                      << " bytes=" << mesh_bytes << "\n";
        }
        std::cout << "  Mesh totals: meshes=" << mod.meshes.size() << " groups=" << groups
                  << " dls=" << lists << " faces=" << faces << " bytes=" << bytes
                  << " emitted_vertices=" << total.vertices
                  << " primitives=" << total.primitives
                  << " state_commands=" << total.state_commands << "\n";
        std::cout << "  Mesh features:";
        for (std::map<std::uint32_t, std::uint64_t>::const_iterator it = mesh_features.begin();
             it != mesh_features.end(); ++it)
            std::cout << " 0x" << std::hex << it->first << std::dec << "=" << it->second;
        std::cout << "\n";
        std::cout << "  Opcodes:";
        for (int op = 0; op < 7; ++op)
            if (total.opcode_counts[op] != 0)
                std::cout << " " << cine::gx_primitive_name(static_cast<std::size_t>(op))
                          << "=" << total.opcode_counts[op];
        std::cout << " VAT:";
        for (int vat = 0; vat < 8; ++vat)
            if (total.vat_counts[vat] != 0)
                std::cout << " " << vat << "=" << total.vat_counts[vat];
        std::cout << "\n  Max indices: matrix=" << total.max_matrix_index
                  << " position=" << total.max_position_index
                  << " normal/NBT=" << total.max_normal_index
                  << " color=" << total.max_color_index;
        for (int tex = 0; tex < 8; ++tex)
            if (total.max_texcoord_index[tex] >= 0)
                std::cout << " tex" << tex << "=" << total.max_texcoord_index[tex];
        std::cout << " unconsumed=0\n";
        const cine::Geometry geometry = cine::build_geometry(mod_bytes, mod, path);
        float bounds_min[3] = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()
        };
        float bounds_max[3] = {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()
        };
        for (std::size_t i = 0; i < geometry.vertices.size(); ++i)
            for (int axis = 0; axis < 3; ++axis) {
                if (geometry.vertices[i].position[axis] < bounds_min[axis])
                    bounds_min[axis] = geometry.vertices[i].position[axis];
                if (geometry.vertices[i].position[axis] > bounds_max[axis])
                    bounds_max[axis] = geometry.vertices[i].position[axis];
            }
        const double ratio = geometry.vertices.empty()
            ? 0.0 : static_cast<double>(total.vertices) / geometry.vertices.size();
        std::cout << "  Geometry: batches=" << geometry.batches.size()
                  << " unique_vertices=" << geometry.vertices.size()
                  << " indices=" << geometry.indices.size()
                  << " triangles=" << geometry.indices.size() / 3
                  << " dedup=" << std::fixed << std::setprecision(3) << ratio << "x\n";
        std::cout << "  Bounds: min=(" << bounds_min[0] << "," << bounds_min[1] << ","
                  << bounds_min[2] << ") max=(" << bounds_max[0] << "," << bounds_max[1]
                  << "," << bounds_max[2] << ")\n";
        std::cout << std::defaultfloat;
        std::cout << "  Rig: vertex_matrices=" << mod.vertex_matrices.size()
                  << " envelopes=" << mod.matrix_envelopes.size()
                  << " joints=" << mod.joints.size() << "\n";
        for (std::size_t i = 0; i < geometry.batches.size(); ++i) {
            const cine::GeoBatch& batch = geometry.batches[i];
            std::cout << "    batch[" << i << "] mesh=" << batch.mesh_index
                      << " group=" << batch.matrix_group_index
                      << " material=" << batch.material_index
                      << " joint=" << batch.parent_joint
                      << " first=" << batch.first_index
                      << " indices=" << batch.index_count << "\n";
        }
    } else if (ext == ".bin" || ext == ".ini") {
        const std::vector<std::uint8_t> bytes = cine::read_file(path);
        std::cout << "  " << (ext == ".bin" ? "actor animation metadata" : "text configuration")
                  << " bytes=" << bytes.size() << " (format decoding deferred)\n";
    } else {
        throw std::runtime_error(path + ": unsupported extension");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: cine_dump ASSET...\n";
        return 2;
    }
    try {
        for (int i = 1; i < argc; ++i)
            dump(argv[i]);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
    return 0;
}
