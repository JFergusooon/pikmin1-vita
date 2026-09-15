#pragma once

// Parser for the P2D screen layouts (".blo") on the Pikmin disc.
//
// Mirrors the upstream decompilation: P2DScreen::makeHiearachyPanes plus the
// stream constructors of P2DPane / P2DPicture / P2DTextBox / P2DWindow under
// upstream/pikmin/src/plugPikiYamashita/.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace blo {

// Matches P2DPaneType in upstream/pikmin/include/P2D/Pane.h.
enum PaneType {
  PANETYPE_End = 0x00,
  PANETYPE_Begin = 0x01,
  PANETYPE_Close = 0x02,
  PANETYPE_Screen = 0x08,
  PANETYPE_Pane = 0x10,
  PANETYPE_Window = 0x11,
  PANETYPE_Picture = 0x12,
  PANETYPE_TextBox = 0x13,
};

struct Color {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;
};

struct Pane {
  uint16_t type = PANETYPE_Pane;
  bool visible = true;
  std::string tag;

  // Bounds are relative to the parent pane and may extend outside it.
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  // PANETYPE_Picture
  std::string texture;
  uint8_t binding = 0;
  uint8_t mirror = 0;
  bool tumble = false;
  uint8_t wrap = 0;

  // PANETYPE_TextBox
  std::string font;
  std::string text;
  Color char_color;
  Color grad_color;
  uint8_t align_h = 0;
  uint8_t align_v = 0;
  int spacing = 0;
  int leading = 0;
  int font_width = 0;
  int font_height = 0;

  // PANETYPE_Window
  std::string corner_texture[4];
  Color corner_color[4];
  int window_x = 0;
  int window_y = 0;
  int window_width = 0;
  int window_height = 0;

  std::vector<Pane> children;

  const Pane* find(const std::string& tag) const;
};

bool parse(const uint8_t* data, size_t size, Pane& root, std::string& error);
bool parse_file(const std::string& path, Pane& root, std::string& error);

} // namespace blo
