#pragma once

// The boot-up Nintendo logo, ported from GameFlow::drawLoadLogo in
// upstream/pikmin/src/plugPikiColin/gameflow.cpp together with the banner fade
// that GameFlow::init, GameLoadIdler::draw and TitlesSection::draw drive
// through GameFlow::mLevelBannerFadeValue.
//
// Keeping this free of vita2d lets the retail timing be checked on the host.

#include <cstdint>

namespace loadlogo {

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct Color {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 0;
};

// drawLoadLogo centres the banner on the full screen. The boot logo alone sits
// 40 pixels higher, which the decomp flags as deliberate but unexplained.
Rect rect(int texture_width, int texture_height, int screen_width, int screen_height,
          bool nintendo_logo);

// The flat colour drawLoadLogo tints the logo with, scaled by the banner fade.
// setColour and setAuxColour are given the same value for the boot logo, so it
// carries none of the vertical gradient a level banner does.
Color color(float fade, bool nintendo_logo);

enum class Phase {
  Hold,    // NinLogoSection: opaque for as long as loading lasts
  FadeOut, // TitlesSection has taken over and is retiring the logo
  Done,    // mIsNintendoLoadLogo cleared, so nothing more is drawn
};

// TitlesSection::draw subtracts one frame's worth of fade per frame, which
// spans a second at the game's 60 Hz update rate.
const float kFadeOutSeconds = 1.0f;

// GameLoadIdler::draw raises the fade by 1/300 per frame, so a banner that
// starts transparent needs five seconds to reach full opacity.
const float kFadeInSeconds = 5.0f;

// GameFlow::init leaves mLevelBannerFadeValue at 1.0 and the load idler clamps
// its rise at that ceiling, so the boot logo stays opaque for the whole of
// NinLogoSection however long the disc takes. TitlesSection::draw then winds it
// back down and clears mIsNintendoLoadLogo on reaching zero, retiring the logo
// one second into the title screen.
class Fade {
public:
  void update(float dt);

  // Loading is finished and the titles section has taken over drawing.
  void release();

  float value() const { return value_; }
  Phase phase() const { return phase_; }
  bool visible() const { return phase_ != Phase::Done; }

private:
  float value_ = 1.0f;
  Phase phase_ = Phase::Hold;
};

} // namespace loadlogo
