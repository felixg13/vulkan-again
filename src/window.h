#pragma once

#include <GLFW/glfw3.h>
#include <string>

struct WindowDesc {
  int width;
  int height;
  std::string title;
};

GLFWwindow *createWindow(const WindowDesc &desc);

void destroyWindow(GLFWwindow *pWindow);
