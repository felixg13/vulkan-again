#pragma once

#include <buffer.h>
#include <device.h>
#include <image.h>
#include <shader.h>

#include <MaterialXCore/Document.h>

#include <cstdint>
#include <string>
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
  // Where the per-frame mirror writes (float3 each), from reflection.
  uint32_t view_position_offset{0};
  uint32_t light_direction_offset{0};
  // File textures this material owns. The shared default image and the
  // engine sampler belong to the library.
  std::vector<Image> textures;
  // Non-owning; the PipelineCache owns both.
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
};

class Renderer;

class MaterialLibrary {
public:
  static constexpr uint32_t MAX_MATERIALS = 64;
  // Descriptor pool budget per material for SAMPLED_IMAGE and SAMPLER: the
  // two engine env textures plus file textures.
  static constexpr uint32_t MAX_TEXTURES_PER_MATERIAL = 16;

  explicit MaterialLibrary(const Device &device);

  // MaterialX -> Slang -> SPIR-V -> reflection -> set 1. `vertex_source` is
  // the engine vertex stage. Throws std::runtime_error (with a message naming
  // the cause) when the document has no renderable element, generation fails
  // or a texture cannot be loaded; nothing is allocated in that case.
  MaterialHandle compile(MaterialX::DocumentPtr document,
                         const std::string &vertex_source);
  CompiledMaterial &get(MaterialHandle handle);
  // Frees the UBO, descriptor set and textures. Asserts on a stale handle.
  void release(MaterialHandle handle);
  // Releases every live material and the shared layouts, sampler, default
  // image, pool and standard library.
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
  VkCommandPool _upload_pool{VK_NULL_HANDLE};
  VkSampler _sampler{VK_NULL_HANDLE};  // engine-wide linear/repeat
  Image _default_image{};              // 1x1 black, bound to env textures
  MaterialX::DocumentPtr _stdlib;      // loaded once, on first compile
  std::vector<Slot> _slots;
  std::vector<uint32_t> _free_slots;
  std::vector<SharedLayout> _layouts;
};
