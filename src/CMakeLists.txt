add_library(
  vulkan_engine_lib
  utils.h
  device.h
  device.cpp
  window.h
  window.cpp
  buffer.h
  buffer.cpp
  asset_loader.h
  asset_loader.cpp
  image.h
  image.cpp
  shader.h
  shader.cpp
  pipeline_cache.h
  pipeline_cache.cpp
  material.h
  material.cpp
  renderer.h
  renderer.cpp)

target_compile_features(vulkan_engine_lib PUBLIC cxx_std_23)
target_include_directories(vulkan_engine_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(
  vulkan_engine_lib PRIVATE
  PIPELINE_CACHE_PATH="${CMAKE_BINARY_DIR}/pipeline_cache.bin")

target_link_libraries(
  vulkan_engine_lib
  PUBLIC Vulkan::Vulkan
         glm
         VulkanMemoryAllocator
         glfw
         spdlog
         tinyobjloader
         ktx_read
         slang::slang
  PRIVATE vk-bootstrap::vk-bootstrap)

include(CompilerWarnings)
set_project_warnings(vulkan_engine_lib)

include(StaticAnalyzers)
enable_static_analyzers(vulkan_engine_lib)

include(Sanitizers)
enable_sanitizers(vulkan_engine_lib)

add_dependencies(vulkan_engine_lib Shaders)

add_executable(engine main.cpp)
target_link_libraries(engine PRIVATE vulkan_engine_lib)
set_target_properties(engine PROPERTIES BUILD_RPATH "$<TARGET_FILE_DIR:slang::slang>")

include(Sanitizers)
enable_sanitizers(engine)
