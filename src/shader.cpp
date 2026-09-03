#include <shader.h>
#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <sstream>

ShaderCompiler createShaderCompiler() {
  ShaderCompiler compiler{};

  slang::createGlobalSession(compiler.globalSession.writeRef());

  auto slangTargets{std::to_array<slang::TargetDesc>({{
      .format{SLANG_SPIRV},
      .profile{compiler.globalSession->findProfile("spirv_1_4")},
  }})};
  auto slangOptions{std::to_array<slang::CompilerOptionEntry>({{
      slang::CompilerOptionName::EmitSpirvDirectly,
      {slang::CompilerOptionValueKind::Int, 1},
  }})};
  slang::SessionDesc slangSessionDesc{
      .targets{slangTargets.data()},
      .targetCount{SlangInt(slangTargets.size())},
      .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
      .compilerOptionEntries{slangOptions.data()},
      .compilerOptionEntryCount{uint32_t(slangOptions.size())}};
  compiler.globalSession->createSession(slangSessionDesc,
                                        compiler.session.writeRef());

  SPDLOG_INFO("Created Slang shader compiler session");

  return compiler;
}

void destroyShaderCompiler(ShaderCompiler &compiler) {
  compiler.session = nullptr;
  compiler.globalSession = nullptr;

  SPDLOG_INFO("Destroyed Slang shader compiler session");
}

Shader compileShader(const ShaderCompiler &compiler, const std::string &moduleName,
                     const std::filesystem::path &path) {
  Shader shader{};

  std::ifstream file{path, std::ios::binary};
  std::stringstream sourceStream;
  sourceStream << file.rdbuf();
  std::string source = sourceStream.str();
  Slang::ComPtr<ISlangBlob> sourceBlob;
  sourceBlob.attach(slang_createBlob(source.data(), source.size()));

  Slang::ComPtr<ISlangBlob> diagnostics;
  Slang::ComPtr<slang::IModule> slangModule{compiler.session->loadModuleFromSource(
      moduleName.c_str(), path.string().c_str(), sourceBlob, diagnostics.writeRef())};
  if (!slangModule) {
    SPDLOG_CRITICAL("Failed to load Slang shader module: {}: {}", path.string(),
                    diagnostics
                        ? static_cast<const char *>(diagnostics->getBufferPointer())
                        : "no diagnostics");
    abort();
  }
  if (diagnostics) {
    SPDLOG_WARN("Slang diagnostics for {}: {}", path.string(),
               static_cast<const char *>(diagnostics->getBufferPointer()));
  }

  slangModule->getTargetCode(0, shader.spirv.writeRef());
  if (!shader.spirv) {
    SPDLOG_CRITICAL("Failed to compile Slang shader to SPIR-V: {}", path.string());
    abort();
  }

  shader.shaderModuleCI = VkShaderModuleCreateInfo{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shader.spirv->getBufferSize(),
      .pCode = static_cast<const uint32_t *>(shader.spirv->getBufferPointer())};

  SPDLOG_INFO("Compiled shader '{}' to SPIR-V ({} bytes)", moduleName,
             shader.spirv->getBufferSize());

  return shader;
}
