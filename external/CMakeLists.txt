include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG v2.0.0rc13
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG v2.9.6
  GIT_SHALLOW TRUE)

set(TINYGLTF_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)

set(TINYGLTF_BUILD_LOADER_EXAMPLE
    OFF
    CACHE BOOL "" FORCE)

set(TINYGLTF_BUILD_GLTF_SAMPLE
    OFF
    CACHE BOOL "" FORCE)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS)

FetchContent_Declare(
  vk-bootstrap
  GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
  GIT_TAG v1.4.336
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY
    https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.3.0
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  ktx
  GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software
  GIT_TAG v4.4.2
  GIT_SHALLOW TRUE)

set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "" FORCE)
set(LIBKTX_VERSION_FULL
    OFF
    CACHE BOOL "" FORCE)
set(LIBKTX_VERSION_READ_ONLY
    ON
    CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS
    OFF
    CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS
    OFF
    CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS
    OFF
    CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC
    OFF
    CACHE BOOL "" FORCE)
set(KTX_FEATURE_PYTHON
    OFF
    CACHE BOOL "" FORCE)
set(BASISU_SUPPORT_OPENCL
    OFF
    CACHE BOOL "" FORCE)
set(BASISU_SUPPORT_SSE
    OFF
    CACHE BOOL "" FORCE)
set(KTX_GENERATE_VK_FILES
    OFF
    CACHE BOOL "" FORCE)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  EnTT
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.16.0
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS NAMES GTest)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.16.0
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  slang
  URL https://github.com/shader-slang/slang/releases/download/v2026.14/slang-2026.14-linux-x86_64.tar.gz
  URL_HASH SHA256=9445cf0b35332bfda055c9424e307f957ff58befb04c5b45b9f9f29894a41cf8)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
  add_library(
    imgui
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR}
                                          ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
endif()

FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
  FetchContent_Populate(stb)
  add_library(stb_image INTERFACE)
  target_include_directories(stb_image INTERFACE ${stb_SOURCE_DIR})
endif()

set(VK_BOOTSTRAP_TEST
    OFF
    CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_DISABLE_WARNINGS
    ON
    CACHE BOOL "" FORCE)
set(gtest_force_shared_crt
    ON
    CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(
  glfw
  glm
  VulkanMemoryAllocator
  vk-bootstrap
  tinygltf
  tinyobjloader
  EnTT
  ktx
  spdlog
  slang
  googletest)

find_package(slang CONFIG REQUIRED PATHS ${slang_SOURCE_DIR} NO_DEFAULT_PATH)
set_target_properties(slang::slang PROPERTIES IMPORTED_GLOBAL TRUE)

get_target_property(VK_BOOTSTRAP_INCLUDE_DIRS vk-bootstrap
                    INTERFACE_INCLUDE_DIRECTORIES)
if(VK_BOOTSTRAP_INCLUDE_DIRS)
  set_target_properties(
    vk-bootstrap PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                            "${VK_BOOTSTRAP_INCLUDE_DIRS}")
endif()

get_target_property(VMA_INCLUDE_DIRS VulkanMemoryAllocator
                    INTERFACE_INCLUDE_DIRECTORIES)
if(VMA_INCLUDE_DIRS)
  set_target_properties(
    VulkanMemoryAllocator PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                     "${VMA_INCLUDE_DIRS}")
endif()
