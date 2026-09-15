#include "gcn_texture.hpp"

#include <cstdio>

namespace gcn {
namespace {

// The BTI header stores a GX format index; the decomp remaps it through
// TexImg::convFormat before anything else touches the pixels.
const TexFormat kGxToInternal[15] = {
    TEX_FMT_I4,   TEX_FMT_I8,   TEX_FMT_IA4,  TEX_FMT_IA8, TEX_FMT_RGB565,
    TEX_FMT_RGB5A3, TEX_FMT_RGBA8, TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_NULL,
    TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_S3TC,
};

const size_t kBtiHeaderSize = 32;

uint16_t read_be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// A 4-bit channel is replicated into 8 bits, so 0xF becomes 0xFF.
uint8_t expand4(uint8_t value) { return static_cast<uint8_t>(value * 0x11); }

uint8_t expand5(uint8_t value) { return static_cast<uint8_t>((value << 3) | (value >> 2)); }

uint8_t expand6(uint8_t value) { return static_cast<uint8_t>((value << 2) | (value >> 4)); }

void put(Image& image, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
    return;
  }
  const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
                        static_cast<size_t>(x)) * 4;
  image.rgba[index + 0] = r;
  image.rgba[index + 1] = g;
  image.rgba[index + 2] = b;
  image.rgba[index + 3] = a;
}

void decode_rgb565_value(uint16_t value, uint8_t& r, uint8_t& g, uint8_t& b) {
  r = expand5(static_cast<uint8_t>((value >> 11) & 0x1F));
  g = expand6(static_cast<uint8_t>((value >> 5) & 0x3F));
  b = expand5(static_cast<uint8_t>(value & 0x1F));
}

void decode_rgb5a3_value(uint16_t value, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
  if (value & 0x8000) {
    // Top bit set means opaque 5-bit colour.
    a = 255;
    r = expand5(static_cast<uint8_t>((value >> 10) & 0x1F));
    g = expand5(static_cast<uint8_t>((value >> 5) & 0x1F));
    b = expand5(static_cast<uint8_t>(value & 0x1F));
  } else {
    const uint8_t alpha3 = static_cast<uint8_t>((value >> 12) & 0x7);
    a = static_cast<uint8_t>((alpha3 << 5) | (alpha3 << 2) | (alpha3 >> 1));
    r = expand4(static_cast<uint8_t>((value >> 8) & 0xF));
    g = expand4(static_cast<uint8_t>((value >> 4) & 0xF));
    b = expand4(static_cast<uint8_t>(value & 0xF));
  }
}

// GameCube CMPR reuses DXT1 blocks but stores the endpoints big-endian and
// emits the leftmost pixel of each row in the high bits of its index byte.
void decode_cmpr_block(const uint8_t* block, Image& image, int origin_x, int origin_y) {
  const uint16_t c0 = read_be16(block + 0);
  const uint16_t c1 = read_be16(block + 2);

  uint8_t palette[4][4];
  decode_rgb565_value(c0, palette[0][0], palette[0][1], palette[0][2]);
  palette[0][3] = 255;
  decode_rgb565_value(c1, palette[1][0], palette[1][1], palette[1][2]);
  palette[1][3] = 255;

  if (c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
    }
    palette[2][3] = 255;
    palette[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0;
    }
    palette[2][3] = 255;
    palette[3][3] = 0;
  }

  for (int row = 0; row < 4; ++row) {
    const uint8_t bits = block[4 + row];
    for (int column = 0; column < 4; ++column) {
      const uint8_t index = static_cast<uint8_t>((bits >> (6 - 2 * column)) & 0x3);
      put(image, origin_x + column, origin_y + row, palette[index][0], palette[index][1],
          palette[index][2], palette[index][3]);
    }
  }
}

bool decode_cmpr(const uint8_t* data, size_t size, Image& image, std::string& error) {
  // Four 4x4 DXT1 blocks are grouped into each 8x8 macroblock.
  size_t offset = 0;
  for (int y = 0; y < image.height; y += 8) {
    for (int x = 0; x < image.width; x += 8) {
      for (int sub = 0; sub < 4; ++sub) {
        if (offset + 8 > size) {
          error = "truncated CMPR payload";
          return false;
        }
        decode_cmpr_block(data + offset, image, x + (sub & 1) * 4, y + (sub >> 1) * 4);
        offset += 8;
      }
    }
  }
  return true;
}

bool decode_rgba8(const uint8_t* data, size_t size, Image& image, std::string& error) {
  // Each 4x4 tile is 64 bytes: 32 bytes of alpha/red pairs then 32 of green/blue.
  size_t offset = 0;
  for (int y = 0; y < image.height; y += 4) {
    for (int x = 0; x < image.width; x += 4) {
      if (offset + 64 > size) {
        error = "truncated RGBA8 payload";
        return false;
      }
      for (int pixel = 0; pixel < 16; ++pixel) {
        const uint8_t a = data[offset + pixel * 2 + 0];
        const uint8_t r = data[offset + pixel * 2 + 1];
        const uint8_t g = data[offset + 32 + pixel * 2 + 0];
        const uint8_t b = data[offset + 32 + pixel * 2 + 1];
        put(image, x + (pixel & 3), y + (pixel >> 2), r, g, b, a);
      }
      offset += 64;
    }
  }
  return true;
}

// Handles every format whose tiles are a straightforward row-major run of
// pixels; CMPR and RGBA8 have interleaved layouts and are decoded separately.
bool decode_tiled(TexFormat format, const uint8_t* data, size_t size, Image& image,
                  std::string& error) {
  int tile_width = 0;
  int tile_height = 0;
  tile_size(format, tile_width, tile_height);

  size_t offset = 0;
  for (int tile_y = 0; tile_y < image.height; tile_y += tile_height) {
    for (int tile_x = 0; tile_x < image.width; tile_x += tile_width) {
      for (int row = 0; row < tile_height; ++row) {
        for (int column = 0; column < tile_width; ++column) {
          const int x = tile_x + column;
          const int y = tile_y + row;
          const int index = row * tile_width + column;

          switch (format) {
          case TEX_FMT_I4: {
            const size_t byte = offset + static_cast<size_t>(index) / 2;
            if (byte >= size) {
              error = "truncated I4 payload";
              return false;
            }
            const uint8_t nibble = (index & 1) ? (data[byte] & 0xF) : (data[byte] >> 4);
            const uint8_t value = expand4(nibble);
            put(image, x, y, value, value, value, 255);
            break;
          }
          case TEX_FMT_I8: {
            const size_t byte = offset + static_cast<size_t>(index);
            if (byte >= size) {
              error = "truncated I8 payload";
              return false;
            }
            const uint8_t value = data[byte];
            put(image, x, y, value, value, value, 255);
            break;
          }
          case TEX_FMT_IA4: {
            const size_t byte = offset + static_cast<size_t>(index);
            if (byte >= size) {
              error = "truncated IA4 payload";
              return false;
            }
            const uint8_t value = expand4(static_cast<uint8_t>(data[byte] & 0xF));
            const uint8_t alpha = expand4(static_cast<uint8_t>(data[byte] >> 4));
            put(image, x, y, value, value, value, alpha);
            break;
          }
          case TEX_FMT_IA8: {
            const size_t byte = offset + static_cast<size_t>(index) * 2;
            if (byte + 1 >= size) {
              error = "truncated IA8 payload";
              return false;
            }
            const uint8_t alpha = data[byte + 0];
            const uint8_t value = data[byte + 1];
            put(image, x, y, value, value, value, alpha);
            break;
          }
          case TEX_FMT_RGB565: {
            const size_t byte = offset + static_cast<size_t>(index) * 2;
            if (byte + 1 >= size) {
              error = "truncated RGB565 payload";
              return false;
            }
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            decode_rgb565_value(read_be16(data + byte), r, g, b);
            put(image, x, y, r, g, b, 255);
            break;
          }
          case TEX_FMT_RGB5A3: {
            const size_t byte = offset + static_cast<size_t>(index) * 2;
            if (byte + 1 >= size) {
              error = "truncated RGB5A3 payload";
              return false;
            }
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            uint8_t a = 0;
            decode_rgb5a3_value(read_be16(data + byte), r, g, b, a);
            put(image, x, y, r, g, b, a);
            break;
          }
          case TEX_FMT_Z8: {
            const size_t byte = offset + static_cast<size_t>(index);
            if (byte >= size) {
              error = "truncated Z8 payload";
              return false;
            }
            const uint8_t value = data[byte];
            put(image, x, y, value, value, value, 255);
            break;
          }
          default:
            error = "unsupported tiled format";
            return false;
          }
        }
      }
      offset += static_cast<size_t>(data_size(format, tile_width, tile_height));
    }
  }
  return true;
}

bool decode_payload(TexFormat format, const uint8_t* data, size_t size, Image& image,
                    std::string& error) {
  image.source_format = format;
  image.rgba.assign(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4, 0);

  switch (format) {
  case TEX_FMT_S3TC:
    return decode_cmpr(data, size, image, error);
  case TEX_FMT_RGBA8:
    return decode_rgba8(data, size, image, error);
  default:
    return decode_tiled(format, data, size, image, error);
  }
}

bool validate_dimensions(const Image& image, std::string& error) {
  if (image.width <= 0 || image.height <= 0 || image.width > 4096 || image.height > 4096) {
    error = "implausible texture dimensions";
    return false;
  }
  return true;
}

} // namespace

TexFormat format_from_gx(uint8_t gx_format) {
  if (gx_format >= sizeof(kGxToInternal) / sizeof(kGxToInternal[0])) {
    return TEX_FMT_NULL;
  }
  return kGxToInternal[gx_format];
}

int data_size(TexFormat format, int width, int height) {
  switch (format) {
  case TEX_FMT_S3TC:
    return (width * height / 8) * 4;
  case TEX_FMT_I4:
    return width * height / 2;
  case TEX_FMT_I8:
  case TEX_FMT_IA4:
  case TEX_FMT_Z8:
    return width * height;
  case TEX_FMT_IA8:
  case TEX_FMT_RGB565:
  case TEX_FMT_RGB5A3:
    return width * height * 2;
  case TEX_FMT_RGBA8:
    return width * height * 4;
  default:
    return 0;
  }
}

void tile_size(TexFormat format, int& tile_width, int& tile_height) {
  switch (format) {
  case TEX_FMT_I4:
    tile_width = 8;
    tile_height = 8;
    break;
  case TEX_FMT_I8:
  case TEX_FMT_IA4:
    tile_width = 8;
    tile_height = 4;
    break;
  default:
    tile_width = 4;
    tile_height = 4;
    break;
  }
}

bool decode_texture_payload(TexFormat format, int width, int height, const uint8_t* data,
                            size_t size, Image& out, std::string& error) {
  out.width = width;
  out.height = height;
  if (!validate_dimensions(out, error)) {
    return false;
  }
  const int required = data_size(format, width, height);
  if (required <= 0 || static_cast<size_t>(required) > size) {
    error = "embedded texture payload is truncated or unsupported";
    return false;
  }
  return decode_payload(format, data, size, out, error);
}

bool decode_bti(const uint8_t* data, size_t size, Image& out, std::string& error) {
  if (size < kBtiHeaderSize) {
    error = "file shorter than BTI header";
    return false;
  }

  const TexFormat format = format_from_gx(data[0]);
  if (format == TEX_FMT_NULL) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "unsupported BTI GX format %u", data[0]);
    error = buffer;
    return false;
  }

  out.width = read_be16(data + 2);
  out.height = read_be16(data + 4);
  if (!validate_dimensions(out, error)) {
    return false;
  }

  const uint32_t image_offset = read_be32(data + 0x1C);
  if (image_offset != kBtiHeaderSize) {
    char buffer[80];
    std::snprintf(buffer, sizeof(buffer), "BTI image data at unexpected offset %u", image_offset);
    error = buffer;
    return false;
  }

  return decode_payload(format, data + image_offset, size - image_offset, out, error);
}

bool decode_txe(const uint8_t* data, size_t size, Image& out, std::string& error) {
  const size_t kTxeHeaderSize = 32;
  if (size < kTxeHeaderSize) {
    error = "file shorter than TXE header";
    return false;
  }

  out.width = read_be16(data + 0);
  out.height = read_be16(data + 2);
  if (!validate_dimensions(out, error)) {
    return false;
  }

  // The format short packs wrap flags into its high byte.
  const TexFormat format = static_cast<TexFormat>(read_be16(data + 4) & 0xFF);
  if (data_size(format, out.width, out.height) == 0) {
    error = "unsupported TXE format";
    return false;
  }

  return decode_payload(format, data + kTxeHeaderSize, size - kTxeHeaderSize, out, error);
}

bool decode_texture_file(const std::string& path, Image& out, std::string& error) {
  std::vector<uint8_t> bytes;
  if (!read_file(path, bytes, error)) {
    return false;
  }

  const bool is_txe = path.size() >= 4 && path.compare(path.size() - 4, 4, ".txe") == 0;
  if (is_txe) {
    return decode_txe(bytes.data(), bytes.size(), out, error);
  }
  return decode_bti(bytes.data(), bytes.size(), out, error);
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "could not open " + path;
    return false;
  }

  std::fseek(file, 0, SEEK_END);
  const long length = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (length <= 0) {
    std::fclose(file);
    error = "empty file " + path;
    return false;
  }

  out.resize(static_cast<size_t>(length));
  const size_t got = std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  if (got != out.size()) {
    error = "short read on " + path;
    return false;
  }
  return true;
}

} // namespace gcn
