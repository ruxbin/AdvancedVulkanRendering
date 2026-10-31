#pragma once
#include <cmath>
#include <cstdint>

namespace scene_selection {
class ClickTracker {
public:
  void Press(float x, float y) { active = true; dragging = false; startX=x; startY=y; }
  bool Move(float x, float y) {
    if (!active) return false;
    const float dx=x-startX, dy=y-startY;
    dragging = dragging || dx*dx+dy*dy > 16.0f;
    return dragging;
  }
  bool Release(float x, float y) {
    Move(x,y); const bool clicked = active && !dragging; Cancel(); return clicked;
  }
  bool IsDragging() const { return dragging; }
  void Cancel() { active=false; dragging=false; }
private:
  bool active=false, dragging=false;
  float startX=0, startY=0;
};
inline bool ScreenPixel(float mx, float my, int width, int height,
                        uint32_t renderW, uint32_t renderH, uint32_t& x, uint32_t& y) {
  if (width<=0 || height<=0 || !renderW || !renderH ||
      !std::isfinite(mx) || !std::isfinite(my) ||
      mx<0 || my<0 || mx>=width || my>=height) return false;
  x=static_cast<uint32_t>(double(mx)*renderW/width);
  y=static_cast<uint32_t>(double(my)*renderH/height);
  return x<renderW && y<renderH;
}
}
