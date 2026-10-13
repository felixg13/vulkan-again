#include <material.h>

#include <utils.h>

#include <MaterialXFormat/Util.h>
#include <MaterialXGenHw/HwConstants.h>
#include <MaterialXGenHw/HwShaderGenerator.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenSlang/SlangShaderGenerator.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace mx = MaterialX;

namespace {

// Light id handed to bindLightShader; MaterialX's generated code dispatches on
// `u_lightData[i].type == id`, so the UBO must carry the same number.
constexpr unsigned DIRECTIONAL_LIGHT_TYPE_ID = 1;

// Prepended to the generated pixel source (which is never edited).
//  - `cbuffer`: MaterialX's cbuffer has no explicit binding and would land in
//    set 0, binding 0 (spike F2). Prefixing it moves the block and the
//    texture/sampler members nested in it to set 1.
//  - `shader(...)`: the generated `fragmentMain` is tagged [shader("fragment")].
//    Slang 2026.14 crashes (null IRLayoutDecoration in
//    TransformParamsToConstRefContext) when a module's generated entry point is
//    left out of the composed program, so the tag is neutralised while the
//    generated text is parsed. The adapter's own entry point re-enables it.
constexpr std::string_view PREAMBLE =
    "#define cbuffer [[vk::binding(0, 1)]] cbuffer\n"
    "#define shader(stage) ForceInline\n";
constexpr std::string_view PREAMBLE_END = "\n#undef shader\n";

// One write into the material UBO, by reflected field name.
struct FieldWrite {
  std::string name;
  std::array<std::byte, 64> bytes{};
  uint32_t size{0};
};

// A file texture waiting for GPU upload.
struct PendingTexture {
  std::string resource_name;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{nullptr,
                                                              &stbi_image_free};
  uint32_t width{0};
  uint32_t height{0};
  VkFormat format{VK_FORMAT_UNDEFINED};
};

} // namespace

MaterialLibrary::MaterialLibrary(const Device &device) : _device{device} {
  const std::array<VkDescriptorPoolSize, 3> pool_sizes{
      VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           .descriptorCount = MAX_MATERIALS},
      VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                           .descriptorCount =
                               MAX_MATERIALS * MAX_TEXTURES_PER_MATERIAL},
      VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER,
                           .descriptorCount =
                               MAX_MATERIALS * MAX_TEXTURES_PER_MATERIAL}};
  VkDescriptorPoolCreateInfo pool_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = MAX_MATERIALS,
      .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data()};
  utils::validation::chk(
      vkCreateDescriptorPool(_device.device, &pool_ci, nullptr, &_pool));

  // Texture uploads are submitted and waited on inside compile().
  VkCommandPoolCreateInfo upload_pool_ci{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = _device.graphicsQueueFamily};
  utils::validation::chk(vkCreateCommandPool(_device.device, &upload_pool_ci,
                                             nullptr, &_upload_pool));

  // One engine-wide linear/repeat sampler with the full mip range. Per-node
  // address/filter state is a stated Phase 2 limitation.
  _sampler = createSampler(_device, {.maxLod = VK_LOD_CLAMP_NONE});
}

MaterialHandle MaterialLibrary::compile(MaterialX::DocumentPtr document,
                                        const std::string &vertex_source) {
  // -- STAGE 1: everything that can throw, before anything is allocated -----

  // STANDARD LIBRARY: loaded once, on first use
  const std::filesystem::path libraries_path{MATERIALX_LIBRARIES_PATH};
  const mx::FileSearchPath search_path{
      mx::FilePath{libraries_path.parent_path().string()}};
  if (!_stdlib) {
    _stdlib = mx::createDocument();
    mx::loadLibraries({mx::FilePath{libraries_path.filename().string()}},
                      search_path, _stdlib);
  }
  document->setDataLibrary(_stdlib);
  std::string validation_message{};
  if (!document->validate(&validation_message)) {
    SPDLOG_WARN("MaterialX document validation: {}", validation_message);
  }
  std::vector<mx::TypedElementPtr> renderable = mx::findRenderableElements(document);
  if (renderable.empty()) {
    throw std::runtime_error(
        "MaterialX document has no renderable element (expected a material "
        "node or shader node)");
  }

  // GENERATION (settings per the spike: F1, F6)
  mx::ShaderGeneratorPtr generator = mx::SlangShaderGenerator::create();
  mx::GenContext context{generator};
  context.registerSourceCodeSearchPath(search_path);
  context.getOptions().targetColorSpaceOverride = "lin_rec709";
  context.getOptions().hwMaxActiveLightSources = 1;
  context.getOptions().hwDirectionalAlbedoMethod = mx::DIRECTIONAL_ALBEDO_ANALYTIC;
  mx::NodeDefPtr light_def = _stdlib->getNodeDef("ND_directional_light");
  if (!light_def) {
    throw std::runtime_error(
        "MaterialX standard library has no ND_directional_light");
  }
  mx::HwShaderGenerator::bindLightShader(*light_def, DIRECTIONAL_LIGHT_TYPE_ID,
                                         context);
  mx::ShaderPtr generated{};
  try {
    generated = generator->generate("material", renderable.front(), context);
  } catch (const mx::Exception &error) {
    throw std::runtime_error(
        std::format("MaterialX shader generation failed for '{}': {}",
                    renderable.front()->getNamePath(), error.what()));
  }
  const mx::ShaderStage &pixel_stage = generated->getStage(mx::Stage::PIXEL);

  // ADAPTER: pixel entry point taking the engine varyings, building the
  // generated `VertexData` by field name (F4). The field list and order vary
  // per material; the body is emitted from the pixel stage's input block.
  std::string adapter{
      "\n[shader(\"fragment\")]\n"
      "float4 main(EngineVaryings v) : SV_Target\n"
      "{\n"
      "    VertexData d = {};\n"
      "    d.SV_Position = v.position;\n"};
  const mx::VariableBlock &vertex_data =
      pixel_stage.getInputBlock(mx::HW::VERTEX_DATA);
  for (const mx::ShaderPort *field : vertex_data.getVariableOrder()) {
    const std::string &field_name = field->getVariable();
    const mx::TypeDesc field_type = field->getType();
    if (field_name == "normalWorld") {
      adapter += "    d.normalWorld = normalize(v.world_normal);\n";
    } else if (field_name == "positionWorld") {
      adapter += "    d.positionWorld = v.world_position;\n";
    } else if (field_name == "texcoord_0" && field_type == mx::Type::VECTOR2) {
      adapter += "    d.texcoord_0 = v.uv;\n";
    } else if (field_name == "tangentWorld") {
      // Documented default: the engine has no tangent stream.
      adapter += "    d.tangentWorld = float3(1.0, 0.0, 0.0);\n";
    }
    // Every other field stays zero from `VertexData d = {}`: the engine
    // provides position, normal and one uv set, nothing else.
  }
  adapter += "    return fragmentMain(d);\n}\n";

  // MODULE: preamble + generated + engine vertex stage + adapter (the adapter
  // needs EngineVaryings from the vertex stage). Generated text is verbatim.
  std::string source{PREAMBLE};
  source += generated->getSourceCode(mx::Stage::PIXEL);
  source += PREAMBLE_END;
  source += vertex_source;
  source += adapter;

  ShaderCompiler shader_compiler = createShaderCompiler();
  Shader shader = compileShader(shader_compiler, "material", source);
  destroyShaderCompiler(shader_compiler);
  const MaterialReflection &reflection = shader.material;

  // UBO WRITES: every uniform port with a value, by name; plus the engine
  // lighting constants (F1).
  std::vector<FieldWrite> writes{};
  std::vector<PendingTexture> pending_textures{};
  std::filesystem::path document_dir =
      std::filesystem::path{document->getSourceUri()}.parent_path();
  for (const auto &[block_name, block] : pixel_stage.getUniformBlocks()) {
    if (block_name == mx::HW::LIGHT_DATA) {
      continue; // struct members of u_lightData; written below
    }
    for (const mx::ShaderPort *port : block->getVariableOrder()) {
      const mx::ValuePtr value = port->getValue();
      const mx::TypeDesc type = port->getType();
      if (type == mx::Type::STRING) {
        continue; // F5: declared as int, no value; stays zero
      }
      if (!value) {
        continue; // env textures, shader structs, ...: nothing to write
      }
      if (type == mx::Type::FILENAME) {
        // TEXTURE: path relative to the .mtlx; format from the image node's
        // output type and the file input's colorspace.
        std::filesystem::path texture_path{value->getValueString()};
        if (texture_path.is_relative()) {
          texture_path = document_dir / texture_path;
        }
        mx::ElementPtr input = document->getDescendant(port->getPath());
        assert(input && input->getParent() &&
               "filename port has no element in the document");
        const std::string &image_type =
            input->getParent()->asA<mx::TypedElement>()->getType();
        const std::string &color_space = input->getActiveColorSpace();
        const bool is_color = image_type == "color3" || image_type == "color4";
        // Colour data is sRGB-encoded unless the document says it is linear
        // (lin_rec709, lin_displayp3, acescg, ...); everything else is data.
        const bool is_linear =
            color_space.starts_with("lin_") || color_space == "acescg";
        PendingTexture texture{};
        texture.resource_name = port->getVariable();
        texture.format = is_color && !is_linear ? VK_FORMAT_R8G8B8A8_SRGB
                                                : VK_FORMAT_R8G8B8A8_UNORM;
        int width{0};
        int height{0};
        int channels{0};
        texture.pixels.reset(stbi_load(texture_path.string().c_str(), &width,
                                       &height, &channels, 4));
        if (!texture.pixels) {
          throw std::runtime_error(
              std::format("failed to load texture '{}' for port '{}': {}",
                          texture_path.string(), port->getVariable(),
                          stbi_failure_reason()));
        }
        texture.width = static_cast<uint32_t>(width);
        texture.height = static_cast<uint32_t>(height);
        pending_textures.push_back(std::move(texture));
        continue;
      }

      // Some ports are typed `integer` but carry a string value (e.g.
      // geomprop_UV0_index); re-parse from the port's declared type.
      const mx::ValuePtr typed_value =
          mx::Value::createValueFromStrings(value->getValueString(), type.getName());
      if (!typed_value) {
        throw std::runtime_error(
            std::format("cannot parse value '{}' of port '{}' as {}",
                        value->getValueString(), port->getVariable(), type.getName()));
      }
      FieldWrite write{.name = port->getVariable()};
      if (type == mx::Type::FLOAT) {
        const float data = typed_value->asA<float>();
        write.size = sizeof(float);
        std::memcpy(write.bytes.data(), &data, write.size);
      } else if (type == mx::Type::INTEGER) {
        const int32_t data = typed_value->asA<int>();
        write.size = sizeof(int32_t);
        std::memcpy(write.bytes.data(), &data, write.size);
      } else if (type == mx::Type::BOOLEAN) {
        // Slang lays a bool out as a 4-byte 0/1 in a cbuffer.
        const uint32_t data = typed_value->asA<bool>() ? 1u : 0u;
        write.size = sizeof(uint32_t);
        std::memcpy(write.bytes.data(), &data, write.size);
      } else if (type == mx::Type::VECTOR2) {
        const mx::Vector2 &data = typed_value->asA<mx::Vector2>();
        write.size = 2 * sizeof(float);
        std::memcpy(write.bytes.data(), data.data(), write.size);
      } else if (type == mx::Type::VECTOR3) {
        const mx::Vector3 &data = typed_value->asA<mx::Vector3>();
        write.size = 3 * sizeof(float);
        std::memcpy(write.bytes.data(), data.data(), write.size);
      } else if (type == mx::Type::COLOR3) {
        const mx::Color3 &data = typed_value->asA<mx::Color3>();
        write.size = 3 * sizeof(float);
        std::memcpy(write.bytes.data(), data.data(), write.size);
      } else if (type == mx::Type::VECTOR4) {
        const mx::Vector4 &data = typed_value->asA<mx::Vector4>();
        write.size = 4 * sizeof(float);
        std::memcpy(write.bytes.data(), data.data(), write.size);
      } else if (type == mx::Type::COLOR4) {
        const mx::Color4 &data = typed_value->asA<mx::Color4>();
        write.size = 4 * sizeof(float);
        std::memcpy(write.bytes.data(), data.data(), write.size);
      } else if (type == mx::Type::MATRIX44) {
        // MaterialX matrices are row-major; the engine's Slang session uses
        // column-major layout, so write the transpose.
        const mx::Matrix44 &data = typed_value->asA<mx::Matrix44>();
        std::array<float, 16> columns{};
        for (size_t row = 0; row < 4; row++) {
          for (size_t column = 0; column < 4; column++) {
            columns[column * 4 + row] = data[row][column];
          }
        }
        write.size = sizeof(columns);
        std::memcpy(write.bytes.data(), columns.data(), write.size);
      } else {
        SPDLOG_CRITICAL("unsupported material uniform type '{}' on port '{}'",
                        type.getName(), port->getVariable());
        assert(!"unsupported material uniform type");
        continue;
      }
      writes.push_back(write);
    }
  }

  // LIGHTING CONSTANTS: engine constants for a single white directional light.
  // Direction and view position are mirrored in every frame by the renderer.
  const std::string light_prefix = std::format("{}[0]", mx::HW::LIGHT_DATA_INSTANCE);
  const int32_t active_light_count = 1;
  const int32_t light_type = static_cast<int32_t>(DIRECTIONAL_LIGHT_TYPE_ID);
  const std::array<float, 3> light_color{1.0f, 1.0f, 1.0f};
  const float light_intensity = 1.0f;
  writes.push_back({.name = mx::HW::NUM_ACTIVE_LIGHT_SOURCES});
  std::memcpy(writes.back().bytes.data(), &active_light_count, sizeof(int32_t));
  writes.back().size = sizeof(int32_t);
  writes.push_back({.name = light_prefix + ".type"});
  std::memcpy(writes.back().bytes.data(), &light_type, sizeof(int32_t));
  writes.back().size = sizeof(int32_t);
  writes.push_back({.name = light_prefix + ".color"});
  std::memcpy(writes.back().bytes.data(), light_color.data(), sizeof(light_color));
  writes.back().size = sizeof(light_color);
  writes.push_back({.name = light_prefix + ".intensity"});
  std::memcpy(writes.back().bytes.data(), &light_intensity, sizeof(float));
  writes.back().size = sizeof(float);

  std::vector<std::byte> ubo_bytes(reflection.ubo_size);
  for (const FieldWrite &write : writes) {
    auto field = std::ranges::find_if(
        reflection.ubo_fields,
        [&](const ReflectedField &candidate) { return candidate.name == write.name; });
    assert(field != reflection.ubo_fields.end() &&
           "material UBO has no field for a valued uniform port");
    if (field == reflection.ubo_fields.end()) {
      continue;
    }
    assert(field->size == write.size && "UBO field size differs from value size");
    std::memcpy(ubo_bytes.data() + field->offset, write.bytes.data(),
                std::min(field->size, write.size));
  }

  // PER-FRAME MIRROR OFFSETS: where render() writes the camera position and
  // the light direction (both float3).
  const std::string light_direction_name = light_prefix + ".direction";
  auto view_position_field = std::ranges::find_if(
      reflection.ubo_fields, [](const ReflectedField &candidate) {
        return candidate.name == mx::HW::VIEW_POSITION;
      });
  auto light_direction_field = std::ranges::find_if(
      reflection.ubo_fields, [&](const ReflectedField &candidate) {
        return candidate.name == light_direction_name;
      });
  assert(view_position_field != reflection.ubo_fields.end() &&
         view_position_field->size == 3 * sizeof(float));
  assert(light_direction_field != reflection.ubo_fields.end() &&
         light_direction_field->size == 3 * sizeof(float));

  // -- STAGE 2: allocation --------------------------------------------------
  uint32_t index{0};
  if (!_free_slots.empty()) {
    index = _free_slots.back();
    _free_slots.pop_back();
  } else {
    assert(_slots.size() < MAX_MATERIALS && "material capacity exceeded");
    index = static_cast<uint32_t>(_slots.size());
    _slots.emplace_back();
  }
  Slot &slot = _slots[index];
  CompiledMaterial &material = slot.material;
  material.shader = shader;
  material.view_position_offset = view_position_field->offset;
  material.light_direction_offset = light_direction_field->offset;

  // SET 1 LAYOUT: from reflection; reuse an existing layout with an equal
  // binding list.
  const std::vector<ReflectedBinding> &reflected = reflection.set1_bindings;
  auto shared = std::ranges::find_if(_layouts, [&](const SharedLayout &entry) {
    return entry.bindings == reflected;
  });
  if (shared == _layouts.end()) {
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings{};
    for (const ReflectedBinding &binding : reflected) {
      layout_bindings.push_back({.binding = binding.binding,
                                 .descriptorType = binding.type,
                                 .descriptorCount = binding.count,
                                 .stageFlags = binding.stages});
    }
    VkDescriptorSetLayoutCreateInfo layout_ci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(layout_bindings.size()),
        .pBindings = layout_bindings.data()};
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    utils::validation::chk(vkCreateDescriptorSetLayout(
        _device.device, &layout_ci, nullptr, &layout));
    _layouts.push_back({.bindings = reflected, .layout = layout});
    shared = _layouts.end() - 1;
  }
  material.set1_layout = shared->layout;

  // PARAMS UBO: sized from reflection, initial values in place
  material.params_ubo =
      createBuffer(_device, {.size = reflection.ubo_size,
                             .usageFlags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             .pContents = ubo_bytes.data()});

  // TEXTURES: file textures with a full mip chain, plus the shared 1x1 black
  // default on first use. Every image is filled by one command buffer.
  struct Upload {
    Image *image;
    const stbi_uc *pixels;
    uint32_t width;
    uint32_t height;
    Buffer staging;
  };
  std::vector<Upload> uploads{};
  const std::array<stbi_uc, 4> black_pixel{0, 0, 0, 255};
  material.textures.resize(pending_textures.size());
  for (size_t i = 0; i < pending_textures.size(); i++) {
    const PendingTexture &texture = pending_textures[i];
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(_device.physicalDevice, texture.format,
                                        &format_properties);
    constexpr VkFormatFeatureFlags REQUIRED_FEATURES =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    assert((format_properties.optimalTilingFeatures & REQUIRED_FEATURES) ==
               REQUIRED_FEATURES &&
           "texture format cannot be blitted for mip generation");
    (void)REQUIRED_FEATURES;
    material.textures[i] = createImage(
        _device,
        {.width = texture.width,
         .height = texture.height,
         .mipCount = static_cast<uint32_t>(
             std::bit_width(std::max(texture.width, texture.height))),
         .format = texture.format,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         .layout = VK_IMAGE_LAYOUT_UNDEFINED,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT});
    uploads.push_back({.image = &material.textures[i],
                       .pixels = texture.pixels.get(),
                       .width = texture.width,
                       .height = texture.height});
  }
  if (_default_image.resource == VK_NULL_HANDLE) {
    _default_image = createImage(
        _device, {.width = 1,
                  .height = 1,
                  .mipCount = 1,
                  .format = VK_FORMAT_R8G8B8A8_UNORM,
                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  .layout = VK_IMAGE_LAYOUT_UNDEFINED,
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT});
    uploads.push_back({.image = &_default_image,
                       .pixels = black_pixel.data(),
                       .width = 1,
                       .height = 1});
  }
  if (!uploads.empty()) {
    VkCommandBufferAllocateInfo upload_cb_ci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = _upload_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer cb{VK_NULL_HANDLE};
    utils::validation::chk(
        vkAllocateCommandBuffers(_device.device, &upload_cb_ci, &cb));
    VkCommandBufferBeginInfo cb_bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    utils::validation::chk(vkBeginCommandBuffer(cb, &cb_bi));
    for (Upload &upload : uploads) {
      const Image &image = *upload.image;
      upload.staging = createBuffer(
          _device,
          {.size = static_cast<VkDeviceSize>(upload.width) * upload.height * 4,
           .usageFlags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
           .pContents = upload.pixels});

      // all mips: UNDEFINED -> TRANSFER_DST
      VkImageMemoryBarrier2 to_transfer_dst{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
          .srcAccessMask = VK_ACCESS_2_NONE,
          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
          .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .image = image.resource,
          .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .levelCount = image.mipCount,
                            .layerCount = 1}};
      VkDependencyInfo to_transfer_dst_info{
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .imageMemoryBarrierCount = 1,
          .pImageMemoryBarriers = &to_transfer_dst};
      vkCmdPipelineBarrier2(cb, &to_transfer_dst_info);

      // mip 0 from the staging buffer
      VkBufferImageCopy2 copy_region{
          .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
          .imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .layerCount = 1},
          .imageExtent{.width = upload.width, .height = upload.height, .depth = 1}};
      VkCopyBufferToImageInfo2 copy_info{
          .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
          .srcBuffer = upload.staging.resource,
          .dstImage = image.resource,
          .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .regionCount = 1,
          .pRegions = &copy_region};
      vkCmdCopyBufferToImage2(cb, &copy_info);

      // mips 1..n-1: each blitted from the previous one
      for (uint32_t mip = 1; mip < image.mipCount; mip++) {
        VkImageMemoryBarrier2 previous_to_src{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = image.resource,
            .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .baseMipLevel = mip - 1,
                              .levelCount = 1,
                              .layerCount = 1}};
        VkDependencyInfo previous_to_src_info{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &previous_to_src};
        vkCmdPipelineBarrier2(cb, &previous_to_src_info);

        VkImageBlit2 blit_region{
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = mip - 1,
                            .layerCount = 1},
            .srcOffsets = {{0, 0, 0},
                           {static_cast<int32_t>(
                                std::max(1u, upload.width >> (mip - 1))),
                            static_cast<int32_t>(
                                std::max(1u, upload.height >> (mip - 1))),
                            1}},
            .dstSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = mip,
                            .layerCount = 1},
            .dstOffsets = {{0, 0, 0},
                           {static_cast<int32_t>(
                                std::max(1u, upload.width >> mip)),
                            static_cast<int32_t>(
                                std::max(1u, upload.height >> mip)),
                            1}}};
        VkBlitImageInfo2 blit_info{
            .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .srcImage = image.resource,
            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dstImage = image.resource,
            .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .regionCount = 1,
            .pRegions = &blit_region,
            .filter = VK_FILTER_LINEAR};
        vkCmdBlitImage2(cb, &blit_info);
      }

      // everything -> SHADER_READ_ONLY: mips 0..n-2 are TRANSFER_SRC, the
      // last mip is still TRANSFER_DST
      std::array<VkImageMemoryBarrier2, 2> to_shader_read{
          VkImageMemoryBarrier2{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
              .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .image = image.resource,
              .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = 0,
                                .levelCount = image.mipCount - 1,
                                .layerCount = 1}},
          VkImageMemoryBarrier2{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .image = image.resource,
              .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = image.mipCount - 1,
                                .levelCount = 1,
                                .layerCount = 1}}};
      // A one-mip image has only the second barrier.
      VkDependencyInfo to_shader_read_info{
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .imageMemoryBarrierCount = image.mipCount > 1 ? 2u : 1u,
          .pImageMemoryBarriers =
              image.mipCount > 1 ? to_shader_read.data() : &to_shader_read[1]};
      vkCmdPipelineBarrier2(cb, &to_shader_read_info);
    }
    utils::validation::chk(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fence_ci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{VK_NULL_HANDLE};
    utils::validation::chk(
        vkCreateFence(_device.device, &fence_ci, nullptr, &fence));
    VkCommandBufferSubmitInfo cb_submit_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cb};
    VkSubmitInfo2 submit_info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .commandBufferInfoCount = 1,
                              .pCommandBufferInfos = &cb_submit_info};
    utils::validation::chk(
        vkQueueSubmit2(_device.graphicsQueue, 1, &submit_info, fence));
    utils::validation::chk(
        vkWaitForFences(_device.device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(_device.device, fence, nullptr);
    vkFreeCommandBuffers(_device.device, _upload_pool, 1, &cb);
    for (Upload &upload : uploads) {
      destroyBuffer(_device, upload.staging);
    }
  }

  // SET 1: the UBO and every reflected texture/sampler pair. Resources with
  // no file texture (the env textures, F3) get the shared black default.
  VkDescriptorSetAllocateInfo set_alloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = _pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &material.set1_layout};
  utils::validation::chk(
      vkAllocateDescriptorSets(_device.device, &set_alloc, &material.set1));
  VkDescriptorBufferInfo ubo_info{.buffer = material.params_ubo.resource,
                                  .offset = 0,
                                  .range = reflection.ubo_size};
  std::vector<VkDescriptorImageInfo> image_infos(2 * reflection.resources.size());
  std::vector<VkWriteDescriptorSet> descriptor_writes{};
  descriptor_writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = material.set1,
                               .dstBinding = reflection.ubo_binding,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                               .pBufferInfo = &ubo_info});
  for (size_t i = 0; i < reflection.resources.size(); i++) {
    const ReflectedResource &resource = reflection.resources[i];
    const Image *image = &_default_image;
    for (size_t t = 0; t < pending_textures.size(); t++) {
      if (pending_textures[t].resource_name == resource.name) {
        image = &material.textures[t];
      }
    }
    image_infos[2 * i] = {.imageView = image->view,
                          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    image_infos[2 * i + 1] = {.sampler = _sampler};
    descriptor_writes.push_back(
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = material.set1,
         .dstBinding = resource.image_binding,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .pImageInfo = &image_infos[2 * i]});
    descriptor_writes.push_back(
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = material.set1,
         .dstBinding = resource.sampler_binding,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
         .pImageInfo = &image_infos[2 * i + 1]});
  }
  vkUpdateDescriptorSets(_device.device,
                         static_cast<uint32_t>(descriptor_writes.size()),
                         descriptor_writes.data(), 0, nullptr);

  slot.live = true;
  return {.index = index, .generation = slot.generation};
}

CompiledMaterial &MaterialLibrary::get(MaterialHandle handle) {
  assert(handle.index < _slots.size() && _slots[handle.index].live &&
         _slots[handle.index].generation == handle.generation &&
         "stale or invalid MaterialHandle");
  return _slots[handle.index].material;
}

void MaterialLibrary::release(MaterialHandle handle) {
  assert(handle.index < _slots.size() && _slots[handle.index].live &&
         _slots[handle.index].generation == handle.generation &&
         "stale or invalid MaterialHandle");
  Slot &slot = _slots[handle.index];
  utils::validation::chk(vkFreeDescriptorSets(_device.device, _pool, 1,
                                              &slot.material.set1));
  destroyBuffer(_device, slot.material.params_ubo);
  for (Image &texture : slot.material.textures) {
    destroyImage(_device, texture);
  }
  slot.material = {};
  slot.live = false;
  slot.generation++;
  _free_slots.push_back(handle.index);
}

void MaterialLibrary::destroy() {
  for (uint32_t i = 0; i < _slots.size(); i++) {
    if (_slots[i].live) {
      release({.index = i, .generation = _slots[i].generation});
    }
  }
  for (const SharedLayout &entry : _layouts) {
    vkDestroyDescriptorSetLayout(_device.device, entry.layout, nullptr);
  }
  _layouts.clear();
  if (_default_image.resource != VK_NULL_HANDLE) {
    destroyImage(_device, _default_image);
    _default_image = {};
  }
  destroySampler(_device, _sampler);
  vkDestroyCommandPool(_device.device, _upload_pool, nullptr);
  _upload_pool = VK_NULL_HANDLE;
  vkDestroyDescriptorPool(_device.device, _pool, nullptr);
  _pool = VK_NULL_HANDLE;
  _stdlib.reset();
}
