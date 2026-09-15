// Decodes a MOD's embedded textures to PNG-free PPM files so the decode path
// can be checked on the host, which is where a black-rendering scene is
// cheapest to diagnose.

#include <cstdio>
#include <string>
#include <vector>

#include "cine/cine.h"
#include "gcn_texture.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: mod_textures <model.mod> <output-dir>\n");
        return 2;
    }
    const std::string mod_path = argv[1];
    const std::string out_dir = argv[2];

    try {
        const std::vector<std::uint8_t> bytes = cine::read_file(mod_path);
        const cine::ModInventory model = cine::parse_mod(bytes, mod_path);
        std::printf("textures: %zu\n", model.textures.size());

        for (std::size_t i = 0; i < model.textures.size(); ++i) {
            const cine::ModTexture& source = model.textures[i];
            gcn::Image image;
            std::string error;
            if (!gcn::decode_texture_payload(static_cast<gcn::TexFormat>(source.format),
                                             source.width, source.height, source.data.data(),
                                             source.data.size(), image, error)) {
                std::printf("  %2zu: %ux%u format=%u DECODE FAILED: %s\n", i, source.width,
                            source.height, source.format, error.c_str());
                continue;
            }

            // Summarise brightness so an all-black decode is obvious without
            // opening the file.
            unsigned long long sum = 0;
            std::size_t opaque = 0;
            for (std::size_t p = 0; p < image.rgba.size(); p += 4) {
                sum += image.rgba[p] + image.rgba[p + 1] + image.rgba[p + 2];
                if (image.rgba[p + 3] > 8) {
                    ++opaque;
                }
            }
            const std::size_t count = image.rgba.size() / 4;
            std::printf("  %2zu: %4dx%-4d format=%-3u mean_rgb=%6.1f opaque=%zu/%zu\n", i,
                        image.width, image.height, source.format,
                        count ? static_cast<double>(sum) / (count * 3.0) : 0.0, opaque, count);

            char path[512];
            std::snprintf(path, sizeof(path), "%s/tex_%02zu.ppm", out_dir.c_str(), i);
            std::FILE* file = std::fopen(path, "wb");
            if (file == nullptr) {
                continue;
            }
            std::fprintf(file, "P6\n%d %d\n255\n", image.width, image.height);
            for (std::size_t p = 0; p < image.rgba.size(); p += 4) {
                std::fputc(image.rgba[p + 0], file);
                std::fputc(image.rgba[p + 1], file);
                std::fputc(image.rgba[p + 2], file);
            }
            std::fclose(file);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
