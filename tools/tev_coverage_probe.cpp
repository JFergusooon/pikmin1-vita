#include "cine/cine.h"
#include "render/tev_programs.hpp"
#include <cstdio>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  size_t n = 0;
  const render::TevProgramEntry* table = render::tev_program_table(&n);
  std::set<unsigned long long> have;
  std::printf("generated programs (%zu):\n", n);
  for (size_t i = 0; i < n; ++i) {
    std::printf("  0x%016llx %s texcoords=%d\n", (unsigned long long)table[i].key, table[i].name,
                table[i].texcoords);
    have.insert(table[i].key);
  }
  for (int a = 1; a < argc; ++a) {
    const std::vector<std::uint8_t> bytes = cine::read_file(argv[a]);
    const cine::ModInventory model = cine::parse_mod(bytes, argv[a]);
    const cine::Geometry geo = cine::build_geometry(bytes, model, argv[a]);
    std::printf("\n== %s: %zu batches, %zu materials, %zu tev_infos\n", argv[a],
                geo.batches.size(), model.materials.size(), model.tev_infos.size());
    long drawn = 0, skipped = 0, drawn_idx = 0, skipped_idx = 0;
    for (size_t b = 0; b < geo.batches.size(); ++b) {
      const cine::GeoBatch& batch = geo.batches[b];
      const char* why = "ok";
      unsigned long long key = 0;
      if (batch.material_index < 0 || (size_t)batch.material_index >= model.materials.size()) {
        why = "no material";
      } else {
        const cine::Material& m = model.materials[batch.material_index];
        if (m.tev_info_index < 0 || (size_t)m.tev_info_index >= model.tev_infos.size()) {
          why = "no tev_info";
        } else {
          key = cine::tev_key(model.tev_infos[m.tev_info_index]);
          if (!have.count(key)) why = "MISSING PROGRAM";
        }
      }
      const bool ok = std::string(why) == "ok";
      std::printf("  batch %2zu mat=%2d joint=%2d indices=%6u key=0x%016llx  %s\n", b,
                  batch.material_index, batch.parent_joint, batch.index_count, key, why);
      if (ok) { ++drawn; drawn_idx += batch.index_count; }
      else { ++skipped; skipped_idx += batch.index_count; }
    }
    std::printf("  -> drawn %ld batches (%ld indices), skipped %ld batches (%ld indices)\n",
                drawn, drawn_idx, skipped, skipped_idx);
  }
  return 0;
}
