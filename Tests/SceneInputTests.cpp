#define SDL_MAIN_HANDLED
#include "SceneInput.h"
#include <cstdio>

int main() {
  int failures = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  SDL_Event e{};
  e.type = SDL_KEYDOWN;
  e.key.windowID = 2;
  e.key.keysym.scancode = SDL_SCANCODE_W;
  check(!RouteSceneInput(e, 1, false, false).deliver, "tool keyboard cannot move camera");
  e.key.windowID = 1;
  check(RouteSceneInput(e, 1, false, false).deliver, "scene keyboard moves camera");
  check(!RouteSceneInput(e, 1, false, true).deliver, "text editing captures camera keys");
  e.key.keysym.scancode = SDL_SCANCODE_F1;
  check(RouteSceneInput(e, 1, true, true).toggleEditor, "F1 can restore editor despite UI capture");
  e.key.repeat = 1;
  check(!RouteSceneInput(e, 1, false, false).toggleEditor, "held F1 does not flicker editor");
  e = {};
  e.type = SDL_MOUSEBUTTONDOWN;
  e.button.windowID = 2;
  check(!RouteSceneInput(e, 1, false, false).deliver, "tool click cannot start camera drag");
  e.button.windowID = 1;
  check(RouteSceneInput(e, 1, false, false).deliver, "scene click starts drag");
  check(!RouteSceneInput(e, 1, true, false).deliver, "UI captures scene click");
  e.type = SDL_MOUSEBUTTONUP;
  e.button.windowID = 2;
  check(RouteSceneInput(e, 1, true, false).resetDrag, "release in tool cannot leave camera dragging");
  e = {};
  e.type = SDL_MOUSEMOTION;
  e.motion.windowID = 2;
  check(!RouteSceneInput(e, 1, false, false).deliver, "tool motion cannot rotate camera");
  e = {};
  e.type = SDL_MOUSEWHEEL;
  e.wheel.windowID = 2;
  check(!RouteSceneInput(e, 1, false, false).deliver, "tool scroll cannot edit scene decals");
  e = {};
  e.type = SDL_WINDOWEVENT;
  e.window.windowID = 2;
  e.window.event = SDL_WINDOWEVENT_RESIZED;
  check(!RouteSceneInput(e, 1, false, false).deliver, "tool resize cannot resize scene swapchain");
  e.window.event = SDL_WINDOWEVENT_CLOSE;
  check(!RouteSceneInput(e, 1, false, false).quit, "closing editor keeps scene alive");
  e.window.windowID = 1;
  check(RouteSceneInput(e, 1, false, false).quit, "closing scene exits application");
  e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
  check(RouteSceneInput(e, 1, false, false).resetDrag, "focus loss cancels drag");
  e.type = SDL_QUIT;
  check(RouteSceneInput(e, 1, false, false).quit, "global quit exits application");
  return failures ? 1 : 0;
}
