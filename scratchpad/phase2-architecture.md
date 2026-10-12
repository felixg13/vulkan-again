# Phase 2 Architecture & Implementation Steps

Supersedes the previous revision. Based on `.claude/plans/phase-2-plan-resilient-toast.md`,
`phas2-breakdown.md`, and the design interview that followed. Naming follows `CLAUDE.md`
(snake_case functions, `_leading` private members).

**Phase 2 goal:** a renderer that draws three Suzannes, each with a different `.mtlx`
material, from a scene declared in C++ through an incremental API. No Hydra in this phase.
The API must be the one a Hydra delegate calls in Phase 3, so Phase 3 is registration
boilerplate.

---

## Settled Decisions

| Topic | Decision |
|---|---|
| Material paths | One. UsdPreviewSurface is just another MaterialX graph (via `hdMtlx` in Phase 3). No hand-written uber shader. |
| Material scalars | Per-material UBO in set 1; layout taken from Slang reflection. |
| Material mutability | Immutable after `compile`. A re-sync compiles a new material and returns a new handle. |
| Vertex stage | Engine-owned, hand-written. Fixed output struct `{position, normal, uv}`. Generated MaterialX code is the pixel stage only. |
| Generated-code binding | Small adapter in the pixel stage maps the engine struct to the varyings the generated code expects; modules composed at link time via Slang, never by patching generated text. |
| Vertex layout | Fixed `{pos, normal, uv}`. Importer fills defaults for missing primvars. |
| Ownership | `PipelineCache` and `Material` are members owned by `Renderer`, not statics. |
| Scene API | Incremental: `add_mesh`, `add_prim`, `set_transform`, `remove_prim`, plus explicit `release_*`. |
| Handles | Generational `{index, generation}`. Meshes/materials released explicitly; `remove_prim` never frees them. |
| Transforms | Renderer-owned. Packed per-frame ring buffer (N × mat4), reached by BDA push constant. |
| Per-frame data | Set 0 UBO, one per frame in flight. Contains projection, view, one directional light. |
| Render targets | Swapchain image (standalone) or renderer-owned offscreen color image with `read_back()` (headless test, Hydra). |
| Submission | `render()` submits and waits internally; optional `RenderSync` for the standalone semaphore chain. |
| Pipeline state | Opaque, back-face culled. Not part of the cache key. |
| Textures | PNG/JPG via stb_image, GPU mip generation, `_SRGB`/`_UNORM` from the MaterialX colorspace. KTX optional. |
| Dependencies | OpenUSD as a git submodule pinned at v26.08, built out-of-tree; MaterialX 1.39.5 taken from that install. Slang and gtest via FetchContent. |
| Acceptance | gtest headless render compared with stored reference PNGs, with tolerance. |
| `selected` GPU field | Removed. C++ `selected_prim` kept for mouse rotation. |
| `Device` | No longer owns `VkSurfaceKHR`; surface lives in `main.cpp`. |

---

## Architecture Overview

```
main.cpp  (presentation + input only)
  ├── window, surface, swapchain, swapchain views
  ├── fences / semaphores for acquire + present
  ├── object_rotations[], selected_prim   (animation / selection state)
  └── per frame: renderer.set_transform(prim, mat) ; renderer.render(target, frame, sync)

Renderer  (src/renderer.h/.cpp)  — owns everything surface-agnostic
  ├── Device handle (non-owning copy)
  ├── command pool, completion fence
  ├── set 0: per-frame UBO ring + descriptor sets + layout
  ├── transform ring (BDA)
  ├── meshes   : slot map<MeshHandle>
  ├── prims    : slot map<PrimHandle>  { mesh, material, transform slot }
  ├── PipelineCache _pipelines      (member)
  ├── MaterialLibrary _materials    (member)
  ├── depth image, optional offscreen color image
  └── API
        MeshHandle     add_mesh(const MeshData&)
        void           release_mesh(MeshHandle)
        MaterialHandle compile_material(MaterialX::DocumentPtr)        // Step 5
        MaterialHandle compile_material_from_file(path)                // Step 5
        void           release_material(MaterialHandle)
        PrimHandle     add_prim(MeshHandle, MaterialHandle, const glm::mat4&)
        void           set_transform(PrimHandle, const glm::mat4&)
        void           remove_prim(PrimHandle)
        void           render(const RenderTarget&, const PerFrameData&, const RenderSync& = {})
        void           read_back(std::span<std::byte>)                 // offscreen target

PipelineCache  (src/pipeline_cache.h/.cpp)
  ├── map<Key, VkPipeline>, map<LayoutKey, VkPipelineLayout>
  └── get_or_create(...) → VkPipeline         (owns the pipelines)

MaterialLibrary  (src/material.h/.cpp)
  ├── descriptor pool, slot map<MaterialHandle, CompiledMaterial>
  └── compile / get / release

CompiledMaterial
  ├── Shader shader                    (generated pixel module, SPIR-V + hash)
  ├── VkDescriptorSetLayout set1_layout (from Slang reflection)
  ├── VkDescriptorSet set1             (UBO + textures, written once at compile)
  ├── Buffer params_ubo
  ├── std::vector<Texture> textures
  └── VkPipeline pipeline              (non-owning; PipelineCache owns)
```

Descriptor layout (shared by all pipelines):

```
set 0 | binding 0 | UBO | PerFrameData { projection, view, light_dir }
set 1 | reflected  | per-material UBO + textures (from Slang reflection)
push  | 8 bytes    | BDA → PerDrawData { mat4 transform }, vertex stage
```

### Vertex / pixel contract

- The engine vertex module always outputs one struct: world position, normal, uv.
- The generated pixel module expects MaterialX's own varyings. A small adapter in the
  pixel stage fills them from the engine struct. It is the only code that knows both sides.
- Vertex module compiled once, composed with each generated pixel module through Slang
  composition. Generated source is never edited.
- A geometry feature mask in the pipeline key is **not** built. Add it only if a real scene
  needs another vertex stream.

---

## Step 0 — Spike (throwaway)

**Goal:** prove the MaterialX → Slang → SPIR-V → reflection chain before the plan depends
on it. Throwaway code, not merged into `src/`.

Exit criteria (all must hold):

1. MaterialX 1.39.5 from the USD install (or separately fetched, same version; see Build)
   contains `MaterialXGenSlang` and generates Slang from a flat-color `.mtlx` and a
   textured `.mtlx`.
2. Both compile through the existing `compileShader` to SPIR-V that passes validation.
3. Reflection reports the material uniform block and texture bindings at predictable
   set/binding numbers (space 1).
4. The pixel-stage adapter plus link-time composition with a hand-written vertex module
   compiles and reflects correctly; the generated pixel inputs are satisfiable from the
   engine's `{position, normal, uv}`.
5. One of the two materials renders in a trivial harness.

Failure handling: if the Slang backend is too immature, hand-write Slang for the three
Phase 2 materials and defer MaterialX codegen. The `Material` interface does not change,
so Steps 1–4 stand.

Unverified going in (from a web report, not read in source): generated uniforms are plain
`cbuffer`s with no explicit bindings; textures are `SamplerTexture2D`; entries are
`vertexMain`/`fragmentMain`. The spike confirms or corrects each.

---

## Step 1 — Scene & Render Loop Restructure

**Goal:** replace single-draw instancing with a `DrawablePrim` scene and the two-set
binding layout. Remove `selected` from the GPU path.

- New structs: `PerFrameData { projection, view, light_dir }`,
  `PerDrawData { transform }`, `DrawablePrim { prim_index, mesh_index, texture_desc_set }`.
- `shaders/shader.slang`: drop `SV_VulkanInstanceID`, `model[3]`, `selected`, `Factor`,
  `InstanceIndex`, and the bindless `textures[]`. Add set 0 UBO, set 1 `baseColor`, and
  the BDA `PerDrawData*` push constant.
- Resources: per-frame UBO + set 0 descriptor set per frame in flight; transform buffers
  per frame in flight with `SHADER_DEVICE_ADDRESS_BIT`, persistently mapped; one set 1
  descriptor set per prim from a single pool in `main.cpp`.
- Render loop: bind pipeline, bind set 0 once, then per prim bind set 1, push the BDA,
  `vkCmdDrawIndexed`.

End state: 3 Suzannes, different KTX textures, per-prim draws, no instancing, single
inline pipeline.

---

## Step 2 — Renderer Extraction with Incremental API

**Goal:** extract `Renderer` with the incremental scene API. After this step the
standalone loop calls exactly what a Hydra delegate would.

### Device decoupling

Remove `VkSurfaceKHR` and `VkSurfaceCapabilitiesKHR` from `Device`; surface creation and
capability queries move to `main.cpp`. `create_device` receives the surface for queue
family selection only. The instance/surface creation order is resolved at implementation
time.

### Types

```cpp
struct MeshHandle     { uint32_t index; uint32_t generation; };
struct MaterialHandle { uint32_t index; uint32_t generation; };
struct PrimHandle     { uint32_t index; uint32_t generation; };

struct RenderTarget {
  VkImage       image;
  VkImageView   view;
  VkFormat      format;
  VkExtent2D    extent;
  VkImageLayout final_layout;  // PRESENT_SRC_KHR (swapchain) or TRANSFER_SRC (offscreen)
};

struct RenderSync {            // only the standalone path needs it
  VkSemaphore wait_semaphore   = VK_NULL_HANDLE;
  VkSemaphore signal_semaphore = VK_NULL_HANDLE;
};
```

### Rules

- Stale handles (wrong generation) assert, never draw.
- `remove_prim` frees only the prim and its transform slot.
- `release_mesh` / `release_material` are explicit. Releasing a resource that a live prim
  references is a contract violation (assert).
- Transform slots are reused after removal; `render()` draws only live prims.
- `render()` submits and waits before returning, so released resources can be destroyed
  immediately after the call.

### Offscreen target + readback

`Renderer` can own an offscreen color image (created at a requested extent/format).
`read_back(std::span<std::byte>)` copies it to a host-visible buffer and returns pixels.
Hydra's `HdRenderBuffer` needs CPU pixel data (usdview presents through OpenGL), so this
path is the delegate's output path and the headless test's output path.

### main.cpp after this step

Window, surface, swapchain, sync objects, `object_rotations[]`, `selected_prim`, camera,
input, resize. Per frame it calls `set_transform` for each prim, then `render()`.

### Headless test (gtest)

- gtest fetched via FetchContent; `ctest` target.
- Test creates a device without a window, builds the three-Suzanne scene through the
  incremental API, renders a fixed frame offscreen, `read_back`s, and compares with a
  stored reference PNG using a per-channel tolerance.
- Reference images live in `tests/reference/`. Regenerating them is an explicit manual act.

End state: same visual output, `Renderer` runs without GLFW or a swapchain, gtest green.

---

## Step 3 — PipelineCache

**Goal:** extract pipeline creation into a member of `Renderer`, keyed on shader +
vertex layout + formats.

```cpp
struct VertexLayout {
  VkVertexInputBindingDescription                binding;
  std::vector<VkVertexInputAttributeDescription> attributes;
};

class PipelineCache {
public:
  VkPipeline get_or_create(VkDevice, VkPipelineCache, VkDescriptorSetLayout set0,
                           const Shader&, VkDescriptorSetLayout set1,
                           const VertexLayout&, VkFormat color, VkFormat depth);
  void destroy(VkDevice);
private:
  std::unordered_map<Key, VkPipeline> _pipelines;
  std::unordered_map<LayoutKey, VkPipelineLayout> _layouts;
};
```

- Key: SPIR-V hash (cached on `Shader`) + vertex stride/attributes + color/depth format.
- Pre-warm at material creation; the render loop uses the same call (cache hit).
- Always pass the `VkPipelineCache`, never `VK_NULL_HANDLE`.
- Because the vertex layout is fixed, the layout part of the key has one value; the seam
  stays without cost.

End state: all prims sharing SPIR-V yield exactly one pipeline; inline creation removed.

---

## Step 4 — Material Stub

**Goal:** `MaterialHandle` end to end through `DrawablePrim` and the renderer, with the
per-material UBO and reflected set 1, before MaterialX lands.

- `MaterialLibrary` owns the set 1 descriptor pool and the slot map.
- Stub `compile_from_image(path)`: compiles the existing shader, builds set 1 layout from
  Slang reflection, creates the params UBO and texture, writes set 1, fetches a pipeline.
  Removed in Step 5.
- `add_prim` takes a `MaterialHandle`; the render loop binds `material.set1` and
  `material.pipeline`, nothing material-specific remains in `main.cpp`.
- Shutdown order: materials, then pipeline cache.

End state: same output, `DrawablePrim` fully handle-based, gtest unchanged.

---

## Step 5 — MaterialX Integration

**Goal:** replace the stub with MaterialX → Slang → SPIR-V → reflection → set 1.

`compile(MaterialX::DocumentPtr)`:

1. MaterialX Slang shadergen emits the pixel-stage surface source.
2. Wrap with the adapter module; compose with the engine vertex module; compile to SPIR-V
   via `compileShader` (hash cached).
3. Slang reflection walks space 1 → `VkDescriptorSetLayoutBinding[]` → `set1_layout`.
4. Create the params UBO from the reflected block; load textures referenced by the
   document (stb_image, mips, format from colorspace); write set 1.
5. `PipelineCache::get_or_create` → pipeline; store `CompiledMaterial`; return handle.

`compile_from_file(path)` reads the document and calls `compile`.

Test materials (`assets/materials/`):

- `mat_clay.mtlx` — flat diffuse color, no textures
- `mat_metal.mtlx` — textured base color + roughness
- `mat_emissive.mtlx` — emissive surface

Hydra boundary (Phase 3, no renderer changes): `hdMtlx` produces a
`MaterialX::DocumentPtr`, passed to `compile_material`.

End state: 3 visually distinct materials, one pipeline per unique SPIR-V, gtest reference
images updated and green.

---

## Build & Dependencies

- OpenUSD: git submodule pinned at v26.08. A repo script runs `build_usd.py` out-of-tree
  into a gitignored install prefix (Python, tests, docs off).
- MaterialX: 1.39.5, taken from that install via `find_package(MaterialX CONFIG)`.
  Contingency: if the install lacks `MaterialXGenSlang`, fetch MaterialX 1.39.5 separately
  with FetchContent (same pinned version) for Phase 2. Decided in Step 0.
- Slang: latest release (v2026.19 at time of writing), FetchContent of the prebuilt
  archive.
- gtest: FetchContent.
- Submodule add and any commits are run by the user, per the git workflow rule.

---

## Out of Scope for Phase 2 (record, don't build)

- Transparency / blending, double-sided or cull-mode variation
- Image-based lighting; more than one directional light
- Per-mesh vertex layouts; extra vertex streams
- Live material mutation (re-sync issues a new handle)
- Vertex-stage MaterialX nodes (the vertex stage is engine-owned)
- USD scenes not authored to the renderer's capabilities

## Open Verification Items

1. Does the USD install's MaterialX include and install `MaterialXGenSlang`? (Step 0)
2. What does the generated Slang expect from the vertex stage, and does link-time
   composition work cleanly? (Step 0)
3. Instance/surface creation order for the `Device` split. (Step 2)
4. Whether `hdMtlx` builds with the minimal `build_usd.py` flags chosen. (Phase 3 Step 1)
