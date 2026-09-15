// Host-side verification tool for the disc asset decoders.
//
// Renders a .blo screen layout, or a single .bti/.txe texture, into an
// uncompressed TGA so the port can be checked against the real game without a
// Vita or an emulator in the loop.
//
//   blo_render tex     <file.bti|file.txe>        <out.tga>
//   blo_render screen  <file.blo> <texture-dir>   <out.tga> [selected]
//   blo_render logo    <file.bti|file.txe>        <out.tga> [fade]
//   blo_render tree    <file.blo>
//
// "screen" goes through the same screen::flatten / screen::apply_menu_rules
// path as the Vita build, and "logo" through the same loadlogo::rect and
// loadlogo::color, so a preview matches what the handheld draws.

#include "blo.hpp"
#include "gcn_font.hpp"
#include "gcn_texture.hpp"
#include "load_logo.hpp"
#include "screen.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Canvas {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;

  Canvas(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * h * 4, 0) {}

  void blend(int x, int y, const uint8_t* source) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return;
    }
    uint8_t* destination = &rgba[(static_cast<size_t>(y) * width + x) * 4];
    const int alpha = source[3];
    if (alpha == 0) {
      return;
    }
    if (alpha == 255) {
      std::memcpy(destination, source, 4);
      return;
    }
    for (int i = 0; i < 3; ++i) {
      destination[i] =
          static_cast<uint8_t>((source[i] * alpha + destination[i] * (255 - alpha)) / 255);
    }
    destination[3] = static_cast<uint8_t>(alpha + destination[3] * (255 - alpha) / 255);
  }
};

bool write_tga(const std::string& path, const Canvas& canvas) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "could not write %s\n", path.c_str());
    return false;
  }

  uint8_t header[18] = {0};
  header[2] = 2; // uncompressed true-colour
  header[12] = static_cast<uint8_t>(canvas.width & 0xFF);
  header[13] = static_cast<uint8_t>((canvas.width >> 8) & 0xFF);
  header[14] = static_cast<uint8_t>(canvas.height & 0xFF);
  header[15] = static_cast<uint8_t>((canvas.height >> 8) & 0xFF);
  header[16] = 32;
  header[17] = 0x28; // 8 alpha bits, top-left origin
  std::fwrite(header, 1, sizeof(header), file);

  std::vector<uint8_t> row(static_cast<size_t>(canvas.width) * 4);
  for (int y = 0; y < canvas.height; ++y) {
    for (int x = 0; x < canvas.width; ++x) {
      const uint8_t* pixel = &canvas.rgba[(static_cast<size_t>(y) * canvas.width + x) * 4];
      row[x * 4 + 0] = pixel[2];
      row[x * 4 + 1] = pixel[1];
      row[x * 4 + 2] = pixel[0];
      row[x * 4 + 3] = pixel[3];
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }

  std::fclose(file);
  return true;
}

class TextureCache {
public:
  explicit TextureCache(std::string directory) : directory_(std::move(directory)) {}

  const gcn::Image* get(const std::string& name) {
    auto found = images_.find(name);
    if (found != images_.end()) {
      return found->second.valid() ? &found->second : nullptr;
    }

    gcn::Image image;
    std::string error;
    const std::string path = directory_ + "/" + name;
    if (!gcn::decode_texture_file(path, image, error)) {
      std::fprintf(stderr, "  ! %s: %s\n", name.c_str(), error.c_str());
      images_[name] = gcn::Image();
      return nullptr;
    }
    std::printf("  loaded %-16s %3dx%-3d fmt %d\n", name.c_str(), image.width, image.height,
                static_cast<int>(image.source_format));
    images_[name] = std::move(image);
    return &images_[name];
  }

private:
  std::string directory_;
  std::map<std::string, gcn::Image> images_;
};

// Nearest-neighbour stretch of a decoded texture across the pane's bounds,
// which is what the GX quad in P2DPicture::drawTexCoord effectively does.
void draw_picture(Canvas& canvas, const gcn::Image& image, int x, int y, int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  for (int row = 0; row < height; ++row) {
    const int source_y = image.height * row / height;
    for (int column = 0; column < width; ++column) {
      const int source_x = image.width * column / width;
      const uint8_t* pixel =
          &image.rgba[(static_cast<size_t>(source_y) * image.width + source_x) * 4];
      canvas.blend(x + column, y + row, pixel);
    }
  }
}

uint8_t lerp8(uint8_t a, uint8_t b, float t) {
  return static_cast<uint8_t>(a + (static_cast<float>(b) - a) * t + 0.5f);
}

// GX draws glyphs with GX_MODULATE, so the atlas intensity scales a vertex
// colour that runs from `top` at the quad's top edge to `bottom` at its base.
void draw_glyph_quad(Canvas& canvas, const gcn::Image& atlas, const gcn::GlyphQuad& quad,
                     const blo::Color& top, const blo::Color& bottom) {
  const int x0 = static_cast<int>(quad.x0 + 0.5f);
  const int y0 = static_cast<int>(quad.y0 + 0.5f);
  const int width = static_cast<int>(quad.x1 + 0.5f) - x0;
  const int height = static_cast<int>(quad.y1 + 0.5f) - y0;
  if (width <= 0 || height <= 0) {
    return;
  }

  for (int row = 0; row < height; ++row) {
    const float v = static_cast<float>(row) / static_cast<float>(height);
    const int source_y = quad.source_y + quad.source_height * row / height;
    const uint8_t r = lerp8(top.r, bottom.r, v);
    const uint8_t g = lerp8(top.g, bottom.g, v);
    const uint8_t b = lerp8(top.b, bottom.b, v);
    const uint8_t a = lerp8(top.a, bottom.a, v);

    for (int column = 0; column < width; ++column) {
      const int source_x = quad.source_x + quad.source_width * column / width;
      if (source_x >= atlas.width || source_y >= atlas.height) {
        continue;
      }
      const uint8_t* source =
          &atlas.rgba[(static_cast<size_t>(source_y) * atlas.width + source_x) * 4];
      const uint8_t intensity = source[0];
      uint8_t pixel[4] = {static_cast<uint8_t>(r * intensity / 255),
                          static_cast<uint8_t>(g * intensity / 255),
                          static_cast<uint8_t>(b * intensity / 255),
                          static_cast<uint8_t>(source[3] * a / 255)};
      canvas.blend(x0 + column, y0 + row, pixel);
    }
  }
}

void draw_text(Canvas& canvas, const gcn::Font& font, const screen::Text& text) {
  const int draw_width = text.font_width > 0 ? text.font_width : font.cell_width();
  const int draw_height = text.font_height > 0 ? text.font_height : font.cell_height();

  const float run_width = font.advance_width(text.text, draw_width);
  float x = static_cast<float>(text.x);
  switch (text.align_h) {
  case 0: // TBOXHBIND_Center
    x += (text.width - run_width) * 0.5f;
    break;
  case 1: // TBOXHBIND_Right
    x += text.width - run_width;
    break;
  default: // TBOXHBIND_Left
    break;
  }

  // Glyphs hang above the baseline, so place it at the bottom of the box.
  const float baseline = text.y + (text.height + draw_height) * 0.5f;

  std::vector<gcn::GlyphQuad> quads;
  font.layout(text.text, x, baseline, draw_width, draw_height, quads);
  for (const gcn::GlyphQuad& quad : quads) {
    draw_glyph_quad(canvas, font.atlas(), quad, text.color, text.grad_color);
  }
}

void render_list(Canvas& canvas, const screen::DrawList& list, TextureCache& textures,
                 const gcn::Font* font) {
  for (const screen::Quad& quad : list.quads) {
    if (const gcn::Image* image = textures.get(quad.texture)) {
      draw_picture(canvas, *image, quad.x, quad.y, quad.width, quad.height);
    }
  }
  for (const screen::Text& text : list.texts) {
    std::printf("  text  tag=%-4s \"%s\" at %d,%d %dx%d\n", text.tag.c_str(), text.text.c_str(),
                text.x, text.y, text.width, text.height);
    if (font != nullptr && font->valid()) {
      draw_text(canvas, *font, text);
    }
  }
}

// GX modulates the logo quad by a flat vertex colour, which is what
// vita2d_draw_texture_tint_scale does on the Vita side.
void draw_tinted(Canvas& canvas, const gcn::Image& image, const loadlogo::Rect& rect,
                 const loadlogo::Color& tint) {
  for (int row = 0; row < rect.height && row < image.height; ++row) {
    for (int column = 0; column < rect.width && column < image.width; ++column) {
      const uint8_t* source =
          &image.rgba[(static_cast<size_t>(row) * image.width + column) * 4];
      uint8_t pixel[4] = {static_cast<uint8_t>(source[0] * tint.r / 255),
                          static_cast<uint8_t>(source[1] * tint.g / 255),
                          static_cast<uint8_t>(source[2] * tint.b / 255),
                          static_cast<uint8_t>(source[3] * tint.a / 255)};
      canvas.blend(rect.x + column, rect.y + row, pixel);
    }
  }
}

void print_tree(const blo::Pane& pane, int depth) {
  const char* kind = "pane";
  switch (pane.type) {
  case blo::PANETYPE_Screen:
    kind = "screen";
    break;
  case blo::PANETYPE_Picture:
    kind = "picture";
    break;
  case blo::PANETYPE_TextBox:
    kind = "textbox";
    break;
  case blo::PANETYPE_Window:
    kind = "window";
    break;
  default:
    break;
  }

  std::printf("%*s%-7s tag=%-4s %4d,%-4d %4dx%-4d%s", depth * 2, "", kind, pane.tag.c_str(), pane.x,
              pane.y, pane.width, pane.height, pane.visible ? "" : " (hidden)");
  if (!pane.texture.empty()) {
    std::printf(" tex=%s", pane.texture.c_str());
  }
  if (!pane.font.empty()) {
    std::printf(" font=%s", pane.font.c_str());
  }
  if (!pane.text.empty()) {
    std::printf(" text=\"%s\"", pane.text.c_str());
  }
  if (pane.type == blo::PANETYPE_TextBox) {
    // The menu's selection blend is driven entirely by these, so seeing them
    // is how it can be told whether a fade will be visible at all.
    std::printf(" char=%u,%u,%u,%u grad=%u,%u,%u,%u", pane.char_color.r, pane.char_color.g,
                pane.char_color.b, pane.char_color.a, pane.grad_color.r, pane.grad_color.g,
                pane.grad_color.b, pane.grad_color.a);
  }
  std::printf("\n");

  for (const blo::Pane& child : pane.children) {
    print_tree(child, depth + 1);
  }
}

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  blo_render tex    <file.bti|file.txe> <out.tga>\n"
               "  blo_render screen <file.blo> <texture-dir> <out.tga>\n"
               "  blo_render logo   <file.bti|file.txe> <out.tga> [fade]\n"
               "  blo_render tree   <file.blo>\n");
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return usage();
  }
  const std::string mode = argv[1];

  if (mode == "tex" && argc == 4) {
    gcn::Image image;
    std::string error;
    if (!gcn::decode_texture_file(argv[2], image, error)) {
      std::fprintf(stderr, "%s: %s\n", argv[2], error.c_str());
      return 1;
    }
    std::printf("%s: %dx%d format %d\n", argv[2], image.width, image.height,
                static_cast<int>(image.source_format));

    Canvas canvas(image.width, image.height);
    canvas.rgba = image.rgba;
    return write_tga(argv[3], canvas) ? 0 : 1;
  }

  if (mode == "logo" && (argc == 4 || argc == 5)) {
    gcn::Image image;
    std::string error;
    if (!gcn::decode_texture_file(argv[2], image, error)) {
      std::fprintf(stderr, "%s: %s\n", argv[2], error.c_str());
      return 1;
    }

    const float fade = (argc == 5) ? static_cast<float>(std::atof(argv[4])) : 1.0f;
    const loadlogo::Rect rect = loadlogo::rect(image.width, image.height, screen::kOrthoWidth,
                                               screen::kOrthoHeight, true);
    const loadlogo::Color tint = loadlogo::color(fade, true);

    std::printf("%s: %dx%d format %d\n", argv[2], image.width, image.height,
                static_cast<int>(image.source_format));
    std::printf("  rect  %d,%d %dx%d on a %dx%d screen\n", rect.x, rect.y, rect.width,
                rect.height, screen::kOrthoWidth, screen::kOrthoHeight);
    std::printf("  tint  rgba(%d, %d, %d, %d) at fade %.3f\n", tint.r, tint.g, tint.b, tint.a,
                fade);

    // NinLogoSection clears to opaque black and draws nothing else.
    Canvas canvas(screen::kOrthoWidth, screen::kOrthoHeight);
    for (size_t i = 3; i < canvas.rgba.size(); i += 4) {
      canvas.rgba[i] = 255;
    }
    draw_tinted(canvas, image, rect, tint);
    return write_tga(argv[3], canvas) ? 0 : 1;
  }

  if (mode == "tree" && argc == 3) {
    blo::Pane root;
    std::string error;
    if (!blo::parse_file(argv[2], root, error)) {
      std::fprintf(stderr, "%s: %s\n", argv[2], error.c_str());
      return 1;
    }
    print_tree(root, 0);
    return 0;
  }

  if (mode == "screen" && (argc == 5 || argc == 6)) {
    blo::Pane root;
    std::string error;
    if (!blo::parse_file(argv[2], root, error)) {
      std::fprintf(stderr, "%s: %s\n", argv[2], error.c_str());
      return 1;
    }

    screen::DrawList list;
    screen::flatten(root, list);
    const int selected = (argc == 6) ? std::atoi(argv[5]) : 0;
    // A still shows the selection already fully blended, which is what the
    // menu settles to once its fade finishes.
    blo::Color blend_char;
    blo::Color blend_grad;
    screen::menu_blend_colors(root, blend_char, blend_grad);
    screen::MenuAnimation animation;
    animation.reset(screen::count_menu_items(root));
    animation.update(selected, screen::MenuAnimation::kBlendSeconds);
    screen::apply_menu_rules(list, selected, animation, blend_char, blend_grad);

    std::printf("%s: %dx%d screen, %d entries, selection %d\n", argv[2], list.width, list.height,
                screen::count_menu_items(root), selected);

    Canvas canvas(list.width, list.height);
    TextureCache textures(argv[3]);

    // bigFont.bti sits at the data root, one level above the texture directory.
    gcn::Font font;
    gcn::Image atlas;
    std::string font_error;
    const std::string font_path = std::string(argv[3]) + "/../../bigFont.bti";
    if (gcn::decode_texture_file(font_path, atlas, font_error) &&
        font.build(atlas, 21, 42, font_error)) {
      std::printf("  font  bigFont.bti %dx%d, cells %dx%d\n", atlas.width, atlas.height,
                  font.cell_width(), font.cell_height());
    } else {
      std::fprintf(stderr, "  ! font: %s\n", font_error.c_str());
    }

    render_list(canvas, list, textures, font.valid() ? &font : nullptr);
    return write_tga(argv[4], canvas) ? 0 : 1;
  }

  return usage();
}
