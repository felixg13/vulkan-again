#include <material.h>

#include <utils.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

MaterialLibrary::MaterialLibrary(const Device &device) : _device{device} {
  VkDescriptorPoolSize pool_size{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 .descriptorCount = MAX_MATERIALS};
  VkDescriptorPoolCreateInfo pool_ci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = MAX_MATERIALS,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size};
  utils::validation::chk(
      vkCreateDescriptorPool(_device.device, &pool_ci, nullptr, &_pool));
}

MaterialHandle MaterialLibrary::compile(const Shader &shader,
                                        const glm::vec4 &base_color) {
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

  // SET 1 LAYOUT: from reflection; reuse an existing layout with an equal
  // binding list.
  const std::vector<ReflectedBinding> &reflected =
      shader.material.set1_bindings;
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

  // PARAMS UBO: sized from reflection
  material.params_ubo = createBuffer(
      _device, {.size = shader.material.ubo_size,
                .usageFlags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT});
  material.base_color_factor_offset = shader.material.base_color_factor_offset;
  material.light_dir_offset = shader.material.light_dir_offset;
  assert(shader.material.base_color_factor_size == 4 * sizeof(float));
  assert(shader.material.light_dir_size == 4 * sizeof(float));
  std::memcpy(static_cast<std::byte *>(material.params_ubo.pMappedData) +
                  material.base_color_factor_offset,
              &base_color, sizeof(glm::vec4));

  // SET 1: the UBO at its reflected binding.
  VkDescriptorSetAllocateInfo set_alloc{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = _pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &material.set1_layout};
  utils::validation::chk(
      vkAllocateDescriptorSets(_device.device, &set_alloc, &material.set1));
  VkDescriptorBufferInfo ubo_info{.buffer = material.params_ubo.resource,
                                  .offset = 0,
                                  .range = shader.material.ubo_size};
  std::vector<VkWriteDescriptorSet> writes{};
  for (const ReflectedBinding &binding : reflected) {
    VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = material.set1,
        .dstBinding = binding.binding,
        .descriptorCount = binding.count,
        .descriptorType = binding.type};
    assert(binding.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
           binding.binding == shader.material.ubo_binding &&
           "the stub material binds only its params UBO");
    write.pBufferInfo = &ubo_info;
    writes.push_back(write);
  }
  vkUpdateDescriptorSets(_device.device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

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
  vkDestroyDescriptorPool(_device.device, _pool, nullptr);
  _pool = VK_NULL_HANDLE;
}
