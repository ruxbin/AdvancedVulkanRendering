cmake_minimum_required(VERSION 3.20)
#set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_LIST_DIR})
project(AdvancedVulkanRendering)

set(CMAKE_CXX_STANDARD 20)



# if (WIN32)
#     message($ENV{SYSROOT})
#     list(APPEND CMAKE_PREFIX_PATH 
#         $ENV{SYSROOT}/cmake 
#         $ENV{SYSROOT}/lib/cmake 
#         $ENV{SYSROOT}/lib/cmake/assimp-5.0 
#         $ENV{SYSROOT}/lib/cmake/Catch2
#         $ENV{SYSROOT}/lib/cmake/meshoptimizer
#     )
# endif ()

list(APPEND CMAKE_PREFIX_PATH
    ${CMAKE_CURRENT_LIST_DIR}/ThirdPartyBuild/SDL/cmake
    ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/spdlog/build
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib/cmake
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib/cmake/Catch2
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib/cmake/assimp
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib/cmake/assimp-5.2
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib/cmake/meshoptimizer
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib64/cmake
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib64/cmake/Catch2
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib64/cmake/assimp
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib64/cmake/assimp-5.2
    #${CMAKE_CURRENT_LIST_DIR}/ThirdParty/lib64/cmake/meshoptimizer
    )

option(DEPENDENCY_BUILD "Third party dependencies only" OFF)
option(BUILD_TESTS "Build engine integration and unit tests" OFF)

file(GLOB BIN_DLLS ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/bin/*)
    
file(
    COPY ${BIN_DLLS}
    DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/Bin
)

# See https://stackoverflow.com/questions/7747857/in-cmake-how-do-i-work-around-the-debug-and-release-directories-visual-studio-2
# First for the generic no-config case (e.g. with mingw)
set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/Bin )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/Bin )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/Bin )
# Second, for multi-config builds (e.g. msvc)
foreach( OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES} )
    string( TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG )
    set( CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_CURRENT_LIST_DIR}/Bin )
    set( CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_CURRENT_LIST_DIR}/Bin )
    set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_CURRENT_LIST_DIR}/Bin )
endforeach( OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES )

#include_directories(${CMAKE_CURRENT_LIST_DIR}/stb)
include_directories($ENV{VULKAN_SDK}/Include)
link_directories($ENV{VULKAN_SDK}/Lib)
link_directories(ThirdParty/lzfse/build/Debug)
link_directories(ThirdParty/lzfse/build)
include_directories(${CMAKE_CURRENT_LIST_DIR}/Src/Include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/ThirdParty/json/single_include)
add_subdirectory(Src/)
#add_subdirectory(Examples/)

#include(CTest)
#enable_testing()

#if (BUILD_TESTS)
#    add_subdirectory(Tests)
#endif()
