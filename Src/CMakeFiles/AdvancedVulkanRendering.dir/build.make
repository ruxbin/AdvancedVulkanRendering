# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.23

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/songjiang/SOURCE/AdvancedVulkanRendering

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/songjiang/SOURCE/AdvancedVulkanRendering

# Include any dependencies generated for this target.
include Src/CMakeFiles/AdvancedVulkanRendering.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.make

# Include the progress variables for this target.
include Src/CMakeFiles/AdvancedVulkanRendering.dir/progress.make

# Include the compile flags for this target's objects.
include Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make

Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make
Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o: Src/Camera.cpp
Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o -MF CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o.d -o CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o -c /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Camera.cpp

Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.i"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Camera.cpp > CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.i

Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.s"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Camera.cpp -o CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.s

Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make
Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o: Src/Window.cpp
Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CXX object Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o -MF CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o.d -o CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o -c /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Window.cpp

Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.i"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Window.cpp > CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.i

Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.s"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Window.cpp -o CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.s

Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make
Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o: Src/Mesh.cpp
Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Building CXX object Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o -MF CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o.d -o CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o -c /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Mesh.cpp

Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.i"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Mesh.cpp > CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.i

Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.s"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/Mesh.cpp -o CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.s

Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make
Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o: Src/VulkanSetup.cpp
Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Building CXX object Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o -MF CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o.d -o CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o -c /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/VulkanSetup.cpp

Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.i"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/VulkanSetup.cpp > CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.i

Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.s"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/VulkanSetup.cpp -o CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.s

Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make
Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o: Src/GpuScene.cpp
Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_5) "Building CXX object Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o -MF CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o.d -o CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o -c /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/GpuScene.cpp

Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.i"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/GpuScene.cpp > CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.i

Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.s"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/GpuScene.cpp -o CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.s

Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/flags.make
Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o: Src/ObjLoader.cpp
Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o: Src/CMakeFiles/AdvancedVulkanRendering.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_6) "Building CXX object Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o -MF CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o.d -o CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o -c /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/ObjLoader.cpp

Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.i"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/ObjLoader.cpp > CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.i

Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.s"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/ObjLoader.cpp -o CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.s

# Object files for target AdvancedVulkanRendering
AdvancedVulkanRendering_OBJECTS = \
"CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o" \
"CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o" \
"CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o" \
"CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o" \
"CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o" \
"CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o"

# External object files for target AdvancedVulkanRendering
AdvancedVulkanRendering_EXTERNAL_OBJECTS =

Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/Camera.cpp.o
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/Window.cpp.o
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/Mesh.cpp.o
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/VulkanSetup.cpp.o
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/GpuScene.cpp.o
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/ObjLoader.cpp.o
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/build.make
Bin/AdvancedVulkanRendering: ThirdParty/spdlog/build/libspdlogd.a
Bin/AdvancedVulkanRendering: /usr/lib/libSDL2main.a
Bin/AdvancedVulkanRendering: /usr/lib/libSDL2-2.0.so.0.22.0
Bin/AdvancedVulkanRendering: Src/CMakeFiles/AdvancedVulkanRendering.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/home/songjiang/SOURCE/AdvancedVulkanRendering/CMakeFiles --progress-num=$(CMAKE_PROGRESS_7) "Linking CXX executable ../Bin/AdvancedVulkanRendering"
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/AdvancedVulkanRendering.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
Src/CMakeFiles/AdvancedVulkanRendering.dir/build: Bin/AdvancedVulkanRendering
.PHONY : Src/CMakeFiles/AdvancedVulkanRendering.dir/build

Src/CMakeFiles/AdvancedVulkanRendering.dir/clean:
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering/Src && $(CMAKE_COMMAND) -P CMakeFiles/AdvancedVulkanRendering.dir/cmake_clean.cmake
.PHONY : Src/CMakeFiles/AdvancedVulkanRendering.dir/clean

Src/CMakeFiles/AdvancedVulkanRendering.dir/depend:
	cd /home/songjiang/SOURCE/AdvancedVulkanRendering && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/songjiang/SOURCE/AdvancedVulkanRendering /home/songjiang/SOURCE/AdvancedVulkanRendering/Src /home/songjiang/SOURCE/AdvancedVulkanRendering /home/songjiang/SOURCE/AdvancedVulkanRendering/Src /home/songjiang/SOURCE/AdvancedVulkanRendering/Src/CMakeFiles/AdvancedVulkanRendering.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : Src/CMakeFiles/AdvancedVulkanRendering.dir/depend

