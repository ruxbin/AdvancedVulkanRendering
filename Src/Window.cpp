#include "Common.h"
#include "SDL.h"
#include "spdlog/spdlog.h"
#include "VulkanSetup.h"
#include "GpuScene.h"
#include <string_view>
#include <chrono>
#include <filesystem>

int main(int nargs, char ** args) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        spdlog::error("SDL init failed");
    }

    SDL_Window* window = SDL_CreateWindow("test", 100, 100, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_OPENGL);
    

    if (window == nullptr)
    {
        spdlog::error("SDL create window failed");
    }

    
    std::filesystem::path currentPath = std::filesystem::current_path();
    while (!std::filesystem::exists(currentPath / "shaders"))
    {
        if (currentPath != currentPath.parent_path())
            currentPath = currentPath.parent_path();
        else
            break;
    }
    if (!std::filesystem::exists(currentPath / "shaders"))
    {
        spdlog::error("failed to locate shaders");
        return 1;
    }
        

    VulkanDevice vk(window);


    //std::string_view scenePath = "../GraphicsAPI/UsingMetalToDrawAViewContentsents/Resources/edward.obj";
    GpuScene gpuScene(currentPath, vk);
    //GpuScene gpuScene(std::string_view("useless"), vk); //TODO: why error?

    SDL_Event e; 
    bool quit = false; 
    std::chrono::time_point<std::chrono::system_clock> lastTime =
        std::chrono::system_clock::now();
    bool mouseDown = false;
    float currentZDegree = 0;
    float currentYDegree = 0;
    constexpr float rotateSpeed = 0.1f;
    constexpr float moveSpeed = 3.f;
    while (quit == false) { 
        while (SDL_PollEvent(&e)) {
            
            if (e.type == SDL_QUIT) quit = true; 
            else if (e.type == SDL_KEYDOWN)
            {
                spdlog::info("Scancode: {}\n", e.key.keysym.scancode);
                switch (e.key.keysym.scancode)
                {
                    
                   
                case SDL_SCANCODE_A://a
                    gpuScene.GetMainCamera()->MoveLeft(moveSpeed);
                    break;
                case SDL_SCANCODE_E://e
                    gpuScene.GetMainCamera()->MoveDown(moveSpeed);
                    break;
                case SDL_SCANCODE_D://d
                    gpuScene.GetMainCamera()->MoveRight(moveSpeed);
                    break;
                case SDL_SCANCODE_Q://q
                    gpuScene.GetMainCamera()->MoveUp(moveSpeed);
                    
                    break;
                case SDL_SCANCODE_W://w
                    gpuScene.GetMainCamera()->MoveForward(moveSpeed);
                    break;
                case SDL_SCANCODE_S://s
                    gpuScene.GetMainCamera()->MoveBackward(moveSpeed);
                    break;
                default:
                    spdlog::info("keypressed {}\n", e.key.keysym.scancode);
                    break;
                }
            }
            else if (e.type == SDL_MOUSEMOTION)
            {
                if (mouseDown)
                {
                    spdlog::info("xrel:{} yrel:{} zdegree{}", e.motion.xrel, e.motion.yrel, currentZDegree);
                    if (abs(e.motion.xrel) > abs(e.motion.yrel))
                    {
                        currentYDegree += e.motion.xrel * rotateSpeed;
                        gpuScene.GetMainCamera()->RotateY(currentYDegree);
                    }
                    else
                    {
                        currentZDegree += e.motion.yrel * rotateSpeed;
                        gpuScene.GetMainCamera()->RotateZ(currentZDegree);
                    }
                    
                    
                }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN)
            {
                mouseDown = true;
            }
            else  if (e.type == SDL_MOUSEBUTTONUP)
            {
                mouseDown = false;
                currentZDegree = 0;
                currentYDegree = 0;
            }
            //else
            {
                
                
                if (std::chrono::system_clock::now() - lastTime > std::chrono::milliseconds(100))//TODO: synchronize with vsync signal
                {
                    gpuScene.Draw();
                    lastTime = std::chrono::system_clock::now();
                }
                
            }
		    
        } 
    }

    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}
