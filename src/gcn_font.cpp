#include "gcn_font.hpp"

namespace gcn {
namespace {

uint8_t alpha_at(const Image& image, int x, int y) {
  if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
    return 0;
  }
  return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + 3];
}

} // namespace

bool Font::build(const Image& atlas, int cells_x, int cells_y, std::string& error) {
  if (!atlas.valid() || cells_x <= 0 || cells_y <= 0) {
    error = "invalid font atlas";
    return false;
  }

  // Upstream divides width by the horizontal cell count and height by the
  // vertical one, which is why the arguments look transposed.
  cell_width_ = atlas.width / cells_x;
  cell_height_ = atlas.height / cells_y;
  if (cell_width_ <= 1 || cell_height_ <= 1) {
    error = "font cells too small";
    return false;
  }

  atlas_ = atlas;
  glyphs_.assign(static_cast<size_t>(cells_x) * static_cast<size_t>(cells_y), Glyph());

  // The last scanline of a cell carries metrics, so ink occupies rows
  // [0, cell_height_ - 1).
  const int ink_rows = cell_height_ - 1;

  size_t index = 0;
  for (int cell_y = 0; cell_y < cells_y; ++cell_y) {
    for (int cell_x = 0; cell_x < cells_x; ++cell_x, ++index) {
      const int origin_x = cell_x * cell_width_;
      const int origin_y = cell_y * cell_height_;

      // Walk in from the left while columns are entirely transparent.
      int left_edge = 0;
      for (int x = 0; x < cell_width_; ++x) {
        int clear = 0;
        for (int y = 0; y < ink_rows; ++y) {
          if (alpha_at(atlas, origin_x + x, origin_y + y) == 0) {
            ++clear;
          }
        }
        if (clear != ink_rows) {
          break;
        }
        left_edge = x;
      }

      // And in from the right.
      int right_edge = 0;
      for (int x = cell_width_ - 1; x >= 0; --x) {
        int clear = 0;
        for (int y = 0; y < ink_rows; ++y) {
          if (alpha_at(atlas, origin_x + x, origin_y + y) == 0) {
            ++clear;
          }
        }
        if (clear != ink_rows) {
          break;
        }
        right_edge = cell_width_ - x;
      }

      // The metrics row marks the advance as a transparent run: `baseline` is
      // where it starts and `baseline_end` where opaque pixels resume.
      int baseline = -1;
      int baseline_end = cell_width_;
      for (int x = 0; x < cell_width_; ++x) {
        const uint8_t alpha = alpha_at(atlas, origin_x + x, origin_y + cell_height_ - 1);
        if (baseline < 0) {
          if (alpha == 0) {
            baseline = x;
          }
        } else if (alpha != 0) {
          baseline_end = x;
          break;
        }
      }

      Glyph& glyph = glyphs_[index];
      glyph.advance = baseline_end - baseline;
      glyph.left_offset = baseline - left_edge;
      glyph.texture_x = origin_x + left_edge;
      glyph.texture_y = origin_y;
      glyph.width = cell_width_ - left_edge - right_edge;
      glyph.height = ink_rows;
      glyph.coord_min_x = glyph.texture_x;
      glyph.coord_min_y = glyph.texture_y;
      glyph.coord_max_x = glyph.texture_x + glyph.width;
      glyph.coord_max_y = glyph.texture_y + glyph.height - 1;
    }
  }

  return true;
}

float Font::advance_width(const std::string& text, int draw_width) const {
  if (cell_width_ <= 0) {
    return 0.0f;
  }
  const float scale = static_cast<float>(draw_width) / static_cast<float>(cell_width_);
  float width = 0.0f;
  for (char character : text) {
    if (const Glyph* found = glyph(static_cast<unsigned char>(character))) {
      // P2DFont::getWidth adds a pixel to every character.
      width += found->advance * scale + 1.0f;
    }
  }
  return width;
}

float Font::layout(const std::string& text, float x, float baseline_y, int draw_width,
                   int draw_height, std::vector<GlyphQuad>& out) const {
  out.clear();
  if (cell_width_ <= 0 || cell_height_ <= 0) {
    return 0.0f;
  }

  const float x_scale = static_cast<float>(draw_width) / static_cast<float>(cell_width_);
  const float y_scale = static_cast<float>(draw_height) / static_cast<float>(cell_height_);
  // P2DFont computes descent as height * 0.0f, so the baseline is the bottom.
  const float ascent = static_cast<float>(cell_height_);

  const float start = x;
  for (char character : text) {
    const Glyph* found = glyph(static_cast<unsigned char>(character));
    if (found == nullptr) {
      continue;
    }

    GlyphQuad quad;
    quad.x0 = x - found->left_offset * x_scale;
    quad.x1 = x + (found->width - found->left_offset) * x_scale;
    quad.y0 = baseline_y - ascent * y_scale;
    quad.y1 = baseline_y;
    quad.source_x = found->coord_min_x;
    quad.source_y = found->coord_min_y;
    quad.source_width = found->coord_max_x - found->coord_min_x;
    quad.source_height = found->coord_max_y - found->coord_min_y;
    if (quad.source_width > 0 && quad.source_height > 0) {
      out.push_back(quad);
    }

    x += found->advance * x_scale + 1.0f;
  }

  return x - start;
}

const Glyph* Font::glyph(unsigned char character) const {
  if (character < kFontFirstChar) {
    return nullptr;
  }
  const size_t index = static_cast<size_t>(character) - kFontFirstChar;
  if (index >= glyphs_.size()) {
    return nullptr;
  }
  return &glyphs_[index];
}

int Font::string_width(const std::string& text) const {
  int width = 0;
  for (char character : text) {
    if (const Glyph* found = glyph(static_cast<unsigned char>(character))) {
      width += found->advance;
    }
  }
  return width;
}

} // namespace gcn
