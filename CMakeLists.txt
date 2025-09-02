cmake_minimum_required(VERSION 3.16)
project(handlords_pc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(HANDLORDS_SANITIZE "Enable address sanitizer" OFF)

if(HANDLORDS_SANITIZE)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# SDL2
find_package(SDL2 REQUIRED)
message(STATUS "Found SDL2: ${SDL2_VERSION}" )

# ImGui (expect sources checked out at external/imgui)
# Minimal set: imgui*.cpp + backends for SDL2 + SDL_Renderer2
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(IMGUI_BACKENDS ${IMGUI_DIR}/backends)

set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_BACKENDS}/imgui_impl_sdl2.cpp
  ${IMGUI_BACKENDS}/imgui_impl_sdlrenderer2.cpp
)

add_executable(handlords_pc
  src/main.cpp
  ${IMGUI_SOURCES}
)

target_include_directories(handlords_pc PRIVATE
  ${IMGUI_DIR}
  ${IMGUI_BACKENDS}
)

target_link_libraries(handlords_pc PRIVATE SDL2::SDL2)

# Link SDL2main for proper main function on macOS/Windows
if(TARGET SDL2::SDL2main)
  target_link_libraries(handlords_pc PRIVATE SDL2::SDL2main)
endif()

# Warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(handlords_pc PRIVATE -Wall -Wextra -Wpedantic)
endif()

