// Pikmin Vita: title screen shell.
//
// Renders the game's real P2D menu layouts, decoded from the user's own disc
// data in ux0:data/pikmin, through vita2d instead of GX.

#include "blo.hpp"
#include "gcn_font.hpp"
#include "gcn_texture.hpp"
#include "load_logo.hpp"
#include "screen.hpp"
#include "title_animation.hpp"

#if PIKMIN_RENDER_PROBE
#include "render/probe.hpp"
#endif

#if PIKMIN_OPENING_3D
#include "render/opening_scene.hpp"
#else
#include "render/title_background.hpp"
#endif

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const int kScreenWidth = 960;
const int kScreenHeight = 544;

const char* const kDataRoot = "ux0:data/pikmin/dataDir/";

// USA retail resolves its 2D assets through these directories; see
// GameFlow::setLanguage in upstream/pikmin/src/plugPikiColin/gameflow.cpp.
const char* const kBloDir = "ux0:data/pikmin/dataDir/screen/eng_blo/";
const char* const kTexDir = "ux0:data/pikmin/dataDir/screen/eng_tex/";

bool file_exists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fclose(file);
  return true;
}

// vita2d's default texture format is A8B8G8R8, which on little-endian hardware
// lays out bytes as R,G,B,A - the same order the decoder emits.
vita2d_texture* upload(const gcn::Image& image) {
  vita2d_texture* texture = vita2d_create_empty_texture_format(
      static_cast<unsigned>(image.width), static_cast<unsigned>(image.height),
      SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
  if (texture == nullptr) {
    return nullptr;
  }

  uint8_t* destination = static_cast<uint8_t*>(vita2d_texture_get_datap(texture));
  const unsigned stride = vita2d_texture_get_stride(texture);
  const size_t row_bytes = static_cast<size_t>(image.width) * 4;
  for (int y = 0; y < image.height; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * stride,
                image.rgba.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
  }
  return texture;
}

class TextureCache {
public:
  // Decoding and uploading every texture a layout references up front keeps the
  // first frame of a menu from stalling on disc reads.
  int preload(const screen::DrawList& list) {
    int loaded = 0;
    for (const screen::Quad& quad : list.quads) {
      if (get(quad.texture) != nullptr) {
        ++loaded;
      }
    }
    return loaded;
  }

  vita2d_texture* get(const std::string& name) {
    auto found = textures_.find(name);
    if (found != textures_.end()) {
      return found->second;
    }

    vita2d_texture* texture = nullptr;
    gcn::Image image;
    std::string error;
    if (gcn::decode_texture_file(std::string(kTexDir) + name, image, error)) {
      texture = upload(image);
    }
    textures_[name] = texture;
    return texture;
  }

  ~TextureCache() {
    for (auto& entry : textures_) {
      if (entry.second != nullptr) {
        vita2d_free_texture(entry.second);
      }
    }
  }

private:
  std::map<std::string, vita2d_texture*> textures_;
};

struct Layout {
  blo::Pane root;
  screen::DrawList draws;
  bool loaded = false;
  std::string error;

  bool load(const std::string& name) {
    if (!blo::parse_file(std::string(kBloDir) + name, root, error)) {
      loaded = false;
      return false;
    }
    screen::flatten(root, draws);
    loaded = true;
    return true;
  }
};

void draw_quads(const screen::DrawList& list, const screen::Viewport& viewport,
                TextureCache& textures) {
  for (const screen::Quad& quad : list.quads) {
    if (quad.width <= 0 || quad.height <= 0) {
      continue;
    }
    vita2d_texture* texture = textures.get(quad.texture);
    if (texture == nullptr) {
      continue;
    }

    const float texture_width = static_cast<float>(vita2d_texture_get_width(texture));
    const float texture_height = static_cast<float>(vita2d_texture_get_height(texture));
    if (texture_width <= 0.0f || texture_height <= 0.0f) {
      continue;
    }

    vita2d_draw_texture_scale(texture, viewport.offset_x + quad.x * viewport.scale,
                              viewport.offset_y + quad.y * viewport.scale,
                              quad.width * viewport.scale / texture_width,
                              quad.height * viewport.scale / texture_height);
  }
}

uint8_t lerp8(uint8_t a, uint8_t b, float t) {
  return static_cast<uint8_t>(a + (static_cast<float>(b) - a) * t + 0.5f);
}

// Text is drawn from the game's own bigFont.bti atlas. GX modulated each glyph
// quad with a vertex colour that ran top-to-bottom, so the gradient is
// approximated here by slicing every glyph into horizontal bands.
const int kGradientSlices = 6;

void draw_text_run(const screen::Text& text, const screen::Viewport& viewport,
                   const gcn::Font& font, vita2d_texture* atlas) {
  const int draw_width = text.font_width > 0 ? text.font_width : font.cell_width();
  const int draw_height = text.font_height > 0 ? text.font_height : font.cell_height();

  const float run_width = font.advance_width(text.text, draw_width);
  float x = static_cast<float>(text.x);
  switch (text.align_h) {
  case 0: // TBOXHBIND_Center
    x += (text.width - run_width) * 0.5f;
    break;
  case 1: // TBOXHBIND_Right
    x += text.width - run_width;
    break;
  default: // TBOXHBIND_Left
    break;
  }
  const float baseline = text.y + (text.height + draw_height) * 0.5f;

  std::vector<gcn::GlyphQuad> quads;
  font.layout(text.text, x, baseline, draw_width, draw_height, quads);

  for (const gcn::GlyphQuad& quad : quads) {
    const float height = quad.y1 - quad.y0;
    const float width = quad.x1 - quad.x0;
    if (height <= 0.0f || width <= 0.0f) {
      continue;
    }
    const float x_scale = (width * viewport.scale) / static_cast<float>(quad.source_width);

    for (int slice = 0; slice < kGradientSlices; ++slice) {
      const float t0 = static_cast<float>(slice) / kGradientSlices;
      const float t1 = static_cast<float>(slice + 1) / kGradientSlices;
      const float mid = (t0 + t1) * 0.5f;

      const float source_y = quad.source_y + quad.source_height * t0;
      const float source_height = quad.source_height * (t1 - t0);
      const float y_scale = (height * (t1 - t0) * viewport.scale) / source_height;

      const unsigned color = RGBA8(lerp8(text.color.r, text.grad_color.r, mid),
                                   lerp8(text.color.g, text.grad_color.g, mid),
                                   lerp8(text.color.b, text.grad_color.b, mid),
                                   static_cast<uint8_t>(
                                       lerp8(text.color.a, text.grad_color.a, mid)
                                       * text.alpha / 255));

      vita2d_draw_texture_tint_part_scale(
          atlas, viewport.offset_x + quad.x0 * viewport.scale,
          viewport.offset_y + (quad.y0 + height * t0) * viewport.scale,
          static_cast<float>(quad.source_x), source_y, static_cast<float>(quad.source_width),
          source_height, x_scale, y_scale, color);
    }
  }
}

void draw_texts(const screen::DrawList& list, const screen::Viewport& viewport,
                const gcn::Font& font, vita2d_texture* atlas) {
  if (!font.valid() || atlas == nullptr) {
    return;
  }
  for (const screen::Text& text : list.texts) {
    draw_text_run(text, viewport, font, atlas);
  }
}

void draw_missing_data(vita2d_pgf* font) {
  vita2d_pgf_draw_text(font, 60.0f, 220.0f, RGBA8(255, 120, 90, 255), 1.2f,
                       "Pikmin game data not found");
  vita2d_pgf_draw_text(font, 60.0f, 264.0f, RGBA8(235, 235, 235, 255), 0.9f,
                       "Import your own Pikmin (USA) disc with tools/import_iso.py,");
  vita2d_pgf_draw_text(font, 60.0f, 292.0f, RGBA8(235, 235, 235, 255), 0.9f,
                       "then copy runtime-data/ to ux0:data/pikmin/");
  vita2d_pgf_draw_text(font, 60.0f, 336.0f, RGBA8(160, 160, 170, 255), 0.85f,
                       "Expected: ux0:data/pikmin/dataDir/screen/eng_blo/");
}

enum class Stage {
  NinLogo,
  Opening,
  PressStart,
  MainMenu,
};

// Reports how much of the title background has been rasterised, under the boot
// logo. Retail has nothing like this because its wait is a disc read; here the
// wait is CPU work long enough that an unchanging screen is indistinguishable
// from a hang.
void draw_load_progress(vita2d_pgf* font, float progress, bool rendering) {
  const float width = 420.0f;
  const float height = 10.0f;
  const float x = (static_cast<float>(kScreenWidth) - width) * 0.5f;
  const float y = static_cast<float>(kScreenHeight) * 0.72f;

  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;

  vita2d_draw_rectangle(x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f, RGBA8(40, 40, 40, 255));
  vita2d_draw_rectangle(x, y, width, height, RGBA8(16, 16, 16, 255));
  vita2d_draw_rectangle(x, y, width * progress, height, RGBA8(220, 220, 210, 255));

  if (font != nullptr) {
    char label[64];
    std::snprintf(label, sizeof(label), "%s  %d%%",
                  rendering ? "Rendering title screen" : "Loading title screen",
                  static_cast<int>(progress * 100.0f + 0.5f));
    vita2d_pgf_draw_text(font, x, y - 14.0f, RGBA8(200, 200, 195, 255), 0.9f, label);

    if (rendering) {
      // Only the first run pays for this, and it is long enough that saying so
      // is the difference between waiting and assuming a hang.
      vita2d_pgf_draw_text(font, x, y + height + 20.0f, RGBA8(140, 140, 138, 255), 0.8f,
                           "First run only - saved for next time");
    }
  }
}

// The title sequence is presented at a locked 30 Hz: one displayed frame per
// two vblanks. The cinematic itself is authored against 60 Hz updates, so each
// displayed frame advances it by two to keep the descent at its real speed.
const int kVblanksPerFrame = 2;
const int kCinematicFramesPerUpdate = kVblanksPerFrame;
const float kFrameTime = static_cast<float>(kVblanksPerFrame) / 60.0f;

// NinLogoSection lasts as long as the disc load and the progressive-scan check
// behind it, with Jac_SceneSetup(SCENE_BootUp) playing the boot voice clip over
// the logo. Nothing here loads on a worker thread, so the hold runs for the
// length of that clip rather than ending immediately on a black screen.
const float kBootLogoHoldSeconds = PIKMIN_BOOT_HOLD;


} // namespace

int main() {
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  vita2d_init();
  // Frame pacing is done explicitly after the swap, so vita2d must not add a
  // wait of its own on top.
  vita2d_set_vblank_wait(0);
  vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
  vita2d_pgf* font = vita2d_load_default_pgf();

  const bool has_data = file_exists(std::string(kDataRoot) + "screen/eng_blo/press_s.blo");

  TextureCache textures;
  Layout press_start;
  Layout main_menu;

  // Menus always render through bigFont.bti as a 21x42 grid, whatever font name
  // the layout names; see P2DFont::loadFont in the decomp.
  gcn::Font menu_font;
  vita2d_texture* font_atlas = nullptr;
  vita2d_texture* nintendo_logo = nullptr;
  loadlogo::Rect logo_rect;

  if (has_data) {
    press_start.load("press_s.blo");
    main_menu.load("m_select.blo");
    textures.preload(press_start.draws);
    textures.preload(main_menu.draws);

    gcn::Image atlas;
    std::string font_error;
    if (gcn::decode_texture_file(std::string(kDataRoot) + "bigFont.bti", atlas, font_error) &&
        menu_font.build(atlas, 21, 42, font_error)) {
      font_atlas = upload(atlas);
    }

    // GameFlow::init loads this as the boot-up load banner, before any of the
    // title screen exists.
    gcn::Image logo;
    std::string logo_error;
    if (gcn::decode_texture_file(std::string(kDataRoot) + "intro/nintendo.bti", logo,
                                 logo_error)) {
      nintendo_logo = upload(logo);
      logo_rect = loadlogo::rect(logo.width, logo.height, screen::kOrthoWidth,
                                 screen::kOrthoHeight, true);
    }
  }

#if PIKMIN_RENDER_PROBE
  render::Probe probe;
  const bool probe_ready = font_atlas != nullptr && probe.init(font_atlas);
  int probe_frame = 0;
#endif
#if PIKMIN_OPENING_3D
  render::OpeningScene opening_scene;
  const bool opening_ready = has_data && opening_scene.init(kDataRoot);
#else
  // Rasterised below, on the frame the boot logo goes up, so the CPU work lands
  // where retail is reading the disc.
  render::TitleBackground background;
  const bool background_begun = has_data && background.begin(kDataRoot);
#endif

  const int item_count = main_menu.loaded ? screen::count_menu_items(main_menu.root) : 0;

  // The layout carries the colour a selected entry blends towards in a pane
  // that is never drawn, so it has to be read out before the menu is shown.
  screen::MenuAnimation menu_animation;
  menu_animation.reset(item_count);
  blo::Color menu_blend_char;
  blo::Color menu_blend_grad;
  if (main_menu.loaded) {
    screen::menu_blend_colors(main_menu.root, menu_blend_char, menu_blend_grad);
  }
  const screen::Viewport viewport =
      screen::fit(screen::kOrthoWidth, screen::kOrthoHeight, kScreenWidth, kScreenHeight);

  loadlogo::Fade logo_fade;
  float logo_hold = 0.0f;

#if PIKMIN_FREEZE_FRAME >= 0
  // A frozen capture names a frame of the cinematic, so the boot logo stage is
  // skipped and its banner left to retire over the first second.
  Stage stage = Stage::Opening;
  logo_fade.release();
#else
  Stage stage = Stage::NinLogo;
#endif

  title::PressStartAnimation press_animation;
  bool entering_menu = false;
  int title_frame = 0;
  int selected = 0;
  unsigned previous_buttons = 0;
  bool running = true;

  while (running) {
    SceCtrlData pad;
    std::memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const unsigned pressed = pad.buttons & ~previous_buttons;
    previous_buttons = pad.buttons;

    if ((pressed & SCE_CTRL_SELECT) != 0) {
      running = false;
    }

#if PIKMIN_FREEZE_FRAME >= 0
    // Holds the cinematic still so a window capture lands on a known frame.
    title_frame = PIKMIN_FREEZE_FRAME;
#else
    // The cinematic clock only starts once the boot logo hands over to the
    // titles section; NinLogoSection runs before opening.cin exists.
    //
    // It is deliberately not stopped at the descent's last camera key. The
    // flowers keep swaying on their own loop once the camera lands, and the
    // background plays that back by repeating a cycle that sits past the
    // descent, so capping the clock here froze them on the settled screen.
    if (stage != Stage::NinLogo) {
      title_frame += kCinematicFramesPerUpdate;
    }
#endif

    logo_fade.update(kFrameTime);

    if (stage == Stage::NinLogo) {
      logo_hold += kFrameTime;
#if !PIKMIN_OPENING_3D
      // Retail holds this screen for as long as the disc read takes, which is
      // the same job the rasteriser is doing here: one strip frame per update,
      // so the screen keeps refreshing and can report progress.
      const bool background_pending = has_data && !background.done();
      if (background_pending) {
        background.step();
      }
#else
      const bool background_pending = false;
#endif
      if (logo_hold >= kBootLogoHoldSeconds && !background_pending) {
        // NinLogoSetupSection::update transits to SECTION_Titles, whose draw
        // starts winding the banner back down. release() only moves the fade
        // out of its hold phase, so repeating it is harmless.
        logo_fade.release();

        // The cinematic is held back until the banner has gone entirely.
        // Switching stage as soon as the fade starts runs the first second of
        // the descent behind a logo that is still on screen.
        if (!logo_fade.visible()) {
          stage = Stage::Opening;
        }
      }
    } else if (stage == Stage::Opening) {
      // opening.cin advances one cinematic frame per 60 Hz game update.
      // Its frame-75 notify key calls TITLECMD_ShowStartMenu.
      if (title_frame > 75) {
        press_animation.start();
        stage = Stage::PressStart;
      }
    } else if (stage == Stage::PressStart) {
      if (!entering_menu
          && (pressed & (SCE_CTRL_START | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) != 0) {
        press_animation.stop();
        entering_menu = true;
      }
      press_animation.update(kFrameTime);
      if (entering_menu && !press_animation.active()) {
        stage = Stage::MainMenu;
        entering_menu = false;
      }
    } else if (stage == Stage::MainMenu) {
      if ((pressed & SCE_CTRL_UP) != 0 && item_count > 0) {
        selected = (selected + item_count - 1) % item_count;
      }
      if ((pressed & SCE_CTRL_DOWN) != 0 && item_count > 0) {
        selected = (selected + 1) % item_count;
      }
      if ((pressed & SCE_CTRL_CIRCLE) != 0) {
        stage = Stage::PressStart;
        press_animation.start();
      }
      menu_animation.update(selected, kFrameTime);
    }

    vita2d_start_drawing();
    vita2d_clear_screen();

#if PIKMIN_RENDER_PROBE
    if (probe_ready) {
      probe.draw(static_cast<float>(probe_frame) / 60.0f);
      ++probe_frame;
    }
#endif
    // NinLogoSection clears to black and draws the logo over nothing, so the
    // background stays off screen until the titles section starts.
#if PIKMIN_OPENING_3D
    if (stage == Stage::NinLogo) {
      // nothing behind the logo
    } else if (opening_ready) {
      opening_scene.draw(static_cast<float>(title_frame));
    } else {
      // Say why the 3D background is absent instead of leaving a black screen
      // that looks the same as a scene which drew nothing.
      vita2d_pgf_draw_text(font, 20.0f, 40.0f, RGBA8(255, 180, 120, 255), 1.0f,
                           opening_scene.error());
    }
#else
    if (stage != Stage::NinLogo) {
      if (background.ready()) {
        background.draw(static_cast<float>(title_frame));
      } else if (background_begun) {
        vita2d_pgf_draw_text(font, 20.0f, 40.0f, RGBA8(255, 180, 120, 255), 1.0f,
                             background.error());
      }
    } else if (background_begun && !background.done()) {
      draw_load_progress(font, background.progress(), background.building());
    }
#endif

    if (!has_data) {
      draw_missing_data(font);
    } else {
      if (stage == Stage::PressStart) {
        if (press_start.loaded) {
          draw_quads(press_start.draws, viewport, textures);
          screen::DrawList prompt = press_start.draws;
          for (screen::Text& text : prompt.texts) {
            text.alpha = press_animation.alpha();
          }
          draw_texts(prompt, viewport, menu_font, font_atlas);
        } else {
          vita2d_pgf_draw_text(font, 60.0f, 240.0f, RGBA8(255, 140, 110, 255), 1.0f,
                               press_start.error.c_str());
        }
      } else if (stage == Stage::MainMenu) {
        if (main_menu.loaded) {
          screen::DrawList list = main_menu.draws;
          screen::apply_menu_rules(list, selected, menu_animation, menu_blend_char,
                                   menu_blend_grad);
          draw_quads(list, viewport, textures);
          draw_texts(list, viewport, menu_font, font_atlas);
        } else {
          vita2d_pgf_draw_text(font, 60.0f, 240.0f, RGBA8(255, 140, 110, 255), 1.0f,
                               main_menu.error.c_str());
        }
      }

      // Both NinLogoSetupSection::draw and TitlesSection::draw call
      // drawLoadLogo last, so the banner sits over whatever the section drew.
      if (nintendo_logo != nullptr && logo_fade.visible()) {
        const loadlogo::Color tint = loadlogo::color(logo_fade.value(), true);
        vita2d_draw_texture_tint_scale(
            nintendo_logo, viewport.offset_x + logo_rect.x * viewport.scale,
            viewport.offset_y + logo_rect.y * viewport.scale, viewport.scale, viewport.scale,
            RGBA8(tint.r, tint.g, tint.b, tint.a));
      }
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();

    // vita2d's own wait is switched off at startup so the pacing is entirely
    // these waits: one per vblank the frame should occupy. Relying on vita2d
    // to contribute one of them left the loop running at 60 Hz, which also ran
    // the cinematic and the press-start flash at double speed, since each
    // update advances them by two 60 Hz ticks.
    //
    // While the strip is still being built a single frame takes far longer
    // than two vblanks anyway, so this only paces actual playback.
    for (int i = 0; i < kVblanksPerFrame; ++i) {
      sceDisplayWaitVblankStart();
    }
  }

  vita2d_wait_rendering_done();
#if PIKMIN_RENDER_PROBE
  probe.shutdown();
#endif
#if PIKMIN_OPENING_3D
  opening_scene.shutdown();
#else
  background.release();
#endif
  if (font_atlas != nullptr) {
    vita2d_free_texture(font_atlas);
  }
  if (nintendo_logo != nullptr) {
    vita2d_free_texture(nintendo_logo);
  }
  vita2d_free_pgf(font);
  vita2d_fini();
  sceKernelExitProcess(0);
  return 0;
}
