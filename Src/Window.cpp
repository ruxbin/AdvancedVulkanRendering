#include "Common.h"
#include "SDL.h"
#include "spdlog/spdlog.h"
#include "VulkanSetup.h"
#include "GpuScene.h"
#include <string_view>
#include <chrono>

int main(int nargs, char ** args) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        spdlog::error("SDL init failed");
    }

    SDL_Window* window = SDL_CreateWindow("test", 100, 100, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_OPENGL);
    

    if (window == nullptr)
    {
        spdlog::error("SDL create window failed");
    }

    VulkanDevice vk(window);

    std::string_view scenePath = "D:\\SOURCE\\GraphicsAPI\\UsingMetalToDrawAViewContentsents\\Resources\\edward.obj";
    GpuScene gpuScene(scenePath, vk);
    //GpuScene gpuScene(std::string_view("useless"), vk); //TODO: why error?

    SDL_Event e; 
    bool quit = false; 
    std::chrono::time_point<std::chrono::system_clock> lastTime =
        std::chrono::system_clock::now();
    while (quit == false) { 
        while (SDL_PollEvent(&e)) {
            
            if (e.type == SDL_QUIT) quit = true; 
            else if (e.type == SDL_KEYDOWN)
            {
                spdlog::info("Scancode: {}\n", e.key.keysym.scancode);
                switch (e.key.keysym.scancode)
                {
                    
                   
                case SDL_SCANCODE_1://a
                    gpuScene.GetMainCamera()->MoveLeft(10);
                    break;
                case SDL_SCANCODE_2://s
                    gpuScene.GetMainCamera()->MoveDown(10);
                    break;
                case SDL_SCANCODE_3://d
                    gpuScene.GetMainCamera()->MoveRight(10);
                    break;
                case SDL_SCANCODE_4://q
                    gpuScene.GetMainCamera()->MoveForward(10);
                    break;
                case SDL_SCANCODE_5://w
                    gpuScene.GetMainCamera()->MoveUp(10);
                    break;
                case SDL_SCANCODE_6://e
                    gpuScene.GetMainCamera()->MoveBackward(10);
                    break;
                default:
                    spdlog::info("keypressed {}\n", e.key.keysym.scancode);
                    break;
                }
            }
            //else
            {
                
                
                if (std::chrono::system_clock::now() - lastTime > std::chrono::milliseconds(100))
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
