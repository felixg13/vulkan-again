#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <slang-com-ptr.h>
#include <slang.h>
#include <string>
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

// One scalar/vector/matrix leaf of the material UBO. `name` is the Slang
// field path, with struct members dotted and array elements indexed, e.g.
// "SR_clay_base_color" or "u_lightData[0].direction".
struct ReflectedField {
  std::string name;
  uint32_t offset{0};
  uint32_t size{0};
};

// A texture/sampler pair declared in the material block (MaterialX's
// SamplerTexture2D). Slang splits it into two descriptors.
struct ReflectedResource {
  std::string name;
  uint32_t image_binding{0};
  uint32_t sampler_binding{0};
};

// What the engine needs from the shader's space 1 (set 1): the full binding
// list, every UBO field by name, and the texture/sampler pairs by name.
struct MaterialReflection {
  std::vector<ReflectedBinding> set1_bindings;
  uint32_t ubo_binding{0};
  uint32_t ubo_size{0};
  std::vector<ReflectedField> ubo_fields;
  std::vector<ReflectedResource> resources;
};

struct Shader {
  Slang::ComPtr<ISlangBlob> spirv;
  VkShaderModuleCreateInfo shaderModuleCI{};
  size_t spirv_hash{0};
  MaterialReflection material;
};

// Compiles Slang `source` (one module; every [shader] entry point in it is
// emitted) to SPIR-V and reflects space 1. `moduleName` names it in
// diagnostics.
Shader compileShader(const ShaderCompiler &compiler, const std::string &moduleName,
                     const std::string &source);
