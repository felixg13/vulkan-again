# Vulkan Engine — Claude Guidelines

## Architecture

Small early-stage engine. Source lives in `src/`; shaders in `shaders/` (Slang → SPIR-V at build).

| Module | Files |
|---|---|
| Entry / render loop | `src/main.cpp` (816 lines, monolithic — Phase 2 will break this up) |
| Device init | `src/device.h/cpp` — vk-bootstrap backed |
| Buffers | `src/buffer.h/cpp` — VMA backed |
| Images/textures | `src/image.h/cpp` — VMA backed |
| Shader compilation | `src/shader.h/cpp` — Slang ISession |
| Mesh loading | `src/asset_loader.h/cpp` — tinyobjloader |
| Window | `src/window.h/cpp` — GLFW |
| Utilities | `src/utils.h` — `utils::validation::chk`, `chkSwapchain` |

Phase 2 architecture is in `.claude/plans/phase-2-plan-resilient-toast.md`.
Phase 3 adds a Hydra render delegate. Use `/openusd` skill when working on that.

---

## C++23 Conventions

Language standard: **C++23**. Shaders: **Slang** (not GLSL/HLSL directly).

### Naming

| Element | Convention | Example |
|---|---|---|
| Types, enums | `UpperCamelCase` | `MeshLibrary`, `PipelineCache` |
| Functions, variables | `snake_case` | `create_buffer()`, `frame_index` |
| Constants, `constexpr` | `SCREAMING_SNAKE_CASE` | `MAX_FRAMES_IN_FLIGHT` |
| Private/protected members | leading `_` | `_device`, `_allocator` |
| Namespaces | `snake_case` | `utils::validation` |
| Template params | `UpperCamelCase` | `T`, `Allocator` |

### Files

- Headers use `.h`, sources use `.cpp`
- All headers: `#pragma once`
- Slang shaders: `shaders/*.slang`; do not write raw GLSL/HLSL

### Modern C++

- Prefer ranges and range-for over index loops
- Use `std::span<T>` at buffer/array interface boundaries
- Use `std::optional`, `std::variant`, structured bindings
- Use `std::format` for strings
- Use concepts over SFINAE for template constraints
- `constexpr`/`consteval` wherever compile-time evaluation applies
- `enum class` always; no unscoped enums
- No C-style casts; use named casts (`static_cast`, etc.)
- No raw C arrays; use `std::array`, `std::vector`, `std::span`

### Resource Management — Vulkan Exception

Raw `VkXxx` handles are **not** wrapped in RAII by default. Verbose RAII wrappers add boilerplate for no practical benefit in this codebase.

- **Vulkan objects**: explicit `vkCreate*` / `vkDestroy*` pairs or VMA calls are correct and preferred
- **Memory**: all buffer and image allocation goes through VMA (`vmaCreateBuffer`, `vmaDestroyBuffer`, `vmaCreateImage`, `vmaDestroyImage`)
- **Non-Vulkan resources** (file handles, CPU-side allocations, GLFW objects): use `std::unique_ptr` / RAII as normal
- No owning raw pointers outside of Vulkan handle variables

---

## Vulkan 1.4 Practices

### Rendering

- Use **dynamic rendering** (`VkRenderingInfo`, `vkCmdBeginRendering`) — no `VkRenderPass` or `VkFramebuffer` unless a specific extension requires it
- Swapchain images transition with image memory barriers, not render pass attachments

### Synchronization

- Use **synchronization2** exclusively: `vkCmdPipelineBarrier2`, `VkImageMemoryBarrier2`, `VkBufferMemoryBarrier2`
- Do not use the old `vkCmdPipelineBarrier` / `VkImageMemoryBarrier` API
- `VkSemaphore` for GPU–GPU sync; `VkFence` for CPU–GPU sync

### Descriptors

- Prefer **bindless** descriptor indexing (`UPDATE_AFTER_BIND_BIT`, `PARTIALLY_BOUND_BIT`) — already in use in `shader.slang`
- Per-frame data: set 0 UBO (`PerFrameData`)
- Per-draw data: push constants ≤128 bytes (BDA pointer or packed data)
- No descriptor updates mid-frame for bound sets

### Pipelines

- Always pass a `VkPipelineCache` to `vkCreateGraphicsPipelines` — never `VK_NULL_HANDLE`
- Use the pipeline cache across frames; serialize to disk when caching across launches
- Shader stages compiled from Slang via `slangc` at build time; SPIR-V blobs loaded at runtime

### Memory

- All buffer/image allocation through **VMA**
- Use `VMA_ALLOCATION_CREATE_MAPPED_BIT` + `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` for upload buffers
- Staging buffer → `vkCmdCopyBuffer2` / `vkCmdCopyImage2` for GPU-local resources

### Bootstrap

- Use **vk-bootstrap** for instance, physical device selection, and logical device creation
- Do not hand-roll what vk-bootstrap already provides

---

## Code Structure

Never extract code into a named function. Write inline, always. If code repeats across multiple sites, write it out each time. Function extraction is the user's decision — they will plan it when they want it. The agent's job is a working codebase, not a tidy one.

If significant duplication is noticed, it may be mentioned briefly at the end of a response — one line, no suggestion on what to do. The user will decide whether to act.

## Git Workflow

- **Never** run `git commit`, `git push`, or `git rebase` autonomously
- When a commit message is needed: generate one and present it as a code block to copy — no staging, no committing
- Rebase on demand when asked; provide the commands to run, then wait for the output
- For complex multi-step git workflows, list the exact commands in order for the user to run, then continue after they paste the results
- Commit messages follow Conventional Commits (`feat`, `fix`, `refactor`, `chore`, etc.), imperative mood, ≤50 char subject, no Claude attribution
