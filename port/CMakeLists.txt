cmake_minimum_required(VERSION 3.16)
project(t2port C)

# usage (from the repo root):
#   cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4
# targets: t2port (SDL2 host with Genesis Plus GX linked statically; docs/port.md),
#          difftest (headless HLE equivalence tester; docs/porting.md)

get_filename_component(T2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(cmake/gpgx.cmake)

option(T2_DIST "release build: no development-tree ROM lookups" OFF)

# ---- the browser build (docs/web.md): emcmake cmake -S port -B build/web-cmake; only the `t2_web`
# target (t2.js + t2.wasm; tools/web_build.sh assembles the site) ----
if(EMSCRIPTEN)
  set(WEB_SDL -sUSE_SDL=2)
  file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.c")
  add_executable(t2_web src/web/main_web.c src/emu.c src/gun.c src/mouse.c src/options.c src/overlay.c src/sha1.c
    src/render/render.c src/render/render_emu.c src/render/render_wide.c
    hle/hle.c hle/sandbox.c hle/gx.c hle/native.c ${GAME_SOURCES})
  target_link_libraries(t2_web PRIVATE gpgx_core)
  target_include_directories(t2_web PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/hle"
    "${CMAKE_CURRENT_SOURCE_DIR}/game")
  set_source_files_properties(hle/gx.c hle/native.c PROPERTIES COMPILE_DEFINITIONS "${GPGX_DEFINES}")
  target_compile_options(t2_web PRIVATE ${WEB_SDL} -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
  target_compile_definitions(t2_web PRIVATE T2_ROOT="." T2_DIST)
  set_target_properties(t2_web PROPERTIES OUTPUT_NAME t2 SUFFIX ".js")
  target_link_options(t2_web PRIVATE ${WEB_SDL} -O3
    -sENVIRONMENT=web -sINVOKE_RUN=0 -sEXIT_RUNTIME=0
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB -sASYNCIFY -sASYNCIFY_STACK_SIZE=98304
    -sFORCE_FILESYSTEM=1 -lidbfs.js
    "-sEXPORTED_FUNCTIONS=_main,_web_rom_check,_web_frame_count,_web_ram16,_web_gun_state,_web_set_option,_web_screen,_web_hle_count"
    "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,ccall,UTF8ToString,addRunDependency,removeRunDependency")
  return()
endif()

# ---- emulator wrapper + HLE layer + decompiled game code (shared by t2port and difftest) ----
# OBJECT library so that HLE_FUNC registrations (constructors) are never dropped by the linker.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.c")
add_library(t2_common OBJECT
  src/emu.c src/sha1.c src/script.c src/mouse.c src/gun.c src/capture.c src/ipprof.c
  src/render/render.c src/render/render_emu.c src/render/render_wide.c
  hle/hle.c hle/sandbox.c hle/gx.c hle/native.c
  ${GAME_SOURCES})
target_link_libraries(t2_common PUBLIC gpgx_core)
target_include_directories(t2_common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/hle"
  "${CMAKE_CURRENT_SOURCE_DIR}/game")
target_compile_options(t2_common PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
target_compile_definitions(t2_common PUBLIC T2_ROOT="${T2_ROOT}")
if(T2_DIST)
  target_compile_definitions(t2_common PUBLIC T2_DIST)
endif()
# hle/gx.c is the only file that includes GPGX internals (shared.h): it needs the core's defines
set_source_files_properties(hle/gx.c hle/native.c PROPERTIES COMPILE_DEFINITIONS "${GPGX_DEFINES}")

find_package(SDL2 REQUIRED)
add_executable(t2port
  src/main.c src/options.c src/romfind.c src/overlay.c)
target_link_libraries(t2port PRIVATE t2_common)
if(TARGET SDL2::SDL2)
  target_link_libraries(t2port PRIVATE SDL2::SDL2)
else()
  target_include_directories(t2port PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(t2port PRIVATE ${SDL2_LIBRARIES})
endif()
target_compile_options(t2port PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
target_compile_definitions(t2port PRIVATE T2_ROOT="${T2_ROOT}")
if(T2_DIST)
  target_compile_definitions(t2port PRIVATE T2_DIST)
endif()

# ---- headless differential tester (docs/porting.md) ----
add_executable(difftest tools/difftest.c)
target_link_libraries(difftest PRIVATE t2_common)
target_compile_options(difftest PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
