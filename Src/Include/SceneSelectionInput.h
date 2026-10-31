#pragma once
#include "SceneClickTracker.h"
#include "SceneInput.h"

namespace scene_selection {
struct MouseAction { bool pick=false, rotate=false, cancelPick=false; };
// Scene routing is shared with camera/ImGui. Preserve a legal left release
// before applying resetDrag; foreign releases and captures always cancel it.
class MouseInput {
public:
  void Cancel() { click.Cancel(); right=false; }
  MouseAction Handle(const SDL_Event& e,const SceneInputDecision& route,bool decal) {
    if (route.deliver && e.type==SDL_WINDOWEVENT &&
        (e.window.event==SDL_WINDOWEVENT_RESIZED || e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED ||
         e.window.event==SDL_WINDOWEVENT_MINIMIZED || e.window.event==SDL_WINDOWEVENT_FOCUS_LOST)) {
      Cancel(); return {false,false,true};
    }
    if (decal) { Cancel(); return {}; }
    if (!route.deliver) {
      if(route.resetDrag || e.type==SDL_MOUSEMOTION || e.type==SDL_MOUSEBUTTONDOWN || e.type==SDL_MOUSEBUTTONUP)
        Cancel();
      return {};
    }
    if (route.resetDrag && e.type!=SDL_MOUSEBUTTONUP) { Cancel(); return {}; }
    switch(e.type) {
    case SDL_MOUSEBUTTONDOWN:
      if(e.button.button==SDL_BUTTON_LEFT && !right) click.Press(float(e.button.x),float(e.button.y));
      else if(e.button.button==SDL_BUTTON_RIGHT) { click.Cancel(); right=true; }
      break;
    case SDL_MOUSEMOTION: {
      bool wasDragging=click.IsDragging();
      bool nowDragging=click.Move(float(e.motion.x),float(e.motion.y));
      return {false,right || (wasDragging && nowDragging)};
    }
    case SDL_MOUSEBUTTONUP: {
      bool picked=e.button.button==SDL_BUTTON_LEFT && !right && click.Release(float(e.button.x),float(e.button.y));
      Cancel(); return {picked,false};
    }
    default: break;
    }
    return {};
  }
private:
  ClickTracker click;
  bool right=false;
};
}
