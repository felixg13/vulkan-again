#include <buffer.h>
#include <device.h>

Buffer createBuffer(const Device &device, const BufferDesc &bufferDesc) {

  Buffer buffer{};
  VkBufferCreateInfo bufferCI{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = bufferDesc.size,
                              .usage = bufferDesc.usageFlags};
  VmaAllocationCreateInfo bufferAllocCI{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO};
  utils::validation::chk(vmaCreateBuffer(device.allocator, &bufferCI,
                                         &bufferAllocCI, &buffer.resource,
                                         &buffer.allocation, nullptr));
  utils::validation::chk(vmaMapMemory(device.allocator, buffer.allocation,
                                      &buffer.pMappedData));
  if (bufferDesc.pContents != nullptr) {
    memcpy(buffer.pMappedData, bufferDesc.pContents, bufferDesc.size);
  }
  vmaUnmapMemory(device.allocator, buffer.allocation);

  return buffer;
}

void destroyBuffer(const Device &device, Buffer &buffer) {
  vmaDestroyBuffer(device.allocator, buffer.resource, buffer.allocation);
}
