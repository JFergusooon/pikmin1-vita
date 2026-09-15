#pragma once

// Pure title-screen animation math ported from ogStart.cpp, drawMenu.cpp,
// spectrumCursorMgr.cpp, and MenuPanelMgr.h in the projectPiki decomp.
// Keeping this independent of vita2d makes every retail timing curve testable
// on the host.

#include <cstdint>

namespace title {

struct Scale {
  float x = 1.0f;
  float y = 1.0f;
};

enum class PressState {
  Inactive,
  Oscillate,
  FadeIn,
  FadeOut,
  Exit,
};

class PressStartAnimation {
public:
  void start();
  void stop();
  void update(float dt);

  bool active() const { return state_ != PressState::Inactive; }
  PressState state() const { return state_; }
  uint8_t alpha() const { return alpha_; }

private:
  PressState state_ = PressState::Inactive;
  float timer_ = 0.0f;
  uint8_t alpha_ = 0;
};

// DrawMenu item scale in its 0.5-second open and close phases.
Scale item_open_scale(float ratio);
Scale item_close_scale(float ratio, bool cancelled_selected_item);

// DrawMenuTitle's tag "yoko" open/close scale.
Scale title_open_scale(float ratio);
Scale title_close_scale(float ratio);

// SpectrumCursorMgr's sine ease for movement and scaling.
float cursor_ease(float ratio);

// MenuPanelMgr's cosine ease before its quadratic three-point path.
float panel_ease(float ratio);

// The selected-label blend value used by DrawMenuText over 0.5 seconds.
float text_blend(float timer);

} // namespace title
