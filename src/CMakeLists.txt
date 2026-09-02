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
  main.cpp)

target_compile_features(vulkan_engine_lib PUBLIC cxx_std_23)
target_include_directories(vulkan_engine_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  vulkan_engine_lib
  PUBLIC Vulkan::Vulkan
         glm
         EnTT
         VulkanMemoryAllocator
         glfw
         imgui
         spdlog
         tinyobjloader
         ktx_read
  PRIVATE stb_image vk-bootstrap::vk-bootstrap tinygltf)

include(CompilerWarnings)
set_project_warnings(vulkan_engine_lib)

include(StaticAnalyzers)
enable_static_analyzers(vulkan_engine_lib)

include(Sanitizers)
enable_sanitizers(vulkan_engine_lib)

add_dependencies(vulkan_engine_lib Shaders)

add_executable(engine main.cpp)
target_link_libraries(engine PRIVATE vulkan_engine_lib)

include(Sanitizers)
enable_sanitizers(engine)
