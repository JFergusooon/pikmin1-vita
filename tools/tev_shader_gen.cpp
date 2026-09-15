// Emits one GLSL fragment shader per distinct GX TEV configuration found in the
// given .mod files, plus a table the runtime uses to pick a program.
//
// The GameCube combines textures and lighting in up to eight TEV stages whose
// wiring lives in the model file. A single fixed shader cannot reproduce that,
// and the Vita only runs precompiled shaders, so each configuration becomes its
// own generated shader at build time. That mirrors what the hardware does per
// material without needing a runtime shader compiler.
//
// Generated GLSL stays inside psp2spvc's supported subset: no integer types, no
// clamp(), no arrays of matrices, and uniform blocks hold a single struct. The
// emitter also keeps live values to a minimum, declaring only the TEV registers
// a configuration actually reads and holding colour and alpha separately, since
// psp2spvc's register allocator fails on the longest chains otherwise.

#include "cine/cine.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// GX_TEVPREV, GX_TEVREG0, GX_TEVREG1, GX_TEVREG2.
const int kRegisterCount = 4;

const char* register_base(int index)
{
    switch (index) {
    case 0: return "prev";
    case 1: return "reg0";
    case 2: return "reg1";
    case 3: return "reg2";
    default: throw std::runtime_error("unknown TEV register");
    }
}

// Which register half a GXTevColorArg reads, if any.
bool color_arg_register(std::uint8_t arg, int& index, bool& is_alpha)
{
    if (arg > 7)
        return false;
    index = arg / 2;
    is_alpha = (arg % 2) != 0;
    return true;
}

// Which register half a GXTevAlphaArg reads, if any.
bool alpha_arg_register(std::uint8_t arg, int& index)
{
    if (arg > 3)
        return false;
    index = arg;
    return true;
}

struct Usage {
    bool rgb_read[kRegisterCount];
    bool alpha_read[kRegisterCount];

    Usage()
    {
        for (int i = 0; i < kRegisterCount; ++i) {
            rgb_read[i] = false;
            alpha_read[i] = false;
        }
    }
};

Usage analyse(const cine::TevInfo& info)
{
    Usage usage;
    // The framebuffer takes GX_TEVPREV, so both of its halves are always live.
    usage.rgb_read[0] = true;
    usage.alpha_read[0] = true;

    for (std::size_t i = 0; i < info.stages.size(); ++i) {
        const std::uint8_t color_args[4] = {
            info.stages[i].color.a, info.stages[i].color.b,
            info.stages[i].color.c, info.stages[i].color.d
        };
        for (int j = 0; j < 4; ++j) {
            int index = 0;
            bool is_alpha = false;
            if (!color_arg_register(color_args[j], index, is_alpha))
                continue;
            if (is_alpha)
                usage.alpha_read[index] = true;
            else
                usage.rgb_read[index] = true;
        }

        const std::uint8_t alpha_args[4] = {
            info.stages[i].alpha.a, info.stages[i].alpha.b,
            info.stages[i].alpha.c, info.stages[i].alpha.d
        };
        for (int j = 0; j < 4; ++j) {
            int index = 0;
            if (alpha_arg_register(alpha_args[j], index))
                usage.alpha_read[index] = true;
        }
    }
    return usage;
}

bool stage_samples_texture(const cine::TevStage& stage)
{
    if (stage.texmap == 0xff)
        return false;
    const std::uint8_t color_args[4] = {
        stage.color.a, stage.color.b, stage.color.c, stage.color.d
    };
    for (int i = 0; i < 4; ++i)
        if (color_args[i] == 8 || color_args[i] == 9)
            return true;
    const std::uint8_t alpha_args[4] = {
        stage.alpha.a, stage.alpha.b, stage.alpha.c, stage.alpha.d
    };
    for (int i = 0; i < 4; ++i)
        if (alpha_args[i] == 4)
            return true;
    return false;
}

bool stage_uses_konst_color(const cine::TevStage& stage)
{
    const std::uint8_t args[4] = {
        stage.color.a, stage.color.b, stage.color.c, stage.color.d
    };
    for (int i = 0; i < 4; ++i)
        if (args[i] == 14)
            return true;
    return false;
}

bool stage_uses_konst_alpha(const cine::TevStage& stage)
{
    const std::uint8_t args[4] = {
        stage.alpha.a, stage.alpha.b, stage.alpha.c, stage.alpha.d
    };
    for (int i = 0; i < 4; ++i)
        if (args[i] == 6)
            return true;
    return false;
}

std::string color_input(std::uint8_t arg)
{
    int index = 0;
    bool is_alpha = false;
    if (color_arg_register(arg, index, is_alpha)) {
        const std::string base = register_base(index);
        return is_alpha ? "vec3(" + base + "_a)" : base + "_rgb";
    }
    switch (arg) {
    case 8: return "tex.rgb";
    case 9: return "vec3(tex.a)";
    case 10: return "ras.rgb";
    case 11: return "vec3(ras.a)";
    case 12: return "vec3(1.0)";
    case 13: return "vec3(0.5)";
    case 14: return "konst_color";
    case 15: return "vec3(0.0)";
    default: throw std::runtime_error("unknown TEV colour input");
    }
}

std::string alpha_input(std::uint8_t arg)
{
    int index = 0;
    if (alpha_arg_register(arg, index))
        return std::string(register_base(index)) + "_a";
    switch (arg) {
    case 4: return "tex.a";
    case 5: return "ras.a";
    case 6: return "konst_alpha";
    case 7: return "0.0";
    default: throw std::runtime_error("unknown TEV alpha input");
    }
}

std::string literal(float value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
    return text;
}

// The eight leading GXTevKColorSel / GXTevKAlphaSel entries are constants.
bool konst_fraction(std::uint8_t sel, float& out)
{
    if (sel > 7)
        return false;
    static const float fractions[8] = {
        1.0f, 7.0f / 8.0f, 3.0f / 4.0f, 5.0f / 8.0f,
        1.0f / 2.0f, 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 8.0f
    };
    out = fractions[sel];
    return true;
}

std::string konst_color_expression(std::uint8_t sel, const std::uint8_t konst[4][4])
{
    float fraction = 0.0f;
    if (konst_fraction(sel, fraction))
        return "vec3(" + literal(fraction) + ")";

    const int index = static_cast<int>(sel) & 3;
    const auto channel = [&](int c) { return literal(konst[index][c] / 255.0f); };
    if (sel >= 0x0c && sel <= 0x0f)
        return "vec3(" + channel(0) + ", " + channel(1) + ", " + channel(2) + ")";
    if (sel >= 0x10 && sel <= 0x13)
        return "vec3(" + channel(0) + ")";
    if (sel >= 0x14 && sel <= 0x17)
        return "vec3(" + channel(1) + ")";
    if (sel >= 0x18 && sel <= 0x1b)
        return "vec3(" + channel(2) + ")";
    if (sel >= 0x1c && sel <= 0x1f)
        return "vec3(" + channel(3) + ")";
    throw std::runtime_error("unknown TEV konst colour selector");
}

std::string konst_alpha_expression(std::uint8_t sel, const std::uint8_t konst[4][4])
{
    float fraction = 0.0f;
    if (konst_fraction(sel, fraction))
        return literal(fraction);

    const int index = static_cast<int>(sel) & 3;
    if (sel >= 0x10 && sel <= 0x13)
        return literal(konst[index][0] / 255.0f);
    if (sel >= 0x14 && sel <= 0x17)
        return literal(konst[index][1] / 255.0f);
    if (sel >= 0x18 && sel <= 0x1b)
        return literal(konst[index][2] / 255.0f);
    if (sel >= 0x1c && sel <= 0x1f)
        return literal(konst[index][3] / 255.0f);
    throw std::runtime_error("unknown TEV konst alpha selector");
}

float bias_value(std::uint8_t bias)
{
    switch (bias) {
    case 0: return 0.0f;
    case 1: return 0.5f;
    case 2: return -0.5f;
    default: throw std::runtime_error("unknown TEV bias");
    }
}

float scale_value(std::uint8_t scale)
{
    switch (scale) {
    case 0: return 1.0f;
    case 1: return 2.0f;
    case 2: return 4.0f;
    case 3: return 0.5f;
    default: throw std::runtime_error("unknown TEV scale");
    }
}

// out = (d (+/-) lerp(a, b, c) + bias) * scale, saturated when clamp is set.
std::string combine(const std::string& a, const std::string& b, const std::string& c,
                    const std::string& d, const cine::TevCombiner& combiner,
                    const std::string& zero, const std::string& one)
{
    if (combiner.op > 1)
        throw std::runtime_error("TEV comparison ops are not supported");

    const std::string lerp =
        "((" + one + " - " + c + ") * " + a + " + " + c + " * " + b + ")";
    std::string value = d + (combiner.op == 0 ? " + " : " - ") + lerp;

    const float bias = bias_value(combiner.bias);
    if (bias != 0.0f)
        value = value + " + " + literal(bias);
    value = "(" + value + ")";

    const float scale = scale_value(combiner.scale);
    if (scale != 1.0f)
        value = value + " * " + literal(scale);

    // psp2spvc rejects clamp(), so saturation goes through min/max.
    if (combiner.clamp != 0)
        value = "min(max(" + value + ", " + zero + "), " + one + ")";
    return value;
}

std::string generate(const cine::TevInfo& info)
{
    const Usage usage = analyse(info);
    const int max_texmap = cine::tev_max_texmap(info);

    std::ostringstream out;
    out << "#version 450\n\n"
        << "// Generated by tools/tev_shader_gen from a model's TEV configuration.\n"
        << "// Do not edit; rerun tools/gen_shaders.sh instead.\n"
        << "//\n"
        << "// " << info.stages.size() << " TEV stage(s), "
        << (max_texmap + 1) << " texture map(s).\n\n";

    for (int i = 0; i <= max_texmap; ++i)
        out << "layout(binding = " << i << ") uniform sampler2D tex" << i << ";\n";
    if (max_texmap >= 0)
        out << "\n";

    out << "layout(location = 0) in vec4 v_color;\n";
    for (int i = 0; i <= max_texmap; ++i)
        out << "layout(location = " << (i + 1) << ") in vec2 v_texcoord" << i << ";\n";
    out << "\nlayout(location = 0) out vec4 out_color;\n\n";

    out << "void main() {\n";
    // psp2spvc fails to compile a swizzle applied straight to a varying, so the
    // raster colour is copied into a local before any component is read.
    out << "  vec4 ras = v_color;\n";
    for (int i = 0; i < kRegisterCount; ++i) {
        // GX_TEVPREV has no seed value; the others come from the material.
        const float rgb[3] = {
            i == 0 ? 0.0f : info.registers[i - 1][0] / 255.0f,
            i == 0 ? 0.0f : info.registers[i - 1][1] / 255.0f,
            i == 0 ? 0.0f : info.registers[i - 1][2] / 255.0f
        };
        const float alpha = i == 0 ? 0.0f : info.registers[i - 1][3] / 255.0f;
        if (usage.rgb_read[i])
            out << "  vec3 " << register_base(i) << "_rgb = vec3(" << literal(rgb[0])
                << ", " << literal(rgb[1]) << ", " << literal(rgb[2]) << ");\n";
        if (usage.alpha_read[i])
            out << "  float " << register_base(i) << "_a = " << literal(alpha) << ";\n";
    }

    for (std::size_t i = 0; i < info.stages.size(); ++i) {
        const cine::TevStage& stage = info.stages[i];
        out << "\n  // Stage " << i << "\n  {\n";
        if (stage_samples_texture(stage))
            out << "    vec4 tex = texture(tex" << static_cast<unsigned>(stage.texmap)
                << ", v_texcoord" << static_cast<unsigned>(stage.texcoord) << ");\n";
        if (stage_uses_konst_color(stage))
            out << "    vec3 konst_color = "
                << konst_color_expression(stage.konst_color, info.konst) << ";\n";
        if (stage_uses_konst_alpha(stage))
            out << "    float konst_alpha = "
                << konst_alpha_expression(stage.konst_alpha, info.konst) << ";\n";

        // Both halves read the register state from the start of the stage, so
        // the results land in temporaries before anything is written back.
        // Writes to halves nothing reads are dropped.
        const bool keep_color = usage.rgb_read[stage.color.out_reg];
        const bool keep_alpha = usage.alpha_read[stage.alpha.out_reg];
        if (keep_color)
            out << "    vec3 color_out = "
                << combine(color_input(stage.color.a), color_input(stage.color.b),
                           color_input(stage.color.c), color_input(stage.color.d),
                           stage.color, "vec3(0.0)", "vec3(1.0)")
                << ";\n";
        if (keep_alpha)
            out << "    float alpha_out = "
                << combine(alpha_input(stage.alpha.a), alpha_input(stage.alpha.b),
                           alpha_input(stage.alpha.c), alpha_input(stage.alpha.d),
                           stage.alpha, "0.0", "1.0")
                << ";\n";
        if (keep_color)
            out << "    " << register_base(stage.color.out_reg) << "_rgb = color_out;\n";
        if (keep_alpha)
            out << "    " << register_base(stage.alpha.out_reg) << "_a = alpha_out;\n";
        out << "  }\n";
    }

    out << "\n  out_color = vec4(prev_rgb, prev_a);\n";
    out << "}\n";
    return out.str();
}

std::string key_name(std::uint64_t key)
{
    char text[32];
    std::snprintf(text, sizeof(text), "tev_%016llx", static_cast<unsigned long long>(key));
    return text;
}

int max_texcoord(const cine::TevInfo& info)
{
    int highest = -1;
    for (std::size_t i = 0; i < info.stages.size(); ++i) {
        // 0xff is GX_TEXCOORD_NULL.
        if (info.stages[i].texcoord != 0xff
            && static_cast<int>(info.stages[i].texcoord) > highest)
            highest = static_cast<int>(info.stages[i].texcoord);
    }
    return highest;
}

// Every texgen in both title models is a 2x4 matrix multiply that reads the
// TEX0 attribute for coordinate 0 and the TEX1 attribute for the rest, so the
// generated vertex shaders can bake that wiring in. Anything else would be
// silently mis-transformed, so it fails the build instead.
void validate_texgens(const cine::ModInventory& model, const std::string& path)
{
    for (std::size_t m = 0; m < model.materials.size(); ++m) {
        const std::vector<cine::TexGen>& texgens = model.materials[m].texgens;
        for (std::size_t g = 0; g < texgens.size(); ++g) {
            const unsigned expected_source = g == 0 ? 4u : 5u;
            if (texgens[g].type != 1 || texgens[g].coord != g
                || texgens[g].source != expected_source) {
                char message[256];
                std::snprintf(message, sizeof(message),
                              "%s material[%zu] texgen[%zu] is type=%u coord=%u source=%u, "
                              "outside the pattern the generated vertex shaders assume",
                              path.c_str(), m, g, texgens[g].type, texgens[g].coord,
                              texgens[g].source);
                throw std::runtime_error(message);
            }
        }
    }
}

// Transforms positions and normals, evaluates the one lighting channel the
// title materials use, and runs texgen so the fragment stage only ever reads a
// directly interpolated varying, which is the form the GXP sampler path needs.
std::string generate_vertex(int texcoord_count)
{
    std::ostringstream out;
    out << "#version 450\n\n"
        << "// Generated by tools/tev_shader_gen. Do not edit.\n"
        << "//\n"
        << "// Vertex stage feeding TEV programs that read " << texcoord_count
        << " texture coordinate(s).\n"
        << "// Emitted per coordinate count so a program pair's varyings match exactly.\n\n"
        << "struct SceneData {\n"
        << "  mat4 view_proj;\n"
        << "  mat4 world;\n"
        << "  mat4 tex_matrix0;\n"
        << "  mat4 tex_matrix1;\n"
        << "  mat4 tex_matrix2;\n"
        << "  mat4 tex_matrix3;\n"
        << "  vec4 light_dir0;\n"
        << "  vec4 light_dir1;\n"
        << "  vec4 light_dir2;\n"
        << "  vec4 light_color0;\n"
        << "  vec4 light_color1;\n"
        << "  vec4 light_color2;\n"
        << "  vec4 ambient;\n"
        << "  vec4 material_color;\n"
        << "};\n\n"
        << "layout(binding = 0) uniform SceneBlock { SceneData self; } scene;\n\n"
        << "layout(location = 0) in vec3 in_position;\n"
        << "layout(location = 1) in vec3 in_normal;\n"
        << "layout(location = 2) in vec2 in_texcoord0;\n"
        << "layout(location = 3) in vec2 in_texcoord1;\n"
        << "layout(location = 4) in vec4 in_color;\n\n"
        << "layout(location = 0) out vec4 v_color;\n";
    for (int i = 0; i < texcoord_count; ++i)
        out << "layout(location = " << (i + 1) << ") out vec2 v_texcoord" << i << ";\n";

    out << "\nvoid main() {\n"
        << "  vec4 world_position = scene.self.world * vec4(in_position, 1.0);\n"
        << "  vec3 normal = normalize((scene.self.world * vec4(in_normal, 0.0)).xyz);\n\n"
        << "  vec3 lit = scene.self.ambient.rgb;\n"
        << "  lit += scene.self.light_color0.rgb"
        << " * max(dot(normal, -scene.self.light_dir0.xyz), 0.0);\n"
        << "  lit += scene.self.light_color1.rgb"
        << " * max(dot(normal, -scene.self.light_dir1.xyz), 0.0);\n"
        << "  lit += scene.self.light_color2.rgb"
        << " * max(dot(normal, -scene.self.light_dir2.xyz), 0.0);\n"
        << "  lit = min(max(lit, vec3(0.0)), vec3(1.0));\n\n"
        << "  v_color = vec4(lit, 1.0) * in_color * scene.self.material_color;\n";

    for (int i = 0; i < texcoord_count; ++i) {
        // Coordinate 0 reads the TEX0 attribute; the rest read TEX1.
        const char* attribute = i == 0 ? "in_texcoord0" : "in_texcoord1";
        out << "  v_texcoord" << i << " = (scene.self.tex_matrix" << i << " * vec4("
            << attribute << ", 0.0, 1.0)).xy;\n";
    }

    out << "\n  gl_Position = scene.self.view_proj * world_position;\n"
        << "}\n";
    return out.str();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::cerr << "usage: tev_shader_gen SHADER_DIR TABLE_HEADER MODEL...\n";
        return 2;
    }

    const std::string shader_dir = argv[1];
    const std::string table_path = argv[2];

    try {
        struct Program {
            std::string name;
            int texcoords;
        };
        std::map<std::uint64_t, Program> programs;
        std::map<int, bool> vertex_variants;

        for (int i = 3; i < argc; ++i) {
            const std::string path = argv[i];
            const std::vector<std::uint8_t> bytes = cine::read_file(path);
            const cine::ModInventory model = cine::parse_mod(bytes, path);
            validate_texgens(model, path);
            for (std::size_t t = 0; t < model.tev_infos.size(); ++t) {
                const cine::TevInfo& info = model.tev_infos[t];
                const std::uint64_t key = cine::tev_key(info);
                const int texcoords = max_texcoord(info) + 1;
                if (programs.count(key) != 0)
                    continue;
                const std::string name = key_name(key);
                const std::string source = shader_dir + "/" + name + ".frag";
                std::ofstream file(source.c_str(), std::ios::binary);
                if (!file)
                    throw std::runtime_error("cannot write " + source);
                file << generate(info);

                Program program;
                program.name = name;
                program.texcoords = texcoords;
                programs[key] = program;
                vertex_variants[texcoords] = true;
                std::cout << name << "  stages=" << info.stages.size()
                          << " texmaps=" << (cine::tev_max_texmap(info) + 1)
                          << " texcoords=" << texcoords
                          << "  (" << path << " tev[" << t << "])\n";
            }
        }

        for (std::map<int, bool>::const_iterator it = vertex_variants.begin();
             it != vertex_variants.end(); ++it) {
            char name[32];
            std::snprintf(name, sizeof(name), "tev_vs%d", it->first);
            const std::string source = shader_dir + "/" + name + ".vert";
            std::ofstream file(source.c_str(), std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot write " + source);
            file << generate_vertex(it->first);
            std::cout << name << "  texcoords=" << it->first << "\n";
        }

        std::ofstream table(table_path.c_str(), std::ios::binary);
        if (!table)
            throw std::runtime_error("cannot write " + table_path);
        table << "// Generated by tools/tev_shader_gen. Do not edit.\n"
              << "//\n"
              << "// Maps a TEV configuration's content key to the generated program pair\n"
              << "// that reproduces it, with the number of texture coordinates the pair\n"
              << "// exchanges. Included by src/render/tev_programs.cpp.\n\n";
        for (std::map<std::uint64_t, Program>::const_iterator it = programs.begin();
             it != programs.end(); ++it) {
            char line[160];
            std::snprintf(line, sizeof(line), "TEV_PROGRAM(0x%016llxull, \"%s\", %d)\n",
                          static_cast<unsigned long long>(it->first),
                          it->second.name.c_str(), it->second.texcoords);
            table << line;
        }
        std::cout << programs.size() << " distinct TEV program(s), "
                  << vertex_variants.size() << " vertex variant(s)\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    }
    return 0;
}
