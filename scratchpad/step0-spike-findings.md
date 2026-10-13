# Step 0 — Spike Findings

Reproduce: `scratchpad/spike/run.sh` (builds into `scratchpad/spike/.work`, gitignored).
Throwaway code. Nothing under `src/` was touched.

## Exit criteria

| # | Criterion | Result |
|---|---|---|
| 1 | MaterialX 1.39.5 contains `MaterialXGenSlang`, generates Slang for flat + textured `.mtlx` | **Pass.** Built standalone (`MATERIALX_BUILD_GEN_SLANG=ON`). Not yet checked against the USD install. |
| 2 | Both compile through Slang to SPIR-V that validates | **Pass** (`spirv-val --target-env vulkan1.3`) with Slang **v2026.14**. The Vulkan SDK's `slangc` (1.4.321.1) segfaults on the composed module. |
| 3 | Reflection reports material block + textures at predictable set/binding | **Pass**, with a caveat (see F2, F3). |
| 4 | Adapter + composition with a hand-written vertex module compiles/reflects | **Pass** — but by source concatenation, not link-time composition (see F4). |
| 5 | One material renders in a trivial harness | **Pass** (both materials, headless, Intel UHD 620). |

## Findings that change the plan

**F1 — Lighting and view position live in the generated cbuffer, not set 0.**
`u_viewPosition` and `u_lightData[i] { type, direction, color, intensity }` are in the
material's `pixelCB`. The light only gets `direction/color/intensity` after
`HwShaderGenerator::bindLightShader(ND_directional_light, 1, ctx)` and
`hwMaxActiveLightSources = 1`. The plan's "set 0 holds the light" does not reach MaterialX code.
Decision needed for Step 5 (see Decisions).

**F2 — Everything generated lands in set 0, binding 0.** `cbuffer pixelCB` has no explicit
binding, so it collides with the engine's set 0. Prefixing the module with
`#define cbuffer [[vk::binding(0,1)]] cbuffer` moves the block *and* its nested resources
to set 1 without editing generated text. `-fvk-bind-globals` does not apply (it targets `$Globals`).

**F3 — Slang splits `SamplerTexture2D` into two descriptors.** Layout for textured:
`b0 UBO, b1 tex/b2 sampler (u_envRadiance), b3/b4 (u_envIrradiance), b5/b6 (tex_base_file)`.
So set 1 needs `SAMPLED_IMAGE` + `SAMPLER` pairs, not `COMBINED_IMAGE_SAMPLER`.
Env textures are always declared, even with no image nodes; they must be bound (1x1 dummy).

**F4 — Generated `VertexData` field order varies per material**
(flat: `normalWorld, tangentWorld, positionWorld`; textured: `…, texcoord_0, positionWorld`),
so `Location` numbers differ per material. The robust adapter is a pixel entry point that
takes the engine varyings, builds `VertexData` by field name, and calls the generated
`fragmentMain(d)`. This works because Slang lets one entry point call another.
The adapter body must be emitted per material from the `VertexData` field list
(MaterialX: `stage.getInputBlock(HW::VERTEX_DATA)`).

**F5 — `string`-typed ports have no value** (`tex_base_layer`, `tex_base_framerange`) but are
declared `int` in the cbuffer. Leave zero.

**F6 — `hwSpecularEnvironmentMethod = SPECULAR_ENVIRONMENT_NONE` generates uncompilable Slang**
(`u_refractionTwoSided` is dropped but still referenced). Keep the default method.

**F7 — Generated SPIR-V is ~750 KB per material** (unoptimized). Pipeline creation cost and
cache size matter earlier than expected.

## Open: the "link-time composition" claim in the architecture doc
Slang composition of the generated module + an adapter module was not attempted through
`ISession::createCompositeComponentType`; the spike concatenated `preamble + generated + adapter`.
The `#define` preamble in F2 has to be in the generated module's own source anyway, so
concatenation (never editing the generated text) is the practical form. Doc should say so.

## Decisions needed before Step 1
1. Per-frame data in material cbuffer (F1): mirror camera position and light into every
   material's UBO each frame, or drop `u_lightData` from the contract and have the adapter
   supply them from set 0?
2. Is concatenation (F2/F4) acceptable in place of "link-time composition"?
3. Slang version: Makefile links `/opt/vulkan-sdk/.../libslang` via rpath, while
   `external/CMakeLists.txt` pins 2026.14. Which one does the engine actually load at runtime?
