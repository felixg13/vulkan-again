#pragma once

#include <ktx.h>
#include <spdlog/spdlog.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.hpp>

namespace utils::validation {

static inline void chk(VkResult result) {
  if (result != VK_SUCCESS) {
    SPDLOG_CRITICAL("Detected Vulkan error: {}", string_VkResult(result));
    abort();
  }
}

static inline void chk(KTX_error_code result) {
  if (result != KTX_SUCCESS) {
    SPDLOG_CRITICAL("Detected KTX error: {}", ktxErrorString(result));
    abort();
  }
}

// Treats VK_ERROR_OUT_OF_DATE_KHR as recoverable: returns true so the caller
// can trigger a swapchain rebuild instead of aborting. Any other error is
// still fatal. Not yet called anywhere -- wire it in once the render loop
// handles VK_ERROR_OUT_OF_DATE_KHR from vkAcquireNextImageKHR/vkQueuePresentKHR.
static inline bool chkSwapchain(VkResult result) {
  if (result < VK_SUCCESS) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      return true;
    }
    SPDLOG_CRITICAL("Detected Vulkan error: {}", string_VkResult(result));
    abort();
  }
  return false;
}

} // namespace utils::validation
