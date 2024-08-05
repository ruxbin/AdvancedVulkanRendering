

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

set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/Camera.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/VulkanSetup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/GpuScene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ObjLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Shadow.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Light.cpp
)


add_executable(${OUTPUT_NAME} ${SOURCES})

target_link_libraries(${OUTPUT_NAME}
    ${LIBRARIES})

