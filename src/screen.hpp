#pragma once

// Flattens a parsed BLO pane tree into absolute-positioned draw items.
//
// Keeping this separate from the Vita backend means the layout maths can be
// exercised on the host, and leaves the renderer as a simple loop over quads.

#include "blo.hpp"

#include <string>
#include <vector>

namespace screen {

// The P2D coordinate space every Pikmin layout is authored in.
const int kOrthoWidth = 640;
const int kOrthoHeight = 480;

struct Quad {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::string texture;
  std::string tag;
};

struct Text {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::string text; // formatting codes already stripped
  std::string tag;
  blo::Color color;      // top of the vertical gradient
  blo::Color grad_color; // bottom of the vertical gradient
  uint8_t alpha = 255;
  int font_width = 0;
  int font_height = 0;
  uint8_t align_h = 0;
  uint8_t align_v = 0;
};

struct DrawList {
  int width = kOrthoWidth;
  int height = kOrthoHeight;
  std::vector<Quad> quads;
  std::vector<Text> texts;
};

// Walks the tree in draw order, accumulating parent offsets.
void flatten(const blo::Pane& root, DrawList& out);

// Applies the visibility rules DrawMenu imposes at runtime, which the raw
// layout does not encode. See zen::DrawMenu::DrawMenu in
// upstream/pikmin/src/plugPikiYamashita/drawMenu.cpp:
//   - "se_c" only carries the menu font colours and is never drawn.
//   - "z##l"/"z##r" are the cursor trail, parked off-screen at zero scale.
//   - "i##l"/"i##r" are hidden position anchors and are never drawn.
// Per-item colour blend for the menu, following zen::DrawMenuText::update in
// upstream/pikmin/src/plugPikiYamashita/drawMenu.cpp.
//
// Each entry carries a timer that runs up while it is the selection and back
// down when it is not, so moving between entries cross-fades their colours
// instead of switching them. The layout holds no animation of its own, which
// is why a menu drawn straight from it looks like a still image.
class MenuAnimation {
 public:
  // Retail clamps the timer to this, and divides by it to get the blend, so it
  // is both the ceiling and the time a full fade takes.
  static const float kBlendSeconds;

  void reset(int item_count);
  void update(int selected, float dt);

  // 0 for an entry at rest, 1 for one fully selected.
  float blend(int item) const;

 private:
  std::vector<float> timers_;
};

// Applies the visibility rules plus the current selection's colour blend.
void apply_menu_rules(DrawList& list, int selected, const MenuAnimation& animation,
                      const blo::Color& char_blend, const blo::Color& grad_blend);

// Pulls the menu font colours out of the layout's "se_c" pane. The pane is
// never drawn; it exists in the layout purely to carry the colours a selected
// entry blends towards.
bool menu_font_color(const blo::Pane& root, blo::Color& out);
bool menu_blend_colors(const blo::Pane& root, blo::Color& char_out, blo::Color& grad_out);

// Counts menu entries the way DrawMenu does, by probing for "he00", "he01", ...
int count_menu_items(const blo::Pane& root);

// Letterbox transform mapping the 4:3 ortho space onto a wider display.
struct Viewport {
  float scale = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
};

Viewport fit(int source_width, int source_height, int target_width, int target_height);

} // namespace screen
