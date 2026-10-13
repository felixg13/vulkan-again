#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <slang-com-ptr.h>
#include <slang.h>
#include <vector>
#include <vulkan/vulkan.h>

struct ShaderCompiler {
  Slang::ComPtr<slang::IGlobalSession> globalSession;
  Slang::ComPtr<slang::ISession> session;
};

ShaderCompiler createShaderCompiler();

void destroyShaderCompiler(ShaderCompiler &compiler);

struct ReflectedBinding {
  uint32_t binding{0};
  VkDescriptorType type{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
  VkShaderStageFlags stages{0};
  uint32_t count{1};
  friend bool operator==(const ReflectedBinding &,
                         const ReflectedBinding &) = default;
};

// What the engine needs from the shader's space 1 (set 1): the binding list
// and, for the material UBO, where the engine-written fields live.
struct MaterialReflection {
  std::vector<ReflectedBinding> set1_bindings;
  uint32_t ubo_binding{0};
  uint32_t ubo_size{0};
  uint32_t base_color_factor_offset{0};
  uint32_t base_color_factor_size{0};
  uint32_t light_dir_offset{0};
  uint32_t light_dir_size{0};
};

struct Shader {
  Slang::ComPtr<ISlangBlob> spirv;
  VkShaderModuleCreateInfo shaderModuleCI{};
  size_t spirv_hash{0};
  MaterialReflection material;
};

Shader compileShader(const ShaderCompiler &compiler, const std::string &moduleName,
                     const std::filesystem::path &path);
