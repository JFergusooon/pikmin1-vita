#pragma once

#include <cstddef>
#include <cstdint>

namespace render {

// One generated shader pair, identified by the content key of the TEV
// configuration it reproduces. The table itself is emitted by
// tools/tev_shader_gen alongside the shaders.
struct TevProgramEntry {
  uint64_t key;
  const char* name;
  int texcoords;
};

const TevProgramEntry* tev_program_table(size_t* count);

}  // namespace render
