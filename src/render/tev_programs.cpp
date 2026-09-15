#include "render/tev_programs.hpp"

namespace render {
namespace {

const TevProgramEntry kPrograms[] = {
#define TEV_PROGRAM(key, name, texcoords) {key, name, texcoords},
#include "render/tev_programs.inc"
#undef TEV_PROGRAM
};

}  // namespace

const TevProgramEntry* tev_program_table(size_t* count) {
  *count = sizeof(kPrograms) / sizeof(kPrograms[0]);
  return kPrograms;
}

}  // namespace render
