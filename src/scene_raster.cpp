#include "scene_raster.hpp"

#include <cmath>
#include <cstdio>

namespace raster {
namespace {

const float kPi = 3.14159265358979323846f;

// GX offers eight texture coordinates per material; these models generate at
// most four, and carrying only those keeps the interpolation loop short.
const int kMaxTexCoords = 4;

// Interpolated vertex, in clip space until the rasteriser divides through.
struct Shaded {
  float position[4];
  float texcoord[kMaxTexCoords][2];
  float color[4];
};

Shaded lerp_shaded(const Shaded& a, const Shaded& b, float t) {
  Shaded out;
  for (int i = 0; i < 4; ++i) {
    out.position[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
    out.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
  }
  for (int c = 0; c < kMaxTexCoords; ++c) {
    for (int i = 0; i < 2; ++i) {
      out.texcoord[c][i] = a.texcoord[c][i] + (b.texcoord[c][i] - a.texcoord[c][i]) * t;
    }
  }
  return out;
}

int wrap_index(int value, int size) {
  // GX only accepts power-of-two dimensions, so the wrap is a mask, and the
  // mask also gives the right answer for negative values in two's complement.
  // This is worth the special case: the Cortex-A9 has no integer divide
  // instruction, so every % is a call into __aeabi_idivmod, and a bilinear
  // sample needs four of them per texture.
  if ((size & (size - 1)) == 0) {
    return value & (size - 1);
  }
  int wrapped = value % size;
  if (wrapped < 0) {
    wrapped += size;
  }
  return wrapped;
}

// GX wraps by default for every material in these two models, and filters them
// linearly. Point sampling instead is visible as blocking: the terrain's
// shadow contribution comes from 256x256 maps stretched across the whole
// ground, so its texels land several pixels wide on screen.
void sample(const gcn::Image& image, float u, float v, float out[4]) {
  const float x = u * image.width - 0.5f;
  const float y = v * image.height - 0.5f;
  const float floor_x = std::floor(x);
  const float floor_y = std::floor(y);
  const float tx = x - floor_x;
  const float ty = y - floor_y;

  const int x0 = wrap_index(static_cast<int>(floor_x), image.width);
  const int y0 = wrap_index(static_cast<int>(floor_y), image.height);
  const int x1 = wrap_index(x0 + 1, image.width);
  const int y1 = wrap_index(y0 + 1, image.height);

  const uint8_t* row0 = &image.rgba[static_cast<size_t>(y0) * image.width * 4];
  const uint8_t* row1 = &image.rgba[static_cast<size_t>(y1) * image.width * 4];
  const uint8_t* p00 = row0 + static_cast<size_t>(x0) * 4;
  const uint8_t* p10 = row0 + static_cast<size_t>(x1) * 4;
  const uint8_t* p01 = row1 + static_cast<size_t>(x0) * 4;
  const uint8_t* p11 = row1 + static_cast<size_t>(x1) * 4;

  for (int i = 0; i < 4; ++i) {
    const float top = p00[i] + (p10[i] - p00[i]) * tx;
    const float bottom = p01[i] + (p11[i] - p01[i]) * tx;
    out[i] = top + (bottom - top) * ty;
  }
}

// Resolves a material's texmap through its texture entry and the model's
// texture attributes, the same chain render::material_texture walks.
const gcn::Image* material_image(const cine::ModInventory& model,
                                 const std::vector<gcn::Image>& images,
                                 const cine::Material& material, size_t texmap) {
  if (texmap >= material.textures.size()) {
    return nullptr;
  }
  const uint32_t attribute = material.textures[texmap].source_attribute;
  if (attribute >= model.texture_attributes.size()) {
    return nullptr;
  }
  const int index = model.texture_attributes[attribute].texture_index;
  if (index < 0 || static_cast<size_t>(index) >= images.size()) {
    return nullptr;
  }
  const gcn::Image& image = images[static_cast<size_t>(index)];
  return image.valid() ? &image : nullptr;
}

// Everything one batch needs to shade a pixel, resolved once per batch so the
// inner loop only does arithmetic.
struct Shading {
  const cine::TevInfo* tev;
  // Image bound to each GX texture map, or null when the material leaves it
  // unbound.
  const gcn::Image* texmap[8];
  // Which vertex texcoord set feeds each generated coordinate, and the 2x4
  // matrix applied to it. Derived from the material's texgens.
  int coord_source[kMaxTexCoords];
  float coord_matrix[kMaxTexCoords][16];
  int coord_count;
  float konst[4][4];
  // GX_TEVPREV, GX_TEVREG0, GX_TEVREG1, GX_TEVREG2 at stage 0.
  float registers[4][4];
  // Each stage's konst selection resolved once per batch rather than per pixel.
  float stage_konst_color[16][3];
  float stage_konst_alpha[16];
};

// GX_TEV_KCSEL. Selectors below 8 are constant fractions; 0x0C..0x0F take a
// konst colour whole; 0x10 and up broadcast one of its channels.
void konst_color(uint8_t selector, const float konst[4][4], float out[3]) {
  float value = 0.0f;
  if (selector < 8) {
    value = static_cast<float>(8 - selector) / 8.0f;
  } else if (selector >= 0x0c && selector <= 0x0f) {
    const int index = selector - 0x0c;
    for (int i = 0; i < 3; ++i) {
      out[i] = konst[index][i];
    }
    return;
  } else if (selector >= 0x10 && selector <= 0x1f) {
    const int channel = (selector - 0x10) / 4;
    const int index = (selector - 0x10) % 4;
    value = konst[index][channel];
  }
  for (int i = 0; i < 3; ++i) {
    out[i] = value;
  }
}

// GX_TEV_KASEL. Alpha needs a scalar, so there is no whole-colour selector.
float konst_alpha(uint8_t selector, const float konst[4][4]) {
  if (selector < 8) {
    return static_cast<float>(8 - selector) / 8.0f;
  }
  if (selector >= 0x10 && selector <= 0x1f) {
    const int channel = (selector - 0x10) / 4;
    const int index = (selector - 0x10) % 4;
    return konst[index][channel];
  }
  return 0.0f;
}

// GXTevColorArg. Registers are indexed by GX register id, so entry 0 is
// GX_TEVPREV and the alpha selectors read channel 3 of the same register.
void color_arg(uint8_t selector, const float registers[4][4], const float texel[4],
               const float raster[4], const float konst[3], float out[3]) {
  float value = 0.0f;
  switch (selector) {
    case 0: case 2: case 4: case 6: {
      const int reg = selector / 2;
      for (int i = 0; i < 3; ++i) {
        out[i] = registers[reg][i];
      }
      return;
    }
    case 1: case 3: case 5: case 7:
      value = registers[selector / 2][3];
      break;
    case 8:
      for (int i = 0; i < 3; ++i) {
        out[i] = texel[i];
      }
      return;
    case 9:
      value = texel[3];
      break;
    case 10:
      for (int i = 0; i < 3; ++i) {
        out[i] = raster[i];
      }
      return;
    case 11:
      value = raster[3];
      break;
    case 12:
      value = 1.0f;
      break;
    case 13:
      value = 0.5f;
      break;
    case 14:
      for (int i = 0; i < 3; ++i) {
        out[i] = konst[i];
      }
      return;
    default:
      value = 0.0f;
      break;
  }
  for (int i = 0; i < 3; ++i) {
    out[i] = value;
  }
}

// GXTevAlphaArg.
float alpha_arg(uint8_t selector, const float registers[4][4], const float texel[4],
                const float raster[4], float konst) {
  switch (selector) {
    case 0: return registers[0][3];
    case 1: return registers[1][3];
    case 2: return registers[2][3];
    case 3: return registers[3][3];
    case 4: return texel[3];
    case 5: return raster[3];
    case 6: return konst;
    default: return 0.0f;
  }
}

// out = (d +/- lerp(a, b, c) + bias) * scale, optionally saturated. This is the
// combiner GX evaluates for both halves of every stage.
float combine(float a, float b, float c, float d, const cine::TevCombiner& op) {
  const float bias = op.bias == 1 ? 0.5f : (op.bias == 2 ? -0.5f : 0.0f);
  float scale = 1.0f;
  if (op.scale == 1) {
    scale = 2.0f;
  } else if (op.scale == 2) {
    scale = 4.0f;
  } else if (op.scale == 3) {
    scale = 0.5f;
  }
  const float mixed = a * (1.0f - c) + b * c;
  // GX_TEV_SUB is the only other arithmetic op these materials use; the
  // comparison ops leave the formula alone in the absence of one.
  float value = (op.op == 1) ? (d - mixed + bias) : (d + mixed + bias);
  value *= scale;
  if (op.clamp != 0) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
  }
  return value;
}

// Runs the material's whole TEV chain for one pixel. Inputs and output are
// 0..1. Without this the renderer only ever reproduced stage 0, which for the
// terrain is the unshadowed texture and is why the scene came out too bright:
// its later stages apply a multiplicative darkening term.
void evaluate_tev(const Shading& shading, const float coords[kMaxTexCoords][2],
                  const float raster[4], float out[4]) {
  float registers[4][4];
  for (int r = 0; r < 4; ++r) {
    for (int i = 0; i < 4; ++i) {
      registers[r][i] = shading.registers[r][i];
    }
  }

  // Several stages resample the same map through the same coordinate -- the
  // terrain reads texmap 0 in stages 0 and 2 -- so each combination is
  // filtered once per pixel and reused.
  float cache_value[8][4];
  int cache_coord[8];
  for (int i = 0; i < 8; ++i) {
    cache_coord[i] = -1;
  }

  const std::vector<cine::TevStage>& stages = shading.tev->stages;
  const size_t stage_count = stages.size() < 16 ? stages.size() : 16;
  for (size_t s = 0; s < stage_count; ++s) {
    const cine::TevStage& stage = stages[s];

    // An unbound map stands in as white so a modulate passes the raster colour
    // through rather than multiplying it to black.
    float texel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (stage.texmap < 8 && shading.texmap[stage.texmap] != nullptr &&
        stage.texcoord < kMaxTexCoords) {
      const int map = stage.texmap;
      if (cache_coord[map] != static_cast<int>(stage.texcoord)) {
        float raw[4];
        sample(*shading.texmap[map], coords[stage.texcoord][0], coords[stage.texcoord][1], raw);
        for (int i = 0; i < 4; ++i) {
          cache_value[map][i] = raw[i] * (1.0f / 255.0f);
        }
        cache_coord[map] = static_cast<int>(stage.texcoord);
      }
      for (int i = 0; i < 4; ++i) {
        texel[i] = cache_value[map][i];
      }
    }

    const float* kc = shading.stage_konst_color[s];
    const float ka = shading.stage_konst_alpha[s];

    float a[3];
    float b[3];
    float c[3];
    float d[3];
    color_arg(stage.color.a, registers, texel, raster, kc, a);
    color_arg(stage.color.b, registers, texel, raster, kc, b);
    color_arg(stage.color.c, registers, texel, raster, kc, c);
    color_arg(stage.color.d, registers, texel, raster, kc, d);

    const float alpha_a = alpha_arg(stage.alpha.a, registers, texel, raster, ka);
    const float alpha_b = alpha_arg(stage.alpha.b, registers, texel, raster, ka);
    const float alpha_c = alpha_arg(stage.alpha.c, registers, texel, raster, ka);
    const float alpha_d = alpha_arg(stage.alpha.d, registers, texel, raster, ka);

    // Both halves read the registers before either writes, so the results are
    // staged and only then committed.
    float color_result[3];
    for (int i = 0; i < 3; ++i) {
      color_result[i] = combine(a[i], b[i], c[i], d[i], stage.color);
    }
    const float alpha_result = combine(alpha_a, alpha_b, alpha_c, alpha_d, stage.alpha);

    const int color_reg = stage.color.out_reg < 4 ? stage.color.out_reg : 0;
    const int alpha_reg = stage.alpha.out_reg < 4 ? stage.alpha.out_reg : 0;
    for (int i = 0; i < 3; ++i) {
      registers[color_reg][i] = color_result[i];
    }
    registers[alpha_reg][3] = alpha_result;
  }

  // The pixel engine takes GX_TEVPREV.
  for (int i = 0; i < 4; ++i) {
    out[i] = registers[0][i];
  }
}

void raster_triangle(Target& target, const Shaded& a, const Shaded& b, const Shaded& c,
                     const Shading& shading, int row_begin, int row_end) {
  const Shaded* tri[3] = {&a, &b, &c};
  float sx[3];
  float sy[3];
  float sz[3];
  float inv_w[3];
  for (int i = 0; i < 3; ++i) {
    inv_w[i] = 1.0f / tri[i]->position[3];
    sx[i] = (tri[i]->position[0] * inv_w[i] * 0.5f + 0.5f) * target.width;
    sy[i] = (1.0f - (tri[i]->position[1] * inv_w[i] * 0.5f + 0.5f)) * target.height;
    sz[i] = tri[i]->position[2] * inv_w[i];
  }

  const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (std::fabs(area) < 1e-9f) {
    return;
  }

  int min_x = static_cast<int>(std::floor(std::fmin(sx[0], std::fmin(sx[1], sx[2]))));
  int max_x = static_cast<int>(std::ceil(std::fmax(sx[0], std::fmax(sx[1], sx[2]))));
  int min_y = static_cast<int>(std::floor(std::fmin(sy[0], std::fmin(sy[1], sy[2]))));
  int max_y = static_cast<int>(std::ceil(std::fmax(sy[0], std::fmax(sy[1], sy[2]))));
  if (min_x < 0) min_x = 0;
  if (min_y < row_begin) min_y = row_begin;
  if (max_x > target.width - 1) max_x = target.width - 1;
  if (max_y > row_end - 1) max_y = row_end - 1;

  const float inv_area = 1.0f / area;
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv_area;
      const float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv_area;
      const float w2 = 1.0f - w0 - w1;
      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
        continue;
      }

      const float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
      const size_t offset = static_cast<size_t>(y) * target.width + x;
      if (z >= target.depth[offset]) {
        continue;
      }

      // Perspective-correct attributes.
      const float sum = w0 * inv_w[0] + w1 * inv_w[1] + w2 * inv_w[2];
      const float inv_sum = 1.0f / sum;
      const float p0 = w0 * inv_w[0] * inv_sum;
      const float p1 = w1 * inv_w[1] * inv_sum;
      const float p2 = w2 * inv_w[2] * inv_sum;

      float raster_color[4];
      for (int i = 0; i < 4; ++i) {
        raster_color[i] =
            (p0 * tri[0]->color[i] + p1 * tri[1]->color[i] + p2 * tri[2]->color[i]) *
            (1.0f / 255.0f);
      }

      float coords[kMaxTexCoords][2];
      for (int t = 0; t < shading.coord_count; ++t) {
        for (int i = 0; i < 2; ++i) {
          coords[t][i] = p0 * tri[0]->texcoord[t][i] + p1 * tri[1]->texcoord[t][i] +
                         p2 * tri[2]->texcoord[t][i];
        }
      }
      for (int t = shading.coord_count; t < kMaxTexCoords; ++t) {
        coords[t][0] = coords[t][1] = 0.0f;
      }

      float unit[4];
      evaluate_tev(shading, coords, raster_color, unit);

      float color[4];
      for (int i = 0; i < 4; ++i) {
        color[i] = unit[i] * 255.0f;
      }

      // Stands in for the materials' alpha test, which is what keeps the
      // cut-out leaves and flower petals from drawing as opaque quads.
      if (color[3] < 8.0f) {
        continue;
      }

      uint8_t* destination = &target.rgba[offset * 4];
      const float alpha = color[3] * (1.0f / 255.0f);
      if (alpha >= 0.99f) {
        target.depth[offset] = z;
        for (int i = 0; i < 3; ++i) {
          float value = color[i];
          if (value < 0.0f) value = 0.0f;
          if (value > 255.0f) value = 255.0f;
          destination[i] = static_cast<uint8_t>(value);
        }
      } else {
        // Each flower in logo.mod lays down its shadow as a separate material
        // that is near-black at a quarter alpha. Writing it opaque, as an alpha
        // test alone does, floods the gaps between petals with solid black
        // instead of darkening the ground showing through them.
        //
        // Depth is deliberately not written: a translucent surface must not
        // occlude what is behind it.
        for (int i = 0; i < 3; ++i) {
          float value = color[i] * alpha + destination[i] * (1.0f - alpha);
          if (value < 0.0f) value = 0.0f;
          if (value > 255.0f) value = 255.0f;
          destination[i] = static_cast<uint8_t>(value);
        }
      }
      destination[3] = 255;
    }
  }
}

// Clips against the near plane so geometry behind the eye does not wrap around
// after the perspective divide. The camera descends into the scene, so by the
// settled frame some of it is genuinely behind.
void clip_and_raster(Target& target, Shaded* input, const Shading& shading, int row_begin,
                     int row_end) {
  const float kNear = 1e-4f;
  Shaded polygon[4];
  int count = 0;

  for (int i = 0; i < 3; ++i) {
    const Shaded& current = input[i];
    const Shaded& next = input[(i + 1) % 3];
    const bool current_in = current.position[3] > kNear;
    const bool next_in = next.position[3] > kNear;

    if (current_in) {
      polygon[count++] = current;
    }
    if (current_in != next_in) {
      const float t = (kNear - current.position[3]) / (next.position[3] - current.position[3]);
      polygon[count++] = lerp_shaded(current, next, t);
    }
  }

  if (count < 3) {
    return;
  }
  raster_triangle(target, polygon[0], polygon[1], polygon[2], shading, row_begin, row_end);
  if (count == 4) {
    raster_triangle(target, polygon[0], polygon[2], polygon[3], shading, row_begin, row_end);
  }
}

// A plain modulate, for the materials that carry no TEV configuration at all.
// Matches what the renderer did for every material before the chain was
// evaluated: texture times raster colour.
const cine::TevInfo& fallback_tev() {
  static cine::TevInfo info;
  static bool ready = false;
  if (!ready) {
    info.stage_count = 1;
    for (int r = 0; r < 3; ++r) {
      for (int i = 0; i < 4; ++i) {
        info.registers[r][i] = 0;
      }
    }
    for (int k = 0; k < 4; ++k) {
      for (int i = 0; i < 4; ++i) {
        info.konst[k][i] = 255;
      }
    }
    cine::TevStage stage;
    stage.texcoord = 0;
    stage.texmap = 0;
    stage.channel = 4;
    stage.konst_color = 12;
    stage.konst_alpha = 28;
    // ZERO, TEXC, RASC, ZERO -> lerp picks TEXC * RASC.
    stage.color.a = 15;
    stage.color.b = 8;
    stage.color.c = 10;
    stage.color.d = 15;
    stage.color.op = 0;
    stage.color.bias = 0;
    stage.color.scale = 0;
    stage.color.clamp = 1;
    stage.color.out_reg = 0;
    stage.alpha.a = 7;
    stage.alpha.b = 4;
    stage.alpha.c = 5;
    stage.alpha.d = 7;
    stage.alpha.op = 0;
    stage.alpha.bias = 0;
    stage.alpha.scale = 0;
    stage.alpha.clamp = 1;
    stage.alpha.out_reg = 0;
    for (int i = 0; i < 12; ++i) {
      stage.color_combiner[i] = 0;
      stage.alpha_combiner[i] = 0;
    }
    info.stages.assign(1, stage);
    ready = true;
  }
  return info;
}

// Resolves a material into everything the pixel loop needs: its bound images,
// how each texture coordinate is generated, and its TEV constants.
Shading build_shading(const cine::ModInventory& model, const std::vector<gcn::Image>& images,
                      const cine::Material& material, float frame) {
  Shading shading;
  shading.tev = (material.tev_info_index >= 0 &&
                 static_cast<size_t>(material.tev_info_index) < model.tev_infos.size())
                    ? &model.tev_infos[static_cast<size_t>(material.tev_info_index)]
                    : &fallback_tev();

  for (int i = 0; i < 8; ++i) {
    shading.texmap[i] = static_cast<size_t>(i) < material.textures.size()
                            ? material_image(model, images, material, static_cast<size_t>(i))
                            : nullptr;
  }

  for (int c = 0; c < kMaxTexCoords; ++c) {
    shading.coord_source[c] = -1;
    for (int i = 0; i < 16; ++i) {
      shading.coord_matrix[c][i] = 0.0f;
    }
    shading.coord_matrix[c][0] = shading.coord_matrix[c][5] = 1.0f;
  }
  shading.coord_count = 1;

  // GX_TG_TEX0 is source 4, so a texgen reading vertex set n arrives as 4 + n.
  // The matrix field indexes the material's textures; anything past them, such
  // as the terrain's 10, means no transform.
  for (size_t g = 0; g < material.texgens.size(); ++g) {
    const cine::TexGen& texgen = material.texgens[g];
    if (texgen.coord >= kMaxTexCoords) {
      continue;
    }
    const int set = static_cast<int>(texgen.source) - 4;
    shading.coord_source[texgen.coord] = (set >= 0 && set < 2) ? set : -1;
    if (texgen.matrix < material.textures.size()) {
      cine::texture_matrix(material.textures[texgen.matrix], frame,
                           shading.coord_matrix[texgen.coord]);
    }
    if (static_cast<int>(texgen.coord) + 1 > shading.coord_count) {
      shading.coord_count = static_cast<int>(texgen.coord) + 1;
    }
  }
  // Coordinate 0 falls back to vertex set 0 for materials that declare no
  // texgens but still sample a texture.
  if (shading.coord_source[0] < 0 && material.texgens.empty()) {
    shading.coord_source[0] = 0;
  }

  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      shading.konst[k][i] = static_cast<float>(shading.tev->konst[k][i]) * (1.0f / 255.0f);
    }
  }

  // GX_TEVPREV starts cleared; GX_TEVREG0..2 come from the material, stored as
  // signed 10-bit channels.
  for (int i = 0; i < 4; ++i) {
    shading.registers[0][i] = 0.0f;
  }
  for (int r = 0; r < 3; ++r) {
    for (int i = 0; i < 4; ++i) {
      shading.registers[r + 1][i] =
          static_cast<float>(shading.tev->registers[r][i]) * (1.0f / 255.0f);
    }
  }

  for (int s = 0; s < 16; ++s) {
    shading.stage_konst_color[s][0] = shading.stage_konst_color[s][1] =
        shading.stage_konst_color[s][2] = 0.0f;
    shading.stage_konst_alpha[s] = 0.0f;
  }
  const size_t stage_count =
      shading.tev->stages.size() < 16 ? shading.tev->stages.size() : 16;
  for (size_t s = 0; s < stage_count; ++s) {
    konst_color(shading.tev->stages[s].konst_color, shading.konst,
                shading.stage_konst_color[s]);
    shading.stage_konst_alpha[s] =
        konst_alpha(shading.tev->stages[s].konst_alpha, shading.konst);
  }
  return shading;
}

}  // namespace

void Target::reset(int new_width, int new_height) {
  width = new_width;
  height = new_height;
  rgba.assign(static_cast<size_t>(width) * height * 4, 0);
  depth.assign(static_cast<size_t>(width) * height, 1e30f);
  for (size_t i = 3; i < rgba.size(); i += 4) {
    rgba[i] = 255;
  }
}

float camera_field(const cine::Dsk& dsk, const char* name, float frame, float fallback) {
  if (dsk.cameras.empty()) {
    return fallback;
  }
  const cine::DskTable& camera = dsk.cameras[0];
  for (size_t i = 0; i < camera.fields.size(); ++i) {
    if (camera.fields[i].name == name) {
      return cine::evaluate(camera.fields[i].param, dsk.camera_values, frame);
    }
  }
  return fallback;
}

render::Mat4 title_view_projection(const cine::Dsk& dsk, float frame, int target_width,
                                   int target_height) {
  const float eye[3] = {
      camera_field(dsk, "cam_pos_x", frame, 0.0f),
      camera_field(dsk, "cam_pos_y", frame, 402.0f),
      camera_field(dsk, "cam_pos_z", frame, 20.0f),
  };
  const float target[3] = {
      camera_field(dsk, "cam_lat_x", frame, 0.0f),
      camera_field(dsk, "cam_lat_y", frame, 0.0f),
      camera_field(dsk, "cam_lat_z", frame, 0.0f),
  };
  // Matrix4f::makeLookat derives its own up when CamDataInfo passes none, from
  // the view direction's XZ heading rather than a world axis, so the camera
  // looking almost straight down at the landing site stays well defined.
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const float fovy = camera_field(dsk, "cam_fovy", frame, 41.5f) * kPi / 180.0f;

  // CamDataInfo never samples the file's cam_near and cam_far, keeping its
  // constructor's 1.0 and 15000.0 instead; the .dsk asks for 0.1 to 32768.
  const render::Mat4 projection = render::perspective(fovy, 4.0f / 3.0f, 1.0f, 15000.0f);
  const render::Mat4 fit = render::pillarbox_ndc(target_width, target_height, 4, 3);
  return render::multiply(fit,
                          render::multiply(projection, render::look_at(eye, target, up)));
}

bool Actor::load(const std::string& mod_path, const std::string& anm_path, const char* label,
                 std::string& error) {
  const std::vector<uint8_t> bytes = cine::read_file(mod_path);
  model = cine::parse_mod(bytes, mod_path);
  geometry = cine::build_geometry(bytes, model, mod_path);
  animation = render::load_dck(anm_path, label);

  images.resize(model.textures.size());
  for (size_t i = 0; i < model.textures.size(); ++i) {
    const cine::ModTexture& source = model.textures[i];
    if (!gcn::decode_texture_payload(static_cast<gcn::TexFormat>(source.format), source.width,
                                     source.height, source.data.data(), source.data.size(),
                                     images[i], error)) {
      return false;
    }
  }
  return true;
}

long Actor::draw(Target& target, const render::Mat4& view_projection, float frame, int row_begin,
                 int row_end) const {
  if (row_end < 0 || row_end > target.height) {
    row_end = target.height;
  }
  if (row_begin < 0) {
    row_begin = 0;
  }
  if (row_begin >= row_end) {
    return 0;
  }

  const std::vector<render::Mat4> joints = render::animated_joint_world_matrices(
      model, animation,
      loops ? render::wrap_frame(frame, animation) : render::clamp_frame(frame, animation));

  long drawn = 0;
  for (size_t b = 0; b < geometry.batches.size(); ++b) {
    const cine::GeoBatch& batch = geometry.batches[b];
    if (batch.material_index < 0 ||
        static_cast<size_t>(batch.material_index) >= model.materials.size()) {
      continue;
    }
    const cine::Material& material = model.materials[static_cast<size_t>(batch.material_index)];
    const Shading shading = build_shading(model, images, material, frame);

    // The raster colour is the vertex colour times the material colour, as the
    // vertex stage computes it. Most of logo.mod carries no texture at all --
    // its one texture is the copyright strip -- so the flowers' white petals
    // and yellow centres are entirely this material colour. Dropping it renders
    // every flower flat white.
    float material_color[4];
    for (int channel = 0; channel < 4; ++channel) {
      material_color[channel] = static_cast<float>(material.color[channel]) / 255.0f;
    }

    render::Mat4 world = render::identity();
    if (batch.parent_joint >= 0 && static_cast<size_t>(batch.parent_joint) < joints.size()) {
      world = joints[static_cast<size_t>(batch.parent_joint)];
    }
    const render::Mat4 mvp = render::multiply(view_projection, world);

    for (uint32_t i = 0; i + 2 < batch.index_count; i += 3) {
      Shaded vertices[3];
      bool ok = true;
      for (int k = 0; k < 3; ++k) {
        const uint32_t index = geometry.indices[batch.first_index + i + k];
        if (index >= geometry.vertices.size()) {
          ok = false;
          break;
        }
        const cine::GeoVertex& source = geometry.vertices[index];
        render::transform_point(mvp, source.position, vertices[k].position);

        // Each generated coordinate takes its vertex set through the texgen's
        // 2x4 matrix, whose rows 0 and 1 hold the transform.
        for (int c = 0; c < kMaxTexCoords; ++c) {
          const int set = shading.coord_source[c];
          const float u = set >= 0 ? source.texcoord[set][0] : 0.0f;
          const float v = set >= 0 ? source.texcoord[set][1] : 0.0f;
          const float* m = shading.coord_matrix[c];
          vertices[k].texcoord[c][0] = m[0] * u + m[1] * v + m[3];
          vertices[k].texcoord[c][1] = m[4] * u + m[5] * v + m[7];
        }

        for (int channel = 0; channel < 4; ++channel) {
          vertices[k].color[channel] =
              static_cast<float>(source.color[channel]) * material_color[channel];
        }
      }
      if (!ok) {
        continue;
      }
      clip_and_raster(target, vertices, shading, row_begin, row_end);
      ++drawn;
    }
  }
  return drawn;
}

}  // namespace raster
