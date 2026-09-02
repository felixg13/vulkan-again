#include <device.h>
#include <image.h>
#include <utils.h>

Image createImage(const Device &device, const ImageDesc &imageDesc) {
  Image image{};
  image.width = imageDesc.width;
  image.height = imageDesc.width;
  image.mipCount = imageDesc.mipCount;
  image.format = imageDesc.format;

  VkImageCreateInfo imageCI{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = nullptr,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = image.format,
      .extent{.width = image.width, .height = image.width, .depth = 1},
      .mipLevels = image.mipCount,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = imageDesc.usage,
      .initialLayout = imageDesc.layout,
  };

  VmaAllocationCreateInfo allocCI{
      .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO};

  vmaCreateImage(device.allocator, &imageCI, &allocCI, &image.resource,
                 &image.allocation, nullptr);

  VkImageViewCreateInfo depthViewCI{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image.resource,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = image.format,
      .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                        .levelCount = 1,
                        .layerCount = 1}};

  VK_CHECK(
      vkCreateImageView(device.device, &depthViewCI, nullptr, &image.view));

  return image;
};

void destroyImage(const Device &device, Image &image) {
  vkDestroyImageView(device.device, image.view, nullptr);
  vmaDestroyImage(device.allocator, image.resource, image.allocation);
};
