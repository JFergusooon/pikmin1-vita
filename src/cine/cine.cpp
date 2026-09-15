#include "cine/cine.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cine {
namespace {

std::runtime_error error(const std::string& source, const std::string& message)
{
    return std::runtime_error(source + ": " + message);
}

std::size_t checked_count(std::uint32_t count, std::size_t element_size,
                          std::size_t available, const std::string& source)
{
    if (element_size != 0 && count > available / element_size)
        throw error(source, "declared count exceeds remaining data");
    return count;
}

class Tokens {
public:
    Tokens(const std::string& input, const std::string& source) : source_(source), at_(0)
    {
        std::string clean;
        clean.reserve(input.size());
        bool comment = false;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const char c = input[i];
            if (!comment && c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
                comment = true;
                ++i;
            } else if (comment && (c == '\n' || c == '\r')) {
                comment = false;
                clean.push_back(' ');
            } else if (!comment) {
                if (c == '{' || c == '}') {
                    clean.push_back(' ');
                    clean.push_back(c);
                    clean.push_back(' ');
                } else {
                    clean.push_back(c);
                }
            }
        }
        std::istringstream stream(clean);
        std::string token;
        while (stream >> token)
            values_.push_back(token);
    }

    bool empty() const { return at_ == values_.size(); }
    const std::string& peek() const
    {
        if (empty())
            throw error(source_, "unexpected end of text");
        return values_[at_];
    }
    std::string take()
    {
        const std::string value = peek();
        ++at_;
        return value;
    }
    void expect(const char* wanted)
    {
        const std::string got = take();
        if (got != wanted)
            throw error(source_, "expected '" + std::string(wanted) + "', got '" + got + "'");
    }
    std::int32_t integer()
    {
        const std::string token = take();
        std::size_t used = 0;
        long value;
        try {
            value = std::stol(token, &used, 10);
        } catch (const std::exception&) {
            throw error(source_, "invalid integer '" + token + "'");
        }
        if (used != token.size() || value < std::numeric_limits<std::int32_t>::min()
            || value > std::numeric_limits<std::int32_t>::max())
            throw error(source_, "invalid integer '" + token + "'");
        return static_cast<std::int32_t>(value);
    }
    float real()
    {
        const std::string token = take();
        std::size_t used = 0;
        float value;
        try {
            value = std::stof(token, &used);
        } catch (const std::exception&) {
            throw error(source_, "invalid float '" + token + "'");
        }
        if (used != token.size() || !std::isfinite(value))
            throw error(source_, "invalid float '" + token + "'");
        return value;
    }
    void skip_section()
    {
        expect("{");
        int depth = 1;
        while (depth != 0) {
            const std::string token = take();
            if (token == "{")
                ++depth;
            else if (token == "}")
                --depth;
        }
    }

private:
    std::string source_;
    std::vector<std::string> values_;
    std::size_t at_;
};

std::vector<float> read_float_chunk(Reader& reader, const std::string& source)
{
    const std::uint32_t count = reader.u32be();
    checked_count(count, 4, reader.remaining(), source);
    std::vector<float> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        result.push_back(reader.f32be());
    return result;
}

AnimParam read_param(Reader& reader)
{
    AnimParam result;
    result.entries = reader.i32be();
    result.offset = reader.i32be();
    result.flags = reader.i32be();
    return result;
}

void validate_param(const AnimParam& param, const std::vector<float>& data,
                    const std::string& source)
{
    if (param.entries < 0 || param.offset < 0)
        throw error(source, "negative animation parameter");
    if (param.entries == 0)
        return;
    const std::size_t width = param.entries == 1 ? 1u : (param.flags == 0 ? 3u : 4u);
    const std::size_t offset = static_cast<std::size_t>(param.offset);
    const std::size_t entries = static_cast<std::size_t>(param.entries);
    if (offset > data.size() || entries > (data.size() - offset) / width)
        throw error(source, "animation parameter exceeds its float pool");
}

bool is_camera_field(const std::string& name)
{
    return name.compare(0, 4, "cam_") == 0;
}

bool is_light_field(const std::string& name)
{
    return name != "light_type" && name.compare(0, 6, "light_") == 0;
}

void parse_float_section(Tokens& tokens, std::vector<float>& values)
{
    std::int32_t declared = -1;
    tokens.expect("{");
    while (tokens.peek() != "}") {
        const std::string key = tokens.take();
        if (key == "size") {
            const std::int32_t count = tokens.integer();
            if (count < 0)
                throw std::runtime_error("negative float pool size");
            declared = count;
            values.reserve(static_cast<std::size_t>(count));
        } else if (key == "float") {
            if (declared < 0)
                throw std::runtime_error("float data precedes pool size");
            while (tokens.peek() != "}" && tokens.peek() != "float")
                values.push_back(tokens.real());
        } else {
            tokens.take();
        }
    }
    tokens.take();
    if (declared < 0 || values.size() != static_cast<std::size_t>(declared))
        throw std::runtime_error("float pool size mismatch");
}

DskTable parse_table(Tokens& tokens, bool camera)
{
    DskTable table;
    table.index = -1;
    tokens.expect("{");
    while (tokens.peek() != "}") {
        const std::string key = tokens.take();
        if (key == "index")
            table.index = tokens.integer();
        else if (key == "name")
            table.name = tokens.take();
        else if ((camera && is_camera_field(key)) || (!camera && is_light_field(key))) {
            NamedAnimParam field;
            field.name = key;
            field.param.entries = tokens.integer();
            field.param.offset = tokens.integer();
            field.param.flags = tokens.integer();
            table.fields.push_back(field);
        } else {
            tokens.take();
        }
    }
    tokens.take();
    return table;
}

} // namespace

std::vector<std::uint8_t> read_file(const std::string& path)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file)
        throw error(path, "cannot open");
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length < 0)
        throw error(path, "cannot determine size");
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), length))
        throw error(path, "cannot read");
    return data;
}

std::string read_text_file(const std::string& path)
{
    const std::vector<std::uint8_t> bytes = read_file(path);
    return std::string(bytes.begin(), bytes.end());
}

Reader::Reader(const std::uint8_t* data, std::size_t size, std::string source)
    : data_(data), size_(size), position_(0), source_(source)
{
    if (data == 0 && size != 0)
        throw error(source_, "null data");
}

Reader::Reader(const std::vector<std::uint8_t>& data, std::string source)
    : Reader(data.data(), data.size(), source)
{
}

void Reader::require(std::size_t amount) const
{
    if (amount > remaining()) {
        std::ostringstream message;
        message << "read of " << amount << " bytes at 0x" << std::hex << position_
                << " exceeds size 0x" << size_;
        throw error(source_, message.str());
    }
}

void Reader::seek(std::size_t position)
{
    if (position > size_)
        throw error(source_, "seek exceeds size");
    position_ = position;
}

void Reader::skip(std::size_t amount)
{
    require(amount);
    position_ += amount;
}

std::uint8_t Reader::u8()
{
    require(1);
    return data_[position_++];
}

std::uint16_t Reader::u16be()
{
    require(2);
    const std::uint16_t value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data_[position_]) << 8) | data_[position_ + 1]);
    position_ += 2;
    return value;
}

std::uint32_t Reader::u32be()
{
    require(4);
    const std::uint32_t value = (static_cast<std::uint32_t>(data_[position_]) << 24)
        | (static_cast<std::uint32_t>(data_[position_ + 1]) << 16)
        | (static_cast<std::uint32_t>(data_[position_ + 2]) << 8)
        | static_cast<std::uint32_t>(data_[position_ + 3]);
    position_ += 4;
    return value;
}

std::int32_t Reader::i32be()
{
    return static_cast<std::int32_t>(u32be());
}

float Reader::f32be()
{
    const std::uint32_t bits = u32be();
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string Reader::string(std::size_t size)
{
    require(size);
    std::string result(reinterpret_cast<const char*>(data_ + position_), size);
    position_ += size;
    const std::size_t nul = result.find('\0');
    if (nul != std::string::npos)
        result.resize(nul);
    return result;
}

Bundle parse_anm(const std::vector<std::uint8_t>& bytes, const std::string& source)
{
    Bundle bundle;
    bundle.bytes = bytes;
    Reader reader(bundle.bytes, source);
    const std::uint32_t count = reader.u32be();
    checked_count(count, 12, reader.remaining(), source);
    bundle.entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        BundleEntry entry;
        entry.type = reader.u32be();
        entry.size = reader.u32be();
        const std::uint32_t path_size = reader.u32be();
        entry.path = reader.string(path_size);
        entry.payload_offset = reader.position();
        reader.skip(entry.size);
        bundle.entries.push_back(entry);
    }
    if (reader.remaining() != 0)
        throw error(source, "trailing bytes after bundle entries");
    return bundle;
}

Dck parse_dck(const std::uint8_t* data, std::size_t size, const std::string& source)
{
    Reader reader(data, size, source);
    Dck result;
    result.joint_count = reader.u32be();
    result.frame_count = reader.u32be();
    result.scale = read_float_chunk(reader, source);
    result.rotation = read_float_chunk(reader, source);
    result.translation = read_float_chunk(reader, source);
    checked_count(result.joint_count, 116, reader.remaining(), source);
    result.joints.reserve(result.joint_count);
    for (std::uint32_t i = 0; i < result.joint_count; ++i) {
        JointAnim joint;
        joint.group = reader.i32be();
        joint.parent = reader.i32be();
        for (int axis = 0; axis < 3; ++axis)
            joint.scale[axis] = read_param(reader);
        for (int axis = 0; axis < 3; ++axis)
            joint.rotation[axis] = read_param(reader);
        for (int axis = 0; axis < 3; ++axis)
            joint.translation[axis] = read_param(reader);
        for (int axis = 0; axis < 3; ++axis) {
            validate_param(joint.scale[axis], result.scale, source);
            validate_param(joint.rotation[axis], result.rotation, source);
            validate_param(joint.translation[axis], result.translation, source);
        }
        result.joints.push_back(joint);
    }
    if (reader.remaining() != 0)
        throw error(source, "trailing bytes after DCK");
    return result;
}

Dck parse_dck(const std::vector<std::uint8_t>& bytes, const std::string& source)
{
    return parse_dck(bytes.data(), bytes.size(), source);
}

Cin parse_cin(const std::string& text, const std::string& source)
{
    Tokens tokens(text, source);
    Cin result;
    result.type = 0;
    result.flags = 0;
    while (!tokens.empty()) {
        const std::string command = tokens.take();
        if (command == "type")
            result.type = tokens.integer();
        else if (command == "flags")
            result.flags = tokens.integer();
        else if (command == "addScene") {
            tokens.expect("{");
            while (tokens.peek() != "}") {
                if (tokens.take() != "scene")
                    throw error(source, "unknown addScene field");
                result.scenes.push_back(tokens.take());
            }
            tokens.take();
        } else if (command == "addActor") {
            CinActor actor;
            tokens.expect("{");
            while (tokens.peek() != "}") {
                const std::string field = tokens.take();
                if (field == "shape")
                    actor.shape = tokens.take();
                else if (field == "anims")
                    actor.anims = tokens.take();
                else if (field == "bundle")
                    actor.bundle = tokens.take();
                else
                    throw error(source, "unknown addActor field '" + field + "'");
            }
            tokens.take();
            if (actor.shape.empty())
                throw error(source, "actor has no shape");
            result.actors.push_back(actor);
        } else if (command == "addCut") {
            CinCut cut;
            cut.scene = cut.start = cut.end = cut.flags = 0;
            CinActorInstance* actor = 0;
            tokens.expect("{");
            while (tokens.peek() != "}") {
                const std::string field = tokens.take();
                if (field == "cut") {
                    cut.scene = tokens.integer();
                    cut.start = tokens.integer();
                    cut.end = tokens.integer();
                } else if (field == "flags") {
                    cut.flags = tokens.integer();
                } else if (field == "actor") {
                    CinActorInstance instance;
                    instance.shape = tokens.take();
                    instance.flags = 0;
                    instance.anim_play_state = 0;
                    instance.colour_anim_index = -1;
                    cut.actors.push_back(instance);
                    actor = &cut.actors.back();
                } else if (field == "acflags" || field == "anim") {
                    if (actor == 0)
                        throw error(source, field + " precedes actor");
                    if (field == "acflags")
                        actor->flags = tokens.integer();
                    else {
                        actor->anim_play_state = tokens.integer();
                        actor->colour_anim_index = tokens.integer();
                    }
                } else if (field == "keys") {
                    const std::int32_t count = tokens.integer();
                    if (count < 0)
                        throw error(source, "negative key count");
                    tokens.expect("{");
                    for (std::int32_t i = 0; i < count; ++i) {
                        CinKey key;
                        key.event_type = tokens.integer();
                        key.event_id = tokens.integer();
                        key.value = tokens.integer();
                        key.frame = tokens.integer();
                        cut.keys.push_back(key);
                    }
                    tokens.expect("}");
                } else {
                    throw error(source, "unknown addCut field '" + field + "'");
                }
            }
            tokens.take();
            if (cut.end < cut.start)
                throw error(source, "cut ends before it starts");
            result.cuts.push_back(cut);
        } else {
            throw error(source, "unknown command '" + command + "'");
        }
    }
    return result;
}

Dsk parse_dsk(const std::string& text, const std::string& source)
{
    Tokens tokens(text, source);
    Dsk result;
    result.frame_count = result.camera_count = result.diffuse_light_count = 0;
    while (!tokens.empty()) {
        const std::string section = tokens.take();
        if (section == "<SCENE_KEY_ANM_INFO>") {
            tokens.expect("{");
            while (tokens.peek() != "}") {
                const std::string key = tokens.take();
                if (key == "numframes")
                    result.frame_count = tokens.integer();
                else if (key == "numcameras")
                    result.camera_count = tokens.integer();
                else if (key == "numDifLights")
                    result.diffuse_light_count = tokens.integer();
                else
                    tokens.take();
            }
            tokens.take();
        } else if (section == "<KEY_CAMERA_ANM>") {
            parse_float_section(tokens, result.camera_values);
        } else if (section == "<KEY_DIFFUSE_LIGHT_ANM>") {
            parse_float_section(tokens, result.light_values);
        } else if (section == "<KEY_CAMERA_TABLE>") {
            result.cameras.push_back(parse_table(tokens, true));
        } else if (section == "<KEY_DIFFUSE_LIGHT_TABLE>") {
            result.lights.push_back(parse_table(tokens, false));
        } else {
            tokens.skip_section();
        }
    }
    if (result.camera_count != static_cast<std::int32_t>(result.cameras.size()))
        throw error(source, "camera count does not match tables");
    if (result.diffuse_light_count != static_cast<std::int32_t>(result.lights.size()))
        throw error(source, "diffuse light count does not match tables");
    for (std::size_t i = 0; i < result.cameras.size(); ++i)
        for (std::size_t j = 0; j < result.cameras[i].fields.size(); ++j)
            validate_param(result.cameras[i].fields[j].param, result.camera_values, source);
    for (std::size_t i = 0; i < result.lights.size(); ++i)
        for (std::size_t j = 0; j < result.lights[i].fields.size(); ++j)
            validate_param(result.lights[i].fields[j].param, result.light_values, source);
    return result;
}

float evaluate(const AnimParam& param, const std::vector<float>& values, float frame)
{
    validate_param(param, values, "animation evaluation");
    if (param.entries == 0)
        return 0.0f;
    if (param.entries == 1)
        return values[static_cast<std::size_t>(param.offset)];
    const std::size_t width = param.flags == 0 ? 3u : 4u;
    std::size_t offset = static_cast<std::size_t>(param.offset);
    bool active = false;
    for (std::int32_t i = 0; i < param.entries - 1; ++i) {
        if (values[offset] <= frame && values[offset + width] >= frame) {
            active = true;
            break;
        }
        offset += width;
    }
    if (!active)
        return values[static_cast<std::size_t>(param.offset)
                      + width * static_cast<std::size_t>(param.entries - 1) + 1];
    const float start_time = values[offset];
    const float start_value = values[offset + 1];
    const float start_tangent = values[offset + (width == 3 ? 2 : 3)];
    offset += width;
    const float end_time = values[offset];
    const float end_value = values[offset + 1];
    const float end_tangent = values[offset + 2];
    if (end_time == start_time)
        return end_value;
    const float fps = 30.0f;
    const float t = (frame - start_time) / fps;
    const float delta = fps / (end_time - start_time);
    const float t2 = t * t;
    const float d2 = delta * delta;
    const float t3 = t2 * t;
    const float d3 = d2 * delta;
    return (2.0f * t3 * d3 - 3.0f * t2 * d2 + 1.0f) * start_value
        + (-2.0f * t3 * d3 + 3.0f * t2 * d2) * end_value
        + (t3 * d2 - 2.0f * t2 * delta + t) * start_tangent
        + (t3 * d2 - t2 * delta) * end_tangent;
}

namespace {

void align32(Reader& reader, std::size_t limit, const std::string& source)
{
    const std::size_t aligned = (reader.position() + 31u) & ~std::size_t(31u);
    if (aligned > limit)
        throw error(source, "alignment exceeds chunk");
    reader.seek(aligned);
}

void require_chunk(const Reader& reader, std::size_t amount, std::size_t end,
                   const std::string& source)
{
    if (reader.position() > end || amount > end - reader.position())
        throw error(source, "nested record exceeds chunk");
}

std::uint32_t read_count(Reader& reader, std::size_t item_size, std::size_t end,
                         const std::string& source)
{
    require_chunk(reader, 4, end, source);
    const std::uint32_t count = reader.u32be();
    if (item_size != 0 && count > (end - reader.position()) / item_size)
        throw error(source, "nested count exceeds chunk");
    return count;
}

void skip_anim1(Reader& reader, std::size_t key_size, std::size_t end,
                const std::string& source)
{
    const std::uint32_t count = read_count(reader, 4 + key_size, end, source);
    reader.skip(static_cast<std::size_t>(count) * (4 + key_size));
}

void skip_anim3(Reader& reader, std::size_t key_size, std::size_t end,
                const std::string& source)
{
    const std::uint32_t count = read_count(reader, 4 + 3 * key_size, end, source);
    reader.skip(static_cast<std::size_t>(count) * (4 + 3 * key_size));
}

void read_tev_color_register(Reader& reader, std::size_t end, const std::string& source,
                             std::int16_t out[4])
{
    require_chunk(reader, 16, end, source);
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<std::int16_t>(reader.u16be());
    reader.skip(8); // Frame count and speed of the register's colour animation.
    skip_anim3(reader, 12, end, source);
    skip_anim1(reader, 12, end, source);
}

TevCombiner combiner_from_bytes(const std::uint8_t bytes[12])
{
    TevCombiner combiner;
    combiner.a = bytes[0];
    combiner.b = bytes[1];
    combiner.c = bytes[2];
    combiner.d = bytes[3];
    combiner.op = bytes[4];
    combiner.bias = bytes[5];
    combiner.scale = bytes[6];
    combiner.clamp = bytes[7];
    combiner.out_reg = bytes[8];
    return combiner;
}

TevInfo read_tev_info(Reader& reader, std::size_t end, const std::string& source)
{
    TevInfo info;
    for (int i = 0; i < 3; ++i)
        read_tev_color_register(reader, end, source, info.registers[i]);
    require_chunk(reader, 20, end, source);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            info.konst[i][j] = reader.u8();
    info.stage_count = reader.u32be();
    if (info.stage_count > (end - reader.position()) / 32)
        throw error(source, "TEV stage count exceeds material chunk");
    info.stages.reserve(info.stage_count);
    for (std::uint32_t i = 0; i < info.stage_count; ++i) {
        TevStage stage;
        reader.u8();
        stage.texcoord = reader.u8();
        stage.texmap = reader.u8();
        stage.channel = reader.u8();
        stage.konst_color = reader.u8();
        stage.konst_alpha = reader.u8();
        reader.skip(2);
        for (int j = 0; j < 12; ++j)
            stage.color_combiner[j] = reader.u8();
        for (int j = 0; j < 12; ++j)
            stage.alpha_combiner[j] = reader.u8();
        stage.color = combiner_from_bytes(stage.color_combiner);
        stage.alpha = combiner_from_bytes(stage.alpha_combiner);
        info.stages.push_back(stage);
    }
    return info;
}

// A PVWAnimInfo3<PVWKeyInfoF32>: a count followed by keyframes of a timeline
// position and three (value, tangent, tangent) components.
std::vector<TexAnimKey> read_tex_anim_track(Reader& reader, std::size_t end,
                                            const std::string& source)
{
    const std::uint32_t count = read_count(reader, 4 + 3 * 12, end, source);
    std::vector<TexAnimKey> keys;
    keys.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        TexAnimKey key;
        // PVWAnimKey3::read takes the timeline position as an integer, then
        // three float components.
        key.position = static_cast<float>(reader.u32be());
        for (int axis = 0; axis < 3; ++axis) {
            key.component[axis].value = reader.f32be();
            key.component[axis].tangent_in = reader.f32be();
            key.component[axis].tangent_out = reader.f32be();
        }
        keys.push_back(key);
    }
    return keys;
}

MaterialTexture read_texture_data(Reader& reader, std::size_t end,
                                  const std::string& source)
{
    require_chunk(reader, 52, end, source);
    MaterialTexture texture;
    texture.source_attribute = reader.u32be();
    reader.skip(8); // Two shorts and four exporter control bytes.
    texture.animation_factor = reader.u32be();
    texture.frame_count = reader.u32be();
    texture.speed = reader.f32be();
    for (int i = 0; i < 7; ++i)
        texture.transform[i] = reader.f32be();
    texture.scale = read_tex_anim_track(reader, end, source);
    texture.rotation = read_tex_anim_track(reader, end, source);
    texture.translation = read_tex_anim_track(reader, end, source);
    return texture;
}

Material read_material(Reader& reader, std::size_t end, const std::string& source)
{
    require_chunk(reader, 12, end, source);
    Material material;
    material.flags = reader.u32be();
    material.texture_index = reader.i32be();
    for (int i = 0; i < 4; ++i)
        material.color[i] = reader.u8();
    material.tev_info_index = -1;
    material.lighting_control = 0;
    material.pe_control = material.alpha_compare = material.depth_test = material.blend_mode = 0;
    material.use_nbt = false;
    material.texture_scale[0] = material.texture_scale[1] = material.texture_scale[2] = 1.0f;
    material.texgen_count = 0;
    material.texture_data_count = 0;
    if ((material.flags & 1u) == 0)
        return material;

    require_chunk(reader, 16, end, source);
    material.tev_info_index = reader.i32be();
    reader.skip(12); // Polygon color, frame count, speed.
    skip_anim3(reader, 12, end, source);
    skip_anim1(reader, 12, end, source);
    require_chunk(reader, 28, end, source);
    material.lighting_control = reader.u32be();
    reader.skip(4); // Unused lighting float.
    material.pe_control = reader.u32be();
    material.alpha_compare = reader.u32be();
    material.depth_test = reader.u32be();
    material.blend_mode = reader.u32be();
    const std::uint32_t use_scale = reader.u32be();
    require_chunk(reader, 12, end, source);
    for (int i = 0; i < 3; ++i)
        material.texture_scale[i] = reader.f32be();
    material.use_nbt = use_scale != 0;
    material.texgen_count = read_count(reader, 4, end, source);
    material.texgens.reserve(material.texgen_count);
    for (std::uint32_t i = 0; i < material.texgen_count; ++i) {
        TexGen texgen;
        texgen.coord = reader.u8();
        texgen.type = reader.u8();
        texgen.source = reader.u8();
        texgen.matrix = reader.u8();
        material.texgens.push_back(texgen);
    }
    material.texture_data_count = read_count(reader, 52, end, source);
    for (std::uint32_t i = 0; i < material.texture_data_count; ++i)
        material.textures.push_back(read_texture_data(reader, end, source));
    return material;
}

void check_zero_padding(const std::vector<std::uint8_t>& bytes, std::size_t begin,
                        std::size_t end, const std::string& source)
{
    for (std::size_t i = begin; i < end; ++i)
        if (bytes[i] != 0)
            throw error(source, "nonzero bytes remain at verified chunk boundary");
}

std::size_t primitive_index(std::uint8_t opcode)
{
    switch (opcode & 0xf8u) {
    case 0x80: return 0;
    case 0x90: return 1;
    case 0x98: return 2;
    case 0xa0: return 3;
    case 0xa8: return 4;
    case 0xb0: return 5;
    case 0xb8: return 6;
    default: return 7;
    }
}

std::uint64_t primitive_count(std::size_t kind, std::uint16_t vertices)
{
    switch (kind) {
    case 0: return vertices / 4;
    case 1: return vertices / 3;
    case 2:
    case 3: return vertices > 2 ? vertices - 2 : 0;
    case 4: return vertices / 2;
    case 5: return vertices > 1 ? vertices - 1 : 0;
    case 6: return vertices;
    default: return 0;
    }
}

void update_max(std::int32_t& target, std::uint16_t value)
{
    if (static_cast<std::int32_t>(value) > target)
        target = value;
}

ModVector2 read_vector2(Reader& reader)
{
    ModVector2 value;
    value.x = reader.f32be();
    value.y = reader.f32be();
    return value;
}

ModVector3 read_vector3(Reader& reader)
{
    ModVector3 value;
    value.x = reader.f32be();
    value.y = reader.f32be();
    value.z = reader.f32be();
    return value;
}

void finish_fixed_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                        std::size_t end, const std::string& source)
{
    if (reader.position() > end || end - reader.position() >= 32)
        throw error(source, "attribute chunk length does not match its count and stride");
    check_zero_padding(bytes, reader.position(), end, source);
    reader.seek(end);
}

void parse_attribute_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                           std::uint32_t id, std::size_t end, ModInventory& result,
                           const std::string& source)
{
    const std::uint32_t count = read_count(reader, 0, end, source);
    align32(reader, end, source);
    std::size_t stride = 0;
    if (id == 0x10 || id == 0x11)
        stride = 12;
    else if (id == 0x12)
        stride = 36;
    else if (id == 0x13)
        stride = 4;
    else
        stride = 8;
    if (count > (end - reader.position()) / stride)
        throw error(source, "attribute count exceeds chunk");
    if (id == 0x10) {
        result.positions.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            result.positions.push_back(read_vector3(reader));
    } else if (id == 0x11) {
        result.normals.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            result.normals.push_back(read_vector3(reader));
    } else if (id == 0x12) {
        result.nbts.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ModNbt nbt;
            nbt.normal = read_vector3(reader);
            nbt.binormal = read_vector3(reader);
            nbt.tangent = read_vector3(reader);
            result.nbts.push_back(nbt);
        }
    } else if (id == 0x13) {
        result.colors.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ModColor color;
            for (int channel = 0; channel < 4; ++channel)
                color.rgba[channel] = reader.u8();
            result.colors.push_back(color);
        }
    } else {
        std::vector<ModVector2>& values = result.texcoords[id - 0x18];
        values.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values.push_back(read_vector2(reader));
    }
    finish_fixed_chunk(reader, bytes, end, source);
}

void parse_texture_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                         std::size_t end, ModInventory& result, const std::string& source)
{
    const std::uint32_t count = read_count(reader, 32, end, source);
    align32(reader, end, source);
    result.textures.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        require_chunk(reader, 32, end, source);
        ModTexture texture;
        texture.width = reader.u16be();
        texture.height = reader.u16be();
        texture.format = reader.i32be();
        texture.image_count = reader.i32be();
        reader.skip(16); // Four fields ignored by the retail TexImg reader too.
        const std::uint32_t data_size = reader.u32be();
        require_chunk(reader, data_size, end, source);
        texture.data.assign(bytes.begin() + reader.position(),
                            bytes.begin() + reader.position() + data_size);
        reader.skip(data_size);
        result.textures.push_back(texture);
    }
    finish_fixed_chunk(reader, bytes, end, source);
}

void parse_texture_attribute_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                                   std::size_t end, ModInventory& result,
                                   const std::string& source)
{
    const std::uint32_t count = read_count(reader, 12, end, source);
    align32(reader, end, source);
    result.texture_attributes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ModTextureAttribute attribute;
        attribute.texture_index = static_cast<std::int16_t>(reader.u16be());
        reader.u16be(); // Runtime index, rebuilt after loading.
        attribute.tiling = static_cast<std::int16_t>(reader.u16be());
        attribute.use_offset_data = reader.u16be();
        attribute.lod_bias = reader.f32be();
        result.texture_attributes.push_back(attribute);
    }
    finish_fixed_chunk(reader, bytes, end, source);
}

void parse_vertex_matrix_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                               std::size_t end, ModInventory& result,
                               const std::string& source)
{
    const std::uint32_t count = read_count(reader, 0, end, source);
    align32(reader, end, source);
    if (count > (end - reader.position()) / 2)
        throw error(source, "vertex-matrix count exceeds chunk");
    result.vertex_matrices.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::int16_t encoded = static_cast<std::int16_t>(reader.u16be());
        VertexMatrix matrix;
        matrix.has_partial_weights = encoded >= 0;
        matrix.index = encoded >= 0
            ? static_cast<std::uint32_t>(encoded)
            : static_cast<std::uint32_t>(-static_cast<std::int32_t>(encoded) - 1);
        result.vertex_matrices.push_back(matrix);
    }
    finish_fixed_chunk(reader, bytes, end, source);
}

void parse_envelope_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                          std::size_t end, ModInventory& result, const std::string& source)
{
    const std::uint32_t count = read_count(reader, 0, end, source);
    align32(reader, end, source);
    result.matrix_envelopes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        require_chunk(reader, 2, end, source);
        const std::uint16_t weight_count = reader.u16be();
        if (weight_count > (end - reader.position()) / 6)
            throw error(source, "matrix-envelope weight count exceeds chunk");
        MatrixEnvelope envelope;
        envelope.joint_indices.reserve(weight_count);
        envelope.weights.reserve(weight_count);
        for (std::uint16_t j = 0; j < weight_count; ++j) {
            envelope.joint_indices.push_back(reader.u16be());
            envelope.weights.push_back(reader.f32be());
        }
        result.matrix_envelopes.push_back(envelope);
    }
    finish_fixed_chunk(reader, bytes, end, source);
}

void parse_material_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                          std::size_t end, ModInventory& result, const std::string& source)
{
    const std::uint32_t material_count = read_count(reader, 0, end, source);
    const std::uint32_t tev_count = read_count(reader, 0, end, source);
    align32(reader, end, source);
    result.tev_infos.reserve(tev_count);
    for (std::uint32_t i = 0; i < tev_count; ++i)
        result.tev_infos.push_back(read_tev_info(reader, end, source));
    result.materials.reserve(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
        Material material = read_material(reader, end, source);
        if (material.tev_info_index < -1
            || material.tev_info_index >= static_cast<std::int32_t>(tev_count))
            throw error(source, "material TEV index out of range");
        result.materials.push_back(material);
    }
    check_zero_padding(bytes, reader.position(), end, source);
    reader.seek(end);
}

void parse_mesh_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                      std::size_t end, ModInventory& result, const std::string& source)
{
    const std::uint32_t mesh_count = read_count(reader, 12, end, source);
    align32(reader, end, source);
    result.meshes.reserve(mesh_count);
    for (std::uint32_t i = 0; i < mesh_count; ++i) {
        require_chunk(reader, 12, end, source);
        Mesh mesh;
        mesh.parent_joint = reader.i32be();
        mesh.feature_flags = reader.u32be();
        mesh.material_index = -1;
        const std::uint32_t group_count = reader.u32be();
        if (group_count > (end - reader.position()) / 8)
            throw error(source, "matrix group count exceeds mesh chunk");
        mesh.matrix_groups.reserve(group_count);
        for (std::uint32_t group_index = 0; group_index < group_count; ++group_index) {
            MatrixGroup group;
            const std::uint32_t dependency_count = read_count(reader, 2, end, source);
            group.dependencies.reserve(dependency_count);
            for (std::uint32_t j = 0; j < dependency_count; ++j)
                group.dependencies.push_back(static_cast<std::int16_t>(reader.u16be()));
            const std::uint32_t list_count = read_count(reader, 12, end, source);
            group.display_lists.reserve(list_count);
            for (std::uint32_t j = 0; j < list_count; ++j) {
                DisplayList list;
                list.flags = reader.u32be();
                list.face_count = reader.u32be();
                list.data_length = reader.u32be();
                align32(reader, end, source);
                require_chunk(reader, list.data_length, end, source);
                list.data_offset = reader.position();
                reader.skip(list.data_length);
                group.display_lists.push_back(list);
            }
            mesh.matrix_groups.push_back(group);
        }
        result.meshes.push_back(mesh);
    }
    check_zero_padding(bytes, reader.position(), end, source);
    reader.seek(end);
}

void parse_joint_chunk(Reader& reader, const std::vector<std::uint8_t>& bytes,
                       std::size_t end, ModInventory& result, const std::string& source)
{
    const std::uint32_t joint_count = read_count(reader, 76, end, source);
    align32(reader, end, source);
    result.joints.reserve(joint_count);
    for (std::uint32_t i = 0; i < joint_count; ++i) {
        require_chunk(reader, 76, end, source);
        ModJoint joint;
        joint.parent_index = reader.i32be();
        joint.flags = reader.u32be();
        joint.bounds_min = read_vector3(reader);
        joint.bounds_max = read_vector3(reader);
        joint.radius = reader.f32be();
        joint.scale = read_vector3(reader);
        joint.rotation = read_vector3(reader);
        joint.translation = read_vector3(reader);
        const std::uint32_t pair_count = reader.u32be();
        if (pair_count > (end - reader.position()) / 4)
            throw error(source, "joint mat-poly count exceeds chunk");
        for (std::uint32_t j = 0; j < pair_count; ++j) {
            const std::uint16_t material = reader.u16be();
            const std::uint16_t mesh = reader.u16be();
            if (material >= result.materials.size() || mesh >= result.meshes.size())
                throw error(source, "joint mat-poly index out of range");
            if (result.meshes[mesh].material_index != -1
                && result.meshes[mesh].material_index != material)
                throw error(source, "mesh is assigned multiple materials");
            result.meshes[mesh].material_index = material;
        }
        result.joints.push_back(joint);
    }
    check_zero_padding(bytes, reader.position(), end, source);
    reader.seek(end);
}

} // namespace

DisplayListStats disassemble_display_list(const std::uint8_t* data, std::size_t size,
                                          std::uint32_t feature_flags, bool use_nbt,
                                          const std::string& source)
{
    Reader reader(data, size, source);
    DisplayListStats stats = {};
    stats.max_matrix_index = stats.max_position_index = stats.max_normal_index
        = stats.max_color_index = -1;
    for (int i = 0; i < 8; ++i)
        stats.max_texcoord_index[i] = -1;
    while (reader.remaining() != 0) {
        const std::uint8_t opcode = reader.u8();
        const std::size_t kind = primitive_index(opcode);
        if (kind < 7) {
            const std::uint16_t count = reader.u16be();
            ++stats.opcode_counts[kind];
            ++stats.vat_counts[opcode & 7u];
            stats.vertices += count;
            stats.primitives += primitive_count(kind, count);
            for (std::uint16_t vertex = 0; vertex < count; ++vertex) {
                if ((feature_flags & 1u) != 0)
                    update_max(stats.max_matrix_index, reader.u8());
                if ((feature_flags & 2u) != 0)
                    reader.u8(); // TEX1MTXIDX is direct.
                update_max(stats.max_position_index, reader.u16be());
                update_max(stats.max_normal_index, reader.u16be());
                if ((feature_flags & 4u) != 0)
                    update_max(stats.max_color_index, reader.u16be());
                for (int tex = 0; tex < 8; ++tex)
                    if ((feature_flags & (1u << (tex + 3))) != 0)
                        update_max(stats.max_texcoord_index[tex], reader.u16be());
            }
            (void)use_nbt; // Same FIFO index width; controls the source array semantics.
        } else if (opcode == 0x00) {
            // GX NOP, also used for display-list padding.
        } else if (opcode == 0x08) {
            reader.skip(5);
            ++stats.state_commands;
        } else if (opcode == 0x10) {
            const std::uint32_t header = reader.u32be();
            const std::size_t words = static_cast<std::size_t>((header >> 16) + 1);
            reader.skip(words * 4);
            ++stats.state_commands;
        } else if (opcode == 0x20 || opcode == 0x28 || opcode == 0x30 || opcode == 0x38) {
            reader.skip(4);
            ++stats.state_commands;
        } else if (opcode == 0x40) {
            reader.skip(8);
            ++stats.state_commands;
        } else if (opcode == 0x48) {
            ++stats.state_commands;
        } else if (opcode == 0x61) {
            reader.skip(4);
            ++stats.state_commands;
        } else {
            std::ostringstream message;
            message << "unsupported GX opcode 0x" << std::hex << static_cast<unsigned>(opcode)
                    << " at 0x" << (reader.position() - 1);
            throw error(source, message.str());
        }
    }
    stats.consumed_bytes = reader.position();
    return stats;
}

ModInventory parse_mod(const std::vector<std::uint8_t>& bytes, const std::string& source)
{
    Reader reader(bytes, source);
    ModInventory result;
    result.has_end = false;
    while (reader.remaining() >= 8) {
        if ((reader.position() & 31u) != 0)
            throw error(source, "chunk is not 32-byte aligned");
        ModChunk chunk;
        chunk.offset = reader.position();
        chunk.id = reader.u32be();
        chunk.length = reader.u32be();
        chunk.primary_count = -1;
        chunk.secondary_count = -1;
        if (chunk.length > reader.remaining())
            throw error(source, "chunk length exceeds file");
        const std::size_t end = reader.position() + chunk.length;
        if (chunk.length >= 4 && chunk.id != 0 && chunk.id != 0xffff) {
            const std::size_t saved = reader.position();
            chunk.primary_count = reader.i32be();
            if (chunk.id == 0x30 && chunk.length >= 8)
                chunk.secondary_count = reader.i32be();
            reader.seek(saved);
        }
        result.chunks.push_back(chunk);
        if ((chunk.id >= 0x10 && chunk.id <= 0x13)
            || (chunk.id >= 0x18 && chunk.id <= 0x1f))
            parse_attribute_chunk(reader, bytes, chunk.id, end, result, source);
        else if (chunk.id == 0x20)
            parse_texture_chunk(reader, bytes, end, result, source);
        else if (chunk.id == 0x22)
            parse_texture_attribute_chunk(reader, bytes, end, result, source);
        else if (chunk.id == 0x30)
            parse_material_chunk(reader, bytes, end, result, source);
        else if (chunk.id == 0x40)
            parse_vertex_matrix_chunk(reader, bytes, end, result, source);
        else if (chunk.id == 0x41)
            parse_envelope_chunk(reader, bytes, end, result, source);
        else if (chunk.id == 0x50)
            parse_mesh_chunk(reader, bytes, end, result, source);
        else if (chunk.id == 0x60)
            parse_joint_chunk(reader, bytes, end, result, source);
        else
            reader.seek(end);
        align32(reader, bytes.size(), source);
        if (chunk.id == 0xffff) {
            result.has_end = true;
            break;
        }
    }
    if (!result.has_end)
        throw error(source, "missing end chunk");
    const auto attribute_count = [&result](std::uint32_t id) -> std::int32_t {
        for (std::size_t i = 0; i < result.chunks.size(); ++i)
            if (result.chunks[i].id == id)
                return result.chunks[i].primary_count;
        return 0;
    };
    for (std::size_t i = 0; i < result.meshes.size(); ++i) {
        Mesh& mesh = result.meshes[i];
        if (mesh.material_index < 0)
            throw error(source, "mesh has no material assignment");
        const Material& material = result.materials[static_cast<std::size_t>(mesh.material_index)];
        for (std::size_t g = 0; g < mesh.matrix_groups.size(); ++g)
            for (std::size_t d = 0; d < mesh.matrix_groups[g].display_lists.size(); ++d) {
                DisplayList& list = mesh.matrix_groups[g].display_lists[d];
                list.stats = disassemble_display_list(bytes.data() + list.data_offset,
                                                      list.data_length, mesh.feature_flags,
                                                      material.use_nbt, source);
                for (int primitive = 0; primitive < 7; ++primitive)
                    if (primitive != 2 && list.stats.opcode_counts[primitive] != 0)
                        throw error(source, "display list contains a non-triangle-strip primitive");
                for (int vat = 1; vat < 8; ++vat)
                    if (list.stats.vat_counts[vat] != 0)
                        throw error(source, "display list uses a non-zero VAT");
                if (list.stats.state_commands != 0)
                    throw error(source, "display list contains an embedded GX state command");
                if (list.stats.consumed_bytes != list.data_length)
                    throw error(source, "display list has unconsumed bytes");
                if (list.stats.max_position_index >= attribute_count(0x10)
                    || list.stats.max_normal_index
                        >= attribute_count(material.use_nbt ? 0x12 : 0x11)
                    || list.stats.max_color_index >= attribute_count(0x13))
                    throw error(source, "display-list attribute index out of range");
                for (int tex = 0; tex < 8; ++tex)
                    if (list.stats.max_texcoord_index[tex]
                        >= attribute_count(static_cast<std::uint32_t>(0x18 + tex)))
                        throw error(source, "display-list texcoord index out of range");
            }
    }
    for (std::size_t i = 0; i < result.joints.size(); ++i) {
        const ModJoint& joint = result.joints[i];
        if (joint.parent_index < -1
            || joint.parent_index >= static_cast<std::int32_t>(result.joints.size()))
            throw error(source, "joint parent index out of range");
    }
    for (std::size_t i = 0; i < result.vertex_matrices.size(); ++i) {
        const VertexMatrix& matrix = result.vertex_matrices[i];
        const std::size_t bound = matrix.has_partial_weights
            ? result.joints.size() : result.matrix_envelopes.size();
        if (matrix.index >= bound)
            throw error(source, matrix.has_partial_weights
                ? "vertex-matrix joint index out of range"
                : "vertex-matrix envelope index out of range");
    }
    for (std::size_t i = 0; i < result.matrix_envelopes.size(); ++i)
        for (std::size_t j = 0; j < result.matrix_envelopes[i].joint_indices.size(); ++j)
            if (result.matrix_envelopes[i].joint_indices[j] >= result.joints.size())
                throw error(source, "matrix-envelope joint index out of range");
    for (std::size_t i = 0; i < result.meshes.size(); ++i)
        for (std::size_t g = 0; g < result.meshes[i].matrix_groups.size(); ++g)
            for (std::size_t d = 0; d < result.meshes[i].matrix_groups[g].dependencies.size(); ++d) {
                const std::int16_t dependency
                    = result.meshes[i].matrix_groups[g].dependencies[d];
                if (dependency < -1
                    || dependency >= static_cast<std::int32_t>(result.vertex_matrices.size()))
                    throw error(source, "matrix-group dependency out of range");
            }
    return result;
}

namespace {

struct VertexKey {
    std::uint32_t values[14];

    bool operator==(const VertexKey& other) const
    {
        for (int i = 0; i < 14; ++i)
            if (values[i] != other.values[i])
                return false;
        return true;
    }
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const
    {
        std::size_t value = 2166136261u;
        for (int i = 0; i < 14; ++i) {
            value ^= key.values[i];
            value *= 16777619u;
        }
        return value;
    }
};

std::runtime_error geometry_index_error(const std::string& source, std::size_t mesh,
                                        const char* attribute, std::uint32_t index)
{
    std::ostringstream message;
    message << "mesh " << mesh << " " << attribute << " index " << index << " out of range";
    return error(source, message.str());
}

std::uint32_t checked_u32(std::size_t value, const std::string& source, const char* field)
{
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw error(source, std::string(field) + " exceeds 32-bit geometry limit");
    return static_cast<std::uint32_t>(value);
}

} // namespace

Geometry build_geometry(const std::vector<std::uint8_t>& bytes, const ModInventory& inventory,
                        const std::string& source)
{
    Geometry geometry;
    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> unique;
    const std::uint32_t absent = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t mesh_index = 0; mesh_index < inventory.meshes.size(); ++mesh_index) {
        const Mesh& mesh = inventory.meshes[mesh_index];
        if (mesh.material_index < 0
            || mesh.material_index >= static_cast<std::int32_t>(inventory.materials.size()))
            throw error(source, "mesh material index out of range");
        const Material& material
            = inventory.materials[static_cast<std::size_t>(mesh.material_index)];
        for (int tex = 2; tex < 8; ++tex)
            if ((mesh.feature_flags & (1u << (tex + 3))) != 0)
                throw error(source, "geometry API cannot represent texcoord2 through texcoord7");
        for (std::size_t group_index = 0; group_index < mesh.matrix_groups.size();
             ++group_index) {
            const MatrixGroup& group = mesh.matrix_groups[group_index];
            for (std::size_t list_index = 0; list_index < group.display_lists.size();
                 ++list_index) {
                const DisplayList& list = group.display_lists[list_index];
                GeoBatch batch;
                batch.mesh_index = mesh_index;
                batch.matrix_group_index = group_index;
                batch.parent_joint = mesh.parent_joint;
                batch.material_index = mesh.material_index;
                batch.first_index = checked_u32(geometry.indices.size(), source, "first index");
                if (list.data_offset > bytes.size()
                    || list.data_length > bytes.size() - list.data_offset)
                    throw error(source, "display-list data range exceeds file");
                Reader reader(bytes.data() + list.data_offset, list.data_length, source);
                std::uint64_t triangles = 0;
                while (reader.remaining() != 0) {
                    const std::uint8_t opcode = reader.u8();
                    if (opcode == 0)
                        continue;
                    const std::size_t kind = primitive_index(opcode);
                    if (kind >= 7)
                        throw error(source, "display list contains an embedded GX state command");
                    if (kind != 2)
                        throw error(source, "display list contains a non-triangle-strip primitive");
                    if ((opcode & 7u) != 0)
                        throw error(source, "display list uses a non-zero VAT");
                    const std::uint16_t count = reader.u16be();
                    std::vector<std::uint32_t> strip;
                    strip.reserve(count);
                    for (std::uint16_t i = 0; i < count; ++i) {
                        VertexKey key;
                        for (int field = 0; field < 14; ++field)
                            key.values[field] = absent;
                        std::uint32_t matrix_index = 0;
                        if ((mesh.feature_flags & 1u) != 0) {
                            matrix_index = reader.u8();
                            if (matrix_index >= group.dependencies.size())
                                throw geometry_index_error(
                                    source, mesh_index, "matrix", matrix_index);
                            const std::int16_t dependency = group.dependencies[matrix_index];
                            if (dependency >= 0
                                && static_cast<std::size_t>(dependency)
                                    >= inventory.vertex_matrices.size())
                                throw geometry_index_error(
                                    source, mesh_index, "vertex-matrix", dependency);
                            key.values[1] = matrix_index;
                        }
                        if ((mesh.feature_flags & 2u) != 0)
                            key.values[2] = reader.u8();
                        const std::uint16_t position_index = reader.u16be();
                        const std::uint16_t normal_index = reader.u16be();
                        key.values[3] = position_index;
                        key.values[4] = normal_index;
                        if ((mesh.feature_flags & 4u) != 0)
                            key.values[5] = reader.u16be();
                        for (int tex = 0; tex < 8; ++tex)
                            if ((mesh.feature_flags & (1u << (tex + 3))) != 0)
                                key.values[6 + tex] = reader.u16be();

                        if (position_index >= inventory.positions.size())
                            throw geometry_index_error(
                                source, mesh_index, "position", position_index);
                        const std::size_t normal_count
                            = material.use_nbt ? inventory.nbts.size() : inventory.normals.size();
                        if (normal_index >= normal_count)
                            throw geometry_index_error(
                                source, mesh_index, material.use_nbt ? "NBT" : "normal",
                                normal_index);
                        if (key.values[5] != absent && key.values[5] >= inventory.colors.size())
                            throw geometry_index_error(
                                source, mesh_index, "color", key.values[5]);
                        for (int tex = 0; tex < 2; ++tex)
                            if (key.values[6 + tex] != absent
                                && key.values[6 + tex] >= inventory.texcoords[tex].size())
                                throw geometry_index_error(
                                    source, mesh_index, tex == 0 ? "texcoord0" : "texcoord1",
                                    key.values[6 + tex]);

                        std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash>::const_iterator
                            found = unique.find(key);
                        std::uint32_t vertex_index;
                        if (found != unique.end()) {
                            vertex_index = found->second;
                        } else {
                            GeoVertex vertex = {};
                            const ModVector3& position = inventory.positions[position_index];
                            const ModVector3& normal = material.use_nbt
                                ? inventory.nbts[normal_index].normal
                                : inventory.normals[normal_index];
                            vertex.position[0] = position.x;
                            vertex.position[1] = position.y;
                            vertex.position[2] = position.z;
                            vertex.normal[0] = normal.x;
                            vertex.normal[1] = normal.y;
                            vertex.normal[2] = normal.z;
                            for (int channel = 0; channel < 4; ++channel)
                                vertex.color[channel] = 0xff;
                            if (key.values[5] != absent)
                                for (int channel = 0; channel < 4; ++channel)
                                    vertex.color[channel]
                                        = inventory.colors[key.values[5]].rgba[channel];
                            for (int tex = 0; tex < 2; ++tex)
                                if (key.values[6 + tex] != absent) {
                                    const ModVector2& coordinate
                                        = inventory.texcoords[tex][key.values[6 + tex]];
                                    vertex.texcoord[tex][0] = coordinate.x;
                                    vertex.texcoord[tex][1] = coordinate.y;
                                }
                            vertex.matrix_index = static_cast<std::uint16_t>(matrix_index);
                            vertex_index
                                = checked_u32(geometry.vertices.size(), source, "vertex index");
                            geometry.vertices.push_back(vertex);
                            unique.insert(std::make_pair(key, vertex_index));
                        }
                        strip.push_back(vertex_index);
                    }
                    for (std::size_t i = 2; i < strip.size(); ++i) {
                        if ((i & 1u) == 0) {
                            geometry.indices.push_back(strip[i - 2]);
                            geometry.indices.push_back(strip[i - 1]);
                        } else {
                            geometry.indices.push_back(strip[i - 1]);
                            geometry.indices.push_back(strip[i - 2]);
                        }
                        geometry.indices.push_back(strip[i]);
                        ++triangles;
                    }
                }
                if (triangles != list.face_count) {
                    std::ostringstream message;
                    message << "mesh " << mesh_index << " display list " << list_index
                            << " tessellated " << triangles << " triangles, declared "
                            << list.face_count;
                    throw error(source, message.str());
                }
                batch.index_count = checked_u32(
                    geometry.indices.size() - batch.first_index, source, "batch index count");
                geometry.batches.push_back(batch);
            }
        }
    }
    return geometry;
}

const char* mod_chunk_name(std::uint32_t id)
{
    switch (id) {
    case 0x00: return "header";
    case 0x10: return "vertices";
    case 0x11: return "normals";
    case 0x12: return "NBT";
    case 0x13: return "colours";
    case 0x18: return "texcoord0";
    case 0x19: return "texcoord1";
    case 0x1a: return "texcoord2";
    case 0x1b: return "texcoord3";
    case 0x1c: return "texcoord4";
    case 0x1d: return "texcoord5";
    case 0x1e: return "texcoord6";
    case 0x1f: return "texcoord7";
    case 0x20: return "textures";
    case 0x22: return "texture attributes";
    case 0x30: return "materials";
    case 0x40: return "vertex matrices";
    case 0x41: return "matrix envelopes";
    case 0x50: return "meshes";
    case 0x60: return "joints";
    case 0x61: return "joint names";
    case 0x100: return "collision prisms";
    case 0x110: return "collision grid";
    case 0xffff: return "end";
    default: return "unknown";
    }
}

const char* gx_primitive_name(std::size_t index)
{
    static const char* names[] = {
        "quads", "triangles", "triangle_strip", "triangle_fan",
        "lines", "line_strip", "points"
    };
    return index < 7 ? names[index] : "unknown";
}

std::uint64_t tev_key(const TevInfo& info)
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash = (hash ^ (value & 0xffu)) * 0x100000001b3ull;
    };
    // Register and konst colours are folded in because the generator bakes them
    // into the shader as constants.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            mix(static_cast<std::uint64_t>(info.registers[i][j]));
            mix(static_cast<std::uint64_t>(info.registers[i][j]) >> 8);
        }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            mix(info.konst[i][j]);
    mix(info.stages.size());
    for (std::size_t i = 0; i < info.stages.size(); ++i) {
        const TevStage& stage = info.stages[i];
        mix(stage.texcoord);
        mix(stage.texmap);
        mix(stage.channel);
        mix(stage.konst_color);
        mix(stage.konst_alpha);
        // Only the decoded fields are folded in. The last three bytes of each
        // raw combiner are exporter padding that still holds uninitialised
        // bytes, which would split identical configurations apart.
        const TevCombiner* halves[2] = { &stage.color, &stage.alpha };
        for (int half = 0; half < 2; ++half) {
            mix(halves[half]->a);
            mix(halves[half]->b);
            mix(halves[half]->c);
            mix(halves[half]->d);
            mix(halves[half]->op);
            mix(halves[half]->bias);
            mix(halves[half]->scale);
            mix(halves[half]->clamp);
            mix(halves[half]->out_reg);
        }
    }
    return hash;
}

namespace {

// subExtract from upstream/pikmin/src/sysCommon/graphics.cpp. Unlike the camera
// splines there is no frame-rate term here: positions are already in the same
// units as the sampled frame.
float tex_anim_segment(float frame, const TexAnimKey& start, const TexAnimKey& end, int axis)
{
    const float span = end.position - start.position;
    if (span == 0.0f)
        return end.component[axis].value;

    const float a = frame - start.position;
    const float b = 1.0f / span;
    const float c = (a * a) * b;
    const float d = c * b;
    const float e = a * d;
    const float f = e * b;

    return (2.0f * f - 3.0f * d + 1.0f) * start.component[axis].value
         + (-2.0f * f + 3.0f * d) * end.component[axis].value
         + (e - 2.0f * c + a) * start.component[axis].tangent_out
         + (e - c) * end.component[axis].tangent_in;
}

// PVWTexAnimInfo::extract. An empty track leaves the fallback in place, a
// single key is constant, and sampling past the end holds the last key.
void extract_tex_anim(const std::vector<TexAnimKey>& keys, float frame, float out[3])
{
    if (keys.empty())
        return;
    if (keys.size() == 1) {
        for (int axis = 0; axis < 3; ++axis)
            out[axis] = keys[0].component[axis].value;
        return;
    }
    if (frame >= keys.back().position) {
        for (int axis = 0; axis < 3; ++axis)
            out[axis] = keys.back().component[axis].value;
        return;
    }

    std::size_t index = 0;
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        if (keys[i].position <= frame && keys[i + 1].position >= frame) {
            index = i;
            break;
        }
    }
    for (int axis = 0; axis < 3; ++axis)
        out[axis] = tex_anim_segment(frame, keys[index], keys[index + 1], axis);
}

} // namespace

void texture_matrix(const MaterialTexture& texture, float frame, float out[16])
{
    for (int i = 0; i < 16; ++i)
        out[i] = 0.0f;
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    if (texture.animation_factor == 0xff)
        return;

    // The hardware seeds these before sampling, so a track that is absent keeps
    // the seed rather than collapsing the transform to zero.
    float scale[3] = { 1.0f, 1.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float translation[3] = { 0.0f, 0.0f, 0.0f };

    if (texture.frame_count == 0) {
        scale[0] = texture.transform[0];
        scale[1] = texture.transform[1];
        rotation[2] = texture.transform[2];
        translation[0] = texture.transform[3];
        translation[1] = texture.transform[4];
    } else {
        const float period = static_cast<float>(texture.frame_count);
        float local = std::fmod(frame, period);
        if (local < 0.0f)
            local += period;
        extract_tex_anim(texture.scale, local, scale);
        extract_tex_anim(texture.rotation, local, rotation);
        extract_tex_anim(texture.translation, local, translation);
    }

    const float pivot_x = texture.transform[5];
    const float pivot_y = texture.transform[6];
    const float radians = rotation[2] * 3.14159265358979323846f / 180.0f;
    const float sin_theta = std::sin(radians);
    const float cos_theta = std::cos(radians);

    out[0] = scale[0] * cos_theta;
    out[1] = -scale[0] * sin_theta;
    out[2] = 0.0f;
    out[3] = -scale[0] * cos_theta * pivot_x + scale[0] * sin_theta * pivot_y + pivot_x
           + translation[0];

    out[4] = scale[1] * sin_theta;
    out[5] = scale[1] * cos_theta;
    out[6] = 0.0f;
    out[7] = -scale[1] * sin_theta * pivot_x - scale[1] * cos_theta * pivot_y + pivot_y
           + translation[1];

    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = 1.0f;
    out[11] = 0.0f;

    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

int tev_max_texmap(const TevInfo& info)
{
    int highest = -1;
    for (std::size_t i = 0; i < info.stages.size(); ++i) {
        // 0xff is GX_TEXMAP_NULL: the stage combines registers only.
        if (info.stages[i].texmap != 0xff
            && static_cast<int>(info.stages[i].texmap) > highest)
            highest = static_cast<int>(info.stages[i].texmap);
    }
    return highest;
}

} // namespace cine
