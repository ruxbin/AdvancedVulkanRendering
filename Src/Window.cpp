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
    
    bool mouseDown = false;
    float currentZDegree = 0;
    float currentYDegree = 0;
    constexpr float rotateSpeed = 0.1f;
    constexpr float moveSpeed = 0.5f;
    constexpr int avg_windows_size = 10;
	int current_window_count = 0;
    std::chrono::milliseconds checkpoint_sum1 = std::chrono::milliseconds(0);
    std::chrono::milliseconds checkpoint_sum2 = std::chrono::milliseconds(0);
    while (quit == false) { 

        std::chrono::time_point<std::chrono::system_clock> time_checkpoint1 =
            std::chrono::system_clock::now();

        while (SDL_PollEvent(&e)) {
            
            if (e.type == SDL_QUIT) quit = true; 
            else if (e.type == SDL_KEYDOWN)
            {
                spdlog::info("Scancode: {}\n", e.key.keysym.scancode);
                switch (e.key.keysym.scancode)
                {
                    
                   
                case SDL_SCANCODE_A://a
                    gpuScene.GetMainCamera()->MoveRight(moveSpeed);
                    break;
                case SDL_SCANCODE_E://e
                    gpuScene.GetMainCamera()->MoveDown(moveSpeed);
                    break;
                case SDL_SCANCODE_D://d
                    gpuScene.GetMainCamera()->MoveLeft(moveSpeed);
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
		case SDL_SCANCODE_C:
		    gpuScene.TriggerClusterLighting();
		    break;
                default:
                    spdlog::info("keypressed {}\n", e.key.keysym.scancode);
                    break;
                }
		spdlog::info("camerapos: {} {} {}",gpuScene.GetMainCamera()->GetOrigin().x,
					gpuScene.GetMainCamera()->GetOrigin().y,
					gpuScene.GetMainCamera()->GetOrigin().z);
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
                    
		    spdlog::info("cameradir: {} {} {}",gpuScene.GetMainCamera()->GetCameraDir().x,
				    gpuScene.GetMainCamera()->GetCameraDir().y,
				    gpuScene.GetMainCamera()->GetCameraDir().z); 
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
            
		    
        }

        std::chrono::time_point<std::chrono::system_clock> time_checkpoint2 =
            std::chrono::system_clock::now();
        {


            //if (std::chrono::system_clock::now() - lastTime > std::chrono::milliseconds(100))//TODO: synchronize with vsync signal
            {
                gpuScene.Draw();
                
            }

        }

        std::chrono::time_point<std::chrono::system_clock> time_checkpoint3 =
            std::chrono::system_clock::now();

        checkpoint_sum1 += std::chrono::milliseconds((time_checkpoint2 - time_checkpoint1).count());
		checkpoint_sum2 += std::chrono::milliseconds((time_checkpoint3 - time_checkpoint2).count());
		++current_window_count;
        if (current_window_count == avg_windows_size)
        {
            checkpoint_sum1 /= avg_windows_size;
			checkpoint_sum2 /= avg_windows_size;
			current_window_count = 0;
            spdlog::info("{}--{}", checkpoint_sum1.count(), checkpoint_sum2.count());
            checkpoint_sum1 = std::chrono::milliseconds(0);
            checkpoint_sum2 = std::chrono::milliseconds(0);
        }
    }

    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}
