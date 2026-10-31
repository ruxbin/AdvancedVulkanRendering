#include "SceneClickTracker.h"
#include <cstdio>
int main() {
  using namespace scene_selection;
  int failures = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  ClickTracker t;
  t.Press(10,10); check(!t.Move(12,12), "micro-motion does not rotate");
  check(t.Release(12,12), "micro-motion remains click");
  t.Press(0,0); check(t.Move(5,0), "movement enters drag");
  t.Move(0,0); check(!t.Release(0,0), "drag out and back is not click");
  t.Press(0,0); check(!t.Release(20,0), "release displacement alone cancels click");
  t.Press(0,0); t.Cancel(); check(!t.Release(0,0), "capture or focus cancellation");
  check(!t.Release(0,0), "release without press");
  t.Press(0,0); t.Move(4,0); check(t.Release(4,0), "threshold is strictly greater than four");
  uint32_t x=0,y=0;
  check(ScreenPixel(50,25,100,50,200,100,x,y) && x==100 && y==50, "DPI conversion");
  check(ScreenPixel(99.9f,49.9f,100,50,200,100,x,y) && x==199 && y==99, "bottom-right inside pixel");
  check(!ScreenPixel(100,0,100,50,200,100,x,y), "half-open right edge");
  check(!ScreenPixel(-1,0,100,50,200,100,x,y), "outside is not clamped");
  check(!ScreenPixel(0,0,0,50,200,100,x,y), "zero-size viewport");
  return failures ? 1 : 0;
}
