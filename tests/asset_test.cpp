// Host tests for the disc asset decoders.
//
// These use synthetic buffers so they run without game data. Decoding against
// the real disc is covered by tools/blo_render.

#include "blo.hpp"
#include "gcn_texture.hpp"
#include "load_logo.hpp"
#include "p2d_text.hpp"
#include "screen.hpp"
#include "title_animation.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

void check_eq(int actual, int expected, const char* what) {
  if (actual != expected) {
    std::printf("FAIL %s: expected %d, got %d\n", what, expected, actual);
    ++failures;
  }
}

void check_eq(const std::string& actual, const std::string& expected, const char* what) {
  if (actual != expected) {
    std::printf("FAIL %s: expected \"%s\", got \"%s\"\n", what, expected.c_str(), actual.c_str());
    ++failures;
  }
}

void push_be16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void push_be32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void test_format_mapping() {
  // A BTI stores a GX format index, which is a different order to TexImgFormat.
  check_eq(gcn::format_from_gx(0), gcn::TEX_FMT_I4, "gx 0 is I4");
  check_eq(gcn::format_from_gx(2), gcn::TEX_FMT_IA4, "gx 2 is IA4");
  check_eq(gcn::format_from_gx(5), gcn::TEX_FMT_RGB5A3, "gx 5 is RGB5A3");
  check_eq(gcn::format_from_gx(14), gcn::TEX_FMT_S3TC, "gx 14 is CMPR");
  check_eq(gcn::format_from_gx(7), gcn::TEX_FMT_NULL, "gx 7 is unused");

  check_eq(gcn::data_size(gcn::TEX_FMT_I4, 32, 32), 512, "I4 is 4bpp");
  check_eq(gcn::data_size(gcn::TEX_FMT_IA4, 32, 32), 1024, "IA4 is 8bpp");
  check_eq(gcn::data_size(gcn::TEX_FMT_RGB5A3, 32, 32), 2048, "RGB5A3 is 16bpp");
  check_eq(gcn::data_size(gcn::TEX_FMT_RGBA8, 32, 32), 4096, "RGBA8 is 32bpp");
  check_eq(gcn::data_size(gcn::TEX_FMT_S3TC, 32, 32), 512, "CMPR is 4bpp");
}

// Builds a 32-byte BTI header followed by a payload.
std::vector<uint8_t> make_bti(uint8_t gx_format, uint16_t width, uint16_t height,
                              const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> bytes;
  bytes.push_back(gx_format);
  bytes.push_back(1); // alpha enabled
  push_be16(bytes, width);
  push_be16(bytes, height);
  bytes.push_back(1); // wrap S
  bytes.push_back(1); // wrap T
  while (bytes.size() < 0x1C) {
    bytes.push_back(0);
  }
  push_be32(bytes, 0x20);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

void test_ia4_tiling() {
  // IA4 uses 8x4 tiles: the 33rd byte is the start of the second tile, which is
  // pixel (8, 0) rather than pixel (0, 4).
  const int width = 16;
  const int height = 4;
  std::vector<uint8_t> payload(static_cast<size_t>(width) * height, 0x00);
  payload[0] = 0xFF;  // tile 0, pixel (0,0): full alpha, full intensity
  payload[32] = 0xF0; // tile 1, pixel (8,0): full alpha, zero intensity

  const std::vector<uint8_t> bti = make_bti(2, width, height, payload);
  gcn::Image image;
  std::string error;
  check(gcn::decode_bti(bti.data(), bti.size(), image, error), "IA4 decodes");
  check_eq(image.width, width, "IA4 width");
  check_eq(image.height, height, "IA4 height");

  check_eq(image.rgba[0], 255, "pixel (0,0) intensity");
  check_eq(image.rgba[3], 255, "pixel (0,0) alpha");

  const size_t at_8_0 = 8 * 4;
  check_eq(image.rgba[at_8_0 + 0], 0, "pixel (8,0) intensity");
  check_eq(image.rgba[at_8_0 + 3], 255, "pixel (8,0) alpha");
}

void test_rgb5a3() {
  // Top bit set selects opaque 5:5:5, so 0xFC00 is pure red at full alpha.
  const int width = 4;
  const int height = 4;
  std::vector<uint8_t> payload;
  for (int i = 0; i < width * height; ++i) {
    push_be16(payload, 0xFC00);
  }

  const std::vector<uint8_t> bti = make_bti(5, width, height, payload);
  gcn::Image image;
  std::string error;
  check(gcn::decode_bti(bti.data(), bti.size(), image, error), "RGB5A3 decodes");
  check_eq(image.rgba[0], 255, "red channel");
  check_eq(image.rgba[1], 0, "green channel");
  check_eq(image.rgba[2], 0, "blue channel");
  check_eq(image.rgba[3], 255, "alpha channel");
}

void test_rejects_bad_bti() {
  gcn::Image image;
  std::string error;

  const std::vector<uint8_t> tiny(8, 0);
  check(!gcn::decode_bti(tiny.data(), tiny.size(), image, error), "rejects short file");

  std::vector<uint8_t> truncated = make_bti(5, 32, 32, std::vector<uint8_t>(16, 0));
  check(!gcn::decode_bti(truncated.data(), truncated.size(), image, error),
        "rejects truncated payload");

  std::vector<uint8_t> bad_format = make_bti(7, 4, 4, std::vector<uint8_t>(64, 0));
  check(!gcn::decode_bti(bad_format.data(), bad_format.size(), image, error),
        "rejects unused GX format");
}

// Assembles a BLO stream: a root pane, a nested child, and a text box.
std::vector<uint8_t> make_blo() {
  std::vector<uint8_t> bytes;

  push_be16(bytes, blo::PANETYPE_Pane);
  bytes.push_back(1); // visible
  bytes.push_back(0);
  const char* root_tag = "ROOT";
  bytes.insert(bytes.end(), root_tag, root_tag + 4);
  push_be16(bytes, 0);
  push_be16(bytes, 0);
  push_be16(bytes, 640);
  push_be16(bytes, 480);

  push_be16(bytes, blo::PANETYPE_Begin);
  push_be16(bytes, 0);

  // A picture nested under ROOT.
  push_be16(bytes, blo::PANETYPE_Picture);
  bytes.push_back(1);
  bytes.push_back(0);
  const char* picture_tag = "back";
  bytes.insert(bytes.end(), picture_tag, picture_tag + 4);
  push_be16(bytes, static_cast<uint16_t>(-10));
  push_be16(bytes, static_cast<uint16_t>(-20));
  push_be16(bytes, 200);
  push_be16(bytes, 100);
  bytes.push_back(2); // TIMG reference type
  bytes.push_back(12);
  const char* texture = "black_32.bti";
  bytes.insert(bytes.end(), texture, texture + 12);
  bytes.push_back(0); // empty TLUT
  bytes.push_back(0);
  bytes.push_back(0x0F); // binding
  bytes.push_back(0x00); // mirror/wrap flags
  while (bytes.size() % 4 != 0) {
    bytes.push_back(0);
  }

  // A text box carrying formatting codes.
  push_be16(bytes, blo::PANETYPE_TextBox);
  bytes.push_back(1);
  bytes.push_back(0);
  const char* text_tag = "he00";
  bytes.insert(bytes.end(), text_tag, text_tag + 4);
  push_be16(bytes, 205);
  push_be16(bytes, 307);
  push_be16(bytes, 242);
  push_be16(bytes, 32);
  bytes.push_back(2); // FONT reference
  bytes.push_back(12);
  const char* font = "sumiw9_2.bfn";
  bytes.insert(bytes.end(), font, font + 12);
  const uint8_t char_color[4] = {0xD7, 0xFF, 0x00, 0xFF};
  bytes.insert(bytes.end(), char_color, char_color + 4);
  const uint8_t grad_color[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  bytes.insert(bytes.end(), grad_color, grad_color + 4);
  bytes.push_back(0x80); // horizontal binding, retail revision flag set
  bytes.push_back(0x00); // vertical binding
  push_be16(bytes, 5);   // spacing
  push_be16(bytes, 28);  // leading
  push_be16(bytes, 16);  // font width
  push_be16(bytes, 16);  // font height
  const std::string text = "\x1b" "SH[-3]Op\x1b" "SH[-5]tions";
  push_be16(bytes, static_cast<uint16_t>(text.size()));
  bytes.insert(bytes.end(), text.begin(), text.end());
  while (bytes.size() % 4 != 0) {
    bytes.push_back(0);
  }

  push_be16(bytes, blo::PANETYPE_Close);
  push_be16(bytes, 0);
  return bytes;
}

void test_blo_parse() {
  const std::vector<uint8_t> bytes = make_blo();
  blo::Pane root;
  std::string error;
  check(blo::parse(bytes.data(), bytes.size(), root, error), "BLO parses");
  if (!error.empty()) {
    std::printf("  error: %s\n", error.c_str());
  }

  check_eq(root.width, 640, "screen width adopted from root pane");
  check_eq(root.height, 480, "screen height adopted from root pane");
  check_eq(static_cast<int>(root.children.size()), 1, "one top-level pane");

  const blo::Pane& root_pane = root.children.front();
  check_eq(root_pane.tag, "ROOT", "root tag");
  check_eq(static_cast<int>(root_pane.children.size()), 2, "two nested panes");

  const blo::Pane* picture = root.find("back");
  check(picture != nullptr, "finds picture by tag");
  if (picture != nullptr) {
    check_eq(picture->texture, "black_32.bti", "picture texture name");
    // Bounds are signed and may sit outside the parent.
    check_eq(picture->x, -10, "picture x is signed");
    check_eq(picture->y, -20, "picture y is signed");
    check_eq(picture->width, 200, "picture width");
  }

  const blo::Pane* textbox = root.find("he00");
  check(textbox != nullptr, "finds text box by tag");
  if (textbox != nullptr) {
    check_eq(textbox->font, "sumiw9_2.bfn", "font name");
    check_eq(textbox->font_height, 16, "font height");
    check_eq(static_cast<int>(textbox->char_color.r), 0xD7, "char colour red");
    check_eq(static_cast<int>(textbox->char_color.g), 0xFF, "char colour green");
  }
}

void test_strip_codes() {
  check_eq(p2d::strip_codes("\x1b" "SH[-3]Op\x1b" "SH[-5]tions"), "Options", "strips SH codes");
  check_eq(p2d::strip_codes("PRESS START"), "PRESS START", "leaves plain text alone");
  check_eq(p2d::strip_codes("\x1b" "CC[ff0000]red"), "red", "strips colour codes");
  check_eq(p2d::strip_codes(""), "", "handles empty string");
  // An unterminated code falls back to consuming just the tag.
  check_eq(p2d::strip_codes("\x1b" "SHtail"), "tail", "handles missing bracket");
}

void test_flatten_and_rules() {
  const std::vector<uint8_t> bytes = make_blo();
  blo::Pane root;
  std::string error;
  if (!blo::parse(bytes.data(), bytes.size(), root, error)) {
    std::printf("FAIL flatten setup: %s\n", error.c_str());
    ++failures;
    return;
  }

  screen::DrawList list;
  screen::flatten(root, list);
  check_eq(static_cast<int>(list.quads.size()), 1, "one quad");
  check_eq(static_cast<int>(list.texts.size()), 1, "one text run");

  if (!list.quads.empty()) {
    // ROOT sits at 0,0 so the child keeps its own offset.
    check_eq(list.quads[0].x, -10, "quad absolute x");
    check_eq(list.quads[0].y, -20, "quad absolute y");
  }
  if (!list.texts.empty()) {
    check_eq(list.texts[0].text, "Options", "text codes stripped during flatten");
  }

  check_eq(screen::count_menu_items(root), 1, "counts he## entries");
}

void test_menu_rules() {
  screen::DrawList list;
  screen::Quad cursor_zero_left;
  cursor_zero_left.tag = "i00l";
  screen::Quad cursor_one_left;
  cursor_one_left.tag = "i01l";
  screen::Quad trail;
  trail.tag = "z04l";
  screen::Quad plain;
  plain.tag = "back";
  list.quads = {cursor_zero_left, cursor_one_left, trail, plain};

  screen::Text colour_carrier;
  colour_carrier.tag = "se_c";
  colour_carrier.text = "toyoda";
  screen::Text entry;
  entry.tag = "he00";
  entry.text = "Start";
  entry.color = {10, 20, 30, 255};
  entry.grad_color = {40, 50, 60, 255};
  list.texts = {colour_carrier, entry};

  const blo::Color blend_char = {200, 210, 220, 255};
  const blo::Color blend_grad = {100, 110, 120, 255};
  screen::MenuAnimation animation;
  animation.reset(1);

  screen::DrawList at_rest = list;
  screen::apply_menu_rules(at_rest, 0, animation, blend_char, blend_grad);

  check_eq(static_cast<int>(at_rest.quads.size()), 0,
           "drops title background, trail, and all icon anchors");
  check_eq(static_cast<int>(at_rest.texts.size()), 1, "drops se_c colour carrier");
  check_eq(at_rest.texts[0].tag, "he00", "keeps menu entry text");
  check_eq(static_cast<int>(at_rest.texts[0].color.r), 10,
           "an unblended entry keeps its own colour");

  // A full blend period puts the selection entirely on the layout's blend
  // colour, and anything short of it lands in between.
  animation.update(0, screen::MenuAnimation::kBlendSeconds);
  screen::DrawList selected_list = list;
  screen::apply_menu_rules(selected_list, 0, animation, blend_char, blend_grad);
  check_eq(static_cast<int>(selected_list.texts[0].color.r), 200,
           "a fully selected entry reaches the blend colour");
  check_eq(static_cast<int>(selected_list.texts[0].grad_color.r), 100,
           "the gradient blends alongside the char colour");

  screen::MenuAnimation midway;
  midway.reset(1);
  midway.update(0, screen::MenuAnimation::kBlendSeconds * 0.5f);
  check(midway.blend(0) > 0.49f && midway.blend(0) < 0.51f, "half a period is a half blend");

  // Moving off an entry runs its timer back down rather than snapping.
  midway.update(1, screen::MenuAnimation::kBlendSeconds);
  check_eq(midway.blend(0), 0.0f, "deselected entries return to rest");
}

void test_title_animation_curves() {
  title::PressStartAnimation prompt;
  check(!prompt.active(), "prompt starts inactive");
  prompt.start();
  check(prompt.active(), "prompt start activates");
  check_eq(prompt.alpha(), 0, "prompt begins transparent");

  prompt.update(0.25f);
  check(prompt.alpha() >= 127 && prompt.alpha() <= 128, "prompt quarter-cycle alpha");
  prompt.update(0.25f);
  check_eq(prompt.alpha(), 255, "prompt half-cycle alpha");
  prompt.update(0.5f);
  check_eq(prompt.alpha(), 0, "prompt full-cycle alpha");

  prompt.update(0.5f);
  prompt.stop();
  check_eq(prompt.alpha(), 255, "prompt fade starts from current peak");
  prompt.update(0.125f);
  check(prompt.alpha() >= 127 && prompt.alpha() <= 128, "prompt fade midpoint");
  prompt.update(0.125f);
  check_eq(prompt.alpha(), 0, "prompt fade reaches transparent");
  check(prompt.active(), "prompt holds one exit frame");
  prompt.update(0.0f);
  check(!prompt.active(), "prompt exit becomes inactive");

  title::Scale scale = title::item_open_scale(0.0f);
  check(scale.x == 2.0f && scale.y == 0.0f, "menu item open start");
  scale = title::item_open_scale(0.75f);
  check(scale.x == 1.5f && scale.y == 0.5f, "menu item open midpoint");
  scale = title::item_open_scale(1.0f);
  check(scale.x == 1.0f && scale.y == 1.0f, "menu item open finish");

  scale = title::item_close_scale(0.5f, false);
  check(scale.x == 1.5f && scale.y == 0.0f, "menu item ordinary close midpoint");
  scale = title::item_close_scale(1.0f, false);
  check(scale.x == 2.0f && scale.y == 0.0f, "menu item ordinary close finish");

  scale = title::title_open_scale(0.0f);
  check(scale.x == 4.0f && scale.y == 0.0f, "title bar open start");
  scale = title::title_open_scale(1.0f);
  check(scale.x > 0.999f && scale.x < 1.001f && scale.y > 0.999f,
        "title bar open finish");

  check(title::cursor_ease(0.0f) == 0.0f, "cursor ease start");
  check(title::cursor_ease(1.0f) > 0.999f, "cursor ease finish");
  check(title::panel_ease(0.5f) > 0.499f && title::panel_ease(0.5f) < 0.501f,
        "panel cosine midpoint");
}

void test_boot_logo() {
  // intro/nintendo.bti is 464x56, so drawLoadLogo's centring lands it here.
  const loadlogo::Rect rect = loadlogo::rect(464, 56, 640, 480, true);
  check_eq(rect.x, 88, "boot logo centred horizontally");
  check_eq(rect.y, 172, "boot logo sits 40 above centre");

  // A level banner uses the same centring without the 40 pixel lift.
  const loadlogo::Rect banner = loadlogo::rect(464, 56, 640, 480, false);
  check_eq(banner.y, 212, "level banner centred vertically");

  const loadlogo::Color tint = loadlogo::color(1.0f, true);
  check(tint.r == 220 && tint.g == 0 && tint.b == 0, "boot logo is retail red");
  check_eq(tint.a, 255, "full fade is opaque");
  check_eq(loadlogo::color(0.5f, true).a, 127, "half fade halves alpha");

  // GameFlow::init leaves the banner opaque, and the load idler's rise clamps
  // there, so the whole hold stays at full alpha however long it runs.
  loadlogo::Fade fade;
  check(fade.value() == 1.0f, "boot logo starts opaque");
  for (int frame = 0; frame < 600; ++frame) {
    fade.update(1.0f / 60.0f);
  }
  check(fade.value() == 1.0f, "hold never dims");
  check(fade.phase() == loadlogo::Phase::Hold, "hold persists until released");
  check(fade.visible(), "logo draws throughout the hold");

  // TitlesSection::draw then retires it over exactly one second.
  fade.release();
  check(fade.phase() == loadlogo::Phase::FadeOut, "release starts the fade");
  for (int frame = 0; frame < 30; ++frame) {
    fade.update(1.0f / 60.0f);
  }
  check(fade.value() > 0.49f && fade.value() < 0.51f, "fade is half gone at 30 frames");
  for (int frame = 0; frame < 30; ++frame) {
    fade.update(1.0f / 60.0f);
  }
  // Retail subtracts a frame time that is not representable in binary, so the
  // second's worth of subtraction leaves a residue and the logo is retired on
  // the frame after. Matching that is the point, so the test allows for it.
  check(fade.value() < 0.0001f, "fade is spent after a second");
  check(fade.visible(), "residue keeps the logo alive one more frame");
  fade.update(1.0f / 60.0f);
  check(fade.phase() == loadlogo::Phase::Done, "fade retires on frame 61");
  check(!fade.visible(), "retired logo stops drawing");

  // Once cleared it stays cleared, the way mIsNintendoLoadLogo does.
  fade.update(1.0f / 60.0f);
  check(fade.value() == 0.0f && !fade.visible(), "retirement is permanent");
}

void test_viewport() {
  // 640x480 into 960x544 is height limited, leaving pillarboxes.
  const screen::Viewport viewport = screen::fit(640, 480, 960, 544);
  check(viewport.scale > 1.133f && viewport.scale < 1.134f, "scale is height limited");
  check(viewport.offset_y > -0.01f && viewport.offset_y < 0.01f, "no vertical letterbox");
  check(viewport.offset_x > 117.0f && viewport.offset_x < 118.0f, "centred horizontally");
}

} // namespace

int main() {
  test_format_mapping();
  test_ia4_tiling();
  test_rgb5a3();
  test_rejects_bad_bti();
  test_blo_parse();
  test_strip_codes();
  test_flatten_and_rules();
  test_menu_rules();
  test_title_animation_curves();
  test_boot_logo();
  test_viewport();

  if (failures == 0) {
    std::printf("all asset tests passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", failures);
  return 1;
}
