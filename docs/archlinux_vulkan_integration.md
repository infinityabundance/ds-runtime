# Arch Linux Vulkan DirectStorage integration (Wine/Proton, no shim)

This document describes a **no-shim** integration strategy for using
`ds-runtime` as a native Linux DirectStorage-style backend on **Arch Linux**.
The goal is to keep the entire data path inside the Wine/Proton process while
reusing Vulkan objects that already exist in Proton’s D3D12 → Vulkan stack.

It focuses on:

- **Direct integration** (no PE/ELF shim or `dlopen` layer)
- **Vulkan-backed file ↔ GPU buffer transfers**
- **Code hygiene** expectations for production-quality integration

> Note: This repository does not include Wine/Proton source code; the steps
> below explain how to link ds-runtime into those projects and map the data
> flow. The integration points are intentionally minimal and explicit.

---

## Architecture summary

DirectStorage-style pipelines separate **queue orchestration** from **execution**:

```
Game/D3D12
   ↓ (requests)
Wine/Proton DirectStorage layer
   ↓ (translated requests)
ds::Queue + ds::VulkanBackend
   ↓ (Vulkan copy + staging)
GPU buffers / disk
```

`ds::VulkanBackend` handles GPU transfers by:

- Reading file data into **host-visible staging buffers**
- Copying staging → GPU buffers via `vkCmdCopyBuffer`
- Copying GPU buffers → staging and writing via `pwrite` for write requests

This mirrors DirectStorage’s intent: **fast disk → GPU** I/O with explicit
queueing and completion semantics.

---

## Arch Linux prerequisites

Install Vulkan and build tooling:

```bash
sudo pacman -S --needed base-devel cmake git vulkan-headers vulkan-loader
```

For GPU drivers:

- **NVIDIA**: `nvidia` + `nvidia-utils`
- **AMD**: `mesa` + `vulkan-radeon`
- **Intel**: `mesa` + `vulkan-intel`

---

## No-shim integration steps (recommended)

### 1. Build ds-runtime as a static library

This avoids dynamic linking in Wine/Proton:

```bash
cmake -B build -S . -DDS_BUILD_SHARED=OFF -DDS_BUILD_STATIC=ON
cmake --build build
```

### 2. Link ds-runtime into Wine/Proton

Add `libds_runtime_static.a` (or the ds-runtime subproject) to the Wine build
system. Then:

- Construct `ds::VulkanBackendConfig` using Vulkan handles already created
  by Proton’s D3D12 → Vulkan runtime.
- Create a `ds::Queue` with `make_vulkan_backend(...)`.
- Translate DirectStorage request descriptors into `ds::Request`.

Key integration points:

| DirectStorage (concept) | ds-runtime (field) |
| --- | --- |
| file handle + offset | `Request::fd`, `Request::offset` |
| size | `Request::size` |
| read/write | `Request::op` |
| destination GPU buffer | `Request::gpu_buffer`, `Request::gpu_offset` |
| completion event/fence | completion callback from backend |

### 3. Use Vulkan buffers already owned by Proton

The Vulkan backend does **not** take ownership of the device/queue/pool.
You should pass the existing Vulkan objects created by Proton to
`VulkanBackendConfig`. This avoids extra device creation and matches Vulkan
best practices from both Valve and the Khronos documentation:

- One device per process (or per adapter) is preferred
- Command pools should be reused where possible

---

## Code hygiene expectations

When integrating into Wine/Proton:

- **Avoid hidden global state**: pass explicit context pointers.
- **Respect ownership**: the backend does **not** destroy external Vulkan handles.
- **Use explicit synchronization**: map completions to existing fence/event flows.
- **Minimize conversions**: translate once at the boundary, keep internal structs stable.

Keep the DirectStorage → ds-runtime mapping in a single translation layer so
the runtime stays agnostic of Wine/Proton internals.

---

## Technical references (conceptual)

This repository follows patterns from public documentation:

- Vulkan staging buffer copy patterns (Khronos/Vulkan spec)
- DirectStorage request queue models (Microsoft DirectStorage docs)
- Wine/Proton D3D12 → Vulkan interop design (Wine/Proton public notes)

These references motivate the architecture, but the implementation remains
intentionally minimal and auditable.
