#pragma once

#include <buffer.h>
#include <device.h>
#include <glm/glm.hpp>
#include <shader.h>

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

struct MaterialHandle {
  uint32_t index;
  uint32_t generation;
};

struct CompiledMaterial {
  Shader shader;
  VkDescriptorSetLayout set1_layout{VK_NULL_HANDLE}; // shared, library-owned
  VkDescriptorSet set1{VK_NULL_HANDLE};
  Buffer params_ubo{}; // persistently mapped, one per material
  uint32_t base_color_factor_offset{0};
  uint32_t light_dir_offset{0};
  // Non-owning; the PipelineCache owns both.
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
};

class Renderer;

class MaterialLibrary {
public:
  static constexpr uint32_t MAX_MATERIALS = 64;

  explicit MaterialLibrary(const Device &device);

  // Builds (or reuses) the set 1 layout from `shader.material`, creates the
  // params UBO with `base_color`, writes set 1.
  MaterialHandle compile(const Shader &shader, const glm::vec4 &base_color);
  CompiledMaterial &get(MaterialHandle handle);
  // Frees the UBO and descriptor set. Asserts on a stale handle.
  void release(MaterialHandle handle);
  // Releases every live material and the shared layouts and pool.
  void destroy();

private:
  friend class Renderer; // iterates live slots for the per-frame mirror

  struct Slot {
    CompiledMaterial material{};
    uint32_t generation{1};
    bool live{false};
  };

  struct SharedLayout {
    std::vector<ReflectedBinding> bindings;
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
  };

  Device _device;
  VkDescriptorPool _pool{VK_NULL_HANDLE};
  std::vector<Slot> _slots;
  std::vector<uint32_t> _free_slots;
  std::vector<SharedLayout> _layouts;
};
