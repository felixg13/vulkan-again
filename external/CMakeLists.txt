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
  tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG v2.0.0rc13
  GIT_SHALLOW TRUE)

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
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.16.0
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  slang
  URL https://github.com/shader-slang/slang/releases/download/v2026.14/slang-2026.14-linux-x86_64.tar.gz
  URL_HASH SHA256=9445cf0b35332bfda055c9424e307f957ff58befb04c5b45b9f9f29894a41cf8)

FetchContent_Declare(
  MaterialX
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/MaterialX.git
  GIT_TAG v1.39.5
  GIT_SHALLOW TRUE)

# stb is header-only and untagged; pinned by commit.
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE)

foreach(
  materialx_option
  MATERIALX_BUILD_TESTS
  MATERIALX_BUILD_VIEWER
  MATERIALX_BUILD_GRAPH_EDITOR
  MATERIALX_BUILD_PYTHON
  MATERIALX_BUILD_JS
  MATERIALX_BUILD_DOCS
  MATERIALX_BUILD_RENDER
  MATERIALX_BUILD_RENDER_PLATFORMS
  MATERIALX_BUILD_GEN_GLSL
  MATERIALX_BUILD_GEN_OSL
  MATERIALX_BUILD_GEN_MDL
  MATERIALX_BUILD_GEN_MSL
  MATERIALX_INSTALL_PYTHON)
  set(${materialx_option}
      OFF
      CACHE BOOL "" FORCE)
endforeach()
set(MATERIALX_BUILD_GEN_SLANG
    ON
    CACHE BOOL "" FORCE)

set(INSTALL_GTEST
    OFF
    CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_TEST
    OFF
    CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_DISABLE_WARNINGS
    ON
    CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(
  glfw
  glm
  VulkanMemoryAllocator
  vk-bootstrap
  tinyobjloader
  ktx
  spdlog
  slang
  MaterialX
  stb
  googletest)

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# MaterialX headers are third-party: treat them as system includes.
foreach(
  materialx_target
  MaterialXCore
  MaterialXFormat
  MaterialXGenShader
  MaterialXGenHw
  MaterialXGenSlang)
  get_target_property(MATERIALX_INCLUDE_DIRS ${materialx_target}
                      INTERFACE_INCLUDE_DIRECTORIES)
  if(MATERIALX_INCLUDE_DIRS)
    set_target_properties(
      ${materialx_target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                     "${MATERIALX_INCLUDE_DIRS}")
  endif()
endforeach()

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
