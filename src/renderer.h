#pragma once

#include <asset_loader.h>
#include <buffer.h>
#include <device.h>
#include <image.h>
#include <material.h>
#include <pipeline_cache.h>
#include <shader.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

struct MeshHandle {
  uint32_t index;
  uint32_t generation;
};

struct PrimHandle {
  uint32_t index;
  uint32_t generation;
};

struct MeshData {
  std::span<const Vertex> vertices;
  std::span<const uint32_t> indices;
};

struct PerFrameData {
  glm::mat4 projection;
  glm::mat4 view;
  // One directional light. World-space unit vector giving the direction the
  // light TRAVELS (from the light toward the scene); w is 0 and unused.
  // render() normalizes it before it reaches the UBO. The default lights the
  // scene from above (world +y is screen-down, see load_model's y flip) and
  // slightly from the camera side (camera looks down -z).
  // render() mirrors this field into each material's `u_lightData[0].direction`
  // (MaterialX's convention is the same: the direction light travels).
  glm::vec4 light_dir{0.0f, 1.0f, -0.5f, 0.0f};
};

struct RenderTarget {
  VkImage image;
  VkImageView view;
  VkFormat format;
  VkExtent2D extent;
  VkImageLayout final_layout; // PRESENT_SRC_KHR (swapchain) or TRANSFER_SRC
};

struct RenderSync { // only the standalone swapchain path needs it
  VkSemaphore wait_semaphore = VK_NULL_HANDLE;
  VkSemaphore signal_semaphore = VK_NULL_HANDLE;
};

class Renderer {
public:
  // `device` must outlive the renderer.
  Renderer(const Device &device, const std::filesystem::path &shader_path);
  ~Renderer();
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  MeshHandle add_mesh(const MeshData &mesh);
  void release_mesh(MeshHandle mesh);

  // MaterialX document -> Slang -> SPIR-V -> reflection -> set 1; pre-warms
  // the pipeline when a target format is known. The first renderable element
  // is used. This is the Hydra boundary (hdMtlx produces the document).
  // Throws std::runtime_error with a message naming the cause when the
  // document has no renderable element, generation fails or a texture cannot
  // be loaded.
  MaterialHandle compile_material(MaterialX::DocumentPtr document);
  // Reads the .mtlx at `path` (texture paths inside it resolve relative to
  // the file) and calls compile_material. Throws std::runtime_error for an
  // unreadable or malformed file.
  MaterialHandle compile_material_from_file(const std::filesystem::path &path);
  // Frees the UBO and descriptor set. Releasing a material a live
  // prim references is a contract violation (assert).
  void release_material(MaterialHandle material);

  PrimHandle add_prim(MeshHandle mesh, MaterialHandle material,
                      const glm::mat4 &transform);
  void set_transform(PrimHandle prim, const glm::mat4 &transform);
  void remove_prim(PrimHandle prim);

  // Creates (or replaces) the renderer-owned offscreen color image used by
  // read_back(). `format` must be a 4-byte-per-pixel format.
  RenderTarget create_offscreen_target(VkExtent2D extent, VkFormat format);

  // Submits and blocks until the GPU is done.
  void render(const RenderTarget &target, const PerFrameData &frame,
              const RenderSync &sync = {});

  // Copies the offscreen target (last rendered with TRANSFER_SRC final layout)
  // into `out`, tightly packed, 4 bytes per pixel.
  void read_back(std::span<std::byte> out);

  // Number of distinct pipelines created so far (test-facing).
  size_t pipeline_count() const;

private:
  struct MeshSlot {
    Buffer vertex_buffer{};
    Buffer index_buffer{};
    uint32_t index_count{0};
    uint32_t generation{1};
    bool live{false};
  };

  struct PrimSlot {
    uint32_t mesh_index{0};
    MaterialHandle material{};
    uint32_t generation{1};
    bool live{false};
  };

  Device _device;
  std::filesystem::path _shader_path;

  VkCommandPool _command_pool{VK_NULL_HANDLE};
  VkCommandBuffer _command_buffer{VK_NULL_HANDLE};
  VkFence _fence{VK_NULL_HANDLE};
  uint32_t _frame_index{0};

  VkDescriptorSetLayout _set0_layout{VK_NULL_HANDLE};
  VkDescriptorPool _set0_pool{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> _set0_sets;
  std::vector<Buffer> _per_frame_buffers;

  std::vector<Buffer> _transform_buffers;
  std::vector<VkDeviceAddress> _transform_addresses;

  VertexLayout _vertex_layout;
  VkPipelineCache _vk_pipeline_cache{VK_NULL_HANDLE};
  PipelineCache _pipelines;
  MaterialLibrary _materials;
  // Color format of the most recent target; VK_FORMAT_UNDEFINED until one
  // exists. Material compile pre-warms its pipeline with it.
  VkFormat _known_color_format{VK_FORMAT_UNDEFINED};

  std::vector<MeshSlot> _meshes;
  std::vector<uint32_t> _free_meshes;
  std::vector<PrimSlot> _prims; // slot index == transform slot index
  std::vector<uint32_t> _free_prims;

  Image _depth_image{};
  Image _offscreen_image{};
  Buffer _readback_buffer{};
};
