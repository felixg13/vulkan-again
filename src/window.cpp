#include "window.h"
#include <cassert>

GLFWwindow *createWindow(const WindowDesc &desc) {

  glfwSetErrorCallback([](int error, const char *description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
  });

  glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);

  if (glfwInit() == 0) {
    assert(!"GLFW not initialized properly!");
  }

  if (glfwVulkanSupported() == 0) {
    assert(!"Vulkan is not supported!");
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  // glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
  // glfwWindowHint(GLFW_FLOATING, GLFW_FALSE);

  GLFWwindow *pWindow = glfwCreateWindow(desc.width, desc.height,
                                         desc.title.c_str(), nullptr, nullptr);
  if (pWindow == nullptr) {
    glfwTerminate();
    assert(!"Window creation failed!");
  }

  return pWindow;
}

void destroyWindow(GLFWwindow *pWindow) {
  glfwDestroyWindow(pWindow);
  glfwTerminate();
}
