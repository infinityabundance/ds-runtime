docs/design.md

## Design: Backend Evolution
### Overview

This document describes the intended evolution of `ds-runtime` backends,
from the current CPU implementation to future GPU-accelerated paths.

The goal is to incrementally approach a DirectStorage-style execution model
while keeping each stage auditable, testable, and usable in isolation.

### Current architecture

At a high level, the runtime is structured as:
```
Client
  ↓
ds::Queue
  ↓
ds::Backend
```

`ds::Queue` is responsible for request lifetime, submission, and waiting.

`ds::Backend` is responsible for executing requests and signaling completion.

This separation is intentional and central to future backend evolution.

---

### CPU backend (current)

The current `CpuBackend` implementation provides:

- POSIX file I/O via `pread()`

- A fixed-size internal thread pool

- One completion callback per request

- A placeholder “decompression” stage

Key properties:

- Semantics-first implementation

- No GPU involvement

- Easy to debug and reason about

- Suitable as a reference backend

This backend establishes correct behavior before introducing
hardware-accelerated paths.

---

### Vulkan backend (experimental)

An experimental `VulkanBackend` implements the same `ds::Backend` interface.

Expected responsibilities:

- Read file data into host-visible staging buffers

- Issue Vulkan buffer copies between staging buffers and GPU buffers

- Dispatch Vulkan compute workloads for future decompression stages

Signal completion via fences or timeline semaphores

Importantly:

- The queue interface does not change

- Only the backend implementation changes

This allows correctness to be validated independently of GPU acceleration.

---

### io_uring backend (experimental)

An experimental `IoUringBackend` executes host-side requests using `io_uring`.

Expected responsibilities:

- Submit read/write operations via the kernel ring
- Invoke completion callbacks when CQEs arrive
- Reject GPU-targeted requests (host-only backend)

This backend provides a path toward lower-overhead I/O without changing the
public queue API.

### Compression handling

The current implementation includes a demo transform to validate
pipeline structure.

Planned progression:

1. CPU-based GDeflate decompression

2. GPU-based GDeflate decompression via compute shaders

3. Backend selection based on:

⋅⋅⋅⋅*data destination

⋅⋅⋅⋅*hardware capabilities

⋅⋅⋅⋅*runtime configuration

Compression is treated as a backend concern, not a queue concern.

---

### Error reporting

The runtime exposes a process-wide error callback (`ds::set_error_callback`)
that receives rich context (subsystem, operation, source location, timestamp)
to aid debugging. Backends call `ds::report_error` whenever they encounter
errors during submission, I/O, or GPU transfers.

### Integration with Wine / Proton

The runtime is designed so that:

- `ds::Request` can map directly to `DSTORAGE_REQUEST_DESC`

Completion callbacks can map to DirectStorage fences/events

GPU backends can integrate with existing D3D12 → Vulkan interop paths

The intent is not to bypass Proton’s graphics stack, but to provide
a clean execution engine beneath it.

---

### Non-goals

This project intentionally avoids:

- Re-implementing full DirectStorage API surface initially

- Vendor-specific optimizations in early stages

- Tight coupling to any single I/O or GPU API

Correctness and maintainability take precedence over raw throughput
during early development.

---

### Summary

The backend architecture is designed to:

- Start simple

- Remain composable

- Allow incremental acceleration

- Avoid redesign when GPU paths are added

Each backend should be independently testable and replaceable.
