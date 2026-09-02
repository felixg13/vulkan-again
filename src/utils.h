#pragma once

#include <spdlog/spdlog.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.hpp>

static constexpr void VK_CHECK(const VkResult &result) {
  if (result != 0) {
    SPDLOG_CRITICAL("Detected Vulkan error: {}", string_VkResult(result));
    abort();
  }
};
