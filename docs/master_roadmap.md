# DirectStorage-Style I/O and Decompression Pipeline: Master Roadmap

**Project**: ds-runtime  
**Goal**: Functioning DirectStorage-style I/O and decompression pipeline implemented natively on Linux  
**Target Platform**: CachyOS/Arch Linux with GPU/Vulkan support  
**Timeline**: 36 weeks (9 months) for complete implementation, 12 weeks for MVP

---

## Executive Summary

This document provides a comprehensive, phased implementation plan for completing the ds-runtime project. It integrates investigations across all components (GDeflate, Vulkan compute, io_uring, cancellation, GPU workflows, Wine/Proton) into a cohesive roadmap with extensive subphasing and microtasking.

---

## 1. Project Status Overview

### 1.1 Current State (Phase 0 - Complete ✅)

**Working Components**:
- ✅ CPU backend with thread pool
- ✅ Read/write operations (pread/pwrite)
- ✅ FakeUppercase demo compression
- ✅ Error reporting with rich context
- ✅ Request completion tracking
- ✅ C ABI for Wine/Proton integration
- ✅ Basic test suite (4 tests, all passing)
- ✅ Build system (CMake, C++20)

**Project Builds Successfully**: All tests pass, demos work

### 1.2 Components Requiring Work

| Component | Status | Priority | Effort |
|-----------|--------|----------|--------|
| **GDeflate Compression** | ⚠️ Stubbed (ENOTSUP) | High | 9-13 weeks |
| **Vulkan GPU Compute** | ⚠️ Partial (copy only) | High | 8 weeks |
| **io_uring Backend** | ⚠️ Partial (single-worker) | Medium | 6 weeks |
| **Request Cancellation** | ❌ Not implemented | Medium | 3 weeks |
| **GPU-Resident Workflows** | ⚠️ Basic only | Medium | 4 weeks |
| **Wine/Proton Integration** | ❌ Documentation only | High | 8 weeks |

---

## 2. Phased Implementation Plan

### PHASE 1: Foundation & Research (Weeks 1-8)

**Goal**: Establish foundation for all advanced features

#### Phase 1.1: GDeflate Format Research (Weeks 1-3)
**Owner**: Research Lead  
**Priority**: Critical Path

**Microtasks**:
- [ ] **1.1.1** Review Microsoft DirectStorage documentation (2 days)
  - SDK docs, headers, samples
  - GDeflate format specification (if available)
  - Create `docs/gdeflate_format.md`

- [ ] **1.1.2** Analyze existing implementations (3 days)
  - Wine/Proton DirectStorage status
  - Community implementations
  - Open-source decompression libraries

- [ ] **1.1.3** Reverse engineer format if needed (5 days)
  - Create compressed test assets
  - Analyze binary structure
  - Document block format, headers, metadata

- [ ] **1.1.4** Validate format understanding (2 days)
  - Create test vectors
  - Verify decompression correctness
  - Document any ambiguities

**Deliverables**:
- ✅ GDeflate format specification document
- ✅ Test asset collection (compressed files)
- ✅ Validation test vectors

**Success Criteria**: Can parse GDeflate headers and understand block structure

---

#### Phase 1.2: Vulkan Compute Infrastructure (Weeks 1-8)
**Owner**: Graphics Engineer  
**Priority**: Critical Path (parallel with GDeflate research)

**Sub-Phase 1.2.1: Shader Module System (Weeks 1-2)**

**Microtasks**:
- [ ] **1.2.1.1** Implement shader file loading (1 day)
  - Read SPIR-V binary from file
  - Error handling for missing files
  - Add to `src/ds_runtime_vulkan.cpp`

- [ ] **1.2.1.2** Create VkShaderModule wrapper (1 day)
  - `vkCreateShaderModule` call
  - Validation of SPIR-V code
  - Error reporting

- [ ] **1.2.1.3** Implement shader caching (2 days)
  - `ShaderModuleCache` class
  - Hash-based caching
  - Lifetime management

- [ ] **1.2.1.4** Test shader loading (1 day)
  - Load existing `copy.comp.spv`
  - Test error cases
  - Add unit test

- [ ] **1.2.1.5** Set up shader build system (2 days)
  - CMake integration for glslangValidator
  - Auto-compile `.comp` files
  - Install shader SPV files

**Deliverable**: Shader module loading system complete

**Sub-Phase 1.2.2: Descriptor Management (Weeks 3-4)**

**Microtasks**:
- [ ] **1.2.2.1** Design descriptor layouts (2 days)
  - Decompression layout (3 bindings)
  - Copy layout (2 bindings)
  - Document layout specifications

- [ ] **1.2.2.2** Implement descriptor set layout creation (1 day)
  - `vkCreateDescriptorSetLayout`
  - Multiple layout types
  - Add to VulkanBackend

- [ ] **1.2.2.3** Implement descriptor pool (2 days)
  - `vkCreateDescriptorPool`
  - Size estimation logic
  - Pool management

- [ ] **1.2.2.4** Implement descriptor set allocation (1 day)
  - `vkAllocateDescriptorSets`
  - Free/reuse logic
  - Error handling

- [ ] **1.2.2.5** Implement buffer binding (2 days)
  - `vkUpdateDescriptorSets`
  - Buffer info structure
  - Dynamic updates

- [ ] **1.2.2.6** Test descriptor system (2 days)
  - Unit tests for allocation/free
  - Buffer binding validation
  - Memory leak testing

**Deliverable**: Descriptor management system complete

**Sub-Phase 1.2.3: Pipeline Creation (Weeks 5-6)**

**Microtasks**:
- [ ] **1.2.3.1** Implement pipeline layout (1 day)
  - `vkCreatePipelineLayout`
  - Push constant support
  - Multiple descriptor layouts

- [ ] **1.2.3.2** Implement compute pipeline creation (2 days)
  - `vkCreateComputePipelines`
  - Pipeline configuration
  - Pipeline caching

- [ ] **1.2.3.3** Create pipeline management system (2 days)
  - `ComputePipeline` struct
  - Pipeline registry (by name)
  - Lifetime management

- [ ] **1.2.3.4** Test pipeline creation (2 days)
  - Create simple copy pipeline
  - Validation layers enabled
  - Error handling tests

- [ ] **1.2.3.5** Create example shaders (3 days)
  - `buffer_copy.comp` - simple copy
  - `uppercase.comp` - ASCII transform
  - Compile to SPIR-V

**Deliverable**: Compute pipeline creation system complete

**Sub-Phase 1.2.4: Compute Dispatch & Synchronization (Weeks 7-8)**

**Microtasks**:
- [ ] **1.2.4.1** Implement command buffer recording (2 days)
  - `vkCmdBindPipeline` for compute
  - `vkCmdBindDescriptorSets`
  - `vkCmdDispatch`

- [ ] **1.2.4.2** Implement synchronization barriers (2 days)
  - Transfer → Compute barrier
  - Compute → Transfer barrier
  - Compute → Compute barrier

- [ ] **1.2.4.3** Integrate with request processing (3 days)
  - Add compute path to VulkanBackend
  - Command buffer management
  - Completion tracking

- [ ] **1.2.4.4** Test compute execution (2 days)
  - Buffer copy shader test
  - Uppercase transform test
  - Synchronization validation

- [ ] **1.2.4.5** Performance profiling (1 day)
  - Measure dispatch overhead
  - GPU utilization metrics
  - Optimization opportunities

**Deliverable**: Full Vulkan compute capability

**Phase 1 Milestones**:
- ✅ Week 3: GDeflate format understood
- ✅ Week 2: Shader loading works
- ✅ Week 4: Descriptor system works
- ✅ Week 6: Compute pipelines created
- ✅ Week 8: Compute dispatch working

---

### PHASE 2: Core Feature Implementation (Weeks 9-18)

**Goal**: Implement essential features (GDeflate CPU, io_uring, cancellation)

#### Phase 2.1: GDeflate CPU Implementation (Weeks 9-13)
**Owner**: Compression Engineer  
**Priority**: High  
**Dependencies**: Phase 1.1 complete

**Sub-Phase 2.1.1: Block Header Parser (Weeks 9-10)**

**Microtasks**:
- [ ] **2.1.1.1** Define block header struct (1 day)
  - Header fields based on format spec
  - Size, offset, compression parameters
  - Create `include/gdeflate_format.h`

- [ ] **2.1.1.2** Implement header parsing (2 days)
  - Parse file/stream header
  - Extract block metadata
  - Validate checksums (if present)

- [ ] **2.1.1.3** Implement block iterator (1 day)
  - Iterate over blocks in file
  - Handle partial files
  - Error reporting

- [ ] **2.1.1.4** Test header parsing (2 days)
  - Test with known assets
  - Edge cases (empty, malformed)
  - Add unit test `tests/gdeflate_header_test.cpp`

**Deliverable**: GDeflate header parser

**Sub-Phase 2.1.2: DEFLATE Integration (Weeks 10-11)**

**Microtasks**:
- [ ] **2.1.2.1** Evaluate libraries (1 day)
  - zlib vs miniz vs custom
  - Performance comparison
  - License compatibility

- [ ] **2.1.2.2** Integrate chosen library (2 days)
  - Add to CMakeLists.txt
  - Wrapper functions
  - Error handling

- [ ] **2.1.2.3** Implement block decompression (2 days)
  - Decompress single block
  - Handle dictionary/state
  - Streaming support

- [ ] **2.1.2.4** Implement multi-block decompression (2 days)
  - Iterate over all blocks
  - Parallel decompression (thread pool)
  - Assembly of output buffer

- [ ] **2.1.2.5** Test decompression (3 days)
  - Single block tests
  - Multi-block tests
  - Correctness validation
  - Add `tests/gdeflate_decompress_test.cpp`

**Deliverable**: Working GDeflate CPU decoder

**Sub-Phase 2.1.3: Backend Integration (Weeks 12-13)**

**Microtasks**:
- [ ] **2.1.3.1** Remove ENOTSUP stub (0.5 day)
  - Delete stub code in `ds_runtime.cpp`
  - Wire in actual decoder

- [ ] **2.1.3.2** Integrate decoder into CPU backend (1 day)
  - Call from decompression pipeline
  - Buffer management
  - Error propagation

- [ ] **2.1.3.3** Add configuration options (1 day)
  - Parallel decompression settings
  - Memory limits
  - Fallback behavior

- [ ] **2.1.3.4** Test integration (2 days)
  - Update `compression_gdeflate_stub_test.cpp`
  - Change from "verify error" to "verify success"
  - End-to-end tests

- [ ] **2.1.3.5** Performance benchmarking (2 days)
  - Measure decompression throughput
  - Compare vs uncompressed
  - Optimize hotspots

- [ ] **2.1.3.6** Documentation (1 day)
  - Update README.md
  - Usage examples
  - Performance characteristics

**Deliverable**: GDeflate CPU support complete

**Phase 2.1 Milestone**: GDeflate CPU decoder fully functional

---

#### Phase 2.2: io_uring Multi-Worker (Weeks 9-14)
**Owner**: Systems Engineer  
**Priority**: Medium (parallel with GDeflate)  
**Dependencies**: None

**Sub-Phase 2.2.1: Multi-Worker Architecture (Weeks 9-11)**

**Microtasks**:
- [ ] **2.2.1.1** Design worker architecture (1 day)
  - Multiple io_uring instances
  - Request distribution strategy
  - Synchronization design

- [ ] **2.2.1.2** Implement worker structure (2 days)
  - `IoUringWorker` class
  - Worker thread management
  - Lifecycle (init/shutdown)

- [ ] **2.2.1.3** Implement request queue per worker (1 day)
  - Thread-safe pending queue
  - Condition variable for signaling
  - Lock-free alternatives (optional)

- [ ] **2.2.1.4** Implement worker event loop (3 days)
  - Submit SQEs from queue
  - Poll for CQEs
  - Timeout handling

- [ ] **2.2.1.5** Implement load balancing (2 days)
  - Round-robin distribution
  - Queue depth aware (optional)
  - Test distribution fairness

- [ ] **2.2.1.6** Test multi-worker (3 days)
  - Submit to multiple workers
  - Verify parallel execution
  - Stress test (1000+ requests)
  - Add `tests/io_uring_multi_worker_test.cpp`

**Deliverable**: Multi-worker io_uring backend

**Sub-Phase 2.2.2: Advanced Features (Weeks 12-14)**

**Microtasks**:
- [ ] **2.2.2.1** Implement fixed files support (2 days)
  - Register file descriptors
  - Use IOSQE_FIXED_FILE flag
  - Benchmark improvement

- [ ] **2.2.2.2** Add SQPOLL mode (optional) (2 days)
  - Configure SQPOLL parameters
  - Test latency improvement
  - Document requirements

- [ ] **2.2.2.3** Enhanced error handling (2 days)
  - EAGAIN retry logic
  - EINTR handling
  - Robust failure recovery

- [ ] **2.2.2.4** Performance tuning (3 days)
  - Optimize queue depth
  - Tune polling interval
  - Batch submission optimization

- [ ] **2.2.2.5** Comprehensive testing (2 days)
  - Error injection tests
  - Performance benchmarks
  - Compare vs CPU backend

- [ ] **2.2.2.6** Documentation (1 day)
  - Update README.md
  - Configuration guide
  - Performance tuning tips

**Deliverable**: Production-ready io_uring backend

**Phase 2.2 Milestone**: io_uring backend feature-complete

---

#### Phase 2.3: Request Cancellation (Weeks 15-18)
**Owner**: Core Engineer  
**Priority**: Medium  
**Dependencies**: None

**Sub-Phase 2.3.1: API Design (Week 15)**

**Microtasks**:
- [ ] **2.3.1.1** Add RequestStatus::Cancelled (0.5 day)
  - Update enum in `ds_runtime.hpp`
  - Update C ABI mapping

- [ ] **2.3.1.2** Add request ID tracking (1 day)
  - `request_id_t` type
  - ID generation (atomic counter)
  - Request ID → Request mapping

- [ ] **2.3.1.3** Add cancellation flag to Request (0.5 day)
  - `std::atomic<bool> cancellation_requested`
  - Memory ordering considerations

- [ ] **2.3.1.4** Design Queue cancellation API (1 day)
  - `bool cancel_request(request_id_t)`
  - `size_t cancel_all_pending()`
  - `size_t cancel_all()`

- [ ] **2.3.1.5** Document cancellation semantics (1 day)
  - Strong vs weak guarantees
  - Race condition handling
  - Callback behavior

**Deliverable**: Cancellation API design

**Sub-Phase 2.3.2: Queue Implementation (Week 16)**

**Microtasks**:
- [ ] **2.3.2.1** Implement request tracking (2 days)
  - Active requests map
  - Thread-safe access
  - ID assignment on enqueue

- [ ] **2.3.2.2** Implement cancel_request (1 day)
  - Lookup request by ID
  - Set cancellation flag
  - Remove if still pending

- [ ] **2.3.2.3** Implement cancel_all methods (1 day)
  - Iterate active requests
  - Mark all for cancellation
  - Return count

- [ ] **2.3.2.4** Test queue cancellation (1 day)
  - Cancel pending request
  - Cancel after submit
  - Race condition tests
  - Add `tests/cancellation_queue_test.cpp`

**Deliverable**: Queue-level cancellation

**Sub-Phase 2.3.3: Backend Integration (Weeks 17-18)**

**Microtasks**:
- [ ] **2.3.3.1** CPU backend cancellation (2 days)
  - Check flag before I/O
  - Check flag after I/O
  - Skip callback if cancelled

- [ ] **2.3.3.2** Vulkan backend cancellation (2 days)
  - Check flag before dispatch
  - Check flag in completion
  - Handle in-flight GPU work

- [ ] **2.3.3.3** io_uring backend cancellation (2 days)
  - Cancel pending SQEs
  - Handle in-flight operations
  - Integration with worker threads

- [ ] **2.3.3.4** Comprehensive testing (3 days)
  - Test all backends
  - Race condition stress tests
  - Performance impact measurement
  - Add `tests/cancellation_backend_test.cpp`

- [ ] **2.3.3.5** Documentation (1 day)
  - API documentation
  - Usage examples
  - Cancellation guarantees

**Deliverable**: Full cancellation support

**Phase 2.3 Milestone**: Request cancellation implemented

---

**Phase 2 Summary**:
- Week 9-13: GDeflate CPU implementation
- Week 9-14: io_uring multi-worker (parallel)
- Week 15-18: Request cancellation
- All components tested independently

---

### PHASE 3: Advanced Features (Weeks 19-28)

**Goal**: GPU acceleration, advanced optimizations

#### Phase 3.1: GDeflate GPU Implementation (Weeks 19-24)
**Owner**: GPU Compute Engineer  
**Priority**: High  
**Dependencies**: Phase 1.2 (Vulkan compute) + Phase 2.1 (GDeflate CPU)

**Sub-Phase 3.1.1: GPU Shader Development (Weeks 19-21)**

**Microtasks**:
- [ ] **3.1.1.1** Design GPU decompression algorithm (2 days)
  - Block-parallel approach
  - Workgroup size selection
  - Memory layout

- [ ] **3.1.1.2** Implement DEFLATE decode shader (5 days)
  - Huffman decoding (GPU-friendly)
  - LZ77 back-reference handling
  - Shared memory optimization
  - Create `shaders/gdeflate_decompress.comp`

- [ ] **3.1.1.3** Test shader in isolation (3 days)
  - Standalone shader test harness
  - Known input/output validation
  - Performance profiling

- [ ] **3.1.1.4** Optimize shader (3 days)
  - Reduce divergence
  - Coalesced memory access
  - Wavefront/warp efficiency

**Deliverable**: GDeflate GPU shader

**Sub-Phase 3.1.2: Backend Integration (Weeks 22-24)**

**Microtasks**:
- [ ] **3.1.2.1** Create GDeflate compute pipeline (1 day)
  - Load shader
  - Configure descriptor layout
  - Add to VulkanBackend

- [ ] **3.1.2.2** Implement GPU decompression dispatch (3 days)
  - Parse block headers on CPU
  - Upload metadata to GPU
  - Dispatch compute workgroups
  - Synchronization

- [ ] **3.1.2.3** Implement CPU/GPU hybrid strategy (2 days)
  - Heuristic for CPU vs GPU
  - Configuration options
  - Fallback logic

- [ ] **3.1.2.4** Test GPU decompression (4 days)
  - Correctness tests
  - Performance benchmarks
  - Comparison vs CPU
  - Add `tests/gdeflate_gpu_test.cpp`

- [ ] **3.1.2.5** Optimize performance (3 days)
  - Profile GPU execution
  - Reduce bottlenecks
  - Tune workgroup sizes

- [ ] **3.1.2.6** Documentation (2 days)
  - GPU requirements
  - Performance characteristics
  - Troubleshooting

**Deliverable**: GDeflate GPU decompression

**Phase 3.1 Milestone**: GPU-accelerated decompression working

---

#### Phase 3.2: GPU-Resident Workflows (Weeks 25-28)
**Owner**: GPU Optimization Engineer  
**Priority**: Medium  
**Dependencies**: Phase 3.1 (GDeflate GPU)

**Sub-Phase 3.2.1: Memory Optimization (Weeks 25-26)**

**Microtasks**:
- [ ] **3.2.1.1** Implement staging buffer pooling (2 days)
  - Buffer pool allocator
  - Reuse across requests
  - Size-based bins

- [ ] **3.2.1.2** Implement async staging copies (2 days)
  - Don't block on staging → GPU
  - Pipeline multiple transfers
  - Synchronization

- [ ] **3.2.1.3** Optimize GPU buffer management (2 days)
  - Reduce allocation frequency
  - Suballocation strategy
  - Memory defragmentation

- [ ] **3.2.1.4** Test memory optimizations (2 days)
  - Memory usage profiling
  - Performance impact
  - Stress tests

**Deliverable**: Optimized memory management

**Sub-Phase 3.2.2: Advanced GPU Paths (Weeks 27-28)
**Microtasks**:
- [ ] **3.2.2.1** Investigate GPUDirect Storage (2 days)
  - NVIDIA GDS API review
  - Feasibility assessment
  - Prototype (if viable)

- [ ] **3.2.2.2** Implement GPU-to-GPU optimization (2 days)
  - Compressed buffer → decompressed buffer
  - Single command buffer
  - No CPU involvement

- [ ] **3.2.2.3** Batch GPU operations (2 days)
  - Multiple decompressions per dispatch
  - Amortize overhead
  - Descriptor set reuse

- [ ] **3.2.2.4** Performance testing (2 days)
  - Benchmark GPU workflows
  - Compare optimization stages
  - Real-world asset tests

**Deliverable**: Optimized GPU-resident workflows

**Phase 3.2 Milestone**: GPU workflows optimized

---

**Phase 3 Summary**:
- Weeks 19-24: GDeflate GPU implementation
- Weeks 25-28: GPU workflow optimization
- All GPU features complete

---

### PHASE 4: Wine/Proton Integration (Weeks 29-36)

**Goal**: Enable DirectStorage games on Linux via Proton

#### Phase 4.1: Shim Development (Weeks 29-32)
**Owner**: Wine Integration Engineer  
**Priority**: High  
**Dependencies**: Phase 2.1 (GDeflate CPU), Phase 1.2 (Vulkan compute)

**Sub-Phase 4.1.1: Shim Skeleton (Week 29)**

**Microtasks**:
- [ ] **4.1.1.1** Create dstorage.dll directory structure (1 day)
  - Set up Wine dlls/dstorage
  - Makefile.in, .spec files
  - Basic build infrastructure

- [ ] **4.1.1.2** Implement DStorageGetFactory (1 day)
  - COM object creation
  - Reference counting
  - Error handling

- [ ] **4.1.1.3** Implement skeleton COM interfaces (2 days)
  - IDStorageFactory
  - IDStorageQueue
  - IDStorageFile
  - Basic vtable setup

- [ ] **4.1.1.4** Test shim loads (1 day)
  - DLL registration
  - COM object creation
  - Basic smoke test

**Deliverable**: dstorage.dll skeleton

**Sub-Phase 4.1.2: Type Mapping (Weeks 30-31)**

**Microtasks**:
- [ ] **4.1.2.1** Implement request descriptor translation (2 days)
  - DSTORAGE_REQUEST → ds_request
  - Field-by-field mapping
  - Validation

- [ ] **4.1.2.2** Implement handle conversion (1 day)
  - Windows HANDLE → Linux fd
  - File handle management
  - Reference counting

- [ ] **4.1.2.3** Implement D3D12 → Vulkan interop (3 days)
  - Get VkDevice from ID3D12Device
  - Get VkBuffer from ID3D12Resource
  - vkd3d-proton integration

- [ ] **4.1.2.4** Implement compression format mapping (1 day)
  - DSTORAGE_COMPRESSION → ds_compression_t
  - Enum translation
  - Validation

- [ ] **4.1.2.5** Test type conversions (2 days)
  - Unit tests for all mappings
  - Edge cases
  - Error handling

**Deliverable**: Complete type mapping

**Sub-Phase 4.1.3: Queue Implementation (Week 32)**

**Microtasks**:
- [ ] **4.1.3.1** Implement IDStorageFactory::CreateQueue (2 days)
  - Create ds_queue via C ABI
  - Wrap in COM object
  - Backend selection logic

- [ ] **4.1.3.2** Implement IDStorageQueue::EnqueueRequest (2 days)
  - Translate request
  - Forward to ds_queue_enqueue
  - Error handling

- [ ] **4.1.3.3** Implement IDStorageQueue::Submit (1 day)
  - Call ds_queue_submit_all
  - Synchronization

- [ ] **4.1.3.4** Implement completion signaling (1 day)
  - IDStorageQueue::EnqueueSignal
  - Map to callbacks/fences
  - Event notification

**Deliverable**: Functional queue implementation

---

#### Phase 4.2: Testing and Integration (Weeks 33-36)
**Owner**: QA/Integration Engineer  
**Priority**: Critical  
**Dependencies**: Phase 4.1 complete

**Sub-Phase 4.2.1: Integration Testing (Weeks 33-34)**

**Microtasks**:
- [ ] **4.2.1.1** Create simple test app (2 days)
  - Minimal DirectStorage usage
  - File read with DirectStorage API
  - Verify data correctness

- [ ] **4.2.1.2** Test with Proton (3 days)
  - Configure Wine environment
  - Test shim loading
  - Debug integration issues

- [ ] **4.2.1.3** Test Vulkan device sharing (2 days)
  - Verify VkDevice passed correctly
  - Test GPU transfers
  - Synchronization validation

- [ ] **4.2.1.4** Performance baseline (2 days)
  - Measure overhead
  - Compare Windows vs Linux
  - Identify bottlenecks

**Deliverable**: Integration test suite

**Sub-Phase 4.2.2: Real Game Testing (Weeks 35-36)**

**Microtasks**:
- [ ] **4.2.2.1** Test Forspoken (if available) (3 days)
  - Launch game via Proton
  - Verify asset loading
  - Performance testing
  - Bug fixing

- [ ] **4.2.2.2** Test other DirectStorage titles (3 days)
  - Ratchet & Clank
  - UE5 games
  - Identify compatibility issues

- [ ] **4.2.2.3** Performance optimization (3 days)
  - Profile hot paths
  - Optimize type conversions
  - Reduce overhead

- [ ] **4.2.2.4** Bug fixing and polish (3 days)
  - Address discovered issues
  - Stability improvements
  - Memory leak fixes

**Deliverable**: Stable Wine/Proton integration

**Sub-Phase 4.2.3: Documentation (Week 36)**

**Microtasks**:
- [ ] **4.2.3.1** Write integration guide (2 days)
  - Build instructions
  - Configuration
  - Troubleshooting

- [ ] **4.2.3.2** Document known issues (1 day)
  - Compatibility list
  - Workarounds
  - Performance notes

- [ ] **4.2.3.3** Create developer guide (1 day)
  - Debugging tips
  - Contributing guidelines
  - Testing procedures

- [ ] **4.2.3.4** Update project documentation (1 day)
  - README.md
  - ROADMAP.md
  - Release notes

**Deliverable**: Complete documentation

**Phase 4 Milestone**: Wine/Proton integration complete

---

## 3. Fast Track Option (12 Weeks MVP)

**Goal**: Minimal viable product for testing

**Included**:
- ✅ CPU backend (already working)
- ⏩ GDeflate CPU (Weeks 1-5)
- ⏩ Vulkan compute (Weeks 1-8, parallel)
- ⏩ Basic Wine shim (Weeks 9-12)

**Excluded**:
- ❌ GDeflate GPU (defer)
- ❌ io_uring multi-worker (defer)
- ❌ Request cancellation (defer)
- ❌ GPU optimizations (defer)
- ❌ Advanced Wine integration (defer)

**Timeline**: 12 weeks to functional MVP

---

## 4. Testing Strategy

### 4.1 Continuous Testing

**Per Phase**:
- Unit tests for all new components
- Integration tests after backend changes
- Performance benchmarks
- Memory leak testing (valgrind)

### 4.2 Test Suites

**New Test Files**:
1. `tests/gdeflate_format_test.cpp`
2. `tests/gdeflate_decompress_test.cpp`
3. `tests/gdeflate_gpu_test.cpp`
4. `tests/vulkan_compute_test.cpp`
5. `tests/io_uring_multi_worker_test.cpp`
6. `tests/cancellation_test.cpp`
7. `tests/wine_integration_test.cpp`

**Existing Tests** (update as needed):
- `tests/basic_queue_test.cpp`
- `tests/cpu_backend_test.cpp`
- `tests/error_handling_test.cpp`
- `tests/compression_gdeflate_stub_test.cpp` (change to success test)

### 4.3 Validation

**Every Phase**:
- [ ] All tests pass
- [ ] No memory leaks (valgrind clean)
- [ ] Vulkan validation layers pass
- [ ] Performance meets targets
- [ ] Documentation updated

---

## 5. Success Criteria

### 5.1 Functional Requirements

**Core**:
- ✅ GDeflate compression works (CPU and GPU)
- ✅ Vulkan GPU compute pipelines functional
- ✅ io_uring backend production-ready
- ✅ Request cancellation implemented
- ✅ GPU-resident workflows optimized
- ✅ Wine/Proton integration working

**Quality**:
- ✅ No memory leaks
- ✅ Thread-safe
- ✅ Vulkan validation clean
- ✅ Comprehensive test coverage (≥80%)
- ✅ Documentation complete

### 5.2 Performance Targets

| Metric | Target | Method |
|--------|--------|--------|
| GDeflate CPU | ≥ 500 MB/s | Benchmark decompression |
| GDeflate GPU | ≥ 2 GB/s | Benchmark decompression |
| io_uring throughput | ≥ 2x CPU backend | File I/O benchmark |
| Vulkan compute overhead | < 100 µs/dispatch | Profiling |
| Wine/Proton overhead | < 10% vs native | Game benchmarks |

### 5.3 Platform Requirements

**Verified On**:
- CachyOS (Linux kernel 5.15+)
- Arch Linux (latest)
- AMD GPU (RADV driver)
- NVIDIA GPU (proprietary driver)
- Intel GPU (ANV driver)

---

## 6. Risk Management

### 6.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| GDeflate format unavailable | Medium | High | Reverse engineer, community help |
| GPU shader too slow | Low | Medium | Optimize, fallback to CPU |
| Wine integration complex | High | High | Start simple, iterate, seek Wine dev help |
| Hardware compatibility | Medium | High | Test multiple GPUs, provide fallbacks |
| liburing unavailable | Low | Low | CPU/Vulkan backends work without it |

### 6.2 Schedule Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| GDeflate research delay | +4 weeks | Start GPU work in parallel |
| Vulkan debugging difficult | +2 weeks | Use validation layers, RenderDoc |
| Wine upstreaming slow | +8 weeks | Maintain out-of-tree, focus on functionality |
| Testing reveals bugs | +4 weeks | Buffer time, incremental fixes |

### 6.3 Resource Risks

**Assumptions**:
- Single developer (can parallelize to some extent)
- Access to CachyOS/Arch Linux system
- Access to Vulkan-capable GPU
- Access to DirectStorage test games (for Phase 4)

**Mitigation**:
- Prioritize critical path
- Use community resources (Wine forums, GitHub)
- Parallelize independent work
- MVP approach if resource-constrained

---

## 7. Deliverables

### 7.1 Code

**New Files**:
- `src/gdeflate_decoder.cpp` - GDeflate CPU implementation
- `shaders/gdeflate_decompress.comp` - GDeflate GPU shader
- `shaders/buffer_copy.comp` - Example compute shader
- `shaders/uppercase.comp` - Transform shader
- 7+ new test files

**Modified Files**:
- `src/ds_runtime.cpp` - Remove GDeflate stub, add cancellation
- `src/ds_runtime_vulkan.cpp` - Add compute pipelines
- `src/ds_runtime_uring.cpp` - Multi-worker support
- `include/ds_runtime.hpp` - API additions (cancellation, IDs)

**External** (Wine tree):
- `dlls/dstorage/*` - Wine shim DLL

### 7.2 Documentation

**New Documents**:
- ✅ `docs/investigation_gdeflate.md` (complete)
- ✅ `docs/investigation_vulkan_compute.md` (complete)
- ✅ `docs/investigation_io_uring.md` (complete)
- ✅ `docs/investigation_remaining_features.md` (complete)
- ✅ `docs/master_roadmap.md` (this document)
- `docs/gdeflate_format.md` (Phase 1)
- `docs/gdeflate_usage.md` (Phase 2)
- `docs/wine_integration_guide.md` (Phase 4)

**Updated Documents**:
- `README.md` - Update status, features
- `MISSING_FEATURES.md` - Mark completed
- `COMPARISON.md` - Update comparisons
- `docs/design.md` - Reflect implementation

### 7.3 Tests

**Comprehensive Test Suite**:
- 12+ test executables (4 existing + 8 new)
- 100% coverage of new features
- Performance benchmarks
- Integration tests
- Wine shim tests

---

## 8. Progress Tracking

### 8.1 Weekly Milestones

| Week | Milestone | Phase |
|------|-----------|-------|
| 3 | GDeflate format understood | 1.1 |
| 2 | Shader loading works | 1.2.1 |
| 4 | Descriptor system works | 1.2.2 |
| 6 | Compute pipelines created | 1.2.3 |
| 8 | Compute dispatch working | 1.2.4 |
| 10 | GDeflate header parser done | 2.1.1 |
| 11 | DEFLATE integration complete | 2.1.2 |
| 13 | GDeflate CPU working | 2.1.3 |
| 14 | io_uring multi-worker done | 2.2 |
| 18 | Request cancellation done | 2.3 |
| 24 | GDeflate GPU working | 3.1 |
| 28 | GPU workflows optimized | 3.2 |
| 32 | Wine shim functional | 4.1 |
| 36 | Full integration complete | 4.2 |

### 8.2 Reporting

**Frequency**: Weekly  
**Format**: GitHub PR updates via `report_progress` tool

**Include**:
- Completed microtasks (✅)
- In-progress tasks (⏩)
- Blockers/issues
- Test results
- Performance metrics

---

## 9. Next Actions

### Immediate (Week 1)

**This Week**:
1. ✅ Complete investigation documents (done)
2. ✅ Report progress with master plan (in progress)
3. ⏩ Begin GDeflate format research
4. ⏩ Start Vulkan shader loading implementation
5. ⏩ Set up development environment (liburing, Vulkan SDK)

### Short Term (Weeks 2-4)

**Focus**:
- Continue GDeflate research
- Complete Vulkan shader module system
- Begin descriptor management
- Create test infrastructure

### Medium Term (Weeks 5-12)

**Focus**:
- Complete GDeflate CPU implementation
- Finish Vulkan compute pipelines
- Begin io_uring multi-worker
- Test all components independently

### Long Term (Weeks 13-36)

**Focus**:
- GPU acceleration (GDeflate, workflows)
- Wine/Proton integration
- Real game testing
- Performance optimization
- Documentation and polish

---

## 10. Conclusion

This master roadmap provides a comprehensive plan for completing the ds-runtime project with extensive subphasing and microtasking. The phased approach allows for:

- **Incremental progress**: Small, verifiable steps
- **Parallel work**: Independent features can progress simultaneously
- **Risk management**: Early identification of issues
- **Flexibility**: Can adjust scope (MVP vs full implementation)
- **Clear milestones**: Weekly checkpoints for progress tracking

**Full Implementation**: 36 weeks (9 months)  
**MVP**: 12 weeks (3 months)

The project is well-positioned with a solid foundation (CPU backend working, tests passing). The investigation phase has identified all requirements and dependencies. Execution can begin immediately on multiple parallel tracks (GDeflate research + Vulkan compute).

---

**Document Status**: Complete v1.0  
**Last Updated**: 2026-02-16  
**Next Update**: Weekly progress reports
