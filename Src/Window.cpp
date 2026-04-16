#include "Common.h"
#include "GpuScene.h"
#include "SDL.h"
#include "VulkanSetup.h"
#include "spdlog/spdlog.h"
#include <chrono>
#ifndef __ANDROID__
#include <filesystem>
#endif
#include <string_view>
#include <cstring>

#ifdef ENABLE_DX12
#include "DX12/DX12Setup.h"
#include "DX12/DX12GpuScene.h"
#include "SDL_syswm.h"
#endif

#ifdef __ANDROID__
#include "AssetLoader.h"
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

int WINDOW_WIDTH = 0;
int WINDOW_HEIGHT = 0;
#endif


int main(int nargs, char **args) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    spdlog::error("SDL init failed");
  }

  // Backend selection: --dx12 or --vulkan (default: vulkan)
  bool useDX12 = false;
#ifdef ENABLE_DX12
  for (int i = 1; i < nargs; ++i) {
    if (strcmp(args[i], "--dx12") == 0) useDX12 = true;
  }
  if (useDX12) spdlog::info("Using DX12 rendering backend");
  else spdlog::info("Using Vulkan rendering backend");
#endif

#ifdef __ANDROID__
  // Query display size for fullscreen on Android
  SDL_DisplayMode displayMode;
  if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0) {
    WINDOW_WIDTH = displayMode.w;
    WINDOW_HEIGHT = displayMode.h;
  } else {
    WINDOW_WIDTH = 1920;
    WINDOW_HEIGHT = 1080;
  }

  // Initialize AAssetManager from SDL's JNI
  {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    jclass clazz = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(clazz, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject assetManagerObj = env->CallObjectMethod(activity, getAssets);
    AAssetManager *mgr = AAssetManager_fromJava(env, assetManagerObj);
    AssetLoader::setAndroidAssetManager(mgr);
    env->DeleteLocalRef(assetManagerObj);
    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(activity);
  }

  SDL_Window *window = SDL_CreateWindow("AdvancedVulkanRendering",
                                        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                        WINDOW_WIDTH, WINDOW_HEIGHT,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
#else
  Uint32 windowFlags = SDL_WINDOW_VULKAN;
#ifdef ENABLE_DX12
  if (useDX12) windowFlags = 0; // No Vulkan flag for DX12
#endif
  SDL_Window *window = SDL_CreateWindow("AdvancedVulkanRendering", 100, 100, WINDOW_WIDTH,
                                        WINDOW_HEIGHT, windowFlags);
#endif

  if (window == nullptr) {
    spdlog::error("SDL create window failed");
  }

#ifdef __ANDROID__
  // On Android, assets use relative paths through AAssetManager
  std::filesystem::path currentPath("");
#else
  std::filesystem::path currentPath = std::filesystem::current_path();
  while (!std::filesystem::exists(currentPath / "shaders")) {
    if (currentPath != currentPath.parent_path())
      currentPath = currentPath.parent_path();
    else
      break;
  }
  if (!std::filesystem::exists(currentPath / "shaders")) {
    spdlog::error("failed to locate shaders");
    return 1;
  }
#endif

#ifdef ENABLE_DX12
  if (useDX12) {
    // --- DX12 Path ---
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);
    HWND hwnd = wmInfo.info.win.window;

    DX12Device dx12(hwnd, WINDOW_WIDTH, WINDOW_HEIGHT);
    DX12GpuScene dx12Scene(currentPath, dx12);
    dx12Scene.InitImGui(window);

    SDL_Event e;
    bool quit = false;
    bool mouseDown = false;
    float currentZDegree = 0, currentYDegree = 0;
    constexpr float rotateSpeed = 0.1f;
    constexpr float moveSpeed = 0.5f;

    while (!quit) {
      while (SDL_PollEvent(&e)) {
        dx12Scene.ProcessImGuiEvent(&e);
        if (e.type == SDL_QUIT) quit = true;
        else if (e.type == SDL_KEYDOWN) {
          switch (e.key.keysym.scancode) {
          case SDL_SCANCODE_A: dx12Scene.GetMainCamera()->MoveRight(moveSpeed); break;
          case SDL_SCANCODE_D: dx12Scene.GetMainCamera()->MoveLeft(moveSpeed); break;
          case SDL_SCANCODE_W: dx12Scene.GetMainCamera()->MoveForward(moveSpeed); break;
          case SDL_SCANCODE_S: dx12Scene.GetMainCamera()->MoveBackward(moveSpeed); break;
          case SDL_SCANCODE_Q: dx12Scene.GetMainCamera()->MoveUp(moveSpeed); break;
          case SDL_SCANCODE_E: dx12Scene.GetMainCamera()->MoveDown(moveSpeed); break;
          default: break;
          }
        } else if (e.type == SDL_MOUSEMOTION && mouseDown) {
          if (abs(e.motion.xrel) > abs(e.motion.yrel)) {
            currentYDegree += e.motion.xrel * rotateSpeed;
            dx12Scene.GetMainCamera()->RotateY(currentYDegree);
          } else {
            currentZDegree += e.motion.yrel * rotateSpeed;
            dx12Scene.GetMainCamera()->RotateZ(currentZDegree);
          }
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
          mouseDown = true;
        } else if (e.type == SDL_MOUSEBUTTONUP) {
          mouseDown = false;
          currentZDegree = 0;
          currentYDegree = 0;
        }
      }
      dx12Scene.Draw();
    }
  } else
#endif
  {
    // --- Vulkan Path ---
    VulkanDevice vk(window);

  GpuScene gpuScene(currentPath, vk);
  gpuScene.InitImGui(window);

  SDL_Event e;
  bool quit = false;

  bool mouseDown = false;
  float currentZDegree = 0;
  float currentYDegree = 0;
  constexpr float rotateSpeed = 0.1f;
  constexpr float moveSpeed = 0.5f;
  constexpr int avg_windows_size = 10;
  int current_window_count = 0;
  std::chrono::milliseconds checkpoint_sum1 = std::chrono::milliseconds(0);
  std::chrono::milliseconds checkpoint_sum2 = std::chrono::milliseconds(0);
  float oneSecondCount = std::chrono::milliseconds(1000).count();
  while (quit == false) {

    std::chrono::time_point<std::chrono::system_clock> time_checkpoint1 =
        std::chrono::system_clock::now();

    while (SDL_PollEvent(&e)) {
      gpuScene.ProcessImGuiEvent(&e);

      if (e.type == SDL_QUIT)
        quit = true;
      else if (e.type == SDL_KEYDOWN) {
        spdlog::info("Scancode: {}\n", e.key.keysym.scancode);
        switch (e.key.keysym.scancode) {

        case SDL_SCANCODE_A: // a
          gpuScene.GetMainCamera()->MoveRight(moveSpeed);
          break;
        case SDL_SCANCODE_E: // e
          gpuScene.GetMainCamera()->MoveDown(moveSpeed);
          break;
        case SDL_SCANCODE_D: // d
          gpuScene.GetMainCamera()->MoveLeft(moveSpeed);
          break;
        case SDL_SCANCODE_Q: // q
          gpuScene.GetMainCamera()->MoveUp(moveSpeed);

          break;
        case SDL_SCANCODE_W: // w
          gpuScene.GetMainCamera()->MoveForward(moveSpeed);
          break;
        case SDL_SCANCODE_S: // s
          gpuScene.GetMainCamera()->MoveBackward(moveSpeed);
          break;
        case SDL_SCANCODE_C:
          gpuScene.TriggerClusterLighting();
          break;
        default:
          spdlog::info("keypressed {}\n", e.key.keysym.scancode);
          break;
        }
        spdlog::info("camerapos: {} {} {}",
                     gpuScene.GetMainCamera()->GetOrigin().x,
                     gpuScene.GetMainCamera()->GetOrigin().y,
                     gpuScene.GetMainCamera()->GetOrigin().z);
      } else if (e.type == SDL_MOUSEMOTION) {
        if (mouseDown) {
          spdlog::info("xrel:{} yrel:{} zdegree{}", e.motion.xrel,
                       e.motion.yrel, currentZDegree);
          if (abs(e.motion.xrel) > abs(e.motion.yrel)) {
            currentYDegree += e.motion.xrel * rotateSpeed;
            gpuScene.GetMainCamera()->RotateY(currentYDegree);
          } else {
            currentZDegree += e.motion.yrel * rotateSpeed;
            gpuScene.GetMainCamera()->RotateZ(currentZDegree);
          }

          spdlog::info("cameradir: {} {} {}",
                       gpuScene.GetMainCamera()->GetCameraDir().x,
                       gpuScene.GetMainCamera()->GetCameraDir().y,
                       gpuScene.GetMainCamera()->GetCameraDir().z);
        }
      } else if (e.type == SDL_MOUSEBUTTONDOWN) {
        mouseDown = true;
      } else if (e.type == SDL_MOUSEBUTTONUP) {
        mouseDown = false;
        currentZDegree = 0;
        currentYDegree = 0;
      }
#ifdef __ANDROID__
      // Touch input: single-finger drag rotates camera
      else if (e.type == SDL_FINGERMOTION) {
        float dx = e.tfinger.dx * WINDOW_WIDTH;
        float dy = e.tfinger.dy * WINDOW_HEIGHT;
        if (abs(dx) > abs(dy)) {
          currentYDegree += dx * rotateSpeed * 0.5f;
          gpuScene.GetMainCamera()->RotateY(currentYDegree);
        } else {
          currentZDegree += dy * rotateSpeed * 0.5f;
          gpuScene.GetMainCamera()->RotateZ(currentZDegree);
        }
      }
      else if (e.type == SDL_FINGERDOWN) {
        currentZDegree = 0;
        currentYDegree = 0;
      }
      // Pinch to zoom
      else if (e.type == SDL_MULTIGESTURE) {
        if (e.mgesture.numFingers == 2) {
          float pinchDist = e.mgesture.dDist;
          if (pinchDist > 0.0f) {
            gpuScene.GetMainCamera()->MoveForward(moveSpeed * pinchDist * 20.0f);
          } else {
            gpuScene.GetMainCamera()->MoveBackward(moveSpeed * (-pinchDist) * 20.0f);
          }
        }
      }
#endif
      // else
    }

    std::chrono::time_point<std::chrono::system_clock> time_checkpoint2 =
        std::chrono::system_clock::now();
    {

      // if (std::chrono::system_clock::now() - lastTime >
      // std::chrono::milliseconds(100))//TODO: synchronize with vsync signal
      { gpuScene.Draw(); }
    }

    std::chrono::time_point<std::chrono::system_clock> time_checkpoint3 =
        std::chrono::system_clock::now();

    
    checkpoint_sum1 += std::chrono::duration_cast<std::chrono::milliseconds>(time_checkpoint2 - time_checkpoint1);
    checkpoint_sum2 += std::chrono::duration_cast<std::chrono::milliseconds>(time_checkpoint3 - time_checkpoint2);
    ++current_window_count;
    if (current_window_count == avg_windows_size) {
      checkpoint_sum1 /= avg_windows_size;
      checkpoint_sum2 /= avg_windows_size;
      current_window_count = 0;
      spdlog::info("PollEvent Duration:{} seconds -- GPUScene Draw Duration:{} seconds. Elapsed:{} seconds", 
        checkpoint_sum1.count()/oneSecondCount, 
        checkpoint_sum2.count()/oneSecondCount,
        (checkpoint_sum1+checkpoint_sum2).count()*avg_windows_size/oneSecondCount);
      checkpoint_sum1 = std::chrono::milliseconds(0);
      checkpoint_sum2 = std::chrono::milliseconds(0);
    }
  } // end while loop
  } // end Vulkan path

  if (window)
    SDL_DestroyWindow(window);

  SDL_Quit();

  return 0;
}
