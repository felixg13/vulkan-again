#include <filesystem>
#include <glm/glm.hpp>
#include <utils.h>

using std::filesystem::path;

struct Vertex {
  glm::vec3 pos;
  glm::vec3 normal;
  glm::vec2 uv;
};

bool load_model(const path &path, std::vector<Vertex> &vertices,
                std::vector<size_t> &indices);
