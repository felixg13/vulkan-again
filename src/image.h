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
  VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
};
struct TextureViewDesc {};

struct SamplerDesc {
  float maxLod = 0.0f;
  float maxAnisotropy = 8.0f;
};

Image createImage(const Device &device, const ImageDesc &imageDesc);

void destroyImage(const Device &device, Image &image);

VkSampler createSampler(const Device &device, const SamplerDesc &samplerDesc);

void destroySampler(const Device &device, VkSampler &sampler);

struct Texture {
  Image image;
  VkSampler sampler = VK_NULL_HANDLE;
};

Texture createTexture(const Device &device, const ImageDesc &imageDesc,
                      const SamplerDesc &samplerDesc);

void destroyTexture(const Device &device, Texture &texture);
