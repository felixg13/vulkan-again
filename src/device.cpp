#include <VkBootstrap.h>
#include <device.h>
#include <spdlog/spdlog.h>
#include <utils.h>

Device createDevice(GLFWwindow *window, const DeviceDesc &desc) {
  Device device{};

  vkb::InstanceBuilder builder;
  vkb::Instance instance = builder.set_app_name(desc.applicationName.c_str())
                               .request_validation_layers(true)
                               .use_default_debug_messenger()
                               .require_api_version(1, 4, 0)
                               .build()
                               .value();

  device.instance = instance;
  device.debugMessenger = instance.debug_messenger;

  VkResult surfaceResult = glfwCreateWindowSurface(device.instance, window,
                                                   nullptr, &device.surface);

  if (surfaceResult != VK_SUCCESS) {
    assert(!"Failed to create Vulkan surface");
  }
  SPDLOG_INFO("Created Vulkan surface");

  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.dynamicRendering = true;
  features13.synchronization2 = true;

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features12.bufferDeviceAddress = true;
  features12.descriptorIndexing = true;
  features12.shaderSampledImageArrayNonUniformIndexing = true;
  features12.descriptorBindingVariableDescriptorCount = true;
  features12.runtimeDescriptorArray = true;

  VkPhysicalDeviceFeatures features10{};
  features10.samplerAnisotropy = VK_TRUE;

  vkb::PhysicalDeviceSelector selector{instance};
  vkb::PhysicalDevice physicalDevice = selector.set_minimum_version(1, 4)
                                           .set_required_features_13(features13)
                                           .set_required_features_12(features12)
                                           .set_required_features(features10)
                                           .set_surface(device.surface)
                                           .select()
                                           .value();

  device.physicalDevice = physicalDevice.physical_device;

  utils::validation::chk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      device.physicalDevice, device.surface, &device.surfaceCapabilities));

  VkPhysicalDeviceProperties deviceProperties;
  vkGetPhysicalDeviceProperties(device.physicalDevice, &deviceProperties);
  SPDLOG_INFO("Selected GPU: {} (Type: {})", deviceProperties.deviceName,
              static_cast<int>(deviceProperties.deviceType));

  vkb::DeviceBuilder deviceBuilder{physicalDevice};
  vkb::Device vkbDevice = deviceBuilder.build().value();
  device.device = vkbDevice.device;
  SPDLOG_INFO("Created logical Vulkan device");

  device.graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
  device.graphicsQueueFamily =
      vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
  SPDLOG_INFO("Acquired graphics queue (family index: {})",
              device.graphicsQueueFamily);

  VmaAllocatorCreateInfo allocatorInfo = {};
  allocatorInfo.physicalDevice = device.physicalDevice;
  allocatorInfo.device = device.device;
  allocatorInfo.instance = device.instance;
  allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  utils::validation::chk(vmaCreateAllocator(&allocatorInfo, &device.allocator));
  SPDLOG_INFO("Initialized Vulkan Memory Allocator");

  return device;
}

void destroyDevice(Device &device) {
  utils::validation::chk(vkDeviceWaitIdle(device.device));

  vmaDestroyAllocator(device.allocator);

  vkDestroyDevice(device.device, nullptr);

  vkDestroySurfaceKHR(device.instance, device.surface, nullptr);

  if (device.debugMessenger != VK_NULL_HANDLE) {
    vkb::destroy_debug_utils_messenger(device.instance, device.debugMessenger);
  }

  vkDestroyInstance(device.instance, nullptr);

  SPDLOG_INFO("Destroyed device and all associated resources");

  device = {};
}
