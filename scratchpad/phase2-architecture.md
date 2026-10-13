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
| Generated-code binding | Pixel entry point (adapter) takes the engine varyings, builds `VertexData` by field name, calls the generated `fragmentMain`. Emitted per material from the `VertexData` field list. Source is concatenated (`preamble + generated + adapter`); generated text is never edited. See spike F2/F4. |
| Generated cbuffer placement | The preamble `#define cbuffer [[vk::binding(0,1)]] cbuffer` moves the block and nested resources to set 1 (spike F2). |
| View/light data | Mirrored into each material's UBO every frame (`u_viewPosition`, `u_lightData`). One persistently mapped UBO per material, no ring (`render()` submits and waits). Set 0 keeps projection, view, light for the engine vertex stage. Light is therefore stored in two places. |
| Samplers | One engine-wide linear/repeat sampler for Phase 2; per-node address/filter state is a stated limitation. Env textures declared by generated code are bound to shared 1x1 black defaults. |
| Slang version | FetchContent 2026.14 only. The Vulkan SDK's slang is not used (its `slangc` crashes on the composed module). |
| MaterialX source (Phase 2) | Standalone MaterialX 1.39.5 via FetchContent. The USD-install equivalence check is deferred to Phase 3 Step 1. |
| SPIR-V size / env cost | Measured once in Step 5 with Slang optimization on; `VkPipelineCache` persisted to disk. Env sample count untouched until it shows as a cost. |
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
        RenderTarget   create_offscreen_target(VkExtent2D, VkFormat)   // built in Step 2
        size_t         pipeline_count() const                          // test accessor (Step 3)

PipelineCache  (src/pipeline_cache.h/.cpp)   — built in Step 3
  ├── map<Key, VkPipeline>, map<LayoutKey, VkPipelineLayout>
  ├── get_or_create(...) → PipelineAndLayout { pipeline, layout }  (owns both)
  └── pipeline_count() const

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
set 1 | reflected  | per-material UBO (incl. u_viewPosition, u_lightData) +
                    SAMPLED_IMAGE / SAMPLER pairs (Slang splits SamplerTexture2D)
                    Step 4 stub: UBO { float4 base_color_factor; float4 light_dir } +
                    base color Sampler2D. light_dir is mirrored in per frame (see Step 4).
push  | 8 bytes    | BDA → PerDrawData { mat4 transform }, vertex stage
```

### Vertex / pixel contract

- The engine vertex module always outputs one struct: world position, normal, uv.
- The generated pixel module expects MaterialX's own varyings, and the `VertexData` field
  order (so `Location` numbers) differs per material. The adapter is a pixel entry point
  that takes the engine varyings, builds `VertexData` by field name, and calls the
  generated `fragmentMain(d)`. Its body is emitted per material from
  `stage.getInputBlock(HW::VERTEX_DATA)`. It is the only code that knows both sides.
- The module is built by source concatenation: `preamble + generated + adapter`, with the
  engine vertex stage in the same module. The vertex stage is therefore compiled once per
  material, not once total. Generated source is never edited. (Link-time composition via
  `createCompositeComponentType` was not attempted; the preamble must live in the
  generated module's own source anyway.)
- A geometry feature mask in the pipeline key is **not** built. Add it only if a real scene
  needs another vertex stream.

---

## Step 0 — Spike (DONE — see `step0-spike-findings.md`)

Result: criteria 1–5 passed (criterion 1 against a standalone MaterialX build, not the
USD install; that check is deferred, see Build). Findings F1–F7 are folded into the
decisions table, the vertex/pixel contract, Step 5 and the engine rules below. The text
below is kept as the original plan.

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

## Step 1 — Scene & Render Loop Restructure (DONE)

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

## Step 2 — Renderer Extraction with Incremental API (DONE)

**Goal:** extract `Renderer` with the incremental scene API. After this step the
standalone loop calls exactly what a Hydra delegate would.

### As built (deviations from the text below)

- `create_device(desc, SurfaceFactory = {})`: the instance is built first, the caller's
  factory creates the surface from it, then the physical device is selected with that
  surface. An empty factory means headless (`set_headless(true)`, no WSI extensions); the
  gtest uses this. `main.cpp` owns the surface and destroys it before `destroyDevice`.
- `create_offscreen_target(extent, format)` was added to the API (not in the original
  listing); it is how the test and the delegate obtain an offscreen target.
- **Temporary:** `add_prim` takes a KTX path. The renderer loads and uploads the texture
  and `remove_prim` destroys that texture and its set 1 descriptor set. This is a stand-in
  that **Step 4 removes**; materials own textures and are released explicitly.
- Transform slot index equals prim slot index; capacity `MAX_PRIMS = 64`, asserted.
- `set_transform` writes every ring copy (the ring is vestigial because `render()`
  blocks, but a transform set once would otherwise be missing from other slots).
- Handle checks are `assert` and are compiled out under `NDEBUG` (decided; keep).
- `light_dir` is a world-space direction of travel, `w = 0`, normalized by `render()` before
  the UBO write; the vertex stage rotates it to view space and the fragment stage uses
  `L = normalize(-light_dir_view)`. Step 5 maps it to MaterialX `u_lightData`.
- main.cpp's per-frame fences were dropped (nothing signals them since `render()` waits).
- Test render format is `R8G8B8A8_SRGB`; the app uses `B8G8R8A8_SRGB`. The test's PNG
  reader accepts only the test's own output (8-bit RGBA, stored deflate).
- `main.cpp` is no longer part of the library target; `renderer.*` is. gtest v1.17.0.

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

## Step 3 — PipelineCache (DONE)

**Goal:** extract pipeline creation into a member of `Renderer`, keyed on shader +
vertex layout + formats.

### As built (deviations from the text below)

- `get_or_create` returns `PipelineAndLayout { pipeline, layout }` because the renderer
  needs the layout to bind sets and push constants. The pipeline layout (two set layouts +
  the 8-byte vertex push constant) is created and cached in `PipelineCache`.
- `PipelineCache::pipeline_count()` was added for tests.
- `Shader` gained `size_t spirv_hash` (`std::hash<std::string_view>` over the SPIR-V).
- `Renderer` holds `Shader _shader` and `VertexLayout _vertex_layout` for its lifetime. The
  constructor no longer takes a color format; the format comes from the target at draw time.
- A real `VkPipelineCache` is created at construction, loaded from `pipeline_cache.bin` in
  the working directory if present, and saved in the destructor. A missing file is not an
  error. The file needs a `.gitignore` entry (repo root for the app, `tests/` for ctest).
- No pre-warming yet: the render loop calls `get_or_create` per prim (cache hits).
  Pre-warm arrives with Step 4 (material creation).

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

## Step 4 — Material Stub (DONE)

**Goal:** `MaterialHandle` end to end through `DrawablePrim` and the renderer, with the
per-material UBO and reflected set 1, before MaterialX lands.

### As built (deviations from the text below)

- Reflection: `getDefinedEntryPoint` -> `createCompositeComponentType` -> `link` ->
  `getLayout(0)`; parameters filtered to `DescriptorTableSlot` with `getBindingSpace() == 1`;
  UBO field offsets looked up by name via `getFieldByIndex` / `getOffset(Uniform)`; stage
  flags from `getEntryPointMetadata` + `isParameterLocationUsed`. Result stored on
  `Shader::material` (`MaterialReflection`). Reflected stub layout: UBO binding 0 (size 32,
  `base_color_factor` @0, `light_dir` @16), COMBINED_IMAGE_SAMPLER binding 1, fragment-only.
- Each `compile_material_from_image` compiles the shader (fresh Slang session); the
  `Renderer` constructor no longer compiles it. Set 1 layouts are de-duplicated by equal
  reflected binding list inside `MaterialLibrary`.
- Pre-warm uses the renderer's last-known target format (set by `create_offscreen_target`
  or `render`); otherwise the first render creates the pipeline. `render()` writes the
  fetched pipeline/layout back to the material each frame.
- `MaterialLibrary::destroy()` is explicit; `Renderer` is a friend of `MaterialLibrary`.
- SPIR-V of the stub shader: 5232 bytes. 11 tests pass against the unchanged reference.
- The mirror test asserts a pixel-difference threshold but did not print the count.

- `MaterialLibrary` (a `Renderer` member) owns the set 1 descriptor pool and a generational
  slot map of `CompiledMaterial`.
- Stub `Renderer::compile_material_from_image(path)` → `MaterialHandle`: loads and uploads
  the texture, builds the set 1 layout **from Slang reflection** (not hand-written), creates
  the params UBO, writes set 1, and calls `get_or_create` to pre-warm the pipeline. Removed
  in Step 5 (replaced by `compile_material`).
- `release_material(MaterialHandle)` is explicit. It frees the material's texture, UBO and
  descriptor set. Releasing a material a live prim references asserts.
- `add_prim(MeshHandle, MaterialHandle, mat4)`. The Step 2 KTX-path stand-in and the
  per-prim texture ownership in `remove_prim` are removed; `remove_prim` frees only the prim
  and its transform slot.
- The render loop binds `material.set1` and the material's pipeline per prim. Nothing
  material-specific remains in `main.cpp`.
- Shutdown order: materials, then pipeline cache.

### Stub material UBO (decided: rehearse the Step 5 mirror)

```
set 1 / binding 0 :  UBO { float4 base_color_factor;   // default (1,1,1,1)
                           float4 light_dir; }          // mirrored in every frame
set 1 / binding N :  base color texture (as reflected)
```

- One persistently mapped UBO per material, no ring (`render()` submits and waits).
- Reflection supplies the byte offsets of `base_color_factor` and `light_dir`; the
  material records them. Names are looked up in reflection, not hard-coded offsets.
- Each frame `render()` writes the normalized world-space `light_dir` into **every live
  material's** UBO at the recorded offset before drawing. This is the exact mechanism
  Step 5 uses for `u_lightData` / `u_viewPosition` (spike F1), exercised here with the stub
  shader so a failure points at the engine and not the generator.
- The fragment stage reads `light_dir` from the material UBO and rotates it to view space
  with the view matrix from set 0 (the interpolated normal is in view space). Set 0 is
  currently visible to the vertex stage only, so its descriptor set layout binding stage
  flags widen to `VERTEX | FRAGMENT` in this step. The `LightDir` varying and the
  vertex-stage rotation of the light are removed.
- The view matrix stays on set 0; only the light moved. Step 5 adds `u_viewPosition` the
  same way.

End state: same visual output (the look must not change), `DrawablePrim` fully
handle-based, the 6 existing tests still pass against the unchanged reference image, plus
new material-lifetime and mirror tests.

---

## Step 5 — MaterialX Integration

**Goal:** replace the stub with MaterialX → Slang → SPIR-V → reflection → set 1.

### Decisions for this step

- **Surface node:** `standard_surface` for all three materials. Test images are 256x256
  procedural PNGs generated by the agent into `assets/` (checker for base color, gradient
  for roughness). Real textures can replace them later without code changes.
- **KTX path removed:** `compile_material_from_image`, the KTX upload in the renderer and
  its call sites are removed. `asset_loader`'s KTX loader stays untouched and unused.
- **Test scene:** the three-Suzanne scene is rebuilt from the three `.mtlx` files with a NEW
  reference image. The agent generates it and stops; **the user approves it by eye** before
  anything is committed.
- **World-space varyings:** generated code expects world-space `normalWorld` /
  `positionWorld` and a world-space `u_viewPosition`. The engine vertex stage therefore
  outputs world position and world normal (see `scratchpad/spike/engine_vertex.slang`).
  The Step 4 view-space Phong path goes away with the stub shader.
- **Data library:** MaterialX's `libraries/` directory is found at runtime through a
  compile definition pointing at the fetched source tree.
- **Generator settings (from the spike):** `targetColorSpaceOverride = "lin_rec709"`,
  `hwMaxActiveLightSources = 1`, `hwDirectionalAlbedoMethod = DIRECTIONAL_ALBEDO_ANALYTIC`,
  `bindLightShader(ND_directional_light, 1, ctx)`, default specular environment method.
- **Light convention check:** confirm in the generated code that `u_lightData[0].direction`
  is the direction light travels (the engine's `light_dir` convention). If it is the
  opposite, negate at the mirror write and record it here, not in the engine convention.

`compile(MaterialX::DocumentPtr)`:

1. MaterialX Slang shadergen emits the pixel-stage surface source. Generator context:
   `hwMaxActiveLightSources = 1`, `bindLightShader(ND_directional_light, 1, ctx)`, default
   `hwSpecularEnvironmentMethod`.
2. Build the module by concatenation: preamble (`#define cbuffer ...`) + generated +
   per-material adapter + engine vertex stage; compile to SPIR-V via `compileShader`
   (hash cached) with Slang optimization enabled.
3. Slang reflection walks space 1 → `VkDescriptorSetLayoutBinding[]` → `set1_layout`
   (UBO, then `SAMPLED_IMAGE` + `SAMPLER` pairs).
4. Create the params UBO from the reflected block (persistently mapped, one per material);
   record the reflected offsets of `u_viewPosition` and `u_lightData`. Load textures
   referenced by the document (stb_image, mips, format from colorspace). Bind the engine
   sampler to every reflected sampler; bind 1x1 black defaults to the env textures. Write
   set 1.
5. `PipelineCache::get_or_create` → pipeline; store `CompiledMaterial`; return handle.

Per frame, `render()` writes the camera position and the directional light into each live
material's UBO at the recorded offsets, then draws.

Measure in this step: SPIR-V size and pipeline creation time for the three materials
(spike saw ~750 KB unoptimized each). Persist the `VkPipelineCache` to disk.

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
  into a gitignored install prefix (Python, tests, docs off). Phase 2 uses no USD code.
- MaterialX: **Phase 2 uses a standalone MaterialX 1.39.5 via FetchContent**
  (`MATERIALX_BUILD_GEN_SLANG=ON`, tests/viewer/python/render off), the Q22 contingency.
  The Step 0 spike validated exactly this. Whether the USD install contains and installs
  `MaterialXGenSlang` is checked as the first task of Phase 3 Step 1; if it fails,
  Phase 3 keeps the standalone fetch with no Phase 2 rework.
- Slang: **2026.14**, FetchContent of the prebuilt archive as already pinned in
  `external/CMakeLists.txt`. The Makefile's `-I/-L/-lslang/-rpath` flags pointing at the
  Vulkan SDK must be removed so the fetched library is the one loaded at runtime. Verify
  with `ldd` on a built binary.
- gtest: FetchContent.
- Submodule add and any commits are run by the user, per the git workflow rule.

---

## Engine Rules from the Spike (re-test on any MaterialX or Slang bump)

- **Strings:** `string`-typed ports (`tex_base_layer`, `tex_base_framerange`) have no value
  but are declared `int` in the cbuffer. Leave them zero. (F5)
- **Env method:** keep the default `hwSpecularEnvironmentMethod`.
  `SPECULAR_ENVIRONMENT_NONE` generates uncompilable Slang (`u_refractionTwoSided` dropped
  but still referenced). (F6)
- **Env textures:** always declared, even with no image nodes. Always bind them. (F3)
- **cbuffer placement:** `-fvk-bind-globals` does not apply; the `#define cbuffer`
  preamble is the mechanism. (F2)
- **Slang binary:** the Vulkan SDK 1.4.321.1 `slangc` segfaults on the composed module;
  do not use it. (F2/Step 0)

## Out of Scope for Phase 2 (record, don't build)

- Transparency / blending, double-sided or cull-mode variation
- Image-based lighting; more than one directional light
- Per-mesh vertex layouts; extra vertex streams
- Live material mutation (re-sync issues a new handle)
- Vertex-stage MaterialX nodes (the vertex stage is engine-owned)
- USD scenes not authored to the renderer's capabilities

## Open Verification Items

1. Does the USD install's MaterialX include and install `MaterialXGenSlang`?
   (Phase 3 Step 1, first task)
2. Instance/surface creation order for the `Device` split. (Step 2)
3. Whether `hdMtlx` builds with the minimal `build_usd.py` flags chosen. (Phase 3 Step 1)
4. Which `libslang` the built binary actually loads after the Makefile change. (Step 1)
5. SPIR-V size and pipeline creation time with Slang optimization on. (Step 5)

Resolved by the spike: generated vertex-stage expectations (F4) and composition (F2/F4).
