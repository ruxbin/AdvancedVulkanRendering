

#find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)
find_package(spdlog REQUIRED)

set(OUTPUT_NAME AdvancedVulkanRendering)
set(LIBRARIES spdlog::spdlog lzfse ${SDL2_LIBRARIES})

if (WIN32)
list(APPEND LIBRARIES vulkan-1)
endif ()

if(UNIX)
list(APPEND LIBRARIES vulkan)
endif ()

include_directories(${SDL2_INCLUDE_DIRS})
#include_directories(${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/spdlog/include)#TODO something like sdl2
set(ROOT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../../)

# ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/imgui)
include_directories(${IMGUI_DIR} ${IMGUI_DIR}/backends)

set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/Camera.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/VulkanSetup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/GpuScene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ObjLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Shadow.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Light.cpp
    ${CMAKE_CURRENT_LIST_DIR}/AssetLoader.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
)

# DX12 backend (Windows only)
if(ENABLE_DX12 AND WIN32)
    list(APPEND SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/DX12/DX12Setup.cpp
        ${CMAKE_CURRENT_LIST_DIR}/DX12/DX12DescriptorHeap.cpp
        ${CMAKE_CURRENT_LIST_DIR}/DX12/DX12GpuScene.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    )
    list(APPEND LIBRARIES d3d12 dxgi dxguid)
endif()

add_executable(${OUTPUT_NAME} ${SOURCES})

if(ENABLE_DX12 AND WIN32)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE ENABLE_DX12=1)
    target_include_directories(${OUTPUT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/DX12)
endif()

target_link_libraries(${OUTPUT_NAME}
    ${LIBRARIES})
