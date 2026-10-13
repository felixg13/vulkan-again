#include <shader.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>

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
  shader.spirv_hash = std::hash<std::string_view>{}(std::string_view{
      static_cast<const char *>(shader.spirv->getBufferPointer()),
      shader.spirv->getBufferSize()});

  // REFLECTION (space 1 only). Linked program = module + its entry points;
  // everything is read here because the session dies with the compiler.
  std::vector<slang::IComponentType *> components{slangModule};
  std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints(
      static_cast<size_t>(slangModule->getDefinedEntryPointCount()));
  for (size_t i = 0; i < entryPoints.size(); i++) {
    slangModule->getDefinedEntryPoint(static_cast<SlangInt32>(i),
                                      entryPoints[i].writeRef());
    components.push_back(entryPoints[i]);
  }
  Slang::ComPtr<slang::IComponentType> composed;
  Slang::ComPtr<slang::IComponentType> linked;
  Slang::ComPtr<ISlangBlob> reflectionDiagnostics;
  if (SLANG_FAILED(compiler.session->createCompositeComponentType(
          components.data(), static_cast<SlangInt>(components.size()),
          composed.writeRef(), reflectionDiagnostics.writeRef())) ||
      SLANG_FAILED(composed->link(linked.writeRef(),
                                  reflectionDiagnostics.writeRef()))) {
    SPDLOG_CRITICAL("Failed to link Slang program for reflection: {}",
                    path.string());
    abort();
  }
  slang::ProgramLayout *programLayout = linked->getLayout(0);
  assert(programLayout != nullptr && "no program layout");

  // Which entry-point stages actually use a given set 1 binding.
  std::vector<std::pair<VkShaderStageFlagBits, Slang::ComPtr<slang::IMetadata>>>
      stageMetadata{};
  for (SlangUInt i = 0; i < programLayout->getEntryPointCount(); i++) {
    VkShaderStageFlagBits stageBit{};
    switch (programLayout->getEntryPointByIndex(i)->getStage()) {
    case SLANG_STAGE_VERTEX:
      stageBit = VK_SHADER_STAGE_VERTEX_BIT;
      break;
    case SLANG_STAGE_FRAGMENT:
      stageBit = VK_SHADER_STAGE_FRAGMENT_BIT;
      break;
    default:
      assert(!"unsupported entry point stage");
    }
    Slang::ComPtr<slang::IMetadata> metadata;
    if (SLANG_FAILED(linked->getEntryPointMetadata(
            static_cast<SlangInt>(i), 0, metadata.writeRef(), nullptr))) {
      SPDLOG_CRITICAL("Failed to get entry point metadata: {}", path.string());
      abort();
    }
    stageMetadata.emplace_back(stageBit, metadata);
  }

  bool foundUbo{false};
  bool foundBaseColorFactor{false};
  bool foundLightDir{false};
  for (unsigned i = 0; i < programLayout->getParameterCount(); i++) {
    slang::VariableLayoutReflection *parameter =
        programLayout->getParameterByIndex(i);
    if (parameter->getCategory() != slang::ParameterCategory::DescriptorTableSlot ||
        parameter->getBindingSpace() != 1) {
      continue;
    }
    slang::TypeLayoutReflection *typeLayout = parameter->getTypeLayout();
    assert(typeLayout->getBindingRangeCount() == 1 &&
           "set 1 parameters are expected to be single-range");
    ReflectedBinding binding{.binding = parameter->getBindingIndex(),
                             .count = static_cast<uint32_t>(
                                 typeLayout->getBindingRangeBindingCount(0))};
    switch (typeLayout->getBindingRangeType(0)) {
    case slang::BindingType::ConstantBuffer:
      binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      break;
    case slang::BindingType::CombinedTextureSampler:
      binding.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      break;
    case slang::BindingType::Texture:
      binding.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      break;
    case slang::BindingType::Sampler:
      binding.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      break;
    default:
      assert(!"unsupported set 1 binding type");
    }
    for (const auto &[stageBit, metadata] : stageMetadata) {
      bool used{false};
      metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
                                        1, binding.binding, used);
      if (used) {
        binding.stages |= stageBit;
      }
    }
    shader.material.set1_bindings.push_back(binding);

    if (binding.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || foundUbo) {
      continue;
    }
    // Material UBO: look the engine-written fields up by name.
    foundUbo = true;
    slang::TypeLayoutReflection *blockLayout = typeLayout->getElementTypeLayout();
    shader.material.ubo_binding = binding.binding;
    shader.material.ubo_size = static_cast<uint32_t>(
        blockLayout->getSize(slang::ParameterCategory::Uniform));
    for (unsigned f = 0; f < blockLayout->getFieldCount(); f++) {
      slang::VariableLayoutReflection *field = blockLayout->getFieldByIndex(f);
      auto offset = static_cast<uint32_t>(
          field->getOffset(slang::ParameterCategory::Uniform));
      auto size = static_cast<uint32_t>(
          field->getTypeLayout()->getSize(slang::ParameterCategory::Uniform));
      if (std::strcmp(field->getName(), "base_color_factor") == 0) {
        foundBaseColorFactor = true;
        shader.material.base_color_factor_offset = offset;
        shader.material.base_color_factor_size = size;
      } else if (std::strcmp(field->getName(), "light_dir") == 0) {
        foundLightDir = true;
        shader.material.light_dir_offset = offset;
        shader.material.light_dir_size = size;
      }
    }
  }
  assert(foundUbo && "no UBO in set 1");
  assert(foundBaseColorFactor && "material UBO has no field base_color_factor");
  assert(foundLightDir && "material UBO has no field light_dir");
  (void)foundBaseColorFactor;
  (void)foundLightDir;
  (void)foundUbo;

  SPDLOG_INFO("Compiled shader '{}' to SPIR-V ({} bytes)", moduleName,
             shader.spirv->getBufferSize());

  return shader;
}
