#include "screen.hpp"

#include "p2d_text.hpp"

namespace screen {
namespace {

void walk(const blo::Pane& pane, int origin_x, int origin_y, DrawList& out) {
  const int x = origin_x + pane.x;
  const int y = origin_y + pane.y;

  if (pane.visible) {
    if (pane.type == blo::PANETYPE_Picture && !pane.texture.empty()) {
      Quad quad;
      quad.x = x;
      quad.y = y;
      quad.width = pane.width;
      quad.height = pane.height;
      quad.texture = pane.texture;
      quad.tag = pane.tag;
      out.quads.push_back(quad);
    } else if (pane.type == blo::PANETYPE_TextBox) {
      const std::string visible = p2d::strip_codes(pane.text);
      if (!visible.empty()) {
        Text text;
        text.x = x;
        text.y = y;
        text.width = pane.width;
        text.height = pane.height;
        text.text = visible;
        text.tag = pane.tag;
        text.color = pane.char_color;
        text.grad_color = pane.grad_color;
        text.font_width = pane.font_width;
        text.font_height = pane.font_height;
        text.align_h = pane.align_h;
        text.align_v = pane.align_v;
        out.texts.push_back(text);
      }
    }
  }

  // Children draw on top of their parent, and are laid out relative to it even
  // when the parent is a zero-sized anchor pane.
  for (const blo::Pane& child : pane.children) {
    walk(child, x, y, out);
  }
}

} // namespace

void flatten(const blo::Pane& root, DrawList& out) {
  out.quads.clear();
  out.texts.clear();
  out.width = root.width > 0 ? root.width : kOrthoWidth;
  out.height = root.height > 0 ? root.height : kOrthoHeight;

  for (const blo::Pane& child : root.children) {
    walk(child, 0, 0, out);
  }
}

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Matches tags shaped like "z04l" or "i00r", returning the two-digit index.
bool indexed_tag(const std::string& tag, char prefix, int& index) {
  if (tag.size() != 4 || tag[0] != prefix) {
    return false;
  }
  if (!is_digit(tag[1]) || !is_digit(tag[2])) {
    return false;
  }
  if (tag[3] != 'l' && tag[3] != 'r') {
    return false;
  }
  index = (tag[1] - '0') * 10 + (tag[2] - '0');
  return true;
}

// Matches menu entry tags shaped like "he00", the same names count_menu_items
// probes for. These carry no 'l'/'r' suffix, so indexed_tag does not fit.
bool menu_item_tag(const std::string& tag, int& index) {
  if (tag.size() != 4 || tag[0] != 'h' || tag[1] != 'e') {
    return false;
  }
  if (!is_digit(tag[2]) || !is_digit(tag[3])) {
    return false;
  }
  index = (tag[2] - '0') * 10 + (tag[3] - '0');
  return true;
}

} // namespace

const float MenuAnimation::kBlendSeconds = 0.5f;

void MenuAnimation::reset(int item_count) {
  timers_.assign(item_count > 0 ? static_cast<size_t>(item_count) : 0, 0.0f);
}

void MenuAnimation::update(int selected, float dt) {
  for (size_t i = 0; i < timers_.size(); ++i) {
    // The selected entry's timer rises, every other entry's falls.
    float value =
        timers_[i] + (static_cast<int>(i) == selected ? dt : -dt);
    if (value < 0.0f) {
      value = 0.0f;
    }
    if (value > kBlendSeconds) {
      value = kBlendSeconds;
    }
    timers_[i] = value;
  }
}

float MenuAnimation::blend(int item) const {
  if (item < 0 || static_cast<size_t>(item) >= timers_.size()) {
    return 0.0f;
  }
  return timers_[static_cast<size_t>(item)] / kBlendSeconds;
}

namespace {

// RoundOff(comp1 * t1 + comp2 * t2), the weighting retail blends colours with.
uint8_t color_blend(uint8_t towards, float t, uint8_t from) {
  const float value = static_cast<float>(towards) * t + static_cast<float>(from) * (1.0f - t);
  if (value <= 0.0f) {
    return 0;
  }
  if (value >= 255.0f) {
    return 255;
  }
  return static_cast<uint8_t>(value + 0.5f);
}

blo::Color blend_color(const blo::Color& towards, float t, const blo::Color& from) {
  blo::Color out;
  out.r = color_blend(towards.r, t, from.r);
  out.g = color_blend(towards.g, t, from.g);
  out.b = color_blend(towards.b, t, from.b);
  out.a = from.a;
  return out;
}

} // namespace

void apply_menu_rules(DrawList& list, int selected, const MenuAnimation& animation,
                      const blo::Color& char_blend, const blo::Color& grad_blend) {
  (void)selected;
  std::vector<Quad> quads;
  quads.reserve(list.quads.size());
  for (const Quad& quad : list.quads) {
    int index = 0;
    // ogScrTitleMgr forces each DrawMenu's background pane to alpha zero so
    // the opening cinematic remains visible underneath it.
    if (quad.tag == "back") {
      continue;
    }
    if (indexed_tag(quad.tag, 'z', index)) {
      continue;
    }
    if (indexed_tag(quad.tag, 'i', index)) {
      continue;
    }
    quads.push_back(quad);
  }
  list.quads.swap(quads);

  std::vector<Text> texts;
  texts.reserve(list.texts.size());
  for (const Text& text : list.texts) {
    if (text.tag == "se_c") {
      continue;
    }
    Text entry = text;
    // Menu entries are tagged "he00", "he01", ... and are the only panes the
    // selection blend touches.
    int index = 0;
    if (menu_item_tag(entry.tag, index)) {
      const float t = animation.blend(index);
      entry.color = blend_color(char_blend, t, text.color);
      entry.grad_color = blend_color(grad_blend, t, text.grad_color);
    }
    texts.push_back(entry);
  }
  list.texts.swap(texts);
}

bool menu_font_color(const blo::Pane& root, blo::Color& out) {
  const blo::Pane* pane = root.find("se_c");
  if (pane == nullptr || pane->type != blo::PANETYPE_TextBox) {
    return false;
  }
  out = pane->char_color;
  return true;
}

bool menu_blend_colors(const blo::Pane& root, blo::Color& char_out, blo::Color& grad_out) {
  const blo::Pane* pane = root.find("se_c");
  if (pane == nullptr || pane->type != blo::PANETYPE_TextBox) {
    return false;
  }
  char_out = pane->char_color;
  grad_out = pane->grad_color;
  return true;
}

int count_menu_items(const blo::Pane& root) {
  int count = 0;
  while (count < 100) {
    char tag[8];
    tag[0] = 'h';
    tag[1] = 'e';
    tag[2] = static_cast<char>('0' + count / 10);
    tag[3] = static_cast<char>('0' + count % 10);
    tag[4] = '\0';
    if (root.find(tag) == nullptr) {
      break;
    }
    ++count;
  }
  return count;
}

Viewport fit(int source_width, int source_height, int target_width, int target_height) {
  Viewport viewport;
  if (source_width <= 0 || source_height <= 0) {
    return viewport;
  }

  const float scale_x = static_cast<float>(target_width) / static_cast<float>(source_width);
  const float scale_y = static_cast<float>(target_height) / static_cast<float>(source_height);
  viewport.scale = scale_x < scale_y ? scale_x : scale_y;
  viewport.offset_x = (static_cast<float>(target_width) - source_width * viewport.scale) * 0.5f;
  viewport.offset_y = (static_cast<float>(target_height) - source_height * viewport.scale) * 0.5f;
  return viewport;
}

} // namespace screen
