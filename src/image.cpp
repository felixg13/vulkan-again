#include <device.h>
#include <image.h>
#include <utils.h>

Image createImage(const Device &device, const ImageDesc &imageDesc) {
  Image image{};
  image.width = imageDesc.width;
  image.height = imageDesc.height;
  image.mipCount = imageDesc.mipCount;
  image.format = imageDesc.format;

  VkImageCreateInfo imageCI{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = nullptr,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = image.format,
      .extent{.width = image.width, .height = image.height, .depth = 1},
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

  utils::validation::chk(vmaCreateImage(device.allocator, &imageCI, &allocCI,
                                        &image.resource, &image.allocation,
                                        nullptr));

  VkImageViewCreateInfo viewCI{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image.resource,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = image.format,
      .subresourceRange{.aspectMask = imageDesc.aspectMask,
                        .levelCount = image.mipCount,
                        .layerCount = 1}};

  utils::validation::chk(
      vkCreateImageView(device.device, &viewCI, nullptr, &image.view));

  return image;
};

void destroyImage(const Device &device, Image &image) {
  vkDestroyImageView(device.device, image.view, nullptr);
  vmaDestroyImage(device.allocator, image.resource, image.allocation);
};

VkSampler createSampler(const Device &device, const SamplerDesc &samplerDesc) {
  VkSampler sampler{VK_NULL_HANDLE};
  VkSamplerCreateInfo samplerCI{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .anisotropyEnable = VK_TRUE,
      .maxAnisotropy = samplerDesc.maxAnisotropy,
      .maxLod = samplerDesc.maxLod,
  };
  utils::validation::chk(
      vkCreateSampler(device.device, &samplerCI, nullptr, &sampler));
  return sampler;
}

void destroySampler(const Device &device, VkSampler &sampler) {
  vkDestroySampler(device.device, sampler, nullptr);
}

Texture createTexture(const Device &device, const ImageDesc &imageDesc,
                      const SamplerDesc &samplerDesc) {
  Texture texture{};
  texture.image = createImage(device, imageDesc);
  texture.sampler = createSampler(device, samplerDesc);
  return texture;
}

void destroyTexture(const Device &device, Texture &texture) {
  destroySampler(device, texture.sampler);
  destroyImage(device, texture.image);
}
