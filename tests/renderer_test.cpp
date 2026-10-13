#include <asset_loader.h>
#include <device.h>
#include <renderer.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace {

constexpr uint32_t WIDTH = 320;
constexpr uint32_t HEIGHT = 180;
constexpr int CHANNEL_TOLERANCE = 4;

const std::filesystem::path assets_dir =
    std::filesystem::path(PROJECT_SOURCE_DIR) / "assets";
const std::filesystem::path shaders_dir =
    std::filesystem::path(PROJECT_SOURCE_DIR) / "shaders";
const std::filesystem::path reference_path =
    std::filesystem::path(PROJECT_SOURCE_DIR) / "tests" / "reference" /
    "three_suzannes.png";

class RendererTest : public ::testing::Test {
protected:
  void SetUp() override {
    device = createDevice(DeviceDesc{});
    renderer = std::make_unique<Renderer>(device, shaders_dir / "shader.slang",
                                          VK_FORMAT_R8G8B8A8_SRGB);
    target = renderer->create_offscreen_target({WIDTH, HEIGHT},
                                               VK_FORMAT_R8G8B8A8_SRGB);

    ASSERT_TRUE(
        load_model(assets_dir / "suzanne.obj", vertices, indices));
    mesh = renderer->add_mesh({.vertices = vertices, .indices = indices});
    for (size_t i = 0; i < prims.size(); i++) {
      prim_transforms[i] =
          glm::translate(glm::mat4(1.0f),
                         glm::vec3((static_cast<float>(i) - 1.0f) * 3.0f, 0.0f,
                                   0.0f)) *
          glm::mat4_cast(glm::quat(
              glm::vec3(0.2f, 0.5f * (static_cast<float>(i) - 1.0f), 0.0f)));
      prims[i] = renderer->add_prim(
          mesh, assets_dir / ("suzanne" + std::to_string(i) + ".ktx"),
          prim_transforms[i]);
    }

    frame.projection = glm::perspective(
        glm::radians(45.0f),
        static_cast<float>(WIDTH) / static_cast<float>(HEIGHT), 0.1f, 32.0f);
    frame.view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
  }

  void TearDown() override {
    renderer.reset();
    if (device.device != VK_NULL_HANDLE) {
      destroyDevice(device);
    }
  }

  Device device{};
  std::unique_ptr<Renderer> renderer;
  RenderTarget target{};
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  MeshHandle mesh{};
  std::array<PrimHandle, 3> prims{};
  std::array<glm::mat4, 3> prim_transforms{};
  PerFrameData frame{};
};

} // namespace

TEST_F(RendererTest, ThreeSuzannesMatchReference) {
  renderer->render(target, frame);
  std::vector<std::byte> pixels(WIDTH * HEIGHT * 4);
  renderer->read_back(pixels);

  if (const char *update = std::getenv("UPDATE_REFERENCE");
      update != nullptr && std::string{update} == "1") {
    // PNG writer
    std::array<uint32_t, 256> crc_table{};
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) {
        c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      crc_table[n] = c;
    }
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> iend{};
    std::vector<uint8_t> ihdr{};
    for (uint32_t value : {WIDTH, HEIGHT}) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        ihdr.push_back(static_cast<uint8_t>(value >> shift));
      }
    }
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});

    std::vector<uint8_t> raw{};
    for (uint32_t y = 0; y < HEIGHT; y++) {
      raw.push_back(0);
      for (uint32_t x = 0; x < WIDTH * 4; x++) {
        raw.push_back(static_cast<uint8_t>(pixels[y * WIDTH * 4 + x]));
      }
    }
    std::vector<uint8_t> zlib{0x78, 0x01};
    uint32_t adler_a = 1;
    uint32_t adler_b = 0;
    for (uint8_t byte : raw) {
      adler_a = (adler_a + byte) % 65521u;
      adler_b = (adler_b + adler_a) % 65521u;
    }
    for (size_t offset = 0; offset < raw.size(); offset += 65535) {
      size_t length = std::min<size_t>(65535, raw.size() - offset);
      bool last = offset + length == raw.size();
      zlib.push_back(last ? 1 : 0);
      zlib.push_back(static_cast<uint8_t>(length & 0xFF));
      zlib.push_back(static_cast<uint8_t>(length >> 8));
      zlib.push_back(static_cast<uint8_t>(~length & 0xFF));
      zlib.push_back(static_cast<uint8_t>((~length >> 8) & 0xFF));
      zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                  raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    uint32_t adler = (adler_b << 16) | adler_a;
    for (int shift = 24; shift >= 0; shift -= 8) {
      zlib.push_back(static_cast<uint8_t>(adler >> shift));
    }
    const std::array<std::pair<const char *, const std::vector<uint8_t> *>, 3>
        chunks{{{"IHDR", &ihdr}, {"IDAT", &zlib}, {"IEND", &iend}}};
    for (const auto &[type, data] : chunks) {
      std::vector<uint8_t> chunk{};
      chunk.insert(chunk.end(), type, type + 4);
      chunk.insert(chunk.end(), data->begin(), data->end());
      uint32_t crc = 0xFFFFFFFFu;
      for (uint8_t byte : chunk) {
        crc = crc_table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
      }
      crc ^= 0xFFFFFFFFu;
      for (int shift = 24; shift >= 0; shift -= 8) {
        png.push_back(static_cast<uint8_t>(data->size() >> shift));
      }
      png.insert(png.end(), chunk.begin(), chunk.end());
      for (int shift = 24; shift >= 0; shift -= 8) {
        png.push_back(static_cast<uint8_t>(crc >> shift));
      }
    }

    std::filesystem::create_directories(reference_path.parent_path());
    std::ofstream out{reference_path, std::ios::binary};
    out.write(reinterpret_cast<const char *>(png.data()),
              static_cast<std::streamsize>(png.size()));
    ASSERT_TRUE(out.good());
    GTEST_SKIP() << "UPDATE_REFERENCE=1: wrote " << reference_path;
  }

  // PNG reader. Minimal and dependency-free: 8-bit RGBA, filter 0, stored
  // (uncompressed) deflate blocks, i.e. only what the writer above produces.
  std::ifstream in{reference_path, std::ios::binary};
  ASSERT_TRUE(in.good()) << "missing reference " << reference_path
                         << " (run once with UPDATE_REFERENCE=1)";
  std::vector<uint8_t> file{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
  ASSERT_GT(file.size(), 8u);
  struct {
    uint32_t width{0};
    uint32_t height{0};
  } reference;
  std::vector<uint8_t> idat{};
  size_t pos = 8;
  while (pos + 12 <= file.size()) {
    uint32_t length = (static_cast<uint32_t>(file[pos]) << 24) |
                      (static_cast<uint32_t>(file[pos + 1]) << 16) |
                      (static_cast<uint32_t>(file[pos + 2]) << 8) |
                      static_cast<uint32_t>(file[pos + 3]);
    std::string type{file.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                     file.begin() + static_cast<std::ptrdiff_t>(pos + 8)};
    auto data = file.begin() + static_cast<std::ptrdiff_t>(pos + 8);
    if (type == "IHDR") {
      reference.width = (static_cast<uint32_t>(data[0]) << 24) |
                        (static_cast<uint32_t>(data[1]) << 16) |
                        (static_cast<uint32_t>(data[2]) << 8) |
                        static_cast<uint32_t>(data[3]);
      reference.height = (static_cast<uint32_t>(data[4]) << 24) |
                         (static_cast<uint32_t>(data[5]) << 16) |
                         (static_cast<uint32_t>(data[6]) << 8) |
                         static_cast<uint32_t>(data[7]);
      ASSERT_EQ(data[8], 8);
      ASSERT_EQ(data[9], 6);
    } else if (type == "IDAT") {
      idat.insert(idat.end(), data, data + length);
    }
    pos += 12 + length;
  }
  ASSERT_EQ(reference.width, WIDTH);
  ASSERT_EQ(reference.height, HEIGHT);
  std::vector<uint8_t> raw{};
  size_t zpos = 2;
  bool last_block = false;
  while (!last_block) {
    ASSERT_LT(zpos + 5, idat.size());
    ASSERT_EQ(idat[zpos] & 0x06, 0)
        << "reference PNG must use stored deflate blocks (written by this test)";
    last_block = (idat[zpos] & 1) != 0;
    size_t length = idat[zpos + 1] | (static_cast<size_t>(idat[zpos + 2]) << 8);
    zpos += 5;
    ASSERT_LE(zpos + length, idat.size());
    raw.insert(raw.end(), idat.begin() + static_cast<std::ptrdiff_t>(zpos),
               idat.begin() + static_cast<std::ptrdiff_t>(zpos + length));
    zpos += length;
  }
  ASSERT_EQ(raw.size(), static_cast<size_t>(HEIGHT) * (1 + WIDTH * 4));

  int max_diff = 0;
  size_t over_tolerance = 0;
  for (uint32_t y = 0; y < HEIGHT; y++) {
    ASSERT_EQ(raw[y * (1 + WIDTH * 4)], 0) << "unsupported PNG filter";
    for (uint32_t x = 0; x < WIDTH * 4; x++) {
      int expected = raw[y * (1 + WIDTH * 4) + 1 + x];
      int actual = static_cast<uint8_t>(pixels[y * WIDTH * 4 + x]);
      int diff = std::abs(expected - actual);
      max_diff = std::max(max_diff, diff);
      if (diff > CHANNEL_TOLERANCE) {
        over_tolerance++;
      }
    }
  }
  EXPECT_EQ(over_tolerance, 0u)
      << "max per-channel difference " << max_diff << " (tolerance "
      << CHANNEL_TOLERANCE << ")";
}

TEST_F(RendererTest, RemovedPrimIsNotDrawnAndSlotIsReused) {
  std::vector<std::byte> all(WIDTH * HEIGHT * 4);
  std::vector<std::byte> without_middle(WIDTH * HEIGHT * 4);
  std::vector<std::byte> restored(WIDTH * HEIGHT * 4);

  renderer->render(target, frame);
  renderer->read_back(all);

  renderer->remove_prim(prims[1]);
  renderer->render(target, frame);
  renderer->read_back(without_middle);

  // The middle prim sits at the image center; the outer two are well clear of
  // columns [WIDTH/2 - 10, WIDTH/2 + 10).
  const uint32_t center_lo = WIDTH / 2 - 10;
  const uint32_t center_hi = WIDTH / 2 + 10;
  const uint32_t outer_margin = 100; // columns left/right of the outer prims' area
  size_t all_center_non_background = 0;
  for (uint32_t y = 0; y < HEIGHT; y++) {
    for (uint32_t x = 0; x < WIDTH; x++) {
      size_t i = (static_cast<size_t>(y) * WIDTH + x) * 4;
      bool differs_from_corner = false;
      bool differs_between_frames = false;
      for (size_t c = 0; c < 4; c++) {
        if (without_middle[i + c] != without_middle[c]) {
          differs_from_corner = true;
        }
        if (all[i + c] != without_middle[i + c]) {
          differs_between_frames = true;
        }
      }
      if (x >= center_lo && x < center_hi) {
        EXPECT_FALSE(differs_from_corner)
            << "removed prim still drawn at (" << x << ", " << y << ")";
        if (all[i] != all[0] || all[i + 1] != all[1] || all[i + 2] != all[2]) {
          all_center_non_background++;
        }
      }
      if (x < outer_margin || x >= WIDTH - outer_margin) {
        EXPECT_FALSE(differs_between_frames)
            << "remaining prim changed at (" << x << ", " << y << ")";
      }
    }
  }
  EXPECT_GT(all_center_non_background, 0u) << "middle prim was never drawn";

  PrimHandle replacement = renderer->add_prim(
      mesh, assets_dir / "suzanne1.ktx", prim_transforms[1]);
  EXPECT_EQ(replacement.index, prims[1].index);
  EXPECT_NE(replacement.generation, prims[1].generation);
  renderer->render(target, frame);
  renderer->read_back(restored);
  int max_diff = 0;
  for (size_t i = 0; i < all.size(); i++) {
    max_diff = std::max(max_diff, std::abs(static_cast<int>(all[i]) -
                                           static_cast<int>(restored[i])));
  }
  EXPECT_EQ(max_diff, 0);
}

// Stale handles are guarded with assert(), so these only die in builds
// without NDEBUG. "threadsafe" re-executes the binary, so the death test
// child never inherits a forked Vulkan driver.
TEST_F(RendererTest, StaleHandleAfterRemovePrimAsserts) {
#ifdef NDEBUG
  GTEST_SKIP() << "assert() is compiled out";
#else
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  renderer->remove_prim(prims[0]);
  EXPECT_DEATH(renderer->set_transform(prims[0], glm::mat4(1.0f)),
               "stale or invalid PrimHandle");
#endif
}

TEST_F(RendererTest, ReleaseMeshWithLivePrimAsserts) {
#ifdef NDEBUG
  GTEST_SKIP() << "assert() is compiled out";
#else
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(renderer->release_mesh(mesh), "still references this mesh");
#endif
}
