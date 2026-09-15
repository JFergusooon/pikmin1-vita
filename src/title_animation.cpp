#include "title_animation.hpp"

#include <cmath>

namespace title {
namespace {

const float kPi = 3.14159265358979323846f;
const float kHalfPi = kPi * 0.5f;
const float kTau = kPi * 2.0f;
const float kPressFadeDuration = 0.25f;

float clamp01(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

uint8_t alpha_byte(float value) {
  if (value <= 0.0f) {
    return 0;
  }
  if (value >= 255.0f) {
    return 255;
  }
  return static_cast<uint8_t>(value);
}

} // namespace

void PressStartAnimation::start() {
  state_ = PressState::Oscillate;
  timer_ = 0.0f;
  alpha_ = 0;
}

void PressStartAnimation::stop() {
  if (state_ == PressState::Inactive || state_ == PressState::Exit) {
    return;
  }
  state_ = PressState::FadeOut;
  timer_ = 0.0f;
}

void PressStartAnimation::update(float dt) {
  if (dt < 0.0f) {
    dt = 0.0f;
  }

  switch (state_) {
  case PressState::Inactive:
    alpha_ = 0;
    return;
  case PressState::Oscillate:
    timer_ += dt;
    while (timer_ > 1.0f) {
      timer_ -= 1.0f;
    }
    alpha_ = alpha_byte(255.0f * (0.5f - 0.5f * std::cos(kTau * timer_)));
    return;
  case PressState::FadeIn:
    timer_ += dt;
    alpha_ = alpha_byte(255.0f * clamp01(timer_ / kPressFadeDuration));
    if (timer_ >= kPressFadeDuration) {
      state_ = PressState::Oscillate;
      timer_ = 0.0f;
    }
    return;
  case PressState::FadeOut:
    timer_ += dt;
    alpha_ = alpha_byte(255.0f * (1.0f - clamp01(timer_ / kPressFadeDuration)));
    if (timer_ >= kPressFadeDuration) {
      state_ = PressState::Exit;
      alpha_ = 0;
    }
    return;
  case PressState::Exit:
    state_ = PressState::Inactive;
    alpha_ = 0;
    return;
  }
}

Scale item_open_scale(float ratio) {
  const float r = clamp01(ratio);
  float y = 2.0f * (r - 0.5f);
  if (y < 0.0f) {
    y = 0.0f;
  }
  Scale result;
  result.x = 2.0f - y;
  result.y = y;
  return result;
}

Scale item_close_scale(float ratio, bool cancelled_selected_item) {
  const float r = clamp01(ratio);
  Scale result;
  if (cancelled_selected_item) {
    result.x = 1.0f;
    result.y = std::sin(1.5f * kPi * r) + 1.0f;
    return result;
  }

  result.x = r + 1.0f;
  result.y = 1.0f - 2.0f * r;
  if (result.y < 0.0f) {
    result.y = 0.0f;
  }
  return result;
}

Scale title_open_scale(float ratio) {
  const float r = clamp01(ratio);
  Scale result;
  result.x = (1.0f - std::sin(kHalfPi * r)) * 3.0f + 1.0f;
  float y_ratio = (r - 0.65f) / 0.35f;
  if (y_ratio < 0.0f) {
    y_ratio = 0.0f;
  }
  result.y = std::sin(kHalfPi * clamp01(y_ratio));
  return result;
}

Scale title_close_scale(float ratio) { return title_open_scale(1.0f - clamp01(ratio)); }

float cursor_ease(float ratio) { return std::sin(kHalfPi * clamp01(ratio)); }

float panel_ease(float ratio) {
  return (1.0f - std::cos(kPi * clamp01(ratio))) * 0.5f;
}

float text_blend(float timer) { return clamp01(timer / 0.5f); }

} // namespace title
