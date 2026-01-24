# Wine / Proton integration notes

This project provides a native Linux shared library (`libds_runtime.so`) and
an optional C ABI (`include/ds_runtime_c.h`) that can be used as the backend
for a Wine/Proton-facing shim. This document outlines how to wire that shim
into Wine/Proton and what pieces still need to be implemented.

## Goals

- Provide a stable Linux `.so` that can be loaded by Wine/Proton glue code.
- Keep the C ABI small and explicit so it can be used from a PE/ELF bridge.
- Allow iterative replacement of stub DirectStorage APIs with real behavior.

## Recommended integration path

### 1. Build and install ds-runtime

```bash
cmake -B build -S . -DDS_BUILD_SHARED=ON -DDS_BUILD_STATIC=OFF
cmake --build build
cmake --install build --prefix ~/.local
```

This installs:

- `libds_runtime.so` in `~/.local/lib`
- `include/ds_runtime_c.h` in `~/.local/include`
- `ds-runtime.pc` for `pkg-config`

### 2. Create a Wine/Proton shim DLL (outside this repo)

Implement a `dstorage.dll` (PE DLL) that forwards key DirectStorage entry points
to the Linux backend through the C ABI. This DLL can be built in the Wine tree
or as a separate out-of-tree module. The shim typically does:

1. `dlopen("libds_runtime.so")`
2. `dlsym()` for the `ds_*` C functions
3. Translate `DSTORAGE_*` requests into `ds_request` entries
4. Submit requests via `ds_queue_submit_all`
5. Call back into Wine completion logic from the C callback

### 3. Hook the DLL into Wine/Proton

- For Wine, build `dstorage.dll` as a builtin and set `WINEDLLOVERRIDES="dstorage=b"`.
- For Proton, add the DLL to the compatible prefix and override in the prefix
  configuration if needed.

### 3a. Direct integration (no shim / no dlopen)

If you want to avoid a separate shim DLL entirely, the simplest path is to
compile ds-runtime directly into Wine/Proton and call the backend from the
DirectStorage implementation itself:

- Add ds-runtime as a subproject or static library dependency in the Wine build.
- Instantiate `ds::VulkanBackendConfig` with the Vulkan device/queue already
  created by Proton’s D3D12 → Vulkan stack.
- Submit requests with `RequestMemory::Gpu` (or `DS_REQUEST_MEMORY_GPU` via
  the C API) and pass the target `VkBuffer` handle through `gpu_buffer`.

This keeps the data path entirely inside the Wine/Proton process and avoids
any PE/ELF bridging. The Vulkan backend in this repo is designed to accept
externally-owned Vulkan objects without taking ownership.

### 4. Extend the mapping

The current runtime offers:

- POSIX file descriptor reads via `pread`
- POSIX file descriptor writes via `pwrite`
- GPU buffer transfers via Vulkan staging copies (read ↔ GPU, write ↔ GPU) using
  `gpu_buffer`, `gpu_offset`, and the host/GPU memory flags in each request

  - `gpu_buffer` must be a valid `VkBuffer` created on the backend's device.
- A fake "uppercase" transform in place of decompression

As the runtime grows, extend the shim to map:

- I/O queues and fences to Wine/Proton synchronization primitives
- GPU decompression paths to Vulkan compute or vendor APIs
- Compression formats (e.g., GDeflate) to real codecs

## Notes on ABI stability

The `ds_runtime_c.h` ABI is intended to stay stable and minimal. If additional
DirectStorage features are needed, add them by introducing new functions rather
than changing existing structs in-place.
