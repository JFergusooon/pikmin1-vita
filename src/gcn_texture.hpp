#pragma once

// Decoder for the GameCube texture files shipped on the Pikmin disc.
//
// Pixel semantics and tile sizes follow the upstream decompilation:
// TexImgFormat / BtiHeader in upstream/pikmin/include/Texture.h and
// TexImg::importBti / importTxe / calcDataSize / getTileSize in
// upstream/pikmin/src/sysCommon/graphics.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gcn {

// Values match TexImgFormat in the decomp, which is *not* the GX format order.
enum TexFormat {
  TEX_FMT_NULL = -1,
  TEX_FMT_RGB565 = 0,
  TEX_FMT_S3TC = 1,
  TEX_FMT_RGB5A3 = 2,
  TEX_FMT_I4 = 3,
  TEX_FMT_I8 = 4,
  TEX_FMT_IA4 = 5,
  TEX_FMT_IA8 = 6,
  TEX_FMT_RGBA8 = 7,
  TEX_FMT_Z8 = 8,
};

// Decoded texture, 8-bit RGBA, top-left origin, tightly packed.
struct Image {
  int width = 0;
  int height = 0;
  TexFormat source_format = TEX_FMT_NULL;
  std::vector<uint8_t> rgba;

  bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// Maps a BTI header's GX format index onto TexFormat. Returns TEX_FMT_NULL for
// the palette-only and reserved slots, which the Pikmin disc does not use.
TexFormat format_from_gx(uint8_t gx_format);

int data_size(TexFormat format, int width, int height);
void tile_size(TexFormat format, int& tile_width, int& tile_height);

bool decode_bti(const uint8_t* data, size_t size, Image& out, std::string& error);
bool decode_txe(const uint8_t* data, size_t size, Image& out, std::string& error);
bool decode_texture_payload(TexFormat format, int width, int height, const uint8_t* data,
                            size_t size, Image& out, std::string& error);

// Dispatches on the file extension, matching Texture::read in the decomp.
bool decode_texture_file(const std::string& path, Image& out, std::string& error);

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& error);

} // namespace gcn
