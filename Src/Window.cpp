#include "Common.h"
#include "SDL.h"
#include "spdlog/spdlog.h"
#include "VulkanSetup.h"
#include "GpuScene.h"
#include <string_view>

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

    std::string_view scenePath = "/home/songjiang/SOURCE/GraphicsAPI/UsingMetalToDrawAViewContentsents/Resources/edward.obj";
    GpuScene gpuScene(scenePath, vk);
    //GpuScene gpuScene(std::string_view("useless"), vk); //TODO: why error?

    SDL_Event e; 
    bool quit = false; 
    while (quit == false) { 
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true; 
	    else
		    gpuScene.Draw();
        } 
    }

    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}
