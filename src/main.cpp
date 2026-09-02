#include <asset_loader.h>
#include <buffer.h>
#include <device.h>
#include <image.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <utils.h>
#include <window.h>

#include <ktx.h>
#include <ktxvulkan.h>
#include <vulkan/vulkan.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

constexpr int MAX_FRAMES_IN_FLIGHT = 3;
constexpr std::string APP_NAME = "Vulkan-Again";
constexpr int DEFAULT_WIDTH = 1700;
constexpr int DEFAULT_HEIGHT = 900;

VkCommandPool commandPool{VK_NULL_HANDLE};
std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> commandBuffers;
VkSwapchainKHR swapchain{VK_NULL_HANDLE};
std::vector<VkImage> swapchainImages;
std::vector<VkImageView> swapchainImageViews;

struct ShaderData {
  glm::mat4 projection;
  glm::mat4 view;
  glm::mat4 model[3];
  glm::vec4 lightPos{0.0f, -10.0f, 10.0f, 0.0f};
  uint32_t selected{1};
};

int main() {
  spdlog::set_pattern("[%^%l%$] %v");
  spdlog::info("Logging framework initialized successfully");

  WindowDesc windowDesc{
      .width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT, .title = APP_NAME};
  GLFWwindow *pWindow = createWindow(windowDesc);

  DeviceDesc deviceDesc{
      .applicationName = APP_NAME,
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .apiVersion = VK_API_VERSION_1_4,
  };

  Device device = createDevice(pWindow, deviceDesc);

  VkCommandPoolCreateInfo commandPoolCI{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = device.graphicsQueueFamily};

  VK_CHECK(vkCreateCommandPool(device.device, &commandPoolCI, nullptr,
                               &commandPool));

  VkCommandBufferAllocateInfo commandBuffersAllocCI{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .pNext = nullptr,
      .commandPool = commandPool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = MAX_FRAMES_IN_FLIGHT};

  VK_CHECK(vkAllocateCommandBuffers(device.device, &commandBuffersAllocCI,
                                    commandBuffers.data()));

  // SWAPCHAIN CREATION
  const VkFormat imageFormat{VK_FORMAT_B8G8R8A8_SRGB};
  VkSwapchainCreateInfoKHR swapchainCI{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .pNext = nullptr,
      .surface = device.surface,
      .minImageCount = device.surfaceCapabilities.minImageCount,
      .imageFormat = imageFormat,
      .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
      .imageExtent{.width = device.surfaceCapabilities.currentExtent.width,
                   .height = device.surfaceCapabilities.currentExtent.height},
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR};

  VK_CHECK(
      vkCreateSwapchainKHR(device.device, &swapchainCI, nullptr, &swapchain));

  uint32_t imageCount{0};
  VK_CHECK(
      vkGetSwapchainImagesKHR(device.device, swapchain, &imageCount, nullptr));
  swapchainImages.resize(imageCount);
  vkGetSwapchainImagesKHR(device.device, swapchain, &imageCount,
                          swapchainImages.data());
  swapchainImageViews.resize(imageCount);
  for (uint32_t i = 0; i < imageCount; i++) {
    VkImageViewCreateInfo viewCI{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = swapchainImages[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = imageFormat,
        .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                          .levelCount = 1,
                          .layerCount = 1}};
    VK_CHECK(vkCreateImageView(device.device, &viewCI, nullptr,
                               &swapchainImageViews[i]));
  }
  Image depthImage =
      createImage(device, {.width = DEFAULT_WIDTH,
                           .height = DEFAULT_HEIGHT,
                           .mipCount = 1,
                           .format = VK_FORMAT_D24_UNORM_S8_UINT,
                           .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           .layout = VK_IMAGE_LAYOUT_UNDEFINED});

  // MESH LOADING
  std::filesystem::path suzannePath = "../../suzanne.obj";
  std::vector<Vertex> suzanneVertices{};
  std::vector<size_t> suzanneIndices{};
  if (!load_model(suzannePath, suzanneVertices, suzanneIndices)) {
    return 1;
  }

  Buffer suzanneVertexBuffer =
      createBuffer(device, {.size = (sizeof(Vertex) * suzanneVertices.size()),
                            .usageFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .pContents = suzanneVertices.data()});

  Buffer suzanneIndicesBuffer =
      createBuffer(device, {.size = (sizeof(size_t) * suzanneIndices.size()),
                            .usageFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                            .pContents = suzanneIndices.data()});

  // SHADER DATA BUFFER
  std::vector<std::pair<Buffer, VkDeviceAddress>> shaderDataBuffers{};
  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    shaderDataBuffers[i].first = createBuffer(
        device, {.size = sizeof(ShaderData),
                 .usageFlags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT});
    VkBufferDeviceAddressInfo uBufferBdaInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = shaderDataBuffers[i].first.resource};
    shaderDataBuffers[i].second =
        vkGetBufferDeviceAddress(device.device, &uBufferBdaInfo);
  }

  // SYNC STRUCTURE
  std::vector<VkFence> fences{MAX_FRAMES_IN_FLIGHT};
  std::vector<VkSemaphore> presentSemaphores{MAX_FRAMES_IN_FLIGHT};
  std::vector<VkSemaphore> renderSemaphores{swapchainImages.size()};
  VkSemaphoreCreateInfo semaphoreCI{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fenceCI{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                            .flags = VK_FENCE_CREATE_SIGNALED_BIT};
  for (auto i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VK_CHECK(vkCreateFence(device.device, &fenceCI, nullptr, &fences[i]));
    VK_CHECK(vkCreateSemaphore(device.device, &semaphoreCI, nullptr,
                               &presentSemaphores[i]));
  }
  renderSemaphores.resize(swapchainImages.size());
  for (auto &semaphore : renderSemaphores) {
    VK_CHECK(
        vkCreateSemaphore(device.device, &semaphoreCI, nullptr, &semaphore));
  }
  // CREATE TEXTURES
  ktxTexture *ktxTexture{nullptr};
  std::filesystem::path suzanneTexPath = "../../suzanne.ktx";
  ktxTexture_CreateFromNamedFile(suzanneTexPath.c_str(),
                                 KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                 &ktxTexture);
  Image texture =
      createImage(device, {.width = ktxTexture->baseWidth,
                           .height = ktxTexture->baseHeight,
                           .mipCount = ktxTexture->numLevels,
                           .format = ktxTexture_GetVkFormat(ktxTexture),
                           .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                           .layout = VK_IMAGE_LAYOUT_UNDEFINED});

  // CLEANUP

  {
    vkDestroySwapchainKHR(device.device, swapchain, nullptr);
    destroyDevice(device);
    destroyWindow(pWindow);
  }
  return 0;
}
