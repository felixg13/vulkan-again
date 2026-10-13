#include <shader.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cassert>
#include <format>
#include <functional>
#include <string_view>

ShaderCompiler createShaderCompiler() {
  ShaderCompiler compiler{};

  slang::createGlobalSession(compiler.globalSession.writeRef());

  auto slangTargets{std::to_array<slang::TargetDesc>({{
      .format{SLANG_SPIRV},
      .profile{compiler.globalSession->findProfile("spirv_1_4")},
  }})};
  // Optimization HIGH. Measured on the generated MaterialX source in Step 5
  // (unsanitized host): NONE ~92-97 KB, Slang's DEFAULT ~757 KB, HIGH
  // ~96-98 KB, so DEFAULT is the outlier to avoid. In the ASan/UBSan Debug
  // build the level has no effect on the output (sizes equal NONE).
  // Warnings 31106/31107 ("member will leak into a separate binding slot")
  // describe the SamplerTexture2D split the engine relies on, not a problem.
  auto slangOptions{std::to_array<slang::CompilerOptionEntry>({
      {slang::CompilerOptionName::EmitSpirvDirectly,
       {slang::CompilerOptionValueKind::Int, 1}},
      {slang::CompilerOptionName::Optimization,
       {slang::CompilerOptionValueKind::Int, SLANG_OPTIMIZATION_LEVEL_HIGH}},
      {slang::CompilerOptionName::DisableWarnings,
       {slang::CompilerOptionValueKind::String, 0, 0, "31106,31107"}},
  })};
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
                     const std::string &source) {
  Shader shader{};

  Slang::ComPtr<ISlangBlob> sourceBlob;
  sourceBlob.attach(slang_createBlob(source.data(), source.size()));

  Slang::ComPtr<ISlangBlob> diagnostics;
  Slang::ComPtr<slang::IModule> slangModule{compiler.session->loadModuleFromSource(
      moduleName.c_str(), moduleName.c_str(), sourceBlob, diagnostics.writeRef())};
  if (!slangModule) {
    SPDLOG_CRITICAL("Failed to load Slang shader module: {}: {}", moduleName,
                    diagnostics
                        ? static_cast<const char *>(diagnostics->getBufferPointer())
                        : "no diagnostics");
    abort();
  }
  if (diagnostics) {
    SPDLOG_WARN("Slang diagnostics for {}: {}", moduleName,
               static_cast<const char *>(diagnostics->getBufferPointer()));
  }

  slangModule->getTargetCode(0, shader.spirv.writeRef());
  if (!shader.spirv) {
    SPDLOG_CRITICAL("Failed to compile Slang shader to SPIR-V: {}", moduleName);
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
                    moduleName);
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
      SPDLOG_CRITICAL("Failed to get entry point metadata: {}", moduleName);
      abort();
    }
    stageMetadata.emplace_back(stageBit, metadata);
  }

  // Space-1 parameters: the material cbuffer, with its uniform data and the
  // texture/sampler pairs nested in it (MaterialX declares everything inside
  // one `cbuffer`, which the preamble in material.cpp moves to set 1). Every
  // offset below comes from reflection; nothing is hard-coded.
  bool foundUbo{false};
  for (unsigned i = 0; i < programLayout->getParameterCount(); i++) {
    slang::VariableLayoutReflection *parameter =
        programLayout->getParameterByIndex(i);
    if (parameter->getCategory() != slang::ParameterCategory::DescriptorTableSlot ||
        parameter->getBindingSpace() != 1) {
      continue;
    }
    slang::TypeLayoutReflection *typeLayout = parameter->getTypeLayout();
    assert(typeLayout->getKind() == slang::TypeReflection::Kind::ConstantBuffer &&
           "set 1 is expected to hold exactly one constant buffer");
    assert(!foundUbo && "more than one constant buffer in set 1");
    foundUbo = true;
    // Descriptor slots are relative to the parameter's own binding index.
    const uint32_t base = parameter->getBindingIndex();
    slang::TypeLayoutReflection *blockLayout = typeLayout->getElementTypeLayout();
    shader.material.ubo_binding = base;
    shader.material.ubo_size = static_cast<uint32_t>(
        blockLayout->getSize(slang::ParameterCategory::Uniform));
    shader.material.set1_bindings.push_back(
        {.binding = base, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER});

    // UBO fields: worklist over struct members and array elements until only
    // scalar/vector/matrix leaves remain.
    struct Pending {
      std::string name;
      slang::TypeLayoutReflection *layout;
      size_t offset;
    };
    std::vector<Pending> pending{};
    for (unsigned f = 0; f < blockLayout->getFieldCount(); f++) {
      slang::VariableLayoutReflection *field = blockLayout->getFieldByIndex(f);
      pending.push_back(
          {field->getName(), field->getTypeLayout(),
           field->getOffset(slang::ParameterCategory::Uniform)});
    }
    while (!pending.empty()) {
      Pending item = std::move(pending.back());
      pending.pop_back();
      slang::TypeLayoutReflection *layout = item.layout;
      if (layout->getSize(slang::ParameterCategory::Uniform) == 0) {
        continue; // resource or empty: handled below / nothing to write
      }
      switch (layout->getKind()) {
      case slang::TypeReflection::Kind::Struct:
        for (unsigned f = 0; f < layout->getFieldCount(); f++) {
          slang::VariableLayoutReflection *member = layout->getFieldByIndex(f);
          pending.push_back(
              {std::format("{}.{}", item.name, member->getName()),
               member->getTypeLayout(),
               item.offset +
                   member->getOffset(slang::ParameterCategory::Uniform)});
        }
        break;
      case slang::TypeReflection::Kind::Array: {
        slang::TypeLayoutReflection *element = layout->getElementTypeLayout();
        size_t stride = layout->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM);
        for (size_t e = 0; e < layout->getElementCount(); e++) {
          pending.push_back({std::format("{}[{}]", item.name, e), element,
                             item.offset + e * stride});
        }
        break;
      }
      default:
        shader.material.ubo_fields.push_back(
            {.name = item.name,
             .offset = static_cast<uint32_t>(item.offset),
             .size = static_cast<uint32_t>(
                 layout->getSize(slang::ParameterCategory::Uniform))});
        break;
      }
    }

    // Texture/sampler pairs: the block's members that consume descriptor
    // slots. Each is a struct of one texture and one sampler.
    for (unsigned f = 0; f < blockLayout->getFieldCount(); f++) {
      slang::VariableLayoutReflection *field = blockLayout->getFieldByIndex(f);
      slang::TypeLayoutReflection *fieldLayout = field->getTypeLayout();
      if (fieldLayout->getSize(slang::ParameterCategory::DescriptorTableSlot) ==
          0) {
        continue;
      }
      assert(fieldLayout->getKind() == slang::TypeReflection::Kind::Struct &&
             "set 1 resources are expected to be texture/sampler structs");
      const uint32_t field_slot = base + static_cast<uint32_t>(field->getOffset(
                                             slang::ParameterCategory::DescriptorTableSlot));
      ReflectedResource resource{.name = field->getName()};
      bool foundImage{false};
      bool foundSampler{false};
      for (unsigned m = 0; m < fieldLayout->getFieldCount(); m++) {
        slang::VariableLayoutReflection *member = fieldLayout->getFieldByIndex(m);
        slang::TypeLayoutReflection *memberLayout = member->getTypeLayout();
        const uint32_t slot =
            field_slot + static_cast<uint32_t>(member->getOffset(
                             slang::ParameterCategory::DescriptorTableSlot));
        if (memberLayout->getKind() == slang::TypeReflection::Kind::Resource) {
          assert(memberLayout->getResourceShape() == SLANG_TEXTURE_2D &&
                 "only 2D sampled textures are supported");
          resource.image_binding = slot;
          foundImage = true;
        } else if (memberLayout->getKind() ==
                   slang::TypeReflection::Kind::SamplerState) {
          resource.sampler_binding = slot;
          foundSampler = true;
        } else {
          assert(!"unsupported member in a set 1 resource struct");
        }
      }
      assert(foundImage && foundSampler && "texture without its sampler");
      (void)foundImage;
      (void)foundSampler;
      shader.material.resources.push_back(resource);
      shader.material.set1_bindings.push_back(
          {.binding = resource.image_binding,
           .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE});
      shader.material.set1_bindings.push_back(
          {.binding = resource.sampler_binding,
           .type = VK_DESCRIPTOR_TYPE_SAMPLER});
    }
  }
  assert(foundUbo && "no UBO in set 1");
  (void)foundUbo;

  // Which stages actually use each binding.
  for (ReflectedBinding &binding : shader.material.set1_bindings) {
    for (const auto &[stageBit, metadata] : stageMetadata) {
      bool used{false};
      metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
                                        1, binding.binding, used);
      if (used) {
        binding.stages |= stageBit;
      }
    }
  }

  SPDLOG_INFO("Compiled shader '{}' to SPIR-V ({} bytes)", moduleName,
             shader.spirv->getBufferSize());

  return shader;
}
