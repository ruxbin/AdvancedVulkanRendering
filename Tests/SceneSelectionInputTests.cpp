#define SDL_MAIN_HANDLED
#include "SceneSelectionInput.h"
#include <cstdio>
int main() {
  scene_selection::MouseInput input;
  SDL_Event e{};
  int failures=0;
  auto check=[&](bool ok,const char* name) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",name); ++failures; } };
  auto send=[&](bool capture=false,bool decal=false) {
    return input.Handle(e,RouteSceneInput(e,1,capture,false),decal);
  };
  auto down=[&](uint8_t button) {
    e={}; e.type=SDL_MOUSEBUTTONDOWN; e.button.windowID=1; e.button.button=button; send();
  };
  auto motion=[&](int x) {
    e={}; e.type=SDL_MOUSEMOTION; e.motion.windowID=1; e.motion.x=x; e.motion.xrel=2; return send();
  };
  auto up=[&](uint32_t window=1) {
    e={}; e.type=SDL_MOUSEBUTTONUP; e.button.windowID=window; e.button.button=SDL_BUTTON_LEFT; return send();
  };
  down(SDL_BUTTON_LEFT); check(up().pick,"legal release survives resetDrag routing");
  down(SDL_BUTTON_LEFT); check(!motion(2).rotate,"micro-motion does not rotate"); check(up().pick,"micro-motion click");
  down(SDL_BUTTON_LEFT); check(!motion(6).rotate,"threshold crossing resets rotation origin");
  check(motion(8).rotate,"subsequent drag rotates"); check(!up().pick,"drag back never clicks");
  down(SDL_BUTTON_LEFT); up(2); check(!up().pick,"release in tool cancels candidate");
  down(SDL_BUTTON_LEFT); motion(1); send(true); check(!up().pick,"UI capture cancels candidate");
  down(SDL_BUTTON_LEFT); e={}; e.type=SDL_WINDOWEVENT; e.window.windowID=1;
  e.window.event=SDL_WINDOWEVENT_FOCUS_LOST; send(); check(!up().pick,"focus loss cancels candidate");
  down(SDL_BUTTON_RIGHT); check(motion(1).rotate,"right drag rotates immediately"); check(!up().pick,"right drag cannot pick");
  down(SDL_BUTTON_LEFT); e={}; e.type=SDL_MOUSEBUTTONUP; e.button.windowID=1;
  e.button.button=SDL_BUTTON_LEFT; check(!send(false,true).pick,"decal consumes left click");
  down(SDL_BUTTON_LEFT); e={}; e.type=SDL_WINDOWEVENT; e.window.windowID=1;
  e.window.event=SDL_WINDOWEVENT_SIZE_CHANGED;
  check(send().cancelPick,"resize invalidates submitted readback before Draw");
  check(!up().pick,"resize also cancels mouse candidate");
  return failures ? 1 : 0;
}
