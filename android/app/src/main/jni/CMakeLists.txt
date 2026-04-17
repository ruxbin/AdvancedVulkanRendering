cmake_minimum_required(VERSION 3.22.1)
project(AdvancedVulkanRendering LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

# Project root is 4 levels up: android/app/src/main/jni -> project root
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(SRC_DIR ${PROJECT_ROOT}/Src)
set(INCLUDE_DIR ${PROJECT_ROOT}/Src/Include)
set(THIRDPARTY_DIR ${PROJECT_ROOT}/ThirdParty)

# Find Vulkan from NDK
find_library(vulkan-lib vulkan)

# --- SDL2 (build from source) ---
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL)

# --- spdlog ---
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/spdlog ${CMAKE_CURRENT_BINARY_DIR}/spdlog)

# --- lzfse (build as static lib from sources) ---
set(LZFSE_SOURCES
    ${THIRDPARTY_DIR}/lzfse/src/lzfse_decode.c
    ${THIRDPARTY_DIR}/lzfse/src/lzfse_decode_base.c
    ${THIRDPARTY_DIR}/lzfse/src/lzfse_encode.c
    ${THIRDPARTY_DIR}/lzfse/src/lzfse_encode_base.c
    ${THIRDPARTY_DIR}/lzfse/src/lzfse_fse.c
    ${THIRDPARTY_DIR}/lzfse/src/lzvn_decode_base.c
    ${THIRDPARTY_DIR}/lzfse/src/lzvn_encode_base.c
)
add_library(lzfse STATIC ${LZFSE_SOURCES})
target_include_directories(lzfse PUBLIC ${THIRDPARTY_DIR}/lzfse/src)

# --- imgui ---
set(IMGUI_DIR ${THIRDPARTY_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL2 ${vulkan-lib})

# --- Main application sources ---
set(APP_SOURCES
    ${SRC_DIR}/Camera.cpp
    ${SRC_DIR}/Window.cpp
    ${SRC_DIR}/Mesh.cpp
    ${SRC_DIR}/VulkanSetup.cpp
    ${SRC_DIR}/GpuScene.cpp
    ${SRC_DIR}/ObjLoader.cpp
    ${SRC_DIR}/Shadow.cpp
    ${SRC_DIR}/Light.cpp
    ${SRC_DIR}/AssetLoader.cpp
)

# Build as shared library (libmain.so) for SDL Android
add_library(main SHARED ${APP_SOURCES})

target_include_directories(main PRIVATE
    ${INCLUDE_DIR}
    ${THIRDPARTY_DIR}/json/single_include
    ${THIRDPARTY_DIR}/lzfse/src
)

target_link_libraries(main PRIVATE
    SDL2
    spdlog::spdlog
    lzfse
    imgui
    ${vulkan-lib}
    android
    log
)
