#pragma once
#include "imgui.h"
#include <SDL.h>
#include <algorithm>

// One shared layout for the Vulkan and DX12 controls. Renderer backends own
// the secondary OS window/swapchain; this class only owns panel visibility.
class EditorUi {
public:
  static bool SupportsDesktopViewports() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && (SDL_strcmp(driver, "windows") == 0 ||
                      SDL_strcmp(driver, "x11") == 0 ||
                      SDL_strcmp(driver, "cocoa") == 0);
  }

  static void Configure(bool enableViewports) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "editor-imgui.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (enableViewports) {
      io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
      io.ConfigViewportsNoAutoMerge = true;
      io.ConfigViewportsNoTaskBarIcon = false;
      io.ConfigViewportsNoDefaultParent = true;
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;
    if (enableViewports) ImGui::GetStyle().WindowRounding = 0.0f;
  }

  void Toggle() {
    _visible = !_visible;
    _resetPlacement = _visible;
  }

  // Call End() only when Begin() returns true.
  bool Begin() {
    if (!_visible) return false;
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const bool detached = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    ImVec2 size(420, 660);
    ImVec2 pos(main->Pos.x + main->Size.x + 12, main->Pos.y);
    if (detached) {
      // Fit the initial window on the scene's monitor. On small screens the
      // windows may overlap; they remain independent and can be moved freely.
      for (const ImGuiPlatformMonitor& monitor : ImGui::GetPlatformIO().Monitors) {
        if (main->Pos.x < monitor.MainPos.x || main->Pos.y < monitor.MainPos.y ||
            main->Pos.x >= monitor.MainPos.x + monitor.MainSize.x ||
            main->Pos.y >= monitor.MainPos.y + monitor.MainSize.y) continue;
        size.x = (std::min)(size.x, monitor.WorkSize.x);
        size.y = (std::min)(size.y, monitor.WorkSize.y);
        pos.x = (std::max)(monitor.WorkPos.x,
            (std::min)(pos.x, monitor.WorkPos.x + monitor.WorkSize.x - size.x));
        pos.y = (std::max)(monitor.WorkPos.y,
            (std::min)(pos.y, monitor.WorkPos.y + monitor.WorkSize.y - size.y));
        break;
      }
    } else {
      pos = ImVec2(main->Pos.x + 10, main->Pos.y + 10);
    }
    const ImGuiCond placement = _resetPlacement ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(pos, placement);
    ImGui::SetNextWindowSize(size, placement);
    _resetPlacement = false;
    // Embedded/touch fallback has no permanent keyboard shortcut to reopen it.
    const bool contentsVisible = ImGui::Begin("Editor", detached ? &_visible : nullptr,
                                            ImGuiWindowFlags_NoDocking);
    if (!contentsVisible) { ImGui::End(); return false; }
    ImGui::TextDisabled("F1: hide / show Editor");
    ImGui::Separator();
    return true;
  }

  static void RenderPlatformWindows() {
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
    }
  }

private:
  bool _visible = true;
  bool _resetPlacement = false;
};
