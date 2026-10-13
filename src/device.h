#pragma once

#include <utils.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

#include <functional>

struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VmaAllocator allocator = VK_NULL_HANDLE;
};

struct DeviceDesc {
  const std::string applicationName = "Vulkan-again";
  uint32_t applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  uint32_t apiVersion = VK_API_VERSION_1_4;
};

// Called with the freshly created instance; returns the surface used for
// queue-family selection. The caller keeps ownership of the surface and must
// destroy it before destroyDevice(). An empty factory creates a headless
// device (no surface, no WSI instance extensions).
using SurfaceFactory = std::function<VkSurfaceKHR(VkInstance)>;

Device createDevice(const DeviceDesc &desc, const SurfaceFactory &create_surface = {});

void destroyDevice(Device &device);
