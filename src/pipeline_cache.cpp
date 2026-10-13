#include <pipeline_cache.h>

#include <utils.h>

#include <cassert>
#include <functional>

size_t PipelineCache::KeyHash::operator()(const Key &key) const {
  size_t h = key.spirv_hash;
  h ^= key.stride + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  for (const AttributeKey &attribute : key.attributes) {
    h ^= attribute.location + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= attribute.format + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= attribute.offset + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  }
  h ^= key.color_format + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  h ^= key.depth_format + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

size_t PipelineCache::LayoutKeyHash::operator()(const LayoutKey &key) const {
  size_t h = std::hash<VkDescriptorSetLayout>{}(key.set0);
  h ^= std::hash<VkDescriptorSetLayout>{}(key.set1) + 0x9e3779b97f4a7c15ull +
       (h << 6) + (h >> 2);
  return h;
}

PipelineAndLayout PipelineCache::get_or_create(
    VkDevice device, VkPipelineCache vk_cache, VkDescriptorSetLayout set0,
    const Shader &shader, VkDescriptorSetLayout set1,
    const VertexLayout &vertex_layout, VkFormat color, VkFormat depth) {
  assert(vk_cache != VK_NULL_HANDLE && "a VkPipelineCache is required");
  assert(vertex_layout.attributes.size() <= MAX_VERTEX_ATTRIBUTES &&
         "vertex layout has more attributes than the cache key holds");

  // PIPELINE LAYOUT: two set layouts + the 8-byte BDA push constant
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  LayoutKey layout_key{.set0 = set0, .set1 = set1};
  if (auto found = _layouts.find(layout_key); found != _layouts.end()) {
    pipeline_layout = found->second;
  } else {
    VkPushConstantRange push_constant_range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size = sizeof(VkDeviceAddress)};
    std::array<VkDescriptorSetLayout, 2> set_layouts{set0, set1};
    VkPipelineLayoutCreateInfo pipeline_layout_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range};
    utils::validation::chk(vkCreatePipelineLayout(
        device, &pipeline_layout_ci, nullptr, &pipeline_layout));
    _layouts.emplace(layout_key, pipeline_layout);
  }

  // KEY
  Key key{.spirv_hash = shader.spirv_hash,
          .stride = vertex_layout.binding.stride,
          .color_format = static_cast<uint32_t>(color),
          .depth_format = static_cast<uint32_t>(depth)};
  for (size_t i = 0; i < vertex_layout.attributes.size(); i++) {
    key.attributes[i] = {.location = vertex_layout.attributes[i].location,
                         .format = static_cast<uint32_t>(
                             vertex_layout.attributes[i].format),
                         .offset = vertex_layout.attributes[i].offset};
  }
  if (auto found = _pipelines.find(key); found != _pipelines.end()) {
    return {.pipeline = found->second, .layout = pipeline_layout};
  }

  // PIPELINE
  VkPipelineVertexInputStateCreateInfo vertex_input_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vertex_layout.binding,
      .vertexAttributeDescriptionCount =
          static_cast<uint32_t>(vertex_layout.attributes.size()),
      .pVertexAttributeDescriptions = vertex_layout.attributes.data(),
  };
  VkPipelineInputAssemblyStateCreateInfo input_assembly_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkPipelineShaderStageCreateInfo vertex_stage_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &shader.shaderModuleCI,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = VK_NULL_HANDLE,
      .pName = "main"};
  VkPipelineShaderStageCreateInfo fragment_stage_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &shader.shaderModuleCI,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = VK_NULL_HANDLE,
      .pName = "main"};
  std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{
      vertex_stage_ci, fragment_stage_ci};
  VkPipelineViewportStateCreateInfo viewport_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1};
  std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data()};
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
  VkPipelineRenderingCreateInfo rendering_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &color,
      .depthAttachmentFormat = depth};
  VkPipelineColorBlendAttachmentState blend_attachment{.colorWriteMask = 0xF};
  VkPipelineColorBlendStateCreateInfo color_blend_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment};
  VkPipelineRasterizationStateCreateInfo rasterization_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .lineWidth = 1.0f};
  VkPipelineMultisampleStateCreateInfo multisample_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkGraphicsPipelineCreateInfo pipeline_ci{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_ci,
      .stageCount = static_cast<uint32_t>(shader_stages.size()),
      .pStages = shader_stages.data(),
      .pVertexInputState = &vertex_input_state,
      .pInputAssemblyState = &input_assembly_state,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization_state,
      .pMultisampleState = &multisample_state,
      .pDepthStencilState = &depth_stencil_state,
      .pColorBlendState = &color_blend_state,
      .pDynamicState = &dynamic_state,
      .layout = pipeline_layout};
  VkPipeline pipeline{VK_NULL_HANDLE};
  utils::validation::chk(vkCreateGraphicsPipelines(device, vk_cache, 1,
                                                   &pipeline_ci, nullptr,
                                                   &pipeline));
  _pipelines.emplace(key, pipeline);
  return {.pipeline = pipeline, .layout = pipeline_layout};
}

void PipelineCache::destroy(VkDevice device) {
  for (const auto &[key, pipeline] : _pipelines) {
    vkDestroyPipeline(device, pipeline, nullptr);
  }
  _pipelines.clear();
  for (const auto &[key, layout] : _layouts) {
    vkDestroyPipelineLayout(device, layout, nullptr);
  }
  _layouts.clear();
}
