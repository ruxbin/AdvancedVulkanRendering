cmake_minimum_required(VERSION 3.20)
# Also usable without the renderer's Vulkan/SDL dependencies:
# cmake -S Tests -B build-ibl-tests
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RendererCpuTests LANGUAGES CXX)
    enable_testing()
endif()

add_executable(KtxTextureTests KtxTextureTests.cpp ../Src/KtxTexture.cpp)
add_executable(SphericalHarmonicsTests SphericalHarmonicsTests.cpp ../Src/SphericalHarmonics.cpp)
add_executable(IBLComparisonTests IBLComparisonTests.cpp)
add_executable(VlmShTests VlmShTests.cpp ../Src/VlmSh.cpp ../Src/SphericalHarmonics.cpp)
add_executable(VlmLayoutTests VlmLayoutTests.cpp ../Src/VlmLayout.cpp)
foreach(test IN ITEMS KtxTextureTests SphericalHarmonicsTests IBLComparisonTests VlmShTests VlmLayoutTests)
    target_compile_features(${test} PRIVATE cxx_std_17)
    target_include_directories(${test} PRIVATE ../Src/Include)
    add_test(NAME ${test} COMMAND ${test})
endforeach()

set(IBL_REFERENCE_DIR "" CACHE PATH "Optional Apple Assets directory for KTX integration tests")
if(IBL_REFERENCE_DIR)
    add_test(NAME KtxReferenceTests COMMAND KtxTextureTests "${IBL_REFERENCE_DIR}")
    set_tests_properties(KtxReferenceTests KtxTextureTests PROPERTIES RUN_SERIAL TRUE)
endif()
