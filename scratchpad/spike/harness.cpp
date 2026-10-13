// Throwaway Step 0 harness: headless render of Suzanne with engine vertex SPIR-V + composed MaterialX pixel SPIR-V.
// usage: harness vert.spv frag.spv offsets.txt values.txt out.ppm
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#define CHK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("VK error %d at %s:%d\n", r_, __FILE__, __LINE__); std::abort(); } } while (0)

static VkDevice dev; static VkPhysicalDevice phys;
static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  std::abort();
}
struct Buf { VkBuffer b; VkDeviceMemory m; void* p; VkDeviceAddress addr; };
static Buf make_buf(VkDeviceSize size, VkBufferUsageFlags usage) {
  Buf r{};
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size; ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  CHK(vkCreateBuffer(dev, &ci, nullptr, &r.b));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
  VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  CHK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
  CHK(vkBindBufferMemory(dev, r.b, r.m, 0));
  CHK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
  VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  bi.buffer = r.b; r.addr = vkGetBufferDeviceAddress(dev, &bi);
  return r;
}
struct Img { VkImage i; VkDeviceMemory m; VkImageView v; };
static Img make_img(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
  Img r{};
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  CHK(vkCreateImage(dev, &ci, nullptr, &r.i));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.i, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CHK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
  CHK(vkBindImageMemory(dev, r.i, r.m, 0));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = r.i; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {aspect, 0, 1, 0, 1};
  CHK(vkCreateImageView(dev, &vi, nullptr, &r.v));
  return r;
}
static std::vector<char> read_file(const char* p) {
  std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  const uint32_t W = 512, H = 512;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
  VkInstance inst; CHK(vkCreateInstance(&ici, nullptr, &inst));
  uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
  phys = pds[0];
  for (auto p : pds) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p, &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = p; break; } }
  { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys, &pr); std::printf("GPU: %s\n", pr.deviceName); }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  { uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr); std::vector<VkQueueFamilyProperties> q(qn); vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, q.data());
    for (uint32_t i = 0; i < qn; i++) if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qci.queueFamilyIndex = i; break; } }
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13}; f12.bufferDeviceAddress = VK_TRUE;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  CHK(vkCreateDevice(phys, &dci, nullptr, &dev));
  VkQueue queue; vkGetDeviceQueue(dev, qci.queueFamilyIndex, 0, &queue);

  // mesh
  tinyobj::attrib_t at; std::vector<tinyobj::shape_t> shapes; std::vector<tinyobj::material_t> mats; std::string warn, err;
  if (!tinyobj::LoadObj(&at, &shapes, &mats, &warn, &err, PROJECT_ASSETS "/suzanne.obj")) { std::printf("obj: %s\n", err.c_str()); return 1; }
  struct V { float p[3], n[3], uv[2]; };
  std::vector<V> verts; std::vector<uint32_t> idx;
  for (auto& s : shapes) for (auto& i : s.mesh.indices) {
    V v{}; for (int k = 0; k < 3; k++) v.p[k] = at.vertices[3 * i.vertex_index + k];
    if (i.normal_index >= 0) for (int k = 0; k < 3; k++) v.n[k] = at.normals[3 * i.normal_index + k];
    if (i.texcoord_index >= 0) { v.uv[0] = at.texcoords[2 * i.texcoord_index]; v.uv[1] = 1.0f - at.texcoords[2 * i.texcoord_index + 1]; }
    idx.push_back(uint32_t(verts.size())); verts.push_back(v);
  }
  Buf vb = make_buf(verts.size() * sizeof(V), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); std::memcpy(vb.p, verts.data(), verts.size() * sizeof(V));
  Buf ib = make_buf(idx.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT); std::memcpy(ib.p, idx.data(), idx.size() * 4);

  // per-frame + per-draw
  struct PerFrame { glm::mat4 proj, view; glm::vec4 light; };
  Buf pf = make_buf(sizeof(PerFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  PerFrame pfd{glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 32.0f), glm::translate(glm::mat4(1), glm::vec3(0, 0, -5)), glm::vec4(0, -1, 1, 0)};
  pfd.proj[1][1] *= -1.0f;
  std::memcpy(pf.p, &pfd, sizeof pfd);
  Buf draw = make_buf(sizeof(glm::mat4), 0);
  glm::mat4 model = glm::rotate(glm::mat4(1), glm::radians(25.0f), glm::vec3(0, 1, 0));
  std::memcpy(draw.p, &model, sizeof model);

  // material UBO from offsets + values
  Buf mu = make_buf(2048, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); std::memset(mu.p, 0, 2048);
  std::map<std::string, std::pair<int, int>> offs;
  { std::ifstream f(argv[3]); std::string name; int o, s; while (f >> name >> o >> s) offs[name] = {o, s}; }
  auto put = [&](const std::string& name, const std::vector<float>& fv, bool as_int = false) {
    auto it = offs.find(name); if (it == offs.end()) { std::printf("  (no uniform %s)\n", name.c_str()); return; }
    char* d = static_cast<char*>(mu.p) + it->second.first;
    for (size_t k = 0; k < fv.size() && k * 4 < size_t(it->second.second); k++) {
      if (as_int) { int32_t v = int32_t(fv[k]); std::memcpy(d + 4 * k, &v, 4); } else std::memcpy(d + 4 * k, &fv[k], 4);
    }
  };
  { std::ifstream f(argv[4]); std::string line;
    while (std::getline(f, line)) {
      std::istringstream ls(line); std::string block, name, type; ls >> block >> name >> type;
      std::string rest; std::getline(ls, rest);
      if (rest.find('-') == 1 && rest.size() == 2) continue;
      if (type == "string" || type == "filename" || type == "surfaceshader" || type == "displacementshader") continue;
      std::vector<float> fv; { std::string r2 = rest; for (auto& c : r2) if (c == ',') c = ' '; std::istringstream rs(r2); std::string tok;
        while (rs >> tok) { if (tok == "true") fv.push_back(1); else if (tok == "false") fv.push_back(0); else fv.push_back(std::stof(tok)); } }
      if (block == "LightData") continue;
      put(name, fv, type == "integer" || type == "boolean");
    } }
  put("u_viewPosition", {0, 0, 5});
  put("u_numActiveLightSources", {1}, true);
  put("u_lightData[0].type", {1}, true);
  put("u_lightData[0].direction", {0.4f, -0.5f, -0.8f});
  put("u_lightData[0].color", {1, 1, 1});
  put("u_lightData[0].intensity", {3.0f});
  put("u_envLightIntensity", {0.0f});
  put("u_envRadianceMips", {1}, true);
  put("u_envRadianceSamples", {16}, true);
  put("u_envMatrix", {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1});

  // textures: 1x1 black env x2 + 2x2 base color; uploaded via staging
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = qci.queueFamilyIndex;
  VkCommandPool pool; CHK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
  VkCommandBuffer cb; CHK(vkAllocateCommandBuffers(dev, &cbai, &cb));
  Img env0 = make_img(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  Img env1 = make_img(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  Img base = make_img(2, 2, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  Img color = make_img(W, H, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  Img depth = make_img(W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
  Buf stg = make_buf(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  uint8_t px[16] = {230, 120, 40, 255, 230, 120, 40, 255, 60, 140, 230, 255, 60, 140, 230, 255};
  std::memset(stg.p, 0, 64); std::memcpy(static_cast<char*>(stg.p) + 16, px, 16);
  Buf rb = make_buf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  CHK(vkBeginCommandBuffer(cb, &bi));
  auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1, VkImageAspectFlags asp) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask = s0; b.srcAccessMask = a0; b.dstStageMask = s1; b.dstAccessMask = a1;
    b.oldLayout = from; b.newLayout = to; b.image = img; b.subresourceRange = {asp, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b; vkCmdPipelineBarrier2(cb, &d);
  };
  Img* ups[3] = {&env0, &env1, &base}; VkBufferImageCopy regs[3] = {
    {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}},
    {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}},
    {16, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {2, 2, 1}}};
  for (int i = 0; i < 3; i++) {
    barrier(ups[i]->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdCopyBufferToImage(cb, stg.b, ups[i]->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regs[i]);
    barrier(ups[i]->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  }

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; sci.maxLod = 1.0f;
  VkSampler samp; CHK(vkCreateSampler(dev, &sci, nullptr, &samp));

  // layouts
  VkDescriptorSetLayoutBinding b0{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo l0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l0.bindingCount = 1; l0.pBindings = &b0;
  VkDescriptorSetLayout set0; CHK(vkCreateDescriptorSetLayout(dev, &l0, nullptr, &set0));
  VkDescriptorSetLayoutBinding b1[7] = {
    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, {4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, {6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo l1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l1.bindingCount = 7; l1.pBindings = b1;
  VkDescriptorSetLayout set1; CHK(vkCreateDescriptorSetLayout(dev, &l1, nullptr, &set1));
  VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_SAMPLER, 3}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 2; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
  VkDescriptorPool dpool; CHK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dpool));
  VkDescriptorSetLayout sls[2] = {set0, set1}; VkDescriptorSet sets[2];
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dpool; dai.descriptorSetCount = 2; dai.pSetLayouts = sls;
  CHK(vkAllocateDescriptorSets(dev, &dai, sets));
  VkDescriptorBufferInfo dbi0{pf.b, 0, sizeof(PerFrame)}, dbi1{mu.b, 0, 2048};
  VkDescriptorImageInfo imgs[3] = {{VK_NULL_HANDLE, env0.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, env1.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, base.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkDescriptorImageInfo smp{samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  std::vector<VkWriteDescriptorSet> w;
  { VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = sets[0]; x.dstBinding = 0; x.descriptorCount = 1; x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; x.pBufferInfo = &dbi0; w.push_back(x); }
  { VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = sets[1]; x.dstBinding = 0; x.descriptorCount = 1; x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; x.pBufferInfo = &dbi1; w.push_back(x); }
  for (uint32_t k = 0; k < 3; k++) {
    VkWriteDescriptorSet a{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; a.dstSet = sets[1]; a.dstBinding = 1 + 2 * k; a.descriptorCount = 1; a.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; a.pImageInfo = &imgs[k]; w.push_back(a);
    VkWriteDescriptorSet s{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; s.dstSet = sets[1]; s.dstBinding = 2 + 2 * k; s.descriptorCount = 1; s.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; s.pImageInfo = &smp; w.push_back(s);
  }
  vkUpdateDescriptorSets(dev, uint32_t(w.size()), w.data(), 0, nullptr);

  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 2; pli.pSetLayouts = sls; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
  VkPipelineLayout pl; CHK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));

  auto vspv = read_file(argv[1]); auto fspv = read_file(argv[2]);
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  VkShaderModule vm, fm;
  smi.codeSize = vspv.size(); smi.pCode = reinterpret_cast<const uint32_t*>(vspv.data()); CHK(vkCreateShaderModule(dev, &smi, nullptr, &vm));
  smi.codeSize = fspv.size(); smi.pCode = reinterpret_cast<const uint32_t*>(fspv.data()); CHK(vkCreateShaderModule(dev, &smi, nullptr, &fm));
  VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", nullptr},
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", nullptr}};
  VkVertexInputBindingDescription vbd{0, sizeof(V), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription vad[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
  VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vis.vertexBindingDescriptionCount = 1; vis.pVertexBindingDescriptions = &vbd; vis.vertexAttributeDescriptionCount = 3; vis.pVertexAttributeDescriptions = vad;
  VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp{0, 0, float(W), float(H), 0, 1}; VkRect2D sc{{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.cullMode = VK_CULL_MODE_BACK_BIT; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
  VkFormat cf = VK_FORMAT_R8G8B8A8_SRGB;
  VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; pri.colorAttachmentCount = 1; pri.pColorAttachmentFormats = &cf; pri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
  VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri}; gpi.stageCount = 2; gpi.pStages = st; gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias;
  gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cbs; gpi.layout = pl;
  VkPipeline pipe; CHK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));

  barrier(color.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  barrier(depth.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = color.v; ca.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue.color = {{0.02f, 0.02f, 0.08f, 1.0f}};
  VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; da.imageView = depth.v; da.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; da.clearValue.depthStencil = {1.0f, 0};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = sc; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
  vkCmdBeginRendering(cb, &ri);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 0, nullptr);
  VkDeviceSize zero = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &zero); vkCmdBindIndexBuffer(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
  vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, &draw.addr);
  vkCmdDrawIndexed(cb, uint32_t(idx.size()), 1, 0, 0, 0);
  vkCmdEndRendering(cb);
  barrier(color.i, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  VkBufferImageCopy rc{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
  vkCmdCopyImageToBuffer(cb, color.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &rc);
  CHK(vkEndCommandBuffer(cb));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  CHK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHK(vkQueueWaitIdle(queue));

  std::FILE* o = std::fopen(argv[5], "wb"); std::fprintf(o, "P6\n%u %u\n255\n", W, H);
  const uint8_t* src = static_cast<const uint8_t*>(rb.p);
  uint64_t sum = 0; uint32_t lit = 0;
  for (uint32_t i = 0; i < W * H; i++) { std::fwrite(src + 4 * i, 1, 3, o); uint32_t s = src[4*i] + src[4*i+1] + src[4*i+2]; sum += s; if (s > 60) lit++; }
  std::fclose(o);
  std::printf("rendered %ux%u, lit pixels=%u, mean=%.2f\n", W, H, lit, double(sum) / (3.0 * W * H));
  return 0;
}
