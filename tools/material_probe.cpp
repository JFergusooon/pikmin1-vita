// Prints each material's colour and pixel-engine state, so it can be seen
// whether a surface is meant to be blended rather than written opaque.

#include <cstdio>
#include <string>
#include <vector>

#include "cine/cine.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: material_probe <model.mod>\n");
    return 2;
  }
  const std::string path = argv[1];
  const std::vector<std::uint8_t> bytes = cine::read_file(path);
  const cine::ModInventory model = cine::parse_mod(bytes, path);
  const cine::Geometry geometry = cine::build_geometry(bytes, model, path);

  std::vector<long> triangles(model.materials.size(), 0);
  for (std::size_t b = 0; b < geometry.batches.size(); ++b) {
    const cine::GeoBatch& batch = geometry.batches[b];
    if (batch.material_index >= 0 &&
        static_cast<std::size_t>(batch.material_index) < triangles.size()) {
      triangles[static_cast<std::size_t>(batch.material_index)] += batch.index_count / 3;
    }
  }

  std::printf("%s: %zu materials\n", path.c_str(), model.materials.size());
  std::printf("%3s %-18s %-10s %-10s %-10s %-10s %s\n", "idx", "color rgba", "flags", "alphacmp",
              "blend", "pe_ctrl", "tris");
  for (std::size_t i = 0; i < model.materials.size(); ++i) {
    const cine::Material& m = model.materials[i];
    char color[32];
    std::snprintf(color, sizeof(color), "%3u,%3u,%3u,%3u", m.color[0], m.color[1], m.color[2],
                  m.color[3]);
    int stages = -1;
    int max_texmap = -1;
    if (m.tev_info_index >= 0 &&
        static_cast<std::size_t>(m.tev_info_index) < model.tev_infos.size()) {
      const cine::TevInfo& tev = model.tev_infos[static_cast<std::size_t>(m.tev_info_index)];
      stages = static_cast<int>(tev.stages.size());
      max_texmap = cine::tev_max_texmap(tev);
    }
    std::printf("%3zu %-18s 0x%08x 0x%08x 0x%08x 0x%08x %-7ld texs=%zu texgens=%u stages=%d "
                "maxtexmap=%d\n",
                i, color, m.flags, m.alpha_compare, m.blend_mode, m.pe_control, triangles[i],
                m.textures.size(), m.texgen_count, stages, max_texmap);
  }
  // With a material index, dump its TEV chain and which image each texmap
  // resolves to, which is what a correct shader has to reproduce.
  if (argc >= 3) {
    const std::size_t want = static_cast<std::size_t>(std::atoi(argv[2]));
    if (want >= model.materials.size()) {
      std::fprintf(stderr, "material %zu out of range\n", want);
      return 2;
    }
    const cine::Material& m = model.materials[want];
    std::printf("\nmaterial %zu detail\n", want);
    std::printf("  lighting_control=0x%08x depth_test=0x%08x\n", m.lighting_control, m.depth_test);
    for (std::size_t g = 0; g < m.texgens.size(); ++g) {
      const cine::TexGen& tg = m.texgens[g];
      std::printf("  texgen %zu: coord=%u type=%u source=%u matrix=%u\n", g, tg.coord, tg.type,
                  tg.source, tg.matrix);
    }
    for (std::size_t t = 0; t < m.textures.size(); ++t) {
      const cine::MaterialTexture& mt = m.textures[t];
      std::printf("  texture %zu transform: scale=(%.3f,%.3f) rot=%.2f trans=(%.3f,%.3f) "
                  "pivot=(%.3f,%.3f) anim_factor=%u\n",
                  t, mt.transform[0], mt.transform[1], mt.transform[2], mt.transform[3],
                  mt.transform[4], mt.transform[5], mt.transform[6], mt.animation_factor);
    }
    for (std::size_t t = 0; t < m.textures.size(); ++t) {
      const std::uint32_t attr = m.textures[t].source_attribute;
      int tex = -1;
      if (attr < model.texture_attributes.size()) {
        tex = model.texture_attributes[attr].texture_index;
      }
      std::printf("  texmap %zu -> attribute %u -> image %d\n", t, attr, tex);
    }
    if (m.tev_info_index >= 0 &&
        static_cast<std::size_t>(m.tev_info_index) < model.tev_infos.size()) {
      const cine::TevInfo& tev = model.tev_infos[static_cast<std::size_t>(m.tev_info_index)];
      for (int r = 0; r < 3; ++r) {
        std::printf("  reg%d = %d,%d,%d,%d\n", r, tev.registers[r][0], tev.registers[r][1],
                    tev.registers[r][2], tev.registers[r][3]);
      }
      for (int k = 0; k < 4; ++k) {
        std::printf("  konst%d = %u,%u,%u,%u\n", k, tev.konst[k][0], tev.konst[k][1],
                    tev.konst[k][2], tev.konst[k][3]);
      }
      for (std::size_t s = 0; s < tev.stages.size(); ++s) {
        const cine::TevStage& st = tev.stages[s];
        std::printf("  stage %zu: texcoord=%u texmap=%u chan=%u kc=%u ka=%u\n", s, st.texcoord,
                    st.texmap, st.channel, st.konst_color, st.konst_alpha);
        std::printf("    color a=%u b=%u c=%u d=%u op=%u bias=%u scale=%u clamp=%u out=%u\n",
                    st.color.a, st.color.b, st.color.c, st.color.d, st.color.op, st.color.bias,
                    st.color.scale, st.color.clamp, st.color.out_reg);
        std::printf("    alpha a=%u b=%u c=%u d=%u op=%u bias=%u scale=%u clamp=%u out=%u\n",
                    st.alpha.a, st.alpha.b, st.alpha.c, st.alpha.d, st.alpha.op, st.alpha.bias,
                    st.alpha.scale, st.alpha.clamp, st.alpha.out_reg);
      }
    }
  }
  return 0;
}
