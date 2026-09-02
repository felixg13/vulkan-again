#pragma once

#include <GLFW/glfw3.h>
#include <utils.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSurfaceCapabilitiesKHR surfaceCapabilities;
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

Device createDevice(GLFWwindow *window, const DeviceDesc &desc);

void destroyDevice(Device &device);
