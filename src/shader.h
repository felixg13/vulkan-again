#pragma once

#include <cstddef>
#include <filesystem>
#include <slang-com-ptr.h>
#include <slang.h>
#include <vulkan/vulkan.h>

struct ShaderCompiler {
  Slang::ComPtr<slang::IGlobalSession> globalSession;
  Slang::ComPtr<slang::ISession> session;
};

ShaderCompiler createShaderCompiler();

void destroyShaderCompiler(ShaderCompiler &compiler);

struct Shader {
  Slang::ComPtr<ISlangBlob> spirv;
  VkShaderModuleCreateInfo shaderModuleCI{};
  size_t spirv_hash{0};
};

Shader compileShader(const ShaderCompiler &compiler, const std::string &moduleName,
                     const std::filesystem::path &path);
