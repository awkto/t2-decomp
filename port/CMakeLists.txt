cmake_minimum_required(VERSION 3.16)
project(t2port C)

# usage (from the repo root):
#   cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4
# target: t2port (SDL2 host with Genesis Plus GX linked statically; docs/port.md)

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
  add_executable(t2_web src/web/main_web.c src/emu.c src/gun.c src/mouse.c src/options.c src/overlay.c src/sha1.c)
  target_link_libraries(t2_web PRIVATE gpgx_core)
  target_include_directories(t2_web PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_options(t2_web PRIVATE ${WEB_SDL} -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
  target_compile_definitions(t2_web PRIVATE T2_ROOT="." T2_DIST)
  set_target_properties(t2_web PROPERTIES OUTPUT_NAME t2 SUFFIX ".js")
  target_link_options(t2_web PRIVATE ${WEB_SDL} -O3
    -sENVIRONMENT=web -sINVOKE_RUN=0 -sEXIT_RUNTIME=0
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB
    -sFORCE_FILESYSTEM=1 -lidbfs.js
    "-sEXPORTED_FUNCTIONS=_main,_web_rom_check,_web_frame_count,_web_ram16,_web_gun_state,_web_set_option,_web_screen"
    "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,ccall,UTF8ToString,addRunDependency,removeRunDependency")
  return()
endif()

find_package(SDL2 REQUIRED)
add_executable(t2port
  src/main.c src/emu.c src/gun.c src/mouse.c src/script.c src/options.c src/romfind.c src/overlay.c src/sha1.c)
target_link_libraries(t2port PRIVATE gpgx_core)
target_include_directories(t2port PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
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
