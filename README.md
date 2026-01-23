# ds-runtime

<p align="center">
  <img src="ds-runtime/assets/ds_runtime_logo.png" alt="ds-runtime logo" width="180">
</p>

Experimental Linux-native DirectStorage-style runtime (CPU today, GPU tomorrow) GPU/Vulkan backend, towards Wine/Proton integration. 


## 🔎 Overview

ds-runtime is an experimental C++ runtime that explores how a
DirectStorage-style I/O and decompression pipeline can be implemented natively on Linux.

The project focuses on:

- Clean, idiomatic systems-level C++

- Explicit and auditable concurrency

- POSIX-first file I/O

- A backend-based architecture that cleanly separates:

- request orchestration (queues, lifetime, synchronization)

- execution backends (CPU now, Vulkan/GPU planned)

The long-term motivation is to provide a solid architectural foundation for a
native Linux implementation of DirectStorage-like workloads, suitable for eventual integration with Wine / Proton.

This repository intentionally prioritizes structure, clarity, and correctness over premature optimization.

---
## 🚧 Project status

- Status: Experimental
- Backend: CPU (implemented)
- GPU/Vulkan backend: Planned

The current codebase implements a complete, working CPU backend and a clean public API designed to support GPU-accelerated backends in the future.

---

## ℹ️ What this is (and isn’t)

This is:

- A reference-quality runtime skeleton

- A clean async I/O queue with pluggable backends

- A realistic starting point for Linux-native asset streaming

- A codebase meant to be read, reviewed, and extended

This is not:

- A drop-in replacement for Microsoft DirectStorage

- A performance benchmark

- A full GPU decompression implementation (yet)

- Production-ready middleware

---

## 🧱 High-level architecture
```
┌─────────────┐
│   Client    │
│ (game/app)  │
└──────┬──────┘
       │ enqueue Request
       ▼
┌─────────────┐
│  ds::Queue  │   ← orchestration, lifetime, waiting
└──────┬──────┘
       │ submit
       ▼
┌────────────────────┐
│   ds::Backend      │   ← execution (CPU / Vulkan / future)
└────────────────────┘
```
See [docs/design.md](docs/design.md) for details on backend evolution.

---

## 💡 Core concepts

`ds::Request`

Describes what to load:

- POSIX file descriptor

- Byte offset

- Size

- Destination pointer

- Compression mode

- Completion status / error

This maps cleanly to:

- Linux I/O semantics

- DirectStorage request descriptors

- Future GPU-resident workflows

`ds::Queue`

Responsible for:

- Collecting requests

- Submitting them to a backend

- Tracking in-flight work

- Optional blocking via `wait_all()`

The queue **does not perform I/O itself**.

`ds::Backend`

Abstract execution interface.

Current implementation:

- **CPU backend**

- `pread()`-based I/O

- Small internal thread pool

- Demo “decompression” stage (uppercase transform)

Planned backends:

- Vulkan compute backend (GPU copy / decompression)

- `io_uring`-based backend

- Vendor-specific GPU paths

---

## 🎨 Why this design?

This mirrors how real DirectStorage systems are structured:

- A front-end queue API

- A backend that owns execution and synchronization

- Clear separation between:

- disk I/O

- decompression

- GPU involvement

Keeping these layers explicit makes the code:

- Easier to reason about

- Easier to test

- Easier to extend without rewrites

- Code hygiene goals

The project follows conventions expected by experienced Linux developers:

- Header / implementation split

- No global state

- RAII throughout

- Direct use of POSIX APIs (open, pread, close)

- No exceptions crossing public API boundaries

- Minimal but explicit threading

- No macro or template magic

- If something happens, it should be obvious where and why.

---

## Wine 🍷 / Proton 🧪

Modern Windows titles increasingly rely on DirectStorage-style APIs for
asset streaming and decompression. On Linux, these calls are currently
handled via compatibility-layer shims or fall back to traditional I/O paths.

This project explores what a **native Linux runtime** for DirectStorage-like
workloads could look like, with an emphasis on:

- Correct API semantics
- Clean separation between queue orchestration and execution
- Explicit backend design (CPU today, GPU later)
- Compatibility with Wine / Proton architecture

The current implementation focuses on a **CPU backend** that provides:
- Asynchronous I/O semantics
- Explicit completion tracking
- A decompression stage hook (currently a demo transform)

This is intended as a **foundational layer** that could back a future
`dstorage.dll` implementation in Wine/Proton, with GPU acceleration added
incrementally once semantics and integration points are validated.

--- 

## 🎬 Demo

The included demo program:

- Writes a small test asset to disk

- Enqueues two asynchronous requests:

- One raw read

- One “compressed” read (fake uppercase transform)

- Submits them concurrently

- Waits for completion

- Prints the results

Example output:

``` 
[demo] starting DirectStorage-style CPU demo
[demo] wrote 41 bytes to demo_asset.bin
[demo] submitting 2 requests
[demo] waiting for completion (in-flight=2)
[demo] all requests completed (in-flight=0)
raw   : "Hello DirectStorage-style queue on Linux!"
upper : "HELLO DIRECTSTORAGE-STYLE QUEUE ON LINUX!"

``` 
---

## 🛠️ Building

### Requirements

Linux

C++20 compiler (Clang or GCC)

pthreads

CMake ≥ 3.16

### Build steps

```bash 
git clone https://github.com/infinityabundance/ds-runtime.git
cd ds-runtime


mkdir build
cd build
cmake ..
cmake --build .
``` 
Run the demo:

```bash 
# from inside build/examples/
./ds_demo
``` 
---

## 🔭 Repository layout
``` 

├── CMakeLists.txt            # Top-level CMake build configuration
│
├── include/                  # Public C++ API headers
│   └── ds_runtime.hpp        # Core DirectStorage-style runtime interface
│
├── src/                      # Runtime implementation
│   └── ds_runtime.cpp        # Queue, backend, and CPU execution logic
│
├── examples/                 # Standalone example programs
│   ├── ds_demo_main.cpp      # CPU-only demo exercising ds::Queue and requests
│   │
│   └── vk-copy-test/         # Experimental Vulkan groundwork
│       ├── copy.comp         # Vulkan compute shader (GLSL)
│       ├── copy.comp.spv     # Precompiled SPIR-V shader
│       ├── demo_asset.bin    # Small test asset for GPU copy
│       ├── vk_copy_test.cpp  # Vulkan copy demo (CPU → GPU → CPU)
├── docs/                     # Design and architecture documentation
│   └── design.md             # Backend evolution and architectural notes
│
├── assets/                   # Non-code assets used by documentation
│   └── logo.png              # Project logo displayed in README
│
├── README.md                 # Project overview, build instructions, roadmap
└── LICENSE                   # Apache-2.0 license
``` 
---

## 🧩 Relationship to DirectStorage, Wine, and Proton

This project is not affiliated with Microsoft, Valve, or the Wine project.

However, it is intentionally structured so that:

- `ds::Request` can map to `DSTORAGE_REQUEST_DESC`

- `ds::Queue` can map to a DirectStorage queue object

- A Vulkan backend can integrate with Proton’s D3D12 → Vulkan interop

The goal is to explore what a **native Linux DirectStorage-style runtime** could look like, with real code and real execution paths.

---

## 🛣️ Roadmap (rough)

 ◻️ Vulkan backend (compute copy → compute decompression)

 ◻️ Real compression format (CPU GDeflate first)

 ◻️ `io_uring` backend

 ◻️ Wine / Proton integration experiments

 ◻️ Real-world game testing

This project intentionally starts small and correct.

---

## 🌱 Contributing

Discussion, feedback, and code review are welcome.

If you are a:

- Linux systems developer

- Graphics / Vulkan developer

- Wine or Proton contributor

…your perspective is especially appreciated.

---

## 🪪 License

This project is licensed under the Apache License 2.0.
See the LICENSE file for details.

---

## 📝 Final note

Linux deserves first-class asset streaming paths — not just compatibility shims.

Even if this repository never becomes the solution, it aims to push that discussion forward with real, auditable code rather than speculation.

---
