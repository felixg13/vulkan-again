#include <renderer.h>

#include <shader.h>
#include <utils.h>

#include <ktx.h>
#include <ktxvulkan.h>

#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>
#include <glm/gtc/type_ptr.hpp>

namespace {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
constexpr uint32_t MAX_PRIMS = 64;

struct PerDrawData {
  glm::mat4 transform;
};

} // namespace

Renderer::Renderer(const Device &device,
                   const std::filesystem::path &shader_path)
    : _device{device} {
  // SHADER
  ShaderCompiler shader_compiler = createShaderCompiler();
  _shader = compileShader(shader_compiler, "shader", shader_path);
  destroyShaderCompiler(shader_compiler);

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

  // DESCRIPTORS: set 0 = per-frame UBO, set 1 = base color texture per prim
  VkDescriptorSetLayoutBinding set0_binding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
  VkDescriptorSetLayoutCreateInfo set0_layout_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &set0_binding};
  utils::validation::chk(vkCreateDescriptorSetLayout(
      _device.device, &set0_layout_ci, nullptr, &_set0_layout));
  VkDescriptorSetLayoutBinding set1_binding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo set1_layout_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &set1_binding};
  utils::validation::chk(vkCreateDescriptorSetLayout(
      _device.device, &set1_layout_ci, nullptr, &_set1_layout));

  VkDescriptorPoolSize set0_pool_size{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      .descriptorCount = MAX_FRAMES_IN_FLIGHT};
  VkDescriptorPoolCreateInfo set0_pool_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = MAX_FRAMES_IN_FLIGHT,
      .poolSizeCount = 1,
      .pPoolSizes = &set0_pool_size};
  utils::validation::chk(vkCreateDescriptorPool(_device.device, &set0_pool_ci,
                                                nullptr, &_set0_pool));
  VkDescriptorPoolSize set1_pool_size{
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = MAX_PRIMS};
  VkDescriptorPoolCreateInfo set1_pool_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = MAX_PRIMS,
      .poolSizeCount = 1,
      .pPoolSizes = &set1_pool_size};
  utils::validation::chk(vkCreateDescriptorPool(_device.device, &set1_pool_ci,
                                                nullptr, &_set1_pool));

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
  if (std::ifstream cache_file{"pipeline_cache.bin", std::ios::binary};
      cache_file.good()) {
    cache_bytes.assign(std::istreambuf_iterator<char>(cache_file),
                       std::istreambuf_iterator<char>());
    SPDLOG_INFO("Loaded pipeline cache from pipeline_cache.bin ({} bytes)",
                cache_bytes.size());
  } else {
    SPDLOG_INFO("No pipeline_cache.bin to load; starting with an empty cache");
  }
  VkPipelineCacheCreateInfo pipeline_cache_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      .initialDataSize = cache_bytes.size(),
      .pInitialData = cache_bytes.data()};
  utils::validation::chk(vkCreatePipelineCache(
      _device.device, &pipeline_cache_ci, nullptr, &_vk_pipeline_cache));
}

Renderer::~Renderer() {
  for (auto &prim : _prims) {
    if (prim.live) {
      destroyTexture(_device, prim.texture);
    }
  }
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
  if (std::ofstream cache_file{"pipeline_cache.bin",
                               std::ios::binary | std::ios::trunc};
      cache_file.write(cache_bytes.data(),
                       static_cast<std::streamsize>(cache_size))) {
    SPDLOG_INFO("Saved pipeline cache to pipeline_cache.bin ({} bytes)",
                cache_size);
  } else {
    SPDLOG_INFO("Could not write pipeline_cache.bin");
  }
  vkDestroyPipelineCache(_device.device, _vk_pipeline_cache, nullptr);
  vkDestroyDescriptorPool(_device.device, _set1_pool, nullptr);
  vkDestroyDescriptorPool(_device.device, _set0_pool, nullptr);
  vkDestroyDescriptorSetLayout(_device.device, _set1_layout, nullptr);
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

PrimHandle Renderer::add_prim(MeshHandle mesh,
                              const std::filesystem::path &texture_path,
                              const glm::mat4 &transform) {
  assert(mesh.index < _meshes.size() && _meshes[mesh.index].live &&
         _meshes[mesh.index].generation == mesh.generation &&
         "stale or invalid MeshHandle");
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

  // TEXTURE (stand-in for the material's texture set; Step 4 replaces it)
  ktxTexture *ktx_texture{nullptr};
  utils::validation::chk(ktxTexture_CreateFromNamedFile(
      texture_path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
      &ktx_texture));
  slot.texture = createTexture(
      _device,
      {.width = ktx_texture->baseWidth,
       .height = ktx_texture->baseHeight,
       .mipCount = ktx_texture->numLevels,
       .format = ktxTexture_GetVkFormat(ktx_texture),
       .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
       .layout = VK_IMAGE_LAYOUT_UNDEFINED,
       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
      {.maxLod = static_cast<float>(ktx_texture->numLevels)});

  Buffer staging_buffer = createBuffer(
      _device, {.size = static_cast<VkDeviceSize>(ktx_texture->dataSize),
                .usageFlags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .pContents = ktx_texture->pData});

  VkCommandBufferAllocateInfo upload_cb_alloc_ci{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = _command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  VkCommandBuffer upload_cb{VK_NULL_HANDLE};
  utils::validation::chk(
      vkAllocateCommandBuffers(_device.device, &upload_cb_alloc_ci, &upload_cb));
  VkCommandBufferBeginInfo upload_cb_bi{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  utils::validation::chk(vkBeginCommandBuffer(upload_cb, &upload_cb_bi));

  VkImageMemoryBarrier2 to_transfer_dst{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .image = slot.texture.image.resource,
      .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = slot.texture.image.mipCount,
                        .layerCount = 1}};
  VkDependencyInfo upload_barrier_dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &to_transfer_dst};
  vkCmdPipelineBarrier2(upload_cb, &upload_barrier_dep);

  std::vector<VkBufferImageCopy> copy_regions{};
  for (uint32_t level = 0; level < ktx_texture->numLevels; level++) {
    ktx_size_t mip_offset{0};
    utils::validation::chk(
        ktxTexture_GetImageOffset(ktx_texture, level, 0, 0, &mip_offset));
    copy_regions.push_back(
        {.bufferOffset = mip_offset,
         .imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = level,
                           .layerCount = 1},
         .imageExtent{.width = ktx_texture->baseWidth >> level,
                      .height = ktx_texture->baseHeight >> level,
                      .depth = 1}});
  }
  vkCmdCopyBufferToImage(
      upload_cb, staging_buffer.resource, slot.texture.image.resource,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      static_cast<uint32_t>(copy_regions.size()), copy_regions.data());

  VkImageMemoryBarrier2 to_read_only{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
      .image = slot.texture.image.resource,
      .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = slot.texture.image.mipCount,
                        .layerCount = 1}};
  upload_barrier_dep.pImageMemoryBarriers = &to_read_only;
  vkCmdPipelineBarrier2(upload_cb, &upload_barrier_dep);
  utils::validation::chk(vkEndCommandBuffer(upload_cb));

  VkCommandBufferSubmitInfo upload_cb_submit_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = upload_cb};
  VkSubmitInfo2 upload_submit_info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &upload_cb_submit_info};
  utils::validation::chk(
      vkQueueSubmit2(_device.graphicsQueue, 1, &upload_submit_info, _fence));
  utils::validation::chk(
      vkWaitForFences(_device.device, 1, &_fence, VK_TRUE, UINT64_MAX));
  utils::validation::chk(vkResetFences(_device.device, 1, &_fence));

  vkFreeCommandBuffers(_device.device, _command_pool, 1, &upload_cb);
  destroyBuffer(_device, staging_buffer);
  ktxTexture_Destroy(ktx_texture);

  VkDescriptorSetAllocateInfo tex_desc_set_alloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = _set1_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &_set1_layout};
  utils::validation::chk(vkAllocateDescriptorSets(
      _device.device, &tex_desc_set_alloc, &slot.texture_desc_set));
  VkDescriptorImageInfo texture_descriptor{
      .sampler = slot.texture.sampler,
      .imageView = slot.texture.image.view,
      .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write_tex_desc_set{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = slot.texture_desc_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &texture_descriptor};
  vkUpdateDescriptorSets(_device.device, 1, &write_tex_desc_set, 0, nullptr);

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
  // The stand-in texture belongs to the prim; the mesh is never freed here.
  utils::validation::chk(vkFreeDescriptorSets(_device.device, _set1_pool, 1,
                                              &slot.texture_desc_set));
  destroyTexture(_device, slot.texture);
  slot.texture = {};
  slot.texture_desc_set = VK_NULL_HANDLE;
  slot.live = false;
  slot.generation++;
  _free_prims.push_back(prim.index);
}

RenderTarget Renderer::create_offscreen_target(VkExtent2D extent,
                                               VkFormat format) {
  assert((format == VK_FORMAT_R8G8B8A8_SRGB ||
          format == VK_FORMAT_B8G8R8A8_SRGB) &&
         "read_back assumes 4 bytes per pixel");
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
                  .format = VK_FORMAT_D24_UNORM_S8_UINT,
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
    PipelineAndLayout pipeline = _pipelines.get_or_create(
        _device.device, _vk_pipeline_cache, _set0_layout, _shader, _set1_layout,
        _vertex_layout, target.format, _depth_image.format);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout, 0, 1, &_set0_sets[_frame_index], 0,
                            nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout, 1, 1, &prim.texture_desc_set, 0,
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
