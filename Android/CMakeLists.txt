# Supermodel VR for Meta Quest (Android, arm64-v8a).
#
# Built by Gradle (Android/app/build.gradle) with the NDK. SDL2 and SDL2_net come from
# Android/third_party (the CI workflow downloads them; see Docs/QUEST.md).
#
# The source list mirrors SRC_FILES in Makefiles/Rules.inc. Differences from the PC build:
#   - Src/Pkgs/glew.cpp is not built: Android/glcompat/GL/glew.h maps GLEW onto OpenGL ES 3.2.
#   - Legacy3D (fixed-function desktop GL) is not built; VR needs New3D anyway.
#   - AndroidBoot.cpp supplies the working folder and the VR command line.
cmake_minimum_required(VERSION 3.22)
project(supermodel_quest C CXX)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET default)   # SDL_main must stay exported from libmain.so
set(CMAKE_C_VISIBILITY_PRESET default)

# ---- SDL2 ----------------------------------------------------------------------------------------
# SDL2 normally ships its own CMake build; as a subproject it produces libSDL2.so for the APK.
# HOST_REHEARSAL is for compile-checking on a desktop machine and uses the system SDL2 instead.
option(HOST_REHEARSAL "Compile on a desktop machine against system SDL2 (no APK)" OFF)
if(HOST_REHEARSAL)
  find_package(SDL2 REQUIRED)
  set(SDL2_TARGET SDL2::SDL2)
else()
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  add_subdirectory("${TP}/SDL2" SDL2-build)
  set(SDL2_TARGET SDL2::SDL2)
endif()

# ---- Musashi 68K: generate the opcode tables with a host-built tool -------------------------------
find_program(HOST_CC NAMES cc gcc clang REQUIRED)
set(M68K_DIR "${REPO}/Src/CPU/68K/Musashi")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/m68k_gen")
file(MAKE_DIRECTORY "${GEN}")
if(NOT EXISTS "${GEN}/m68kops.c")
  execute_process(COMMAND "${HOST_CC}" -w -o "${GEN}/m68kmake" "${M68K_DIR}/m68kmake.c"
                  RESULT_VARIABLE r1 OUTPUT_QUIET ERROR_VARIABLE e1)
  if(NOT r1 EQUAL 0)
    message(FATAL_ERROR "could not build m68kmake with ${HOST_CC}: ${e1}")
  endif()
  execute_process(COMMAND "${GEN}/m68kmake" "${GEN}" "${M68K_DIR}/m68k_in.c"
                  RESULT_VARIABLE r2 OUTPUT_QUIET ERROR_VARIABLE e2)
  if(NOT r2 EQUAL 0)
    message(FATAL_ERROR "m68kmake failed: ${e2}")
  endif()
endif()

# ---- Sources ---------------------------------------------------------------------------------------
set(SM_CXX_SOURCES
  ${REPO}/Src/CPU/PowerPC/PPCDisasm.cpp
  ${REPO}/Src/BlockFile.cpp
  ${REPO}/Src/Model3/93C46.cpp
  ${REPO}/Src/Graphics/Shader.cpp
  ${REPO}/Src/Model3/Real3D.cpp
  ${REPO}/Src/Graphics/New3D/GLSLShader.cpp
  ${REPO}/Src/Graphics/New3D/R3DFrameBuffers.cpp
  ${REPO}/Src/Graphics/New3D/New3D.cpp
  ${REPO}/Src/Graphics/New3D/Mat4.cpp
  ${REPO}/Src/Graphics/New3D/Model.cpp
  ${REPO}/Src/Graphics/New3D/PolyHeader.cpp
  ${REPO}/Src/Graphics/New3D/VBO.cpp
  ${REPO}/Src/Graphics/New3D/Vec.cpp
  ${REPO}/Src/Graphics/New3D/R3DShader.cpp
  ${REPO}/Src/Graphics/New3D/R3DFloat.cpp
  ${REPO}/Src/Graphics/New3D/R3DScrollFog.cpp
  ${REPO}/Src/Graphics/New3D/TextureBank.cpp
  ${REPO}/Src/Graphics/FBO.cpp
  ${REPO}/Src/Graphics/Render2D.cpp
  ${REPO}/Src/Graphics/SuperAA.cpp
  ${REPO}/Src/Model3/TileGen.cpp
  ${REPO}/Src/Model3/Model3.cpp
  ${REPO}/Src/CPU/PowerPC/ppc.cpp
  ${REPO}/Src/OSD/SDL/Main.cpp
  ${REPO}/Src/OSD/VR/DesktopXR.cpp
  ${REPO}/Src/OSD/VR/VRMenu.cpp
  ${REPO}/Src/OSD/SDL/Audio.cpp
  ${REPO}/Src/OSD/SDL/Thread.cpp
  ${REPO}/Src/OSD/SDL/Gui.cpp
  ${REPO}/Src/Model3/SoundBoard.cpp
  ${REPO}/Src/Sound/SCSP.cpp
  ${REPO}/Src/Sound/SCSPDSP.cpp
  ${REPO}/Src/CPU/68K/68K.cpp
  ${REPO}/Src/Model3/DSB.cpp
  ${REPO}/Src/CPU/Z80/Z80.cpp
  ${REPO}/Src/Model3/IRQ.cpp
  ${REPO}/Src/Model3/53C810.cpp
  ${REPO}/Src/Model3/PCI.cpp
  ${REPO}/Src/Model3/RTC72421.cpp
  ${REPO}/Src/Model3/DriveBoard/DriveBoard.cpp
  ${REPO}/Src/Model3/DriveBoard/WheelBoard.cpp
  ${REPO}/Src/Model3/DriveBoard/JoystickBoard.cpp
  ${REPO}/Src/Model3/DriveBoard/SkiBoard.cpp
  ${REPO}/Src/Model3/DriveBoard/BillBoard.cpp
  ${REPO}/Src/Model3/DriveBoard/Z80CTC.cpp
  ${REPO}/Src/Model3/MPC10x.cpp
  ${REPO}/Src/Inputs/Input.cpp
  ${REPO}/Src/Inputs/Inputs.cpp
  ${REPO}/Src/Inputs/InputSource.cpp
  ${REPO}/Src/Inputs/InputSystem.cpp
  ${REPO}/Src/Inputs/InputTypes.cpp
  ${REPO}/Src/Inputs/MultiInputSource.cpp
  ${REPO}/Src/OSD/SDL/SDLInputSystem.cpp
  ${REPO}/Src/OSD/SDL/Crosshair.cpp
  ${REPO}/Src/OSD/Outputs.cpp
  ${REPO}/Src/Sound/MPEG/MpegAudio.cpp
  ${REPO}/Src/Model3/Crypto.cpp
  ${REPO}/Src/OSD/Logger.cpp
  ${REPO}/Src/Util/Format.cpp
  ${REPO}/Src/Util/NewConfig.cpp
  ${REPO}/Src/Util/ByteSwap.cpp
  ${REPO}/Src/Util/ConfigBuilders.cpp
  ${REPO}/Src/GameLoader.cpp
  ${REPO}/Src/Pkgs/tinyxml2.cpp
  ${REPO}/Src/Pkgs/imgui/imgui.cpp
  ${REPO}/Src/Pkgs/imgui/imgui_draw.cpp
  ${REPO}/Src/Pkgs/imgui/imgui_impl_sdl2.cpp
  ${REPO}/Src/Pkgs/imgui/imgui_impl_opengl3.cpp
  ${REPO}/Src/Pkgs/imgui/imgui_tables.cpp
  ${REPO}/Src/Pkgs/imgui/imgui_widgets.cpp
  ${REPO}/Src/ROMSet.cpp
  ${REPO}/Src/OSD/SDL/NetOutputs.cpp
  ${REPO}/Src/Network/TCPReceive.cpp
  ${REPO}/Src/Network/TCPSend.cpp
  ${REPO}/Src/Network/NetBoard.cpp
  ${REPO}/Src/Network/SimNetBoard.cpp
  ${REPO}/Src/OSD/SDL/AndroidBoot.cpp
  ${REPO}/Src/OSD/Unix/FileSystemPath.cpp
)

set(SM_C_SOURCES
  ${REPO}/Src/Pkgs/unzip.c
  ${REPO}/Src/Pkgs/ioapi.c
)

set(M68K_SOURCES
  ${M68K_DIR}/m68kcpu.c
  ${GEN}/m68kops.c ${GEN}/m68kopac.c ${GEN}/m68kopdm.c ${GEN}/m68kopnz.c
)
set_source_files_properties(${M68K_SOURCES} PROPERTIES
  COMPILE_FLAGS "-DINLINE=\"static inline\" -Wno-unused-variable -include sys/types.h -include stdio.h")

# SDL2_net is four small C files; compiling them in avoids a second CMake subproject.
if(HOST_REHEARSAL)
  set(SDLNET_SOURCES "")
else()
  set(SDLNET_SOURCES
    ${TP}/SDL2_net/SDLnet.c ${TP}/SDL2_net/SDLnetTCP.c ${TP}/SDL2_net/SDLnetUDP.c ${TP}/SDL2_net/SDLnetselect.c
  )
endif()

add_library(main SHARED ${SM_CXX_SOURCES} ${SM_C_SOURCES} ${M68K_SOURCES} ${SDLNET_SOURCES})

# Include directories: the makefile adds every source directory, so do the same.
set(SM_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/glcompat")   # first: GL/glew.h -> OpenGL ES
foreach(f ${SM_CXX_SOURCES} ${SM_C_SOURCES})
  get_filename_component(d "${f}" DIRECTORY)
  list(APPEND SM_INCLUDES "${d}")
endforeach()
list(APPEND SM_INCLUDES "${REPO}/Src" "${M68K_DIR}" "${GEN}")
if(NOT HOST_REHEARSAL)
  list(APPEND SM_INCLUDES "${TP}/SDL2_net")
endif()
list(REMOVE_DUPLICATES SM_INCLUDES)
target_include_directories(main PRIVATE ${SM_INCLUDES})
if(NOT HOST_REHEARSAL)
  target_include_directories(main PRIVATE "${TP}/SDL2/include")
endif()

target_compile_definitions(main PRIVATE GLEW_STATIC IMGUI_IMPL_OPENGL_ES3)
target_compile_options(main PRIVATE -O3 -w)   # the PC build's warnings are not useful on a port
set_source_files_properties(${SM_C_SOURCES} PROPERTIES COMPILE_FLAGS "-std=gnu11")

if(ANDROID)
  target_link_libraries(main PRIVATE ${SDL2_TARGET} EGL GLESv3 android log z dl m)
else()
  target_link_libraries(main PRIVATE ${SDL2_TARGET} SDL2_net EGL GLESv2 z dl m pthread)
endif()
