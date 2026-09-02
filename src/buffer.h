#pragma once

#include <cassert>
#include <device.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

struct Buffer {
  VkBuffer resource;
  VmaAllocation allocation;
  VkDeviceSize size;
  void *pMappedData = nullptr;
};

struct BufferDesc {
  VkDeviceSize size;
  VkBufferUsageFlags usageFlags = 0;
  const void *pContents = nullptr;
};

Buffer createBuffer(const Device &device, const BufferDesc &desc);

void destroyBuffer(const Device &device, Buffer &buffer);
