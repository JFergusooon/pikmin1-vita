#pragma once

// The game's proportional bitmap font, derived from a glyph atlas texture.
//
// Ports Font::setTexture / Font::stringWidth from
// upstream/pikmin/src/sysCommon/graphics.cpp. Pikmin's menus always use
// bigFont.bti as a 21x42 grid of 24x24 cells, regardless of the font name a
// layout asks for (see P2DFont::loadFont, which discards the name).
//
// Each cell's bottom scanline is not artwork: it encodes the glyph's baseline
// and advance in its alpha channel, so the visible glyph is one pixel shorter
// than the cell.

#include "gcn_texture.hpp"

#include <string>
#include <vector>

namespace gcn {

// Menus index glyphs as `character - 0x20`, so the atlas starts at space.
const int kFontFirstChar = 0x20;

struct Glyph {
  int texture_x = 0;
  int texture_y = 0;
  int width = 0;
  int height = 0;
  int advance = 0;     // FontChar::mCharSpacing
  int left_offset = 0; // FontChar::mLeftOffset

  // FontChar::mTextureCoords, which deliberately samples one row short.
  int coord_min_x = 0;
  int coord_min_y = 0;
  int coord_max_x = 0;
  int coord_max_y = 0;
};

// One glyph placed in screen space, with its source rect in the atlas.
struct GlyphQuad {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  int source_x = 0;
  int source_y = 0;
  int source_width = 0;
  int source_height = 0;
};

class Font {
public:
  // `cells_x` by `cells_y` grid; Pikmin passes 21 and 42.
  bool build(const Image& atlas, int cells_x, int cells_y, std::string& error);

  bool valid() const { return !glyphs_.empty(); }
  int cell_width() const { return cell_width_; }
  int cell_height() const { return cell_height_; }

  const Glyph* glyph(unsigned char character) const;

  // Advance width of `text` in atlas pixels, before any scaling.
  int string_width(const std::string& text) const;

  const Image& atlas() const { return atlas_; }

  // Total advance of `text` when drawn at `draw_width` per cell. Mirrors
  // P2DFont::getWidth, including its per-character +1 pixel.
  float advance_width(const std::string& text, int draw_width) const;

  // Places each glyph of `text`, with `baseline_y` as the pen baseline; glyphs
  // extend upward by the ascent. Mirrors P2DFont::drawChar.
  float layout(const std::string& text, float x, float baseline_y, int draw_width,
               int draw_height, std::vector<GlyphQuad>& out) const;

private:
  Image atlas_;
  std::vector<Glyph> glyphs_;
  int cell_width_ = 0;
  int cell_height_ = 0;
};

} // namespace gcn
