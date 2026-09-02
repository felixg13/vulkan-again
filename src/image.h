#pragma once

#include <device.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

struct Image {
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t mipIndex = 0U;
  uint32_t mipCount = 1U;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageView view = VK_NULL_HANDLE;
  VkImage resource = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
};

struct ImageDesc {
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t mipCount = 1U;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0U;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct TextureViewDesc {};

Image createImage(const Device &device, const ImageDesc &imageDesc);

void destroyImage(const Device &device, Image &image);
