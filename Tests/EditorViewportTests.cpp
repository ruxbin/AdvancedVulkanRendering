#define SDL_MAIN_HANDLED
#include "EditorUi.h"
#include "SceneInput.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include <SDL_vulkan.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// Optional desktop/GPU integration test: real SDL windows and Vulkan renderer,
// using the same EditorUi as the application, without loading the full scene.
static void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
static void checkVk(VkResult result) {
  if (result < 0) throw std::runtime_error("Vulkan error " + std::to_string(result));
}

int main() try {
  SDL_SetMainReady();
  require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
  SDL_Window* scene = SDL_CreateWindow("Editor viewport test scene", 80, 80, 640, 480,
                                       SDL_WINDOW_VULKAN);
  require(scene != nullptr, SDL_GetError());
  require(EditorUi::SupportsDesktopViewports(), "desktop multi-viewports unavailable");
  unsigned extensionCount = 0;
  require(SDL_Vulkan_GetInstanceExtensions(scene, &extensionCount, nullptr), SDL_GetError());
  std::vector<const char*> extensions(extensionCount);
  require(SDL_Vulkan_GetInstanceExtensions(scene, &extensionCount, extensions.data()), SDL_GetError());
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instanceInfo.pApplicationInfo = &app;
  instanceInfo.enabledExtensionCount = extensionCount;
  instanceInfo.ppEnabledExtensionNames = extensions.data();
  VkInstance instance;
  checkVk(vkCreateInstance(&instanceInfo, nullptr, &instance));
  VkSurfaceKHR surface;
  require(SDL_Vulkan_CreateSurface(scene, instance, &surface), SDL_GetError());

  uint32_t gpuCount = 0;
  checkVk(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr));
  std::vector<VkPhysicalDevice> gpus(gpuCount);
  checkVk(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()));
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t family = 0;
  for (auto gpu : gpus) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
      VkBool32 present = VK_FALSE;
      checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present));
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        physical = gpu; family = i; break;
      }
    }
    if (physical) break;
  }
  require(physical != VK_NULL_HANDLE, "no graphics/present queue");
  const float priority = 1;
  VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = family;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;
  const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.enabledExtensionCount = 1;
  deviceInfo.ppEnabledExtensionNames = &swapchainExtension;
  VkDevice device;
  checkVk(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);

  // The scene viewport is intentionally empty: only the backend's secondary
  // window owns a swapchain. A compatible render pass is enough for Init().
  VkAttachmentDescription color{};
  color.format = VK_FORMAT_B8G8R8A8_UNORM;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  passInfo.attachmentCount = 1;
  passInfo.pAttachments = &color;
  passInfo.subpassCount = 1;
  passInfo.pSubpasses = &subpass;
  VkRenderPass pass;
  checkVk(vkCreateRenderPass(device, &passInfo, nullptr, &pass));

  ImGui::CreateContext();
  EditorUi::Configure(true);
  ImGui::GetIO().IniFilename = nullptr;
  require(ImGui_ImplSDL2_InitForVulkan(scene), "SDL backend initialization");
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = VK_API_VERSION_1_2;
  info.Instance = instance;
  info.PhysicalDevice = physical;
  info.Device = device;
  info.QueueFamily = family;
  info.Queue = queue;
  info.DescriptorPoolSize = 64;
  info.MinImageCount = info.ImageCount = 2;
  info.PipelineInfoMain.RenderPass = pass;
  info.CheckVkResultFn = checkVk;
  require(ImGui_ImplVulkan_Init(&info), "Vulkan backend initialization");
  EditorUi editor;
  auto frame = [&] {
    SDL_Event event;
    while (SDL_PollEvent(&event)) ImGui_ImplSDL2_ProcessEvent(&event);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (editor.Begin()) {
      ImGui::TextUnformatted("Independent tool window");
      static float roughness = 0.5f;
      ImGui::SliderFloat("Roughness", &roughness, 0, 1);
      ImGui::End();
    }
    ImGui::Render();
    require(ImGui::GetDrawData()->TotalVtxCount == 0, "Editor geometry leaked into scene viewport");
    EditorUi::RenderPlatformWindows();
    SDL_Delay(20);
  };
  auto settle = [&] { for (int i = 0; i < 5; ++i) frame(); };
  auto editorWindow = [&]() -> SDL_Window* {
    auto& viewports = ImGui::GetPlatformIO().Viewports;
    require(viewports.Size == 2, "expected scene + Editor viewports");
    require(viewports[1]->DrawData && viewports[1]->DrawData->TotalVtxCount > 0,
            "Editor has no rendered controls");
    auto* window = SDL_GetWindowFromID(static_cast<Uint32>(reinterpret_cast<uintptr_t>(viewports[1]->PlatformHandle)));
    require(window && window != scene, "Editor is not an independent SDL window");
    return window;
  };
  settle();
  SDL_Window* tool = editorWindow();
  SDL_SetWindowSize(tool, 480, 540);
  SDL_SetWindowPosition(tool, 750, 120);
  settle();
  editorWindow();
  SDL_MinimizeWindow(scene);
  settle();
  editorWindow();
  SDL_RestoreWindow(scene);
  settle();
  SDL_Event close{};
  close.type = SDL_WINDOWEVENT;
  close.window.windowID = SDL_GetWindowID(editorWindow());
  close.window.event = SDL_WINDOWEVENT_CLOSE;
  require(!RouteSceneInput(close, SDL_GetWindowID(scene), false, false).quit,
          "tool close requested application exit");
  ImGui_ImplSDL2_ProcessEvent(&close);
  settle();
  require(ImGui::GetPlatformIO().Viewports.Size == 1, "closing Editor did not remove secondary viewport");
  editor.Toggle();
  settle();
  editorWindow();
  editor.Toggle();
  settle();
  require(ImGui::GetPlatformIO().Viewports.Size == 1, "F1 hide left Editor viewport alive");
  editor.Toggle();
  settle();
  editorWindow();

  checkVk(vkDeviceWaitIdle(device));
  ImGui::DestroyPlatformWindows();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  vkDestroyRenderPass(device, pass, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
  SDL_DestroyWindow(scene);
  SDL_Quit();
  std::puts("PASS: detached rendering, resize/move, scene minimize, close/reopen, hide/show, shutdown");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "FAIL: %s\n", e.what());
  return 1;
}
