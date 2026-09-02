cmake_minimum_required(VERSION 3.24)

project(
  vulkan_engine
  VERSION 0.1.0
  LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/Ccache.cmake)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

find_package(Vulkan REQUIRED)

include(FetchContent)

add_compile_definitions(PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

include(cmake/Shaders.cmake)
validate_shaders(Shaders "${PROJECT_SOURCE_DIR}/shaders")

add_subdirectory(external)
add_subdirectory(src)
