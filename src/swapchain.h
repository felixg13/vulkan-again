#pragma once

#include <device.h>
#include <vulkan/vulkan.h>

struct Swapchain {
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent{};
  VkFormat format = VK_FORMAT_UNDEFINED;
  std::vector<Texture> textures{};
};

struct SwapchainDesc {
  VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE;
};

Swapchain createSwapchain(GLFWwindow *_pWindow, const Device &_rDevice,
                          const SwapchainDesc &_desc);

void destroySwapchain(const Device &device, Swapchain &swapchain);
