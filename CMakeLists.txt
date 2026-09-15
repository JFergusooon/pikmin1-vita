cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK to your VitaSDK installation")
  endif()
endif()

project(pikmin_vita LANGUAGES C CXX)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Pikmin Vita")
set(VITA_TITLEID "PIKMINVIT")
set(VITA_VERSION "01.00")

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/blo.cpp
  src/gcn_texture.cpp
  src/gcn_font.cpp
  src/load_logo.cpp
  src/p2d_text.cpp
  src/screen.cpp
  src/scene_raster.cpp
  src/title_animation.cpp
  src/cine/cine.cpp
  src/render/matrix.cpp
  src/render/skeleton.cpp
  src/render/gxm3d.cpp
  src/render/tev_programs.cpp
)

# Keep the CPU-rasterised title background at the Vita's native 960x544.
# Performance is adjusted only through temporal sampling. A stride of two
# samples the 60 Hz cinematic at 30 fps, which the 16-bit frame strip has the
# memory for. The boot logo covers however long the strip takes to render.
set(PIKMIN_BACKGROUND_STRIDE "2" CACHE STRING "Cinematic frames between rasterised descent frames")
target_compile_definitions(${PROJECT_NAME} PRIVATE
  PIKMIN_BACKGROUND_STRIDE=${PIKMIN_BACKGROUND_STRIDE})

# Draws a depth-tested textured cube instead of the title background, to verify
# the GXM 3D path on hardware and under Vita3K. Never enabled for a real build.
option(PIKMIN_RENDER_PROBE "Build the 3D pipeline bring-up probe" OFF)
if(PIKMIN_RENDER_PROBE)
  target_sources(${PROJECT_NAME} PRIVATE src/render/probe.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE PIKMIN_RENDER_PROBE=1)
endif()

# Draws the opening.cin 3D background behind the title panes. The per-material
# TEV pipeline is still a single-stage approximation, so the result does not
# match GX yet; keep it off until it does.
option(PIKMIN_OPENING_3D "Draw the opening.cin 3D background" OFF)
if(PIKMIN_OPENING_3D)
  target_sources(${PROJECT_NAME} PRIVATE src/render/opening_scene.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE PIKMIN_OPENING_3D=1)
else()
  target_sources(${PROJECT_NAME} PRIVATE src/render/title_background.cpp)
endif()

# Holds the title cinematic on one frame so tools/capture.sh lands on a known
# point in the sequence. Reading the framebuffer from inside the app was tried
# instead and returns blank pixels under Vita3K, so capture goes through the
# emulator window.
set(PIKMIN_FREEZE_FRAME "-1" CACHE STRING "Hold the title cinematic on this frame")
target_compile_definitions(${PROJECT_NAME} PRIVATE PIKMIN_FREEZE_FRAME=${PIKMIN_FREEZE_FRAME})

# Same idea for the boot logo, which retail shows for however long the disc
# load takes. The Vita3K window takes longer than the default hold to appear,
# so a capture of the logo needs it stretched.
set(PIKMIN_BOOT_HOLD "2.5" CACHE STRING "Seconds to hold the boot-up Nintendo logo")
target_compile_definitions(${PROJECT_NAME} PRIVATE PIKMIN_BOOT_HOLD=${PIKMIN_BOOT_HOLD})

# vita-elf-create appends its SCE module data after the read-only segment and
# fails outright if the read-write segment's page-aligned start leaves it less
# room than it needs. The default layout put the two within 2KB of each other,
# so the 3D build stopped linking after a kilobyte of unrelated code was added.
# Placing the read-write segment on a fixed higher page keeps tens of kilobytes
# of slack there regardless of how the code grows.
target_link_options(${PROJECT_NAME} PRIVATE
  "LINKER:--section-start=.init_array=0x81100000"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_14)
target_compile_options(${PROJECT_NAME} PRIVATE
  -O3
  # The rasteriser is the whole boot cost and it is all float work. Relaxed
  # math lets floor/min/max compile to single VFP instructions instead of
  # errno-setting calls.
  -ffast-math
  -Wall
  -Wextra
  -Wno-unused-parameter
)
target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceAppMgr_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceKernelDmacMgr_stub
  ScePgf_stub
  SceSysmodule_stub
  png
  jpeg
  z
  m
)

# The per-material TEV programs are generated, so the set of shader binaries is
# discovered rather than listed. Rerun shaders/compile.sh before configuring if
# the generator has produced new ones.
file(GLOB PIKMIN_SHADER_BINARIES
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gxp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gxp/*.vgxp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gxp/*.fgxp)
set(PIKMIN_SHADER_FILES "")
foreach(shader ${PIKMIN_SHADER_BINARIES})
  list(APPEND PIKMIN_SHADER_FILES FILE shaders/gxp/${shader} shaders/${shader})
endforeach()

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${PIKMIN_SHADER_FILES}
)
