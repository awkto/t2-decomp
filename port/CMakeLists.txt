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

find_package(SDL2 REQUIRED)
add_executable(t2port
  src/main.c src/emu.c src/gun.c src/script.c src/options.c src/romfind.c src/overlay.c src/sha1.c)
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
