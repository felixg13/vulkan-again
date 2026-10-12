---
name: openusd
description: >-
  OpenUSD and Hydra 2.0 reference for Phase 3 Hydra render delegate work.
  Covers USD scene graph, C++ API idioms, Hydra render delegate interface,
  Scene Indices, and the MaterialX→Slang integration path. Load when writing
  USD or Hydra code.
user-invokable: true
---

# OpenUSD + Hydra 2.0 Reference

Guidance for integrating this Vulkan engine as a Hydra render delegate (Phase 3).

---

## USD Scene Graph Model

A **Stage** (`UsdStage`) is the composed view of one or more **Layers** (`SdfLayer`).
A layer is the unit of file I/O (`.usd`, `.usda`, `.usdc`, `.usdz`).

**Prims** (`UsdPrim`) are the nodes of the scene graph, addressed by `SdfPath` (e.g. `/World/Cube`).
A prim has a **type** (e.g. `Mesh`, `Camera`, `Material`) and holds **Properties**:
- **Attributes** (`UsdAttribute`) — typed values (float, vec3, matrix, string, …) that can be time-sampled
- **Relationships** (`UsdRelationship`) — typed pointers to other prims

### Composition Arcs (LIVRPS evaluation order)

USD resolves opinions in this order (strongest → weakest):

| Priority | Arc | Typical use |
|---|---|---|
| 1 | **L**ocal opinions | Direct edits on the stage |
| 2 | **I**nherits | Class-based property propagation |
| 3 | **V**ariantSets | Switchable alternatives (LOD, materials) |
| 4 | **R**eferences | Scene assembly / asset instancing |
| 5 | **P**ayloads | Deferred-load heavy assets |
| 6 | **S**pecializes | Archetype refinement |

---

## C++ API Idioms

```cpp
// Open a stage
UsdStageRefPtr stage = UsdStage::Open("scene.usd");

// Traverse prims
for (UsdPrim const& prim : stage->Traverse()) {
    if (prim.IsA<UsdGeomMesh>()) { /* ... */ }
}

// Read an attribute value at time 0
UsdGeomMesh mesh(prim);
VtArray<GfVec3f> points;
mesh.GetPointsAttr().Get(&points, UsdTimeCode::Default());

// Token and path types
TfToken colorRole("color");          // interned string
SdfPath meshPath("/World/Suzanne");  // scene graph address

// Math types (Gf)
GfVec3f  pos(0.f, 1.f, 0.f);
GfMatrix4d xform = GfMatrix4d(1.0); // identity
GfRange3d  bounds;
```

Key headers: `pxr/usd/usd/stage.h`, `pxr/usd/usdGeom/mesh.h`, `pxr/base/tf/token.h`, `pxr/usd/sdf/path.h`, `pxr/base/gf/vec3f.h`

---

## Hydra 2.0

### Two Paradigms

| | Scene Delegate (legacy) | Scene Index (modern, Hydra 2.0) |
|---|---|---|
| Data flow | Pull: render delegate asks for data | Push: scene index emits diffs |
| API base | `HdSceneDelegate` | `HdSceneIndexBase` |
| Composition | Not composable | Chainable filtering scene indices |
| Recommendation | Avoid for new work | Prefer for Phase 3 |

### Core Classes

```
HdRenderIndex      — owns all prims, scene delegates, and the render delegate
HdRenderDelegate   — YOUR implementation (the Vulkan backend)
HdEngine           — drives Execute() on the render pass
HdRenderPass       — one render pass (collection of prims to draw)
HdRprim            — renderable prim (mesh, points, curves)
HdBprim            — buffer prim (texture, render buffer)
HdSprim            — state prim (camera, light, material)
HdInstancer        — instancing
```

### Render Delegate Interface (what you implement)

```cpp
class VulkanRenderDelegate : public HdRenderDelegate {
public:
    // Resource creation
    HdRprim*      CreateRprim(TfToken const& typeId, SdfPath const& rprimId) override;
    HdBprim*      CreateBprim(TfToken const& typeId, SdfPath const& bprimId) override;
    HdSprim*      CreateSprim(TfToken const& typeId, SdfPath const& sprimId) override;
    HdInstancer*  CreateInstancer(HdSceneDelegate*, SdfPath const&) override;

    // Render pass
    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex*, HdRprimCollection const&) override;

    // Resource registry (owns GPU resources)
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;

    // Supported prim types
    HdRprimCollection GetSupportedRprimTypes() const override;
    // ... similarly for Bprim, Sprim

    // Render settings / AOVs
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const& name) const override;
};
```

### Minimal Setup

```cpp
HdDriver driver;   // wraps your VkDevice / HgiVulkan
HdRenderIndex* index = HdRenderIndex::New(&renderDelegate, {&driver});

// Legacy scene delegate path:
UsdImagingDelegate sceneDelegate(index, SdfPath::AbsoluteRootPath());
sceneDelegate.Populate(stage->GetPseudoRoot());

HdEngine engine;
HdRenderPassSharedPtr pass = renderDelegate.CreateRenderPass(
    index, HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));

HdTaskSharedPtrVector tasks = { /* ... */ };
engine.Execute(index, &tasks);
```

---

## MaterialX → Slang Path (Phase 3)

```
HdMaterialNetwork2  (Hydra material graph)
        ↓  HdMtlxCreateMtlxDocumentFromHdNetwork()
MaterialX::DocumentPtr
        ↓  MaterialX codegen or custom traversal
Slang source string
        ↓  Slang ISession::createCompositeComponentType()
SPIR-V blob  →  VkShaderModule
```

Key header: `pxr/imaging/hdMtlx/hdMtlx.h` — `HdMtlxCreateMtlxDocumentFromHdNetwork`

This is the Phase 3 target. The renderer stays unchanged; only the material → shader pipeline changes.

---

## Useful References

- USD API docs: `https://openusd.org/release/api/`
- Hydra Architecture: `https://openusd.org/release/api/hd_page_front.html`
- HdRenderDelegate: `pxr/imaging/hd/renderDelegate.h`
- HdMtlx: `pxr/imaging/hdMtlx/hdMtlx.h`
