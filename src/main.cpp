#include <device.h>
#include <renderer.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <utils.h>
#include <window.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <memory>

constexpr int MAX_FRAMES_IN_FLIGHT = 3;
uint32_t imageIndex{0};
uint32_t frameIndex{0};
constexpr size_t PRIM_COUNT = 3;
constexpr std::string APP_NAME = "Vulkan-Again";
constexpr int DEFAULT_WIDTH = 1700;
constexpr int DEFAULT_HEIGHT = 900;

VkSwapchainKHR swapchain{VK_NULL_HANDLE};
std::vector<VkImage> swapchainImages;
std::vector<VkImageView> swapchainImageViews;
double scrollOffsetY{0.0};

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

  // The instance is created inside createDevice; it hands it back through the
  // factory so the surface exists before physical-device selection.
  VkSurfaceKHR surface{VK_NULL_HANDLE};
  Device device = createDevice(deviceDesc, [&](VkInstance instance) {
    VkResult surfaceResult =
        glfwCreateWindowSurface(instance, pWindow, nullptr, &surface);
    if (surfaceResult != VK_SUCCESS) {
      assert(!"Failed to create Vulkan surface");
    }
    SPDLOG_INFO("Created Vulkan surface");
    return surface;
  });

  VkSurfaceCapabilitiesKHR surfaceCapabilities{};
  utils::validation::chk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      device.physicalDevice, surface, &surfaceCapabilities));

  // SWAPCHAIN CREATION
  const VkFormat imageFormat{VK_FORMAT_B8G8R8A8_SRGB};

  VkExtent2D swapchainExtent = surfaceCapabilities.currentExtent;
  if (swapchainExtent.width == UINT32_MAX) {
    int framebufferWidth{0};
    int framebufferHeight{0};
    glfwGetFramebufferSize(pWindow, &framebufferWidth, &framebufferHeight);
    swapchainExtent.width =
        std::clamp(static_cast<uint32_t>(framebufferWidth),
                   surfaceCapabilities.minImageExtent.width,
                   surfaceCapabilities.maxImageExtent.width);
    swapchainExtent.height =
        std::clamp(static_cast<uint32_t>(framebufferHeight),
                   surfaceCapabilities.minImageExtent.height,
                   surfaceCapabilities.maxImageExtent.height);
  }

  VkSwapchainCreateInfoKHR swapchainCI{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .pNext = nullptr,
      .surface = surface,
      .minImageCount = surfaceCapabilities.minImageCount,
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

  // RENDERER AND SCENE
  auto renderer =
      std::make_unique<Renderer>(device, shadersDir / "shader.slang");

  std::vector<Vertex> suzanneVertices{};
  std::vector<uint32_t> suzanneIndices{};
  if (!load_model(assetsDir / "suzanne.obj", suzanneVertices, suzanneIndices)) {
    return 1;
  }
  MeshHandle suzanneMesh = renderer->add_mesh(
      {.vertices = suzanneVertices, .indices = suzanneIndices});

  std::array<PrimHandle, PRIM_COUNT> prims{};
  for (size_t i = 0; i < prims.size(); i++) {
    prims[i] = renderer->add_prim(
        suzanneMesh, assetsDir / ("suzanne" + std::to_string(i) + ".ktx"),
        glm::translate(glm::mat4(1.0f),
                       glm::vec3((static_cast<float>(i) - 1.0f) * 3.0f, 0.0f,
                                 0.0f)));
  }

  // SYNC STRUCTURE (swapchain acquire / present only; render() owns the fence)
  std::vector<VkSemaphore> presentSemaphores{MAX_FRAMES_IN_FLIGHT};
  std::vector<VkSemaphore> renderSemaphores{swapchainImages.size()};
  VkSemaphoreCreateInfo semaphoreCI{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    utils::validation::chk(vkCreateSemaphore(device.device, &semaphoreCI,
                                             nullptr, &presentSemaphores[i]));
  }
  renderSemaphores.resize(swapchainImages.size());
  for (auto &semaphore : renderSemaphores) {
    utils::validation::chk(
        vkCreateSemaphore(device.device, &semaphoreCI, nullptr, &semaphore));
  }

  // RENDER LOOP
  PerFrameData perFrameData{};
  uint32_t selected_prim{1};
  glm::vec3 camPos{0.0f, 0.0f, -10.0f};
  std::array<glm::vec3, PRIM_COUNT> object_rotations{};
  double lastTime{glfwGetTime()};
  double lastMouseX{0.0};
  double lastMouseY{0.0};
  glfwGetCursorPos(pWindow, &lastMouseX, &lastMouseY);
  bool plusWasDown{false};
  bool minusWasDown{false};
  bool updateSwapchain{false};
  glfwSetScrollCallback(pWindow, [](GLFWwindow *, double, double yoffset) {
    scrollOffsetY += yoffset;
  });
  while (!glfwWindowShouldClose(pWindow)) {
    glfwPollEvents();
    // Handle input
    float elapsedTime{static_cast<float>(glfwGetTime() - lastTime)};
    lastTime = glfwGetTime();

    // Rotate the selected object with mouse drag
    double mouseX{0.0};
    double mouseY{0.0};
    glfwGetCursorPos(pWindow, &mouseX, &mouseY);
    double xrel{mouseX - lastMouseX};
    double yrel{mouseY - lastMouseY};
    lastMouseX = mouseX;
    lastMouseY = mouseY;
    if (glfwGetMouseButton(pWindow, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
      object_rotations[selected_prim].x -= (float)yrel * elapsedTime;
      object_rotations[selected_prim].y += (float)xrel * elapsedTime;
    }

    // Zooming with the mouse wheel
    camPos.z += (float)scrollOffsetY * elapsedTime * 10.0f;
    scrollOffsetY = 0.0;

    // Select active model instance
    bool plusIsDown = glfwGetKey(pWindow, GLFW_KEY_EQUAL) == GLFW_PRESS ||
                      glfwGetKey(pWindow, GLFW_KEY_KP_ADD) == GLFW_PRESS;
    if (plusIsDown && !plusWasDown) {
      selected_prim = (selected_prim < 2) ? selected_prim + 1 : 0;
    }
    plusWasDown = plusIsDown;
    bool minusIsDown = glfwGetKey(pWindow, GLFW_KEY_MINUS) == GLFW_PRESS ||
                       glfwGetKey(pWindow, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS;
    if (minusIsDown && !minusWasDown) {
      selected_prim = (selected_prim > 0) ? selected_prim - 1 : 2;
    }
    minusWasDown = minusIsDown;

    // Window resize
    int framebufferWidth{0};
    int framebufferHeight{0};
    glfwGetFramebufferSize(pWindow, &framebufferWidth, &framebufferHeight);
    if (static_cast<uint32_t>(framebufferWidth) != swapchainExtent.width ||
        static_cast<uint32_t>(framebufferHeight) != swapchainExtent.height) {
      updateSwapchain = true;
    }

    // Recreate swapchain
    if (updateSwapchain) {
      updateSwapchain = false;
      utils::validation::chk(vkDeviceWaitIdle(device.device));
      utils::validation::chk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
          device.physicalDevice, surface, &surfaceCapabilities));
      swapchainExtent = surfaceCapabilities.currentExtent;
      if (swapchainExtent.width == UINT32_MAX) {
        swapchainExtent.width =
            std::clamp(static_cast<uint32_t>(framebufferWidth),
                       surfaceCapabilities.minImageExtent.width,
                       surfaceCapabilities.maxImageExtent.width);
        swapchainExtent.height =
            std::clamp(static_cast<uint32_t>(framebufferHeight),
                       surfaceCapabilities.minImageExtent.height,
                       surfaceCapabilities.maxImageExtent.height);
      }
      swapchainCI.imageExtent = swapchainExtent;
      swapchainCI.minImageCount = surfaceCapabilities.minImageCount;
      swapchainCI.oldSwapchain = swapchain;
      utils::validation::chk(
          vkCreateSwapchainKHR(device.device, &swapchainCI, nullptr, &swapchain));

      for (auto &imageView : swapchainImageViews) {
        vkDestroyImageView(device.device, imageView, nullptr);
      }
      utils::validation::chk(vkGetSwapchainImagesKHR(device.device, swapchain,
                                                     &imageCount, nullptr));
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
        utils::validation::chk(vkCreateImageView(
            device.device, &viewCI, nullptr, &swapchainImageViews[i]));
      }

      for (auto &semaphore : renderSemaphores) {
        vkDestroySemaphore(device.device, semaphore, nullptr);
      }
      renderSemaphores.resize(imageCount);
      for (auto &semaphore : renderSemaphores) {
        utils::validation::chk(
            vkCreateSemaphore(device.device, &semaphoreCI, nullptr, &semaphore));
      }

      vkDestroySwapchainKHR(device.device, swapchainCI.oldSwapchain, nullptr);
    }
    // Acquire next image
    if (utils::validation::chkSwapchain(vkAcquireNextImageKHR(
            device.device, swapchain, UINT64_MAX,
            presentSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex))) {
      updateSwapchain = true;
      continue;
    }
    // Update per-frame data and per-prim transforms
    perFrameData.projection = glm::perspective(glm::radians(45.0f),
                                               (float)swapchainExtent.width /
                                                   (float)swapchainExtent.height,
                                               0.1f, 32.0f);
    perFrameData.view = glm::translate(glm::mat4(1.0f), camPos);
    for (size_t i = 0; i < prims.size(); i++) {
      auto instancePos =
          glm::vec3((static_cast<float>(i) - 1.0f) * 3.0f, 0.0f, 0.0f);
      renderer->set_transform(
          prims[i], glm::translate(glm::mat4(1.0f), instancePos) *
                        glm::mat4_cast(glm::quat(object_rotations[i])));
    }
    // Render (submits and waits), then present
    RenderTarget target{.image = swapchainImages[imageIndex],
                        .view = swapchainImageViews[imageIndex],
                        .format = imageFormat,
                        .extent = swapchainExtent,
                        .final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    renderer->render(target, perFrameData,
                     {.wait_semaphore = presentSemaphores[frameIndex],
                      .signal_semaphore = renderSemaphores[imageIndex]});
    frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    // Present image
    VkPresentInfoKHR presentInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                 .waitSemaphoreCount = 1,
                                 .pWaitSemaphores =
                                     &renderSemaphores[imageIndex],
                                 .swapchainCount = 1,
                                 .pSwapchains = &swapchain,
                                 .pImageIndices = &imageIndex};
    if (utils::validation::chkSwapchain(
            vkQueuePresentKHR(device.graphicsQueue, &presentInfo))) {
      updateSwapchain = true;
    }
  }

  // CLEANUP
  {
    utils::validation::chk(vkDeviceWaitIdle(device.device));
    renderer.reset();
    for (auto &semaphore : presentSemaphores) {
      vkDestroySemaphore(device.device, semaphore, nullptr);
    }
    for (auto &semaphore : renderSemaphores) {
      vkDestroySemaphore(device.device, semaphore, nullptr);
    }
    for (auto &imageView : swapchainImageViews) {
      vkDestroyImageView(device.device, imageView, nullptr);
    }
    vkDestroySwapchainKHR(device.device, swapchain, nullptr);
    vkDestroySurfaceKHR(device.instance, surface, nullptr);
    destroyDevice(device);
    destroyWindow(pWindow);
  }
  return 0;
}
