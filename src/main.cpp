#include <asset_loader.h>
#include <buffer.h>
#include <device.h>
#include <image.h>
#include <shader.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <utils.h>
#include <window.h>

#include <ktx.h>
#include <ktxvulkan.h>
#include <vulkan/vulkan.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>

constexpr int MAX_FRAMES_IN_FLIGHT = 3;
constexpr size_t TEXTURE_COUNT = 3;
constexpr std::string APP_NAME = "Vulkan-Again";
constexpr int DEFAULT_WIDTH = 1700;
constexpr int DEFAULT_HEIGHT = 900;

VkCommandPool commandPool{VK_NULL_HANDLE};
std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> commandBuffers;
VkSwapchainKHR swapchain{VK_NULL_HANDLE};
VkDescriptorSetLayout descriptorSetLayoutTex{VK_NULL_HANDLE};
VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
VkDescriptorSet descriptorSetTex{VK_NULL_HANDLE};
VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
VkPipeline pipeline{VK_NULL_HANDLE};
std::vector<VkImage> swapchainImages;
std::vector<VkImageView> swapchainImageViews;
std::array<Texture, TEXTURE_COUNT> textures{};

struct ShaderData {
  glm::mat4 projection;
  glm::mat4 view;
  glm::mat4 model[3];
  glm::vec4 lightPos{0.0f, -10.0f, 10.0f, 0.0f};
  uint32_t selected{1};
};

const std::filesystem::path assetsDir =
    std::filesystem::path(PROJECT_SOURCE_DIR) / "assets";
const std::filesystem::path shadersDir =
    std::filesystem::path(PROJECT_SOURCE_DIR) / "shaders";

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

  utils::validation::chk(vkCreateCommandPool(device.device, &commandPoolCI,
                                             nullptr, &commandPool));

  VkCommandBufferAllocateInfo commandBuffersAllocCI{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .pNext = nullptr,
      .commandPool = commandPool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = MAX_FRAMES_IN_FLIGHT};

  utils::validation::chk(vkAllocateCommandBuffers(
      device.device, &commandBuffersAllocCI, commandBuffers.data()));

  // SWAPCHAIN CREATION
  const VkFormat imageFormat{VK_FORMAT_B8G8R8A8_SRGB};

  VkExtent2D swapchainExtent = device.surfaceCapabilities.currentExtent;
  if (swapchainExtent.width == UINT32_MAX) {
    int framebufferWidth{0};
    int framebufferHeight{0};
    glfwGetFramebufferSize(pWindow, &framebufferWidth, &framebufferHeight);
    swapchainExtent.width =
        std::clamp(static_cast<uint32_t>(framebufferWidth),
                   device.surfaceCapabilities.minImageExtent.width,
                   device.surfaceCapabilities.maxImageExtent.width);
    swapchainExtent.height =
        std::clamp(static_cast<uint32_t>(framebufferHeight),
                   device.surfaceCapabilities.minImageExtent.height,
                   device.surfaceCapabilities.maxImageExtent.height);
  }

  VkSwapchainCreateInfoKHR swapchainCI{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .pNext = nullptr,
      .surface = device.surface,
      .minImageCount = device.surfaceCapabilities.minImageCount,
      .imageFormat = imageFormat,
      .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = swapchainExtent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR};

  utils::validation::chk(
      vkCreateSwapchainKHR(device.device, &swapchainCI, nullptr, &swapchain));

  uint32_t imageCount{0};
  utils::validation::chk(
      vkGetSwapchainImagesKHR(device.device, swapchain, &imageCount, nullptr));
  swapchainImages.resize(imageCount);
  utils::validation::chk(vkGetSwapchainImagesKHR(
      device.device, swapchain, &imageCount, swapchainImages.data()));
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
    utils::validation::chk(vkCreateImageView(device.device, &viewCI, nullptr,
                                             &swapchainImageViews[i]));
  }
  Image depthImage =
      createImage(device, {.width = DEFAULT_WIDTH,
                           .height = DEFAULT_HEIGHT,
                           .mipCount = 1,
                           .format = VK_FORMAT_D24_UNORM_S8_UINT,
                           .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           .layout = VK_IMAGE_LAYOUT_UNDEFINED,
                           .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT});

  // MESH LOADING
  std::vector<Vertex> suzanneVertices{};
  std::vector<uint32_t> suzanneIndices{};
  if (!load_model(assetsDir / "suzanne.obj", suzanneVertices, suzanneIndices)) {
    return 1;
  }

  Buffer suzanneVertexBuffer =
      createBuffer(device, {.size = (sizeof(Vertex) * suzanneVertices.size()),
                            .usageFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .pContents = suzanneVertices.data()});

  Buffer suzanneIndicesBuffer =
      createBuffer(device, {.size = (sizeof(uint32_t) * suzanneIndices.size()),
                            .usageFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                            .pContents = suzanneIndices.data()});

  // SHADER LOADING
  ShaderCompiler shaderCompiler = createShaderCompiler();
  Shader shader =
      compileShader(shaderCompiler, "shader", shadersDir / "shader.slang");

  // SHADER DATA BUFFER
  std::vector<std::pair<Buffer, VkDeviceAddress>> shaderDataBuffers(
      MAX_FRAMES_IN_FLIGHT);
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
    utils::validation::chk(
        vkCreateFence(device.device, &fenceCI, nullptr, &fences[i]));
    utils::validation::chk(vkCreateSemaphore(device.device, &semaphoreCI,
                                             nullptr, &presentSemaphores[i]));
  }
  renderSemaphores.resize(swapchainImages.size());
  for (auto &semaphore : renderSemaphores) {
    utils::validation::chk(
        vkCreateSemaphore(device.device, &semaphoreCI, nullptr, &semaphore));
  }
  // CREATE TEXTURES
  std::vector<VkDescriptorImageInfo> textureDescriptors{};
  for (size_t i = 0; i < textures.size(); i++) {
    ktxTexture *pKtxTexture{nullptr};
    std::filesystem::path texturePath =
        assetsDir / ("suzanne" + std::to_string(i) + ".ktx");
    utils::validation::chk(ktxTexture_CreateFromNamedFile(
        texturePath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &pKtxTexture));

    textures[i] = createTexture(
        device,
        {.width = pKtxTexture->baseWidth,
         .height = pKtxTexture->baseHeight,
         .mipCount = pKtxTexture->numLevels,
         .format = ktxTexture_GetVkFormat(pKtxTexture),
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         .layout = VK_IMAGE_LAYOUT_UNDEFINED,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT},
        {.maxLod = static_cast<float>(pKtxTexture->numLevels)});

    Buffer stagingBuffer = createBuffer(
        device, {.size = static_cast<VkDeviceSize>(pKtxTexture->dataSize),
                 .usageFlags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 .pContents = pKtxTexture->pData});

    VkCommandBufferAllocateInfo uploadCbAllocCI{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer uploadCb{VK_NULL_HANDLE};
    utils::validation::chk(
        vkAllocateCommandBuffers(device.device, &uploadCbAllocCI, &uploadCb));

    VkCommandBufferBeginInfo uploadCbBI{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    utils::validation::chk(vkBeginCommandBuffer(uploadCb, &uploadCbBI));

    VkImageMemoryBarrier2 toTransferDst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = textures[i].image.resource,
        .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                          .levelCount = textures[i].image.mipCount,
                          .layerCount = 1}};
    VkDependencyInfo uploadBarrierDep{.sType =
                                          VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &toTransferDst};
    vkCmdPipelineBarrier2(uploadCb, &uploadBarrierDep);

    std::vector<VkBufferImageCopy> copyRegions{};
    for (uint32_t level = 0; level < pKtxTexture->numLevels; level++) {
      ktx_size_t mipOffset{0};
      utils::validation::chk(
          ktxTexture_GetImageOffset(pKtxTexture, level, 0, 0, &mipOffset));
      copyRegions.push_back(
          {.bufferOffset = mipOffset,
           .imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = level,
                             .layerCount = 1},
           .imageExtent{.width = pKtxTexture->baseWidth >> level,
                        .height = pKtxTexture->baseHeight >> level,
                        .depth = 1}});
    }
    vkCmdCopyBufferToImage(
        uploadCb, stagingBuffer.resource, textures[i].image.resource,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

    VkImageMemoryBarrier2 toReadOnly{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .image = textures[i].image.resource,
        .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                          .levelCount = textures[i].image.mipCount,
                          .layerCount = 1}};
    uploadBarrierDep.pImageMemoryBarriers = &toReadOnly;
    vkCmdPipelineBarrier2(uploadCb, &uploadBarrierDep);

    utils::validation::chk(vkEndCommandBuffer(uploadCb));

    VkFenceCreateInfo uploadFenceCI{.sType =
                                        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence uploadFence{VK_NULL_HANDLE};
    utils::validation::chk(
        vkCreateFence(device.device, &uploadFenceCI, nullptr, &uploadFence));

    VkCommandBufferSubmitInfo uploadCbSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = uploadCb};

    VkSubmitInfo2 uploadSubmitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &uploadCbSubmitInfo};
    utils::validation::chk(vkQueueSubmit2(device.graphicsQueue, 1,
                                          &uploadSubmitInfo, uploadFence));
    utils::validation::chk(
        vkWaitForFences(device.device, 1, &uploadFence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(device.device, uploadFence, nullptr);
    vkFreeCommandBuffers(device.device, commandPool, 1, &uploadCb);
    destroyBuffer(device, stagingBuffer);

    textureDescriptors.push_back(
        {.sampler = textures[i].sampler,
         .imageView = textures[i].image.view,
         .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL});

    ktxTexture_Destroy(pKtxTexture);
  }

  {
    VkDescriptorBindingFlags descVariableFlag{
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo descBindingFlags{
        .sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1,
        .pBindingFlags = &descVariableFlag};
    VkDescriptorSetLayoutBinding descLayoutBindingTex{
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = static_cast<uint32_t>(textures.size()),
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo descLayoutTexCI{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &descBindingFlags,
        .bindingCount = 1,
        .pBindings = &descLayoutBindingTex};
    utils::validation::chk(vkCreateDescriptorSetLayout(
        device.device, &descLayoutTexCI, nullptr, &descriptorSetLayoutTex));
    VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = static_cast<uint32_t>(textures.size())};
    VkDescriptorPoolCreateInfo descPoolCI{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize};
    utils::validation::chk(vkCreateDescriptorPool(device.device, &descPoolCI,
                                                  nullptr, &descriptorPool));
    uint32_t variableDescCount{static_cast<uint32_t>(textures.size())};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI{
        .sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &variableDescCount};
    VkDescriptorSetAllocateInfo texDescSetAlloc{

        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableDescCountAI,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptorSetLayoutTex};
    utils::validation::chk(vkAllocateDescriptorSets(
        device.device, &texDescSetAlloc, &descriptorSetTex));
    VkWriteDescriptorSet writeDescSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSetTex,
        .dstBinding = 0,
        .descriptorCount = static_cast<uint32_t>(textureDescriptors.size()),
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = textureDescriptors.data()};
    vkUpdateDescriptorSets(device.device, 1, &writeDescSet, 0, nullptr);
  }
  // PIPELINE
  VkPushConstantRange pushConstantRange{.stageFlags =
                                            VK_SHADER_STAGE_VERTEX_BIT,
                                        .size = sizeof(VkDeviceAddress)};
  VkPipelineLayoutCreateInfo pipelineLayoutCI{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &descriptorSetLayoutTex,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pushConstantRange};
  utils::validation::chk(vkCreatePipelineLayout(
      device.device, &pipelineLayoutCI, nullptr, &pipelineLayout));
  VkVertexInputBindingDescription vertexBinding{
      .binding = 0,
      .stride = sizeof(Vertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  std::vector<VkVertexInputAttributeDescription> vertexAttributes{
      {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT},
      {.location = 1,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(Vertex, normal)},
      {.location = 2,
       .binding = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(Vertex, uv)},
  };
  VkPipelineVertexInputStateCreateInfo vertexInputState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vertexBinding,
      .vertexAttributeDescriptionCount =
          static_cast<uint32_t>(vertexAttributes.size()),
      .pVertexAttributeDescriptions = vertexAttributes.data(),
  };
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};

  VkPipelineShaderStageCreateInfo vertexStageCI{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &shader.shaderModuleCI,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = VK_NULL_HANDLE,
      .pName = "main"};
  VkPipelineShaderStageCreateInfo fragmentStageCI{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &shader.shaderModuleCI,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = VK_NULL_HANDLE,
      .pName = "main"};
  std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{vertexStageCI,
                                                              fragmentStageCI};

  VkPipelineViewportStateCreateInfo viewportState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1};
  std::vector<VkDynamicState> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamicStates.data()};
  VkPipelineDepthStencilStateCreateInfo depthStencilState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
  VkPipelineRenderingCreateInfo renderingCI{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &imageFormat,
      .depthAttachmentFormat = depthImage.format};
  VkPipelineColorBlendAttachmentState blendAttachment{.colorWriteMask = 0xF};
  VkPipelineColorBlendStateCreateInfo colorBlendState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blendAttachment};
  VkPipelineRasterizationStateCreateInfo rasterizationState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .lineWidth = 1.0f};
  VkPipelineMultisampleStateCreateInfo multisampleState{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkGraphicsPipelineCreateInfo pipelineCI{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &renderingCI,
      .stageCount = 2,
      .pStages = shaderStages.data(),
      .pVertexInputState = &vertexInputState,
      .pInputAssemblyState = &inputAssemblyState,
      .pViewportState = &viewportState,
      .pRasterizationState = &rasterizationState,
      .pMultisampleState = &multisampleState,
      .pDepthStencilState = &depthStencilState,
      .pColorBlendState = &colorBlendState,
      .pDynamicState = &dynamicState,
      .layout = pipelineLayout};
  utils::validation::chk(vkCreateGraphicsPipelines(
      device.device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &pipeline));
  // CLEANUP

  {
    destroyShaderCompiler(shaderCompiler);
    vkDestroyDescriptorPool(device.device, descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device.device, descriptorSetLayoutTex,
                                 nullptr);
    for (auto &texture : textures) {
      destroyTexture(device, texture);
    }
    for (auto &shaderDataBuffer : shaderDataBuffers) {
      destroyBuffer(device, shaderDataBuffer.first);
    }
    destroyBuffer(device, suzanneIndicesBuffer);
    destroyBuffer(device, suzanneVertexBuffer);
    destroyImage(device, depthImage);
    for (auto &fence : fences) {
      vkDestroyFence(device.device, fence, nullptr);
    }
    for (auto &semaphore : presentSemaphores) {
      vkDestroySemaphore(device.device, semaphore, nullptr);
    }
    for (auto &semaphore : renderSemaphores) {
      vkDestroySemaphore(device.device, semaphore, nullptr);
    }
    for (auto &imageView : swapchainImageViews) {
      vkDestroyImageView(device.device, imageView, nullptr);
    }
    vkDestroyCommandPool(device.device, commandPool, nullptr);
    vkDestroySwapchainKHR(device.device, swapchain, nullptr);
    destroyDevice(device);
    destroyWindow(pWindow);
  }
  return 0;
}
