# Phase 2 — Generalized Renderer Architecture

## Context

Phase 1 produced a working Vulkan 1.4 renderer with a single pipeline, single Slang shader, and
three hard-coded Suzanne instances rendered in one draw call. Everything lives in `main.cpp`.
Phase 2 generalizes this so that meshes and materials can be defined in C++ without hard-coding —
the immediate goal being 3 Suzannes each with a different `.mtlx` material file. Phase 3 will
connect this to Hydra; the architecture must make that connection trivial without a second refactor.

---

## Analysis of the Two Core Questions

### Q1 — Is one pipeline realistic for the Hydra end goal?

**Short answer: one pipeline *object* is not realistic with MaterialX; one pipeline *template* is.**

MaterialX shadergen targeting Slang produces different shader source code for different material
networks (different node graph topologies → different fragment programs → different SPIR-V). That
means N pipeline objects — one per unique compiled material.

What CAN be fixed (the template):
- Vertex input layout: `{pos, normal, uv}` — this is the mesh contract for all prims
- Push constant: 8-byte BDA pointing to per-draw data (transform + material ref)
- Descriptor set structure: set 0 = per-frame globals (camera, light), set 1 = per-material params
- Render target format: fixed for the swap chain

What varies per material:
- Fragment SPIR-V (and possibly vertex SPIR-V for future custom vertex logic)
- Descriptor set layout for set 1 (driven by Slang reflection of the generated shader)

The right primitive is a `PipelineCache` keyed on a hash of the material's compiled shader.
For Hydra: each unique `HdMaterial` compiles to one cache entry. This maps cleanly.

**Pipeline creation can absolutely be abstracted into its own class.** The current 100-line block in
`main.cpp` (lines 387–487) is already self-contained. It needs only: vertex layout, shader SPIR-V
blobs, descriptor set layout(s), push constant range, and render target format.

---

### Q2 — Slang reflection vs MaterialX ShaderGraph? hdMtlx vs MaterialX directly?

**Slang reflection alone is sufficient — no need to separately walk the MaterialX ShaderGraph.**

The pipeline:
```
.mtlx file
  → MaterialX shadergen (Slang backend)
  → generated .slang source
  → compile via existing Shader abstraction (slang ISession)
  → Slang reflection API (ShaderReflection, TypeLayoutReflection)
  → VkDescriptorSetLayout + VkPipelineLayout   ← data-driven, no hardcoding
  → VkPipeline (via PipelineCache)
```

The Slang compiler has already resolved all MaterialX abstractions into concrete binding slots,
descriptor types, and sizes. Slang's reflection API exposes exactly what Vulkan needs:
`getElementTypeLayout()`, binding indices, descriptor types, buffer sizes — sufficient to build
`VkDescriptorSetLayoutBinding` entries without ever touching the MaterialX graph nodes.

**Use MaterialX directly for Phase 2, not hdMtlx.**
Design the abstraction boundary to accept `MaterialX::DocumentPtr`.
When Hydra integration comes, `pxr/imaging/hdMtlx` becomes a thin adapter:
```
HdMaterialNetworkMap  →  hdMtlx::CreateMtlxDocumentFromHdNetwork()  →  MaterialX::DocumentPtr
```
Zero changes to the core renderer for Phase 3.

**Both material paths (PBR preview + complex MaterialX) go through the same pipeline.**
Write the PBR preview / UsdPreviewSurface shader as a MaterialX `standard_surface` document.
MaterialX ships with node definitions for both. There is no "uber shader" code path in the renderer —
the "uber shader" is just a specific `.mtlx` file. This keeps pipeline creation fully generic.

---

## Proposed Architecture Sketch

```
Scene (C++ POD)
  ├── DrawablePrim[]
  │     ├── transform: glm::mat4
  │     ├── meshId:     MeshHandle
  │     └── materialId: MaterialHandle
  └── camera, lights

MeshLibrary
  ├── load(path) → MeshHandle
  └── MeshGpu { Buffer vertexBuf, Buffer indexBuf, uint32_t indexCount }

MaterialLibrary
  ├── compile(MaterialX::DocumentPtr) → MaterialHandle
  ├── compileFromFile(path)           → MaterialHandle  (convenience)
  └── CompiledMaterial
        ├── Shader                  (SPIR-V blob, via existing shader.h)
        ├── VkDescriptorSetLayout   (set 1, built from Slang reflection)
        ├── VkDescriptorSet         (set 1, textures + scalars written once at compile time)
        └── VkPipeline              (from PipelineCache — one per unique SPIR-V)

PipelineCache
  └── getOrCreate(Shader&, vertexLayout, renderTargetFormat) → VkPipeline
        key: hash(SPIR-V bytes + vertex stride/attrs + color/depth format)
        layout: [set0=per-frame UBO, set1=per-material] + push=[8-byte BDA transform]

PerFrameData  (set 0, UBO, updated once per frame — same bind for all draws)
  { projection: mat4, view: mat4, lightPos: vec4 }

PerDrawData   (push constant, 8-byte BDA, different per draw call)
  → Buffer containing { transform: mat4 }

Renderer
  ├── uploadMesh(path)             → MeshHandle
  ├── compileMaterial(path/.mtlx) → MaterialHandle
  └── renderFrame(Scene)
        update + bind set 0 (PerFrameData UBO)
        for each DrawablePrim:
          bind pipeline   (material.pipeline)
          bind set 1      (material.descriptorSet)
          push BDA        (per-draw transform buffer address)
          vkCmdDrawIndexed(mesh.indexCount, 1, ...)
```

### Descriptor set layout (fixed across all pipelines)

```
set 0 | binding 0 | UBO              | PerFrameData (camera + light)
set 1 | binding N | per-material     | determined by Slang reflection of generated shader
push  | 8 bytes   | BDA              | → PerDrawData { mat4 transform }
```

Set 0 layout is created once at renderer init. Set 1 layout is created per compiled material
from Slang reflection — the layout structure is whatever MaterialX shadergen emits (named
sampler bindings, scalar uniforms). The pipeline layout template always uses [set0, set1] + push;
only the set 1 descriptor layout changes between materials.

### MaterialX → Pipeline flow

```
.mtlx file
  │
  ├─ MaterialX shadergen (Slang backend)
  │    generates .slang source with named sampler + uniform bindings
  │
  ├─ Slang ISession::compileShader()   (reuses existing shader.h)
  │    produces SPIR-V blob
  │
  ├─ Slang ShaderReflection API
  │    discovers: binding slots, descriptor types, buffer sizes, scalar offsets
  │    → builds VkDescriptorSetLayoutBinding[] for set 1
  │
  ├─ PipelineCache::getOrCreate()
  │    creates VkPipeline (or returns cached one if SPIR-V hash matches)
  │
  └─ VkDescriptorSet allocation + writes
       textures loaded → VkImageView/VkSampler written at reflected slots
```

### Hydra boundary (Phase 3, no renderer changes needed)

```cpp
// HdMtlxRenderDelegate::Sync():
MaterialX::DocumentPtr doc =
    HdMtlxCreateMtlxDocumentFromHdNetwork(networkMap, ...);   // pxr/imaging/hdMtlx
MaterialHandle h = renderer.compileMaterial(doc);              // same API as Phase 2
```

---

## Open Questions Before Finalizing

### Texture / descriptor binding model — DECIDED: per-material descriptor set

MaterialX shadergen is the authority: it declares named sampler bindings per material
(e.g., `Sampler2D base_color`, `Sampler2D roughness`). We follow that convention.

Each `CompiledMaterial` owns a `VkDescriptorSet` (set 1) with its textures and scalars
bound at the slots Slang reflection reports. This maps cleanly to `HdMaterial` in Hydra —
one material object = one descriptor set. No global bindless pool; no index management.

### Phase 2 material diversity — DECIDED: different topologies

The 3 `.mtlx` files will have different node graph structures (flat color, multi-texture PBR,
emissive). This means different generated SPIR-V per material → PipelineCache is load-bearing
for Phase 2, not just future-proofing.

---

## What Changes from Phase 1

| Component | Phase 1 | Phase 2 |
|-----------|---------|---------|
| `main.cpp` | Everything inline | Thin orchestrator |
| Pipeline | One hard-coded `VkPipeline` | `PipelineCache`, N pipelines (one per material type) |
| Shader | Fixed Phong `.slang` | Per-material Slang generated by MaterialX shadergen |
| Textures | Global bindless array, instance-indexed | Per-material descriptor set (set 1), layout from Slang reflection |
| Scene | 3 hard-coded Suzannes | `Scene` + `DrawablePrim[]` |
| Materials | None | `MaterialLibrary` compiling `.mtlx` files |
| Transforms | `model[3]` embedded in ShaderData | Per-draw BDA push constant |
| Instancing | `instanceCount=3` single draw call | Per-prim draw calls (different materials = different pipeline + set 1) |
| Per-frame data | Mixed into ShaderData via BDA | Dedicated set 0 UBO (camera + light) |

---

## Phase 2 End State

```cpp
// Example of what Phase 2 C++ scene definition looks like:
Scene scene;
auto mesh = renderer.uploadMesh("assets/suzanne.obj");
auto mat0 = renderer.compileMaterial("assets/mat_clay.mtlx");
auto mat1 = renderer.compileMaterial("assets/mat_metal.mtlx");
auto mat2 = renderer.compileMaterial("assets/mat_emissive.mtlx");

scene.addPrim({ .transform = translate(glm::vec3(-2, 0, 0)), .mesh = mesh, .material = mat0 });
scene.addPrim({ .transform = translate(glm::vec3( 0, 0, 0)), .mesh = mesh, .material = mat1 });
scene.addPrim({ .transform = translate(glm::vec3( 2, 0, 0)), .mesh = mesh, .material = mat2 });

renderer.renderFrame(scene);
```

## Verification

1. Build and run: 3 Suzannes visible with visually distinct materials
2. Each material loads from a `.mtlx` file (not hard-coded shader values)
3. Changing a `.mtlx` file and restarting reflects the change (dynamic compilation)
4. No Hydra code in the renderer; the interface boundary is `MaterialX::DocumentPtr`
5. A fourth Suzanne can be added in C++ without touching renderer internals
