cmake_minimum_required(VERSION 3.20)
# CPU tests need no Vulkan/SDL libraries (SceneInputTests uses SDL headers only):
# cmake -S Tests -B build-ibl-tests
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # C 语言用于编入 lzfse（SVT 测试/工具静态链接），CXX 用于其余 CPU 测试。
    project(RendererCpuTests LANGUAGES C CXX)
    enable_testing()
endif()

set(extra_cpu_tests)
add_executable(SceneInputTests SceneInputTests.cpp)
target_include_directories(SceneInputTests PRIVATE ../ThirdParty/SDL/include)
list(APPEND extra_cpu_tests SceneInputTests)

# SVT M1（设计 §13.1）：SvtAsset 格式/校验/地址数学 + SvtCooker 切片一致性。
# lzfse 以静态方式编入测试，独立构建（cmake -S Tests）无需预装共享库。
set(SVT_LZFSE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/lzfse/src)
set(SVT_LZFSE_SOURCES
    ${SVT_LZFSE_DIR}/lzfse_decode.c
    ${SVT_LZFSE_DIR}/lzfse_decode_base.c
    ${SVT_LZFSE_DIR}/lzfse_encode.c
    ${SVT_LZFSE_DIR}/lzfse_encode_base.c
    ${SVT_LZFSE_DIR}/lzfse_fse.c
    ${SVT_LZFSE_DIR}/lzvn_decode_base.c
    ${SVT_LZFSE_DIR}/lzvn_encode_base.c
)
add_executable(TextureBlobReaderTests TextureBlobReaderTests.cpp ../Src/TextureBlobReader.cpp)
target_compile_features(TextureBlobReaderTests PRIVATE cxx_std_17)
target_include_directories(TextureBlobReaderTests PRIVATE ../Src/Include)
list(APPEND extra_cpu_tests TextureBlobReaderTests)

add_executable(SvtAssetTests SvtAssetTests.cpp
    ../Src/SvtAsset.cpp ../Src/SvtCooker.cpp ${SVT_LZFSE_SOURCES})
target_compile_features(SvtAssetTests PRIVATE cxx_std_17)
target_include_directories(SvtAssetTests PRIVATE
    ../Src/Include ${SVT_LZFSE_DIR} ../ThirdParty/json/single_include)
list(APPEND extra_cpu_tests SvtAssetTests)

add_executable(SvtPhysicalCacheTests SvtPhysicalCacheTests.cpp
    ../Src/SvtPhysicalCache.cpp ../Src/SvtAsset.cpp ${SVT_LZFSE_SOURCES})
target_compile_features(SvtPhysicalCacheTests PRIVATE cxx_std_17)
target_include_directories(SvtPhysicalCacheTests PRIVATE ../Src/Include)
list(APPEND extra_cpu_tests SvtPhysicalCacheTests)

add_executable(SvtSystemTests SvtSystemTests.cpp
    ../Src/SvtSystem.cpp ../Src/SvtAsset.cpp ${SVT_LZFSE_SOURCES})
target_compile_features(SvtSystemTests PRIVATE cxx_std_17)
target_include_directories(SvtSystemTests PRIVATE
    ../Src/Include ${SVT_LZFSE_DIR})
list(APPEND extra_cpu_tests SvtSystemTests)

add_executable(SvtTileStreamerTests SvtTileStreamerTests.cpp
    ../Src/SvtTileStreamer.cpp ../Src/SvtSystem.cpp
    ../Src/SvtAsset.cpp ../Src/SvtCooker.cpp ${SVT_LZFSE_SOURCES})
target_compile_features(SvtTileStreamerTests PRIVATE cxx_std_17)
target_include_directories(SvtTileStreamerTests PRIVATE
    ../Src/Include ${SVT_LZFSE_DIR} ../ThirdParty/json/single_include)
list(APPEND extra_cpu_tests SvtTileStreamerTests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/KtxTextureTests.cpp")
    add_executable(KtxTextureTests KtxTextureTests.cpp ../Src/KtxTexture.cpp)
    list(APPEND extra_cpu_tests KtxTextureTests)
endif()
add_executable(SphericalHarmonicsTests SphericalHarmonicsTests.cpp ../Src/SphericalHarmonics.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/IBLComparisonTests.cpp")
    add_executable(IBLComparisonTests IBLComparisonTests.cpp)
    list(APPEND extra_cpu_tests IBLComparisonTests)
endif()
add_executable(VlmShTests VlmShTests.cpp ../Src/VlmSh.cpp ../Src/SphericalHarmonics.cpp)
add_executable(VlmLayoutTests VlmLayoutTests.cpp ../Src/VlmLayout.cpp)
add_executable(VlmAssetTests VlmAssetTests.cpp ../Src/VlmAsset.cpp)
add_executable(VlmCommandLineTests VlmCommandLineTests.cpp)
add_executable(VlmRuntimeDataTests VlmRuntimeDataTests.cpp ../Src/VlmAsset.cpp ../Src/VlmSh.cpp)
add_executable(VlmEnvironmentTests VlmEnvironmentTests.cpp ../Src/VlmEnvironment.cpp)
foreach(test IN ITEMS ${extra_cpu_tests} SphericalHarmonicsTests VlmShTests VlmLayoutTests VlmAssetTests VlmCommandLineTests VlmRuntimeDataTests VlmEnvironmentTests)
    target_compile_features(${test} PRIVATE cxx_std_17)
    target_include_directories(${test} PRIVATE ../Src/Include)
    add_test(NAME ${test} COMMAND ${test})
endforeach()

set(IBL_REFERENCE_DIR "" CACHE PATH "Optional Apple Assets directory for KTX integration tests")
option(EDITOR_GPU_TESTS "Build interactive SDL/Vulkan Editor window regression" OFF)
if(EDITOR_GPU_TESTS)
    find_package(Vulkan REQUIRED)
    find_package(SDL2 REQUIRED CONFIG PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdPartyBuild/SDL/cmake")
    set(editor_imgui "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui")
    add_executable(EditorViewportTests EditorViewportTests.cpp
        ${editor_imgui}/imgui.cpp ${editor_imgui}/imgui_draw.cpp
        ${editor_imgui}/imgui_tables.cpp ${editor_imgui}/imgui_widgets.cpp
        ${editor_imgui}/backends/imgui_impl_sdl2.cpp
        ${editor_imgui}/backends/imgui_impl_vulkan.cpp)
    target_compile_features(EditorViewportTests PRIVATE cxx_std_17)
    target_include_directories(EditorViewportTests PRIVATE ../Src/Include
        ${editor_imgui} ${editor_imgui}/backends)
    target_link_libraries(EditorViewportTests PRIVATE Vulkan::Vulkan SDL2::SDL2)
    if(WIN32)
        add_custom_command(TARGET EditorViewportTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:EditorViewportTests>)
    endif()
    add_test(NAME EditorViewportTests COMMAND EditorViewportTests)
    set_tests_properties(EditorViewportTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
option(VLM_GPU_TESTS "Build standalone Vulkan BRDF integration regression" OFF)
if(VLM_GPU_TESTS)
    find_package(Vulkan REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(VLM_DXC dxc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    add_executable(VlmGpuMathTests VlmGpuMathTests.cpp ../Src/VlmEnvironment.cpp ../Src/SphericalHarmonics.cpp)
    target_include_directories(VlmGpuMathTests PRIVATE ../Src/Include)
    target_compile_features(VlmGpuMathTests PRIVATE cxx_std_17)
    target_link_libraries(VlmGpuMathTests PRIVATE Vulkan::Vulkan)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vlm_math.spv"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_vlm_math_shader.py" "${VLM_DXC}" "${CMAKE_CURRENT_BINARY_DIR}/vlm_math.spv"
        DEPENDS build_vlm_math_shader.py ../shaders/rt_path_common.hlsl VERBATIM)
    add_custom_target(VlmMathShader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/vlm_math.spv")
    add_dependencies(VlmGpuMathTests VlmMathShader)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vlm_sampling.spv"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_vlm_sampling_shader.py" "${VLM_DXC}" "${CMAKE_CURRENT_BINARY_DIR}/vlm_sampling.spv"
        DEPENDS build_vlm_sampling_shader.py ../shaders/vlm_common.hlsl ../shaders/ibl_common.hlsl ../shaders/vlm_flags.hlsl VERBATIM)
    add_custom_target(VlmSamplingShader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/vlm_sampling.spv")
    add_dependencies(VlmGpuMathTests VlmSamplingShader)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vlm_environment.spv"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_vlm_environment_shader.py" "${VLM_DXC}" "${CMAKE_CURRENT_BINARY_DIR}/vlm_environment.spv"
        DEPENDS build_vlm_environment_shader.py ../shaders/vlm_env_sampling.hlsl ../shaders/vlm_bake.hlsl ../shaders/rt_path_common.hlsl VERBATIM)
    add_custom_target(VlmEnvironmentShader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/vlm_environment.spv")
    add_dependencies(VlmGpuMathTests VlmEnvironmentShader)
    add_test(NAME VlmGpuMathTests COMMAND VlmGpuMathTests "${CMAKE_CURRENT_BINARY_DIR}/vlm_math.spv")
    set_tests_properties(VlmGpuMathTests PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME VlmGpuSamplingTests COMMAND VlmGpuMathTests "${CMAKE_CURRENT_BINARY_DIR}/vlm_sampling.spv" sampling)
    set_tests_properties(VlmGpuSamplingTests PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME VlmGpuEnvironmentTests COMMAND VlmGpuMathTests "${CMAKE_CURRENT_BINARY_DIR}/vlm_environment.spv" environment)
    set_tests_properties(VlmGpuEnvironmentTests PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME VlmAcceptanceValidatorTests COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}" -p test_validate_vlm_log.py)
endif()
if(IBL_REFERENCE_DIR AND TARGET KtxTextureTests)
    add_test(NAME KtxReferenceTests COMMAND KtxTextureTests "${IBL_REFERENCE_DIR}")
    set_tests_properties(KtxReferenceTests KtxTextureTests PROPERTIES RUN_SERIAL TRUE)
endif()
