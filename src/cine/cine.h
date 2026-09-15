#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cine {

std::vector<std::uint8_t> read_file(const std::string& path);
std::string read_text_file(const std::string& path);

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size, std::string source = "buffer");
    explicit Reader(const std::vector<std::uint8_t>& data, std::string source = "buffer");

    std::size_t position() const { return position_; }
    std::size_t size() const { return size_; }
    std::size_t remaining() const { return size_ - position_; }
    void seek(std::size_t position);
    void skip(std::size_t amount);
    std::uint8_t u8();
    std::uint16_t u16be();
    std::uint32_t u32be();
    std::int32_t i32be();
    float f32be();
    std::string string(std::size_t size);

private:
    void require(std::size_t amount) const;
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_;
    std::string source_;
};

struct BundleEntry {
    std::uint32_t type;
    std::uint32_t size;
    std::string path;
    std::size_t payload_offset;
};

struct Bundle {
    std::vector<std::uint8_t> bytes;
    std::vector<BundleEntry> entries;
};

Bundle parse_anm(const std::vector<std::uint8_t>& bytes, const std::string& source = "ANM");

struct AnimParam {
    std::int32_t entries;
    std::int32_t offset;
    std::int32_t flags;
};

struct JointAnim {
    std::int32_t group;
    std::int32_t parent;
    AnimParam scale[3];
    AnimParam rotation[3];
    AnimParam translation[3];
};

struct Dck {
    std::uint32_t joint_count;
    std::uint32_t frame_count;
    std::vector<float> scale;
    std::vector<float> rotation;
    std::vector<float> translation;
    std::vector<JointAnim> joints;
};

Dck parse_dck(const std::uint8_t* data, std::size_t size, const std::string& source = "DCK");
Dck parse_dck(const std::vector<std::uint8_t>& bytes, const std::string& source = "DCK");

struct CinActor {
    std::string shape;
    std::string anims;
    std::string bundle;
};

struct CinActorInstance {
    std::string shape;
    std::int32_t flags;
    std::int32_t anim_play_state;
    std::int32_t colour_anim_index;
};

struct CinKey {
    std::int32_t event_type;
    std::int32_t event_id;
    std::int32_t value;
    std::int32_t frame;
};

struct CinCut {
    std::int32_t scene;
    std::int32_t start;
    std::int32_t end;
    std::int32_t flags;
    std::vector<CinActorInstance> actors;
    std::vector<CinKey> keys;
};

struct Cin {
    std::int32_t type;
    std::int32_t flags;
    std::vector<std::string> scenes;
    std::vector<CinActor> actors;
    std::vector<CinCut> cuts;
};

Cin parse_cin(const std::string& text, const std::string& source = "CIN");

struct NamedAnimParam {
    std::string name;
    AnimParam param;
};

struct DskTable {
    std::int32_t index;
    std::string name;
    std::vector<NamedAnimParam> fields;
};

struct Dsk {
    std::int32_t frame_count;
    std::int32_t camera_count;
    std::int32_t diffuse_light_count;
    std::vector<float> camera_values;
    std::vector<float> light_values;
    std::vector<DskTable> cameras;
    std::vector<DskTable> lights;
};

Dsk parse_dsk(const std::string& text, const std::string& source = "DSK");
float evaluate(const AnimParam& param, const std::vector<float>& values, float frame);

struct ModChunk {
    std::uint32_t id;
    std::uint32_t length;
    std::size_t offset;
    std::int32_t primary_count;
    std::int32_t secondary_count;
};

// One GX TEV combiner half (colour or alpha). GX evaluates
//   out = clamp? saturate(v) : v,  v = (d + lerp(a, b, c) + bias) * scale
// where the inputs are register/texture/raster selectors.
struct TevCombiner {
    std::uint8_t a;
    std::uint8_t b;
    std::uint8_t c;
    std::uint8_t d;
    std::uint8_t op;
    std::uint8_t bias;
    std::uint8_t scale;
    std::uint8_t clamp;
    std::uint8_t out_reg;
};

struct TevStage {
    std::uint8_t texcoord;
    std::uint8_t texmap;
    std::uint8_t channel;
    std::uint8_t konst_color;
    std::uint8_t konst_alpha;
    TevCombiner color;
    TevCombiner alpha;
    std::uint8_t color_combiner[12];
    std::uint8_t alpha_combiner[12];
};

struct TevInfo {
    std::uint32_t stage_count;
    // Initial values of GX_TEVREG0..2, as signed 10-bit-per-channel RGBA.
    std::int16_t registers[3][4];
    // GX_KCOLOR0..3, selected per stage by konst_color / konst_alpha.
    std::uint8_t konst[4][4];
    std::vector<TevStage> stages;
};

// Identifies a TEV configuration by content, so the offline shader generator
// and the runtime agree on which generated program a material needs without
// depending on model file names or chunk ordering.
std::uint64_t tev_key(const TevInfo& info);

// Highest texture map a configuration samples, or -1 when it samples none.
int tev_max_texmap(const TevInfo& info);

struct TexGen {
    std::uint8_t coord;
    std::uint8_t type;
    std::uint8_t source;
    std::uint8_t matrix;
};

// One component of a texture-transform keyframe: a value with its incoming and
// outgoing tangents, interpolated as a Hermite curve between keys.
struct TexAnimComponent {
    float value;
    float tangent_in;
    float tangent_out;
};

// A keyframe holds a position on the timeline and one component per axis.
struct TexAnimKey {
    float position;
    TexAnimComponent component[3];
};

struct MaterialTexture {
    std::uint32_t source_attribute;
    std::uint32_t animation_factor;
    std::uint32_t frame_count;
    float speed;
    // scale x/y, rotation about z in degrees, translation x/y, then the pivot
    // the rotation and scale are applied about.
    float transform[7];
    std::vector<TexAnimKey> scale;
    std::vector<TexAnimKey> rotation;
    std::vector<TexAnimKey> translation;
};

// Builds the GX 2x4 texture matrix for a material texture at a cinematic frame,
// following PVWTextureData::animate in
// upstream/pikmin/src/sysCommon/graphics.cpp. Written row-major, matching the
// order GX loads it. Textures with animation factor 0xff are not animated and
// come back as the identity.
void texture_matrix(const MaterialTexture& texture, float frame, float out[16]);

struct Material {
    std::uint32_t flags;
    std::int32_t texture_index;
    std::uint8_t color[4];
    std::int32_t tev_info_index;
    std::uint32_t lighting_control;
    std::uint32_t pe_control;
    std::uint32_t alpha_compare;
    std::uint32_t depth_test;
    std::uint32_t blend_mode;
    bool use_nbt;
    float texture_scale[3];
    std::uint32_t texgen_count;
    std::uint32_t texture_data_count;
    std::vector<TexGen> texgens;
    std::vector<MaterialTexture> textures;
};

struct DisplayListStats {
    std::uint64_t opcode_counts[7];
    std::uint64_t vat_counts[8];
    std::uint64_t state_commands;
    std::uint64_t vertices;
    std::uint64_t primitives;
    std::int32_t max_matrix_index;
    std::int32_t max_position_index;
    std::int32_t max_normal_index;
    std::int32_t max_color_index;
    std::int32_t max_texcoord_index[8];
    std::size_t consumed_bytes;
};

struct DisplayList {
    std::uint32_t flags;
    std::uint32_t face_count;
    std::uint32_t data_length;
    std::size_t data_offset;
    DisplayListStats stats;
};

struct MatrixGroup {
    std::vector<std::int16_t> dependencies;
    std::vector<DisplayList> display_lists;
};

struct Mesh {
    std::int32_t parent_joint;
    std::uint32_t feature_flags;
    std::int32_t material_index;
    std::vector<MatrixGroup> matrix_groups;
};

struct ModVector2 {
    float x;
    float y;
};

struct ModVector3 {
    float x;
    float y;
    float z;
};

struct ModColor {
    std::uint8_t rgba[4];
};

struct ModNbt {
    ModVector3 normal;
    ModVector3 binormal;
    ModVector3 tangent;
};

struct ModTexture {
    std::uint16_t width;
    std::uint16_t height;
    std::int32_t format;
    std::int32_t image_count;
    std::vector<std::uint8_t> data;
};

struct ModTextureAttribute {
    std::int16_t texture_index;
    std::int16_t tiling;
    std::uint16_t use_offset_data;
    float lod_bias;
};

struct VertexMatrix {
    bool has_partial_weights;
    std::uint32_t index;
};

struct MatrixEnvelope {
    std::vector<std::uint16_t> joint_indices;
    std::vector<float> weights;
};

struct ModJoint {
    std::int32_t parent_index;
    std::uint32_t flags;
    ModVector3 bounds_min;
    ModVector3 bounds_max;
    float radius;
    ModVector3 scale;
    ModVector3 rotation;
    ModVector3 translation;
};

struct ModInventory {
    std::vector<ModChunk> chunks;
    std::vector<TevInfo> tev_infos;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<ModVector3> positions;
    std::vector<ModVector3> normals;
    std::vector<ModNbt> nbts;
    std::vector<ModColor> colors;
    std::vector<ModVector2> texcoords[8];
    std::vector<ModTexture> textures;
    std::vector<ModTextureAttribute> texture_attributes;
    std::vector<VertexMatrix> vertex_matrices;
    std::vector<MatrixEnvelope> matrix_envelopes;
    std::vector<ModJoint> joints;
    bool has_end;
};

DisplayListStats disassemble_display_list(const std::uint8_t* data, std::size_t size,
                                          std::uint32_t feature_flags, bool use_nbt,
                                          const std::string& source = "GX display list");
ModInventory parse_mod(const std::vector<std::uint8_t>& bytes, const std::string& source = "MOD");

struct GeoVertex {
    float position[3];
    float normal[3];
    float texcoord[2][2];
    std::uint8_t color[4];
    std::uint16_t matrix_index;
};

struct GeoBatch {
    std::size_t mesh_index;
    std::size_t matrix_group_index;
    std::int32_t parent_joint;
    std::int32_t material_index;
    std::uint32_t first_index;
    std::uint32_t index_count;
};

struct Geometry {
    std::vector<GeoVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GeoBatch> batches;
};

Geometry build_geometry(const std::vector<std::uint8_t>& bytes, const ModInventory& inventory,
                        const std::string& source = "MOD");
const char* mod_chunk_name(std::uint32_t id);
const char* gx_primitive_name(std::size_t index);

} // namespace cine
