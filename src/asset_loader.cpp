#include <asset_loader.h>
#include <numeric>
#include <tiny_obj_loader.h>

#include <cstddef>
#include <image.h>

using std::filesystem::path;

bool load_model(const path &path, std::vector<Vertex> &vertices,
                std::vector<size_t> &indices) {

  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;
  if (!tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr,
                        path.c_str())) {
    return false;
  }

  std::iota(indices.begin(), indices.end(), 0);
  for (auto &indice : shapes[0].mesh.indices) {
    size_t vertexIndex = indice.vertex_index;
    size_t normalIndex = indice.normal_index;
    size_t uvIndex = indice.texcoord_index;
    Vertex vertex{};
    vertex.pos = {attrib.vertices[vertexIndex * 3],
                  -attrib.vertices[(vertexIndex * 3) + 1],
                  attrib.vertices[(vertexIndex * 3) + 2]};
    vertex.normal = {attrib.normals[normalIndex * 3],
                     -attrib.normals[(normalIndex * 3) + 1],
                     attrib.normals[(normalIndex * 3) + 2]};
    vertex.uv = {attrib.texcoords[uvIndex * 3],
                 1.0 - attrib.texcoords[(uvIndex * 3) + 2]};
    vertices.push_back(vertex);
  }
  return true;
}

bool load_ktx(const path &path) {}
