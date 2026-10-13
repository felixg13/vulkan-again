#include <renderer.h>

#include <shader.h>
#include <utils.h>

#include <array>
#include <cassert>
#include <cstring>
#include <MaterialXFormat/XmlIo.h>

#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <glm/gtc/type_ptr.hpp>

namespace {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
constexpr uint32_t MAX_PRIMS = 64;
const std::filesystem::path PIPELINE_CACHE_FILE{PIPELINE_CACHE_PATH};
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D24_UNORM_S8_UINT;

struct PerDrawData {
  glm::mat4 transform;
};

} // namespace

Renderer::Renderer(const Device &device,
                   const std::filesystem::path &shader_path)
    : _device{device}, _shader_path{shader_path}, _materials{_device} {
  // COMMAND POOL, COMMAND BUFFER, COMPLETION FENCE
  VkCommandPoolCreateInfo command_pool_ci{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = _device.graphicsQueueFamily};
  utils::validation::chk(
      vkCreateCommandPool(_device.device, &command_pool_ci, nullptr,
                          &_command_pool));
  VkCommandBufferAllocateInfo command_buffer_alloc_ci{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = _command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  utils::validation::chk(vkAllocateCommandBuffers(
      _device.device, &command_buffer_alloc_ci, &_command_buffer));
  VkFenceCreateInfo fence_ci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  utils::validation::chk(
      vkCreateFence(_device.device, &fence_ci, nullptr, &_fence));

  // PER-FRAME UBOS (set 0) AND TRANSFORM RINGS (BDA), ONE PER FRAME IN FLIGHT
  _per_frame_buffers.resize(MAX_FRAMES_IN_FLIGHT);
  _transform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
  _transform_addresses.resize(MAX_FRAMES_IN_FLIGHT);
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    _per_frame_buffers[i] =
        createBuffer(_device, {.size = sizeof(PerFrameData),
                               .usageFlags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT});
    _transform_buffers[i] = createBuffer(
        _device, {.size = sizeof(PerDrawData) * MAX_PRIMS,
                  .usageFlags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT});
    VkBufferDeviceAddressInfo transform_bda_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _transform_buffers[i].resource};
    _transform_addresses[i] =
        vkGetBufferDeviceAddress(_device.device, &transform_bda_info);
  }

  // DESCRIPTORS: set 0 = per-frame UBO (set 1 lives in the MaterialLibrary)
  VkDescriptorSetLayoutBinding set0_binding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo set0_layout_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &set0_binding};
  utils::validation::chk(vkCreateDescriptorSetLayout(
      _device.device, &set0_layout_ci, nullptr, &_set0_layout));

  VkDescriptorPoolSize set0_pool_size{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      .descriptorCount = MAX_FRAMES_IN_FLIGHT};
  VkDescriptorPoolCreateInfo set0_pool_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = MAX_FRAMES_IN_FLIGHT,
      .poolSizeCount = 1,
      .pPoolSizes = &set0_pool_size};
  utils::validation::chk(vkCreateDescriptorPool(_device.device, &set0_pool_ci,
                                                nullptr, &_set0_pool));

  _set0_sets.resize(MAX_FRAMES_IN_FLIGHT);
  for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VkDescriptorSetAllocateInfo set0_alloc{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = _set0_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &_set0_layout};
    utils::validation::chk(
        vkAllocateDescriptorSets(_device.device, &set0_alloc, &_set0_sets[i]));
    VkDescriptorBufferInfo per_frame_buffer_info{
        .buffer = _per_frame_buffers[i].resource,
        .offset = 0,
        .range = sizeof(PerFrameData)};
    VkWriteDescriptorSet write_set0{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = _set0_sets[i],
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &per_frame_buffer_info};
    vkUpdateDescriptorSets(_device.device, 1, &write_set0, 0, nullptr);
  }

  // VERTEX LAYOUT: the fixed {pos, normal, uv} layout
  _vertex_layout = {
      .binding{.binding = 0,
               .stride = sizeof(Vertex),
               .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
      .attributes{
          {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT},
          {.location = 1,
           .binding = 0,
           .format = VK_FORMAT_R32G32B32_SFLOAT,
           .offset = offsetof(Vertex, normal)},
          {.location = 2,
           .binding = 0,
           .format = VK_FORMAT_R32G32_SFLOAT,
           .offset = offsetof(Vertex, uv)}}};

  // VKPIPELINECACHE, seeded from disk when the file is readable
  std::vector<char> cache_bytes{};
  if (std::ifstream cache_file{PIPELINE_CACHE_FILE, std::ios::binary};
      cache_file.good()) {
    cache_bytes.assign(std::istreambuf_iterator<char>(cache_file),
                       std::istreambuf_iterator<char>());
    SPDLOG_INFO("Loaded pipeline cache from {} ({} bytes)",
                PIPELINE_CACHE_FILE.string(), cache_bytes.size());
  } else {
    SPDLOG_INFO("No pipeline cache file to load; starting with an empty cache");
  }
  VkPipelineCacheCreateInfo pipeline_cache_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      .initialDataSize = cache_bytes.size(),
      .pInitialData = cache_bytes.data()};
  utils::validation::chk(vkCreatePipelineCache(
      _device.device, &pipeline_cache_ci, nullptr, &_vk_pipeline_cache));
}

Renderer::~Renderer() {
  // Materials first, then the pipeline cache, then the rest.
  _materials.destroy();
  for (auto &mesh : _meshes) {
    if (mesh.live) {
      destroyBuffer(_device, mesh.vertex_buffer);
      destroyBuffer(_device, mesh.index_buffer);
    }
  }
  if (_readback_buffer.resource != VK_NULL_HANDLE) {
    destroyBuffer(_device, _readback_buffer);
  }
  if (_offscreen_image.resource != VK_NULL_HANDLE) {
    destroyImage(_device, _offscreen_image);
  }
  if (_depth_image.resource != VK_NULL_HANDLE) {
    destroyImage(_device, _depth_image);
  }
  _pipelines.destroy(_device.device);
  size_t cache_size{0};
  utils::validation::chk(vkGetPipelineCacheData(
      _device.device, _vk_pipeline_cache, &cache_size, nullptr));
  std::vector<char> cache_bytes(cache_size);
  utils::validation::chk(vkGetPipelineCacheData(
      _device.device, _vk_pipeline_cache, &cache_size, cache_bytes.data()));
  if (std::ofstream cache_file{PIPELINE_CACHE_FILE,
                               std::ios::binary | std::ios::trunc};
      cache_file.write(cache_bytes.data(),
                       static_cast<std::streamsize>(cache_size))) {
    SPDLOG_INFO("Saved pipeline cache to {} ({} bytes)",
                PIPELINE_CACHE_FILE.string(), cache_size);
  } else {
    SPDLOG_INFO("Could not write {}", PIPELINE_CACHE_FILE.string());
  }
  vkDestroyPipelineCache(_device.device, _vk_pipeline_cache, nullptr);
  vkDestroyDescriptorPool(_device.device, _set0_pool, nullptr);
  vkDestroyDescriptorSetLayout(_device.device, _set0_layout, nullptr);
  for (auto &buffer : _per_frame_buffers) {
    destroyBuffer(_device, buffer);
  }
  for (auto &buffer : _transform_buffers) {
    destroyBuffer(_device, buffer);
  }
  vkDestroyFence(_device.device, _fence, nullptr);
  vkDestroyCommandPool(_device.device, _command_pool, nullptr);
}

MeshHandle Renderer::add_mesh(const MeshData &mesh) {
  assert(!mesh.vertices.empty() && !mesh.indices.empty());
  uint32_t index{0};
  if (!_free_meshes.empty()) {
    index = _free_meshes.back();
    _free_meshes.pop_back();
  } else {
    index = static_cast<uint32_t>(_meshes.size());
    _meshes.emplace_back();
  }
  MeshSlot &slot = _meshes[index];
  slot.vertex_buffer = createBuffer(
      _device, {.size = sizeof(Vertex) * mesh.vertices.size(),
                .usageFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                .pContents = mesh.vertices.data()});
  slot.index_buffer = createBuffer(
      _device, {.size = sizeof(uint32_t) * mesh.indices.size(),
                .usageFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                .pContents = mesh.indices.data()});
  slot.index_count = static_cast<uint32_t>(mesh.indices.size());
  slot.live = true;
  return {.index = index, .generation = slot.generation};
}

void Renderer::release_mesh(MeshHandle mesh) {
  assert(mesh.index < _meshes.size() && _meshes[mesh.index].live &&
         _meshes[mesh.index].generation == mesh.generation &&
         "stale or invalid MeshHandle");
  for (const auto &prim : _prims) {
    assert(!(prim.live && prim.mesh_index == mesh.index) &&
           "release_mesh: a live prim still references this mesh");
  }
  MeshSlot &slot = _meshes[mesh.index];
  destroyBuffer(_device, slot.vertex_buffer);
  destroyBuffer(_device, slot.index_buffer);
  slot.live = false;
  slot.generation++;
  _free_meshes.push_back(mesh.index);
}

MaterialHandle Renderer::compile_material(MaterialX::DocumentPtr document) {
  // ENGINE VERTEX STAGE: concatenated into every material's module
  std::ifstream vertex_file{_shader_path, std::ios::binary};
  if (!vertex_file) {
    throw std::runtime_error(std::format("cannot read the engine vertex stage '{}'",
                                         _shader_path.string()));
  }
  std::string vertex_source{std::istreambuf_iterator<char>(vertex_file),
                            std::istreambuf_iterator<char>()};

  MaterialHandle handle = _materials.compile(document, vertex_source);

  // PRE-WARM: only possible once a target format is known; otherwise the
  // first render creates the pipeline.
  if (_known_color_format != VK_FORMAT_UNDEFINED) {
    CompiledMaterial &material = _materials.get(handle);
    PipelineAndLayout pipeline = _pipelines.get_or_create(
        _device.device, _vk_pipeline_cache, _set0_layout, material.shader,
        material.set1_layout, _vertex_layout, _known_color_format,
        DEPTH_FORMAT);
    material.pipeline = pipeline.pipeline;
    material.pipeline_layout = pipeline.layout;
  }
  return handle;
}

MaterialHandle Renderer::compile_material_from_file(
    const std::filesystem::path &path) {
  MaterialX::DocumentPtr document = MaterialX::createDocument();
  try {
    MaterialX::readFromXmlFile(document, MaterialX::FilePath{path.string()});
  } catch (const MaterialX::Exception &error) {
    throw std::runtime_error(std::format("cannot read MaterialX file '{}': {}",
                                         path.string(), error.what()));
  }
  return compile_material(document);
}

void Renderer::release_material(MaterialHandle material) {
  _materials.get(material); // asserts on a stale handle
  for (const auto &prim : _prims) {
    assert(!(prim.live && prim.material.index == material.index) &&
           "release_material: a live prim still references this material");
  }
  _materials.release(material);
}

PrimHandle Renderer::add_prim(MeshHandle mesh, MaterialHandle material,
                              const glm::mat4 &transform) {
  assert(mesh.index < _meshes.size() && _meshes[mesh.index].live &&
         _meshes[mesh.index].generation == mesh.generation &&
         "stale or invalid MeshHandle");
  _materials.get(material); // asserts on a stale handle
  uint32_t index{0};
  if (!_free_prims.empty()) {
    index = _free_prims.back();
    _free_prims.pop_back();
  } else {
    assert(_prims.size() < MAX_PRIMS && "prim capacity exceeded");
    index = static_cast<uint32_t>(_prims.size());
    _prims.emplace_back();
  }
  PrimSlot &slot = _prims[index];
  slot.mesh_index = mesh.index;
  slot.material = material;

  // TRANSFORM: written into every ring copy so any frame slot sees it
  for (auto &transform_buffer : _transform_buffers) {
    std::memcpy(static_cast<std::byte *>(transform_buffer.pMappedData) +
                    index * sizeof(PerDrawData),
                glm::value_ptr(transform), sizeof(PerDrawData));
  }

  slot.live = true;
  return {.index = index, .generation = slot.generation};
}

void Renderer::set_transform(PrimHandle prim, const glm::mat4 &transform) {
  assert(prim.index < _prims.size() && _prims[prim.index].live &&
         _prims[prim.index].generation == prim.generation &&
         "stale or invalid PrimHandle");
  // Written into every ring copy so any frame slot sees it.
  for (auto &transform_buffer : _transform_buffers) {
    std::memcpy(static_cast<std::byte *>(transform_buffer.pMappedData) +
                    prim.index * sizeof(PerDrawData),
                glm::value_ptr(transform), sizeof(PerDrawData));
  }
}

void Renderer::remove_prim(PrimHandle prim) {
  assert(prim.index < _prims.size() && _prims[prim.index].live &&
         _prims[prim.index].generation == prim.generation &&
         "stale or invalid PrimHandle");
  PrimSlot &slot = _prims[prim.index];
  // Only the prim and its transform slot; mesh and material are explicit.
  slot.live = false;
  slot.generation++;
  _free_prims.push_back(prim.index);
}

RenderTarget Renderer::create_offscreen_target(VkExtent2D extent,
                                               VkFormat format) {
  assert((format == VK_FORMAT_R8G8B8A8_SRGB ||
          format == VK_FORMAT_B8G8R8A8_SRGB) &&
         "read_back assumes 4 bytes per pixel");
  _known_color_format = format;
  if (_readback_buffer.resource != VK_NULL_HANDLE) {
    destroyBuffer(_device, _readback_buffer);
  }
  if (_offscreen_image.resource != VK_NULL_HANDLE) {
    destroyImage(_device, _offscreen_image);
  }
  _offscreen_image = createImage(
      _device, {.width = extent.width,
                .height = extent.height,
                .mipCount = 1,
                .format = format,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT});

  // Host-visible, random-access (readback) buffer; bypasses createBuffer,
  // which requests sequential-write host access.
  VkBufferCreateInfo readback_ci{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VmaAllocationCreateInfo readback_alloc_ci{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO};
  VmaAllocationInfo readback_info{};
  _readback_buffer = {};
  _readback_buffer.size = readback_ci.size;
  utils::validation::chk(vmaCreateBuffer(
      _device.allocator, &readback_ci, &readback_alloc_ci,
      &_readback_buffer.resource, &_readback_buffer.allocation, &readback_info));
  _readback_buffer.pMappedData = readback_info.pMappedData;

  return {.image = _offscreen_image.resource,
          .view = _offscreen_image.view,
          .format = format,
          .extent = extent,
          .final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
}

void Renderer::render(const RenderTarget &target, const PerFrameData &frame,
                      const RenderSync &sync) {
  assert(glm::length(glm::vec3(frame.light_dir)) > 0.0f);
  _known_color_format = target.format;

  // DEPTH IMAGE follows the target extent
  if (_depth_image.width != target.extent.width ||
      _depth_image.height != target.extent.height) {
    if (_depth_image.resource != VK_NULL_HANDLE) {
      destroyImage(_device, _depth_image);
    }
    _depth_image = createImage(
        _device, {.width = target.extent.width,
                  .height = target.extent.height,
                  .mipCount = 1,
                  .format = DEPTH_FORMAT,
                  .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  .layout = VK_IMAGE_LAYOUT_UNDEFINED,
                  .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT});
  }

  // PER-FRAME DATA
  PerFrameData frame_data = frame;
  frame_data.light_dir =
      glm::vec4(glm::normalize(glm::vec3(frame.light_dir)), 0.0f);
  std::memcpy(_per_frame_buffers[_frame_index].pMappedData, &frame_data,
              sizeof(PerFrameData));

  // MATERIAL MIRROR: camera world position and light direction into every
  // live material's UBO (MaterialX keeps both in the material block, F1).
  // u_lightData[0].direction is the direction light travels (the generated
  // code negates it to get L), which is the engine convention: no negation.
  const glm::vec3 view_position{glm::inverse(frame.view)[3]};
  const glm::vec3 light_direction{frame_data.light_dir};
  for (auto &slot : _materials._slots) {
    if (!slot.live) {
      continue;
    }
    auto *ubo = static_cast<std::byte *>(slot.material.params_ubo.pMappedData);
    std::memcpy(ubo + slot.material.view_position_offset,
                glm::value_ptr(view_position), sizeof(glm::vec3));
    std::memcpy(ubo + slot.material.light_direction_offset,
                glm::value_ptr(light_direction), sizeof(glm::vec3));
  }

  // RECORD
  VkCommandBuffer cb = _command_buffer;
  utils::validation::chk(vkResetCommandBuffer(cb, 0));
  VkCommandBufferBeginInfo cb_bi{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  utils::validation::chk(vkBeginCommandBuffer(cb, &cb_bi));
  std::array<VkImageMemoryBarrier2, 2> output_barriers{
      VkImageMemoryBarrier2{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_2_NONE,
          .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
          .image = target.image,
          .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .levelCount = 1,
                            .layerCount = 1}},
      VkImageMemoryBarrier2{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
          .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
          .image = _depth_image.resource,
          .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                          VK_IMAGE_ASPECT_STENCIL_BIT,
                            .levelCount = 1,
                            .layerCount = 1}}};
  VkDependencyInfo output_dependency_info{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = static_cast<uint32_t>(output_barriers.size()),
      .pImageMemoryBarriers = output_barriers.data()};
  vkCmdPipelineBarrier2(cb, &output_dependency_info);

  VkRenderingAttachmentInfo color_attachment_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = target.view,
      .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue{.color{0.0f, 0.0f, 0.2f, 1.0f}}};
  VkRenderingAttachmentInfo depth_attachment_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = _depth_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .clearValue = {.depthStencil = {1.0f, 0}}};
  VkRenderingInfo rendering_info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea{.extent = target.extent},
                                 .layerCount = 1,
                                 .colorAttachmentCount = 1,
                                 .pColorAttachments = &color_attachment_info,
                                 .pDepthAttachment = &depth_attachment_info};
  vkCmdBeginRendering(cb, &rendering_info);
  VkViewport viewport{.width = static_cast<float>(target.extent.width),
                      .height = static_cast<float>(target.extent.height),
                      .minDepth = 0.0f,
                      .maxDepth = 1.0f};
  vkCmdSetViewport(cb, 0, 1, &viewport);
  VkRect2D scissor{.extent{target.extent}};
  vkCmdSetScissor(cb, 0, 1, &scissor);
  VkDeviceSize vertex_offset{0};
  for (uint32_t i = 0; i < _prims.size(); i++) {
    const PrimSlot &prim = _prims[i];
    if (!prim.live) {
      continue;
    }
    const MeshSlot &mesh = _meshes[prim.mesh_index];
    CompiledMaterial &material = _materials.get(prim.material);
    PipelineAndLayout pipeline = _pipelines.get_or_create(
        _device.device, _vk_pipeline_cache, _set0_layout, material.shader,
        material.set1_layout, _vertex_layout, target.format,
        _depth_image.format);
    material.pipeline = pipeline.pipeline;
    material.pipeline_layout = pipeline.layout;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout, 0, 1, &_set0_sets[_frame_index], 0,
                            nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout, 1, 1, &material.set1, 0,
                            nullptr);
    vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertex_buffer.resource,
                           &vertex_offset);
    vkCmdBindIndexBuffer(cb, mesh.index_buffer.resource, 0,
                         VK_INDEX_TYPE_UINT32);
    VkDeviceAddress per_draw_address{_transform_addresses[_frame_index] +
                                     i * sizeof(PerDrawData)};
    vkCmdPushConstants(cb, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(VkDeviceAddress), &per_draw_address);
    vkCmdDrawIndexed(cb, mesh.index_count, 1, 0, 0, 0);
  }
  vkCmdEndRendering(cb);

  VkImageMemoryBarrier2 final_barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_2_NONE,
      .oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
      .newLayout = target.final_layout,
      .image = target.image,
      .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1,
                        .layerCount = 1}};
  if (target.final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    final_barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    final_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  }
  VkDependencyInfo final_dependency_info{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &final_barrier};
  vkCmdPipelineBarrier2(cb, &final_dependency_info);
  utils::validation::chk(vkEndCommandBuffer(cb));

  // SUBMIT AND WAIT
  VkSemaphoreSubmitInfo wait_semaphore_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = sync.wait_semaphore,
      .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSemaphoreSubmitInfo signal_semaphore_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = sync.signal_semaphore,
      .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkCommandBufferSubmitInfo command_buffer_submit_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = cb};
  VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount =
          sync.wait_semaphore != VK_NULL_HANDLE ? 1u : 0u,
      .pWaitSemaphoreInfos = &wait_semaphore_info,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &command_buffer_submit_info,
      .signalSemaphoreInfoCount =
          sync.signal_semaphore != VK_NULL_HANDLE ? 1u : 0u,
      .pSignalSemaphoreInfos = &signal_semaphore_info};
  utils::validation::chk(
      vkQueueSubmit2(_device.graphicsQueue, 1, &submit_info, _fence));
  utils::validation::chk(
      vkWaitForFences(_device.device, 1, &_fence, VK_TRUE, UINT64_MAX));
  utils::validation::chk(vkResetFences(_device.device, 1, &_fence));

  _frame_index = (_frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::read_back(std::span<std::byte> out) {
  assert(_offscreen_image.resource != VK_NULL_HANDLE &&
         "read_back: no offscreen target");
  assert(out.size() >= _readback_buffer.size && "read_back: span too small");

  VkCommandBuffer cb = _command_buffer;
  utils::validation::chk(vkResetCommandBuffer(cb, 0));
  VkCommandBufferBeginInfo cb_bi{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  utils::validation::chk(vkBeginCommandBuffer(cb, &cb_bi));
  VkBufferImageCopy2 copy_region{
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .layerCount = 1},
      .imageExtent{.width = _offscreen_image.width,
                   .height = _offscreen_image.height,
                   .depth = 1}};
  VkCopyImageToBufferInfo2 copy_info{
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = _offscreen_image.resource,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstBuffer = _readback_buffer.resource,
      .regionCount = 1,
      .pRegions = &copy_region};
  vkCmdCopyImageToBuffer2(cb, &copy_info);
  VkBufferMemoryBarrier2 to_host{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .buffer = _readback_buffer.resource,
      .size = VK_WHOLE_SIZE};
  VkDependencyInfo to_host_dependency_info{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers = &to_host};
  vkCmdPipelineBarrier2(cb, &to_host_dependency_info);
  utils::validation::chk(vkEndCommandBuffer(cb));

  VkCommandBufferSubmitInfo command_buffer_submit_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = cb};
  VkSubmitInfo2 submit_info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                            .commandBufferInfoCount = 1,
                            .pCommandBufferInfos = &command_buffer_submit_info};
  utils::validation::chk(
      vkQueueSubmit2(_device.graphicsQueue, 1, &submit_info, _fence));
  utils::validation::chk(
      vkWaitForFences(_device.device, 1, &_fence, VK_TRUE, UINT64_MAX));
  utils::validation::chk(vkResetFences(_device.device, 1, &_fence));

  utils::validation::chk(vmaInvalidateAllocation(
      _device.allocator, _readback_buffer.allocation, 0, VK_WHOLE_SIZE));
  std::memcpy(out.data(), _readback_buffer.pMappedData, _readback_buffer.size);
}

size_t Renderer::pipeline_count() const { return _pipelines.pipeline_count(); }
