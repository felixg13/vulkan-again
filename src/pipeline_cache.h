#pragma once

#include <shader.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

struct VertexLayout {
  VkVertexInputBindingDescription binding;
  std::vector<VkVertexInputAttributeDescription> attributes;
};

struct PipelineAndLayout {
  VkPipeline pipeline;
  VkPipelineLayout layout;
};

class PipelineCache {
public:
  // Returns the cached pipeline for (shader, vertex layout, formats), creating
  // it (and the shared pipeline layout) on first use. The cache owns both.
  PipelineAndLayout get_or_create(VkDevice device, VkPipelineCache vk_cache,
                                  VkDescriptorSetLayout set0,
                                  const Shader &shader,
                                  VkDescriptorSetLayout set1,
                                  const VertexLayout &vertex_layout,
                                  VkFormat color, VkFormat depth);
  void destroy(VkDevice device);
  size_t pipeline_count() const { return _pipelines.size(); }

private:
  static constexpr size_t MAX_VERTEX_ATTRIBUTES = 8;

  struct AttributeKey {
    uint32_t location{0};
    uint32_t format{0};
    uint32_t offset{0};
    friend bool operator==(const AttributeKey &, const AttributeKey &) = default;
  };

  struct Key {
    size_t spirv_hash{0};
    uint32_t stride{0};
    std::array<AttributeKey, MAX_VERTEX_ATTRIBUTES> attributes{};
    uint32_t color_format{0};
    uint32_t depth_format{0};
    friend bool operator==(const Key &, const Key &) = default;
  };

  struct KeyHash {
    size_t operator()(const Key &key) const;
  };

  struct LayoutKey {
    VkDescriptorSetLayout set0{VK_NULL_HANDLE};
    VkDescriptorSetLayout set1{VK_NULL_HANDLE};
    friend bool operator==(const LayoutKey &, const LayoutKey &) = default;
  };

  struct LayoutKeyHash {
    size_t operator()(const LayoutKey &key) const;
  };

  std::unordered_map<Key, VkPipeline, KeyHash> _pipelines;
  std::unordered_map<LayoutKey, VkPipelineLayout, LayoutKeyHash> _layouts;
};
