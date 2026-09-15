#include "load_logo.hpp"

namespace loadlogo {
namespace {

float clamp01(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

} // namespace

Rect rect(int texture_width, int texture_height, int screen_width, int screen_height,
          bool nintendo_logo) {
  Rect out;
  out.width = texture_width;
  out.height = texture_height;
  out.x = screen_width / 2 - texture_width / 2;
  out.y = screen_height / 2 - texture_height / 2;
  if (nintendo_logo) {
    out.y -= 40;
  }
  return out;
}

Color color(float fade, bool nintendo_logo) {
  Color out;
  // USA and PAL discs tint the boot logo red; the Japanese ones use blue.
  out.r = nintendo_logo ? 220 : 192;
  out.g = nintendo_logo ? 0 : 64;
  out.b = 0;
  out.a = static_cast<uint8_t>(clamp01(fade) * 255.0f);
  return out;
}

void Fade::update(float dt) {
  if (dt < 0.0f) {
    dt = 0.0f;
  }

  switch (phase_) {
  case Phase::Hold:
    value_ = clamp01(value_ + dt / kFadeInSeconds);
    return;
  case Phase::FadeOut:
    value_ -= dt / kFadeOutSeconds;
    if (value_ <= 0.0f) {
      value_ = 0.0f;
      phase_ = Phase::Done;
    }
    return;
  case Phase::Done:
    value_ = 0.0f;
    return;
  }
}

void Fade::release() {
  if (phase_ == Phase::Hold) {
    phase_ = Phase::FadeOut;
  }
}

} // namespace loadlogo
