#pragma once
#include <SDL.h>

struct SceneInputDecision {
  bool deliver = false;
  bool resetDrag = false;
  bool quit = false;
  bool toggleEditor = false;
};

// ImGui must receive every event first. Scene controls only receive events for
// their own window which were not captured by a UI widget.
inline SceneInputDecision RouteSceneInput(const SDL_Event& event, Uint32 sceneWindow,
                                         bool captureMouse, bool captureKeyboard) {
  SceneInputDecision result;
  switch (event.type) {
  case SDL_QUIT:
    result.quit = true;
    break;
  case SDL_WINDOWEVENT:
    result.deliver = event.window.windowID == sceneWindow;
    result.quit = result.deliver && event.window.event == SDL_WINDOWEVENT_CLOSE;
    result.resetDrag = result.deliver &&
        (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         event.window.event == SDL_WINDOWEVENT_MINIMIZED);
    break;
  case SDL_KEYDOWN:
  case SDL_KEYUP:
    if (event.key.keysym.scancode == SDL_SCANCODE_F1) {
      result.toggleEditor = event.type == SDL_KEYDOWN && !event.key.repeat;
    } else {
      result.deliver = event.key.windowID == sceneWindow && !captureKeyboard;
    }
    break;
  case SDL_MOUSEMOTION:
    result.deliver = event.motion.windowID == sceneWindow && !captureMouse;
    break;
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP:
    result.deliver = event.button.windowID == sceneWindow && !captureMouse;
    result.resetDrag = event.type == SDL_MOUSEBUTTONUP;
    break;
  case SDL_MOUSEWHEEL:
    result.deliver = event.wheel.windowID == sceneWindow && !captureMouse;
    break;
  case SDL_FINGERDOWN:
  case SDL_FINGERUP:
  case SDL_FINGERMOTION:
    result.deliver = event.tfinger.windowID == sceneWindow && !captureMouse;
    break;
  case SDL_MULTIGESTURE:
    result.deliver = !captureMouse; // SDL2 gesture events have no window ID.
    break;
  default:
    break;
  }
  return result;
}
