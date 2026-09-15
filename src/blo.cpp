#include "blo.hpp"

#include "gcn_texture.hpp"

namespace blo {
namespace {

class Reader {
public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  uint8_t u8() {
    if (position_ + 1 > size_) {
      overflowed_ = true;
      return 0;
    }
    return data_[position_++];
  }

  int16_t s16() {
    const uint8_t high = u8();
    const uint8_t low = u8();
    return static_cast<int16_t>((high << 8) | low);
  }

  uint16_t u16() { return static_cast<uint16_t>(s16()); }

  std::string string(size_t length) {
    if (position_ + length > size_) {
      overflowed_ = true;
      return std::string();
    }
    const std::string value(reinterpret_cast<const char*>(data_ + position_), length);
    position_ += length;
    return value;
  }

  // P2DStream::getResource: a reference-type byte, a length byte, then bytes.
  std::string resource() {
    const uint8_t reference_type = u8();
    const uint8_t length = u8();
    if (length == 0) {
      return std::string();
    }
    if (reference_type != 0 && reference_type != 2) {
      overflowed_ = true;
      return std::string();
    }
    return string(length);
  }

  Color color() {
    Color value;
    value.r = u8();
    value.g = u8();
    value.b = u8();
    value.a = u8();
    return value;
  }

  void align(size_t boundary) {
    while (position_ % boundary != 0 && !overflowed_) {
      u8();
    }
  }

  bool exhausted() const { return position_ >= size_; }
  bool overflowed() const { return overflowed_; }

private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
  bool overflowed_ = false;
};

void read_common(Reader& reader, Pane& pane, uint16_t type) {
  pane.type = type;
  pane.visible = reader.u8() != 0;
  reader.u8();

  char tag[5] = {0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    tag[i] = static_cast<char>(reader.u8());
  }
  // Untagged panes store four NULs; trim so the tag is a plain string.
  size_t length = 4;
  while (length > 0 && (tag[length - 1] == '\0' || tag[length - 1] == ' ')) {
    --length;
  }
  pane.tag.assign(tag, length);

  pane.x = reader.s16();
  pane.y = reader.s16();
  pane.width = reader.s16();
  pane.height = reader.s16();
}

void read_picture(Reader& reader, Pane& pane) {
  pane.texture = reader.resource(); // TIMG
  reader.resource();                // TLUT, unused by the disc's menus
  pane.binding = reader.u8();
  const uint8_t flags = reader.u8();
  pane.mirror = static_cast<uint8_t>(flags & 3);
  pane.tumble = (flags & 4) != 0;
  pane.wrap = static_cast<uint8_t>((flags >> 3) & 3);
  reader.align(4);
}

void read_textbox(Reader& reader, Pane& pane, std::string& error) {
  pane.font = reader.resource(); // FONT
  pane.char_color = reader.color();
  pane.grad_color = reader.color();

  const uint8_t horizontal = reader.u8();
  pane.align_h = static_cast<uint8_t>(horizontal & ~0x80);
  pane.align_v = reader.u8();

  if ((horizontal & 0x80) == 0) {
    // The decomp rejects these outright as "blo data is old".
    error = "text box predates the retail BLO revision";
    return;
  }

  pane.spacing = reader.s16();
  pane.leading = reader.s16();
  pane.font_width = reader.s16();
  pane.font_height = reader.s16();

  const int16_t length = reader.s16();
  if (length > 0) {
    pane.text = reader.string(static_cast<size_t>(length));
  }
  reader.align(4);
}

void read_window(Reader& reader, Pane& pane) {
  pane.window_x = reader.s16();
  pane.window_y = reader.s16();
  pane.window_width = reader.s16();
  pane.window_height = reader.s16();

  for (int i = 0; i < 4; ++i) {
    pane.corner_texture[i] = reader.resource(); // TIMG
  }
  reader.resource(); // TLUT
  reader.u8();       // window flag
  for (int i = 0; i < 4; ++i) {
    pane.corner_color[i] = reader.color();
  }
  reader.align(4);
}

// Reads one sibling run. Panes attach to `parent`, while `recent` tracks the
// last pane so a PANETYPE_Begin marker can open a child scope beneath it.
void read_panes(Reader& reader, Pane& parent, std::string& error, int depth) {
  if (depth > 32) {
    error = "pane hierarchy nested too deeply";
    return;
  }

  Pane* recent = &parent;
  while (!reader.exhausted() && !reader.overflowed() && error.empty()) {
    const uint16_t type = reader.u16();
    switch (type) {
    case PANETYPE_End:
      return;
    case PANETYPE_Begin:
      reader.u16();
      read_panes(reader, *recent, error, depth + 1);
      break;
    case PANETYPE_Close:
      reader.u16();
      return;
    case PANETYPE_Pane:
    case PANETYPE_Window:
    case PANETYPE_Picture:
    case PANETYPE_TextBox: {
      parent.children.emplace_back();
      Pane& pane = parent.children.back();
      read_common(reader, pane, type);
      if (type == PANETYPE_Picture) {
        read_picture(reader, pane);
      } else if (type == PANETYPE_TextBox) {
        read_textbox(reader, pane, error);
      } else if (type == PANETYPE_Window) {
        read_window(reader, pane);
      }
      recent = &pane;
      break;
    }
    default:
      error = "unknown pane type in BLO stream";
      return;
    }
  }
}

} // namespace

const Pane* Pane::find(const std::string& wanted) const {
  if (tag == wanted) {
    return this;
  }
  for (const Pane& child : children) {
    if (const Pane* hit = child.find(wanted)) {
      return hit;
    }
  }
  return nullptr;
}

bool parse(const uint8_t* data, size_t size, Pane& root, std::string& error) {
  Reader reader(data, size);
  error.clear();
  root = Pane();

  read_panes(reader, root, error, 0);
  if (!error.empty()) {
    return false;
  }
  if (reader.overflowed()) {
    error = "BLO stream ended mid-pane";
    return false;
  }
  if (root.children.empty()) {
    error = "BLO contained no panes";
    return false;
  }

  // The outermost pane is the 640x480 screen root; adopt its bounds.
  const Pane& screen = root.children.front();
  root.type = PANETYPE_Screen;
  root.width = screen.width;
  root.height = screen.height;
  return true;
}

bool parse_file(const std::string& path, Pane& root, std::string& error) {
  std::vector<uint8_t> bytes;
  if (!gcn::read_file(path, bytes, error)) {
    return false;
  }
  return parse(bytes.data(), bytes.size(), root, error);
}

} // namespace blo
