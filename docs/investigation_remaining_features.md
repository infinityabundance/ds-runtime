# Request Cancellation, GPU-Resident Workflows, and Wine/Proton Integration

**Status:** Planning Phase  
**Priority:** Medium to High  
**Target:** Complete feature set for production DirectStorage-style runtime

---

## Part 1: Request Cancellation

### 1.1 Current State

**What Exists**:
- `RequestStatus` enum with values: `Pending`, `InProgress`, `Complete`, `Failed`
- No `Cancelled` status
- No `cancel()` method on Queue
- No cancellation support in backends

**What's Missing**:
- ❌ `RequestStatus::Cancelled` enum value
- ❌ `Queue::cancel_request(request_id)` method
- ❌ In-flight request tracking for cancellation
- ❌ Backend cancellation hooks
- ❌ Race condition handling (completion vs cancellation)

### 1.2 Design Requirements

**Use Cases**:
1. **Timeout**: Cancel requests that take too long
2. **User Action**: User cancels loading operation
3. **Priority Change**: Cancel low-priority work to start high-priority
4. **Shutdown**: Cancel all in-flight requests on cleanup

**Semantics**:
```cpp
// Strong guarantee: Request will not complete after cancel
bool cancel(request_id);

// Weak guarantee: Request may complete, but won't invoke callback
bool try_cancel(request_id);
```

### 1.3 Implementation Plan

#### Phase 1: API Design

**Add to Request**:
```cpp
struct Request {
    // Existing fields...
    std::atomic<bool> cancellation_requested = false;
    request_id_t id = 0; // Unique ID for tracking
};
```

**Add to Queue**:
```cpp
class Queue {
public:
    // Cancel specific request (returns true if cancelled before completion)
    bool cancel_request(request_id_t id);
    
    // Cancel all pending requests (not yet submitted)
    size_t cancel_all_pending();
    
    // Cancel all requests (including in-flight)
    size_t cancel_all();
};
```

**Add Status**:
```cpp
enum class RequestStatus {
    Pending,
    InProgress,
    Complete,
    Failed,
    Cancelled  // NEW
};
```

#### Phase 2: Queue Implementation

**Request Tracking**:
```cpp
class Queue::Impl {
    std::unordered_map<request_id_t, Request*> active_requests_;
    std::mutex active_mutex_;
    std::atomic<request_id_t> next_id_{1};
};

void Queue::enqueue(Request& req) {
    req.id = impl_->next_id_.fetch_add(1);
    {
        std::lock_guard lock(impl_->active_mutex_);
        impl_->active_requests_[req.id] = &req;
    }
    // ... existing enqueue logic
}
```

**Cancellation**:
```cpp
bool Queue::cancel_request(request_id_t id) {
    std::lock_guard lock(impl_->active_mutex_);
    
    auto it = impl_->active_requests_.find(id);
    if (it == impl_->active_requests_.end()) {
        return false; // Already completed or never existed
    }
    
    Request* req = it->second;
    
    // Mark as cancellation requested
    req->cancellation_requested.store(true, std::memory_order_release);
    
    // If still pending (not submitted), remove immediately
    if (req->status == RequestStatus::Pending) {
        req->status = RequestStatus::Cancelled;
        impl_->active_requests_.erase(it);
        return true;
    }
    
    // If in-flight, backend must handle cancellation
    // Return false to indicate "in progress, might complete"
    return false;
}
```

#### Phase 3: Backend Support

**CPU Backend**:
```cpp
void CpuBackend::process_request(Request& req) {
    // Check cancellation before I/O
    if (req.cancellation_requested.load(std::memory_order_acquire)) {
        req.status = RequestStatus::Cancelled;
        req.callback(&req);
        return;
    }
    
    // Perform I/O
    ssize_t bytes = pread(req.fd, req.dst, req.size, req.offset);
    
    // Check cancellation after I/O (before callback)
    if (req.cancellation_requested.load(std::memory_order_acquire)) {
        req.status = RequestStatus::Cancelled;
        req.callback(&req);
        return;
    }
    
    // Normal completion
    req.status = RequestStatus::Complete;
    req.callback(&req);
}
```

**Vulkan Backend**:
```cpp
// Harder to cancel GPU work in progress
// Strategy: Don't invoke callback if cancelled
void VulkanBackend::complete_request(Request& req) {
    if (req.cancellation_requested.load(std::memory_order_acquire)) {
        req.status = RequestStatus::Cancelled;
    }
    
    req.callback(&req);
}
```

**io_uring Backend**:
```cpp
// Can cancel SQE before submission
bool IoUringBackend::cancel_sqe(Request& req) {
    // Remove from pending queue if not yet submitted
    std::lock_guard lock(pending_mutex_);
    auto it = std::find(pending_.begin(), pending_.end(), &req);
    if (it != pending_.end()) {
        pending_.erase(it);
        req.status = RequestStatus::Cancelled;
        return true;
    }
    return false; // Already submitted
}
```

### 1.4 Testing

**Test Cases**:
1. Cancel pending request (before submit)
2. Cancel in-flight request (during I/O)
3. Cancel completed request (should fail)
4. Cancel non-existent request (should fail)
5. Race: cancel vs completion
6. Cancel all requests
7. Callback not invoked for cancelled request

**Test File**: `tests/cancellation_test.cpp`

### 1.5 Timeline

**2-3 weeks** for complete cancellation support

---

## Part 2: GPU-Resident Workflows

### 2.1 Motivation

**Goal**: Zero-copy disk → GPU data path

**Traditional Path** (current):
```
Disk → Host Staging Buffer → GPU Buffer
       [copy 1]              [copy 2]
```

**GPU-Resident Path** (target):
```
Disk → GPU Buffer (direct)
       [copy 1 only]
```

### 2.2 DirectStorage GPU Upload Heap

**Microsoft DirectStorage Concept**:
- GPU upload heap: CPU-visible, GPU-accessible memory
- Direct writes from storage controller to GPU memory
- Requires hardware support (PCIe peer-to-peer, GPU Direct Storage)

**Linux Equivalent**:
- **NVIDIA GPUDirect Storage**: Kernel driver enables direct NVMe → GPU transfers
- **AMD equivalent**: DirectGMA (less documented)
- **Standard Vulkan**: No direct disk → GPU (must use staging)

### 2.3 Implementation Strategies

#### Strategy 1: Vulkan External Memory (Current)

**Approach**: Staging buffer + GPU copy (already implemented)

**Pros**:
- Works on all Vulkan hardware
- Portable across vendors
- Already implemented

**Cons**:
- Extra copy (staging → GPU)
- Higher latency
- More memory usage

#### Strategy 2: GPU Direct Storage Integration

**Approach**: Integrate with vendor-specific APIs

**NVIDIA GPUDirect Storage**:
```cpp
// Open file with GDS flags
int fd = open(path, O_RDONLY | O_DIRECT);

// Register GPU buffer with GDS
cuFileDriverOpen();
CUfileHandle_t handle;
cuFileHandleRegister(&handle, &cufile_desc);

// Direct read to GPU memory
cuFileRead(handle, gpu_buffer, size, offset, 0);
```

**Pros**:
- Zero extra copies
- Lowest latency
- Highest throughput

**Cons**:
- NVIDIA-only (no AMD/Intel equivalent)
- Requires special driver setup
- O_DIRECT alignment requirements
- Complex integration

#### Strategy 3: Memory-Mapped Files + GPU Upload

**Approach**: mmap file, map to GPU upload heap

**Implementation**:
```cpp
// Map file to host memory
void* mapped = mmap(nullptr, file_size, PROT_READ, 
                   MAP_SHARED, fd, 0);

// Allocate GPU upload heap (CPU-visible, GPU-accessible)
VkBuffer upload_buffer = create_upload_buffer(device);
void* gpu_mapped = map_buffer(upload_buffer);

// Copy file data to upload heap
memcpy(gpu_mapped, mapped, file_size);

// Unmap
munmap(mapped, file_size);
unmap_buffer(upload_buffer);

// Use upload buffer directly in GPU (no staging copy needed)
```

**Pros**:
- Simpler than GDS
- Works across vendors
- Reduces staging buffer usage

**Cons**:
- Still one copy (mmap → GPU)
- Page cache overhead
- Not true "direct to GPU"

### 2.4 Recommended Approach

**Phase 1: Optimize Current Path**
- Reuse staging buffers (pool)
- Async staging → GPU copy (don't wait)
- Batch multiple requests

**Phase 2: Vendor-Specific Paths** (Optional)
- Add GDS backend for NVIDIA
- Conditional compilation (#ifdef NVIDIA_GDS)
- Fallback to standard path

**Phase 3: Future Hardware**
- Wait for standardized GPU Direct Storage in Vulkan
- Integrate when available

### 2.5 GPU-to-GPU Transfers

**Use Case**: Texture decompression GPU → GPU

**Current Path**:
```
Disk → Staging → GPU Compressed Buffer → GPU Decompressed Buffer
                 [compute shader]
```

**Optimization**:
```
Disk → GPU Compressed Buffer → GPU Decompressed Buffer
                               [single command buffer]
```

**Implementation**: Already supported via Vulkan backend + compute pipelines

### 2.6 Testing

**Benchmarks**:
- Staging vs direct (if GDS available)
- Throughput (MB/s)
- Latency (ms)
- CPU overhead (%)

**Validation**:
- Data integrity (checksums)
- Memory usage
- GPU utilization

### 2.7 Timeline

**Phase 1 (Optimization)**: 2 weeks  
**Phase 2 (GDS Integration)**: 4-6 weeks (if needed)

---

## Part 3: Wine/Proton Integration

### 3.1 Architecture Overview

**Goal**: Enable Windows DirectStorage games to run on Linux via Proton

**Strategy**:
```
Windows Game (DirectStorage API)
  ↓
Wine/Proton dstorage.dll Shim
  ↓ (translate calls)
ds-runtime (Linux native)
  ↓ (execute)
Linux Kernel (io_uring, Vulkan)
```

### 3.2 Integration Approaches

#### Approach 1: PE DLL Shim (Recommended)

**Architecture**:
```
dstorage.dll (PE) - Windows ABI
  ↓ dlopen
libds_runtime.so - Linux ABI
```

**Implementation**:
1. Create `dstorage.dll` (Wine builtin DLL)
2. Implement DirectStorage API entry points
3. Forward to `libds_runtime.so` via C ABI
4. Translate types (HANDLE → fd, etc.)

**Example**:
```cpp
// dstorage.dll (Wine)
HRESULT WINAPI DStorageCreateQueue(
    const DSTORAGE_QUEUE_DESC* desc,
    REFIID riid,
    void** ppv
) {
    // Load libds_runtime.so
    void* handle = dlopen("libds_runtime.so", RTLD_NOW);
    
    // Get C API functions
    auto ds_create_queue = (ds_queue_t* (*)(ds_backend_t*))
        dlsym(handle, "ds_create_queue");
    
    // Create backend
    ds_backend_t* backend = ds_make_cpu_backend();
    
    // Create queue
    ds_queue_t* queue = ds_create_queue(backend);
    
    // Wrap in COM object
    *ppv = new DStorageQueueImpl(queue);
    return S_OK;
}
```

#### Approach 2: Direct Integration (No Shim)

**Architecture**:
```
Wine/Proton DirectStorage Implementation
  ↓ (link directly)
libds_runtime_static.a
```

**Implementation**:
1. Build ds-runtime as static library
2. Link into Wine dlls/dstorage build
3. Call C++ API directly (no PE/ELF bridge)
4. Share Vulkan device with vkd3d-proton

**Pros**:
- No dlopen overhead
- Simpler debugging
- Shared Vulkan context

**Cons**:
- Tighter coupling
- Requires Wine build modifications

#### Approach 3: Kernel Module (Experimental)

**Architecture**:
```
DirectStorage Requests
  ↓
ioctl to kernel module
  ↓
Kernel-side I/O handling
```

**Not Recommended**: Too complex, overkill for userspace I/O

### 3.3 Type Mapping

**Windows → Linux Translation**:

| Windows Type | Linux Type | Conversion |
|--------------|------------|------------|
| `HANDLE` | `int` | `fd = _open_osfhandle(handle)` |
| `DSTORAGE_REQUEST` | `ds_request` | Struct field mapping |
| `ID3D12Resource*` | `VkBuffer` | vkd3d-proton interop |
| `DSTORAGE_COMPRESSION` | `ds_compression_t` | Enum mapping |
| `OVERLAPPED` | Completion callback | Async model |

**Example Struct Mapping**:
```cpp
void translate_request(
    const DSTORAGE_REQUEST_DESC* windows_req,
    ds_request* linux_req
) {
    linux_req->fd = get_fd_from_handle(windows_req->Source.File.Handle);
    linux_req->offset = windows_req->Source.File.Offset;
    linux_req->size = windows_req->Source.File.Size;
    linux_req->dst = get_buffer_pointer(windows_req->Destination);
    linux_req->op = (windows_req->DestinationType == DSTORAGE_REQUEST_DESTINATION_MEMORY)
        ? DS_REQUEST_OP_READ : DS_REQUEST_OP_WRITE;
    linux_req->compression = translate_compression(
        windows_req->CompressionFormat
    );
}
```

### 3.4 Vulkan Device Sharing

**Challenge**: DirectStorage expects D3D12 device, we need Vulkan

**Solution**: vkd3d-proton already handles D3D12 → Vulkan translation

**Integration**:
```cpp
// Get Vulkan device from vkd3d-proton
VkDevice vk_device = vkd3d_get_vk_device(d3d12_device);
VkQueue vk_queue = vkd3d_get_vk_queue(d3d12_device);

// Create ds-runtime Vulkan backend with shared device
ds_vulkan_backend_config config;
config.device = vk_device;
config.queue = vk_queue;
config.take_ownership = false; // Don't destroy device

ds_backend_t* backend = ds_make_vulkan_backend(&config);
```

### 3.5 Implementation Steps

#### Step 1: C ABI Wrapper (Already Exists)

**Status**: ✅ Complete
- `include/ds_runtime_c.h` provides C API
- Type conversions implemented
- Tested with `c_abi_stats_test.c`

#### Step 2: Create dstorage.dll Skeleton

**Location**: Outside ds-runtime repo (in Wine tree)

**Files**:
```
dlls/dstorage/
├── Makefile.in
├── dstorage.spec
├── dstorage_main.c
├── queue.c
└── request.c
```

**Implement**:
- `DStorageGetFactory`
- `DStorageSetConfiguration`
- `IDStorageFactory::CreateQueue`
- `IDStorageQueue::EnqueueRequest`
- `IDStorageQueue::Submit`
- `IDStorageQueue::EnqueueSignal`

#### Step 3: Link with ds-runtime

**Option A: Dynamic Linking**
```makefile
EXTRADLLFLAGS = -Wl,--no-undefined
EXTRALIBS = -lds_runtime
```

**Option B: Static Linking**
```makefile
EXTRALIBS = $(LIBDS_RUNTIME_STATIC)
```

#### Step 4: Test with Real Games

**Test Titles**:
- Forspoken (uses DirectStorage)
- Ratchet & Clank: Rift Apart
- Any UE5 game with DirectStorage support

**Validation**:
- Game launches without crashes
- Asset loading works
- Performance acceptable
- No memory leaks

### 3.6 Documentation

**Create**: `docs/wine_integration_guide.md`

**Contents**:
- Build dstorage.dll
- Configure Wine to use builtin override
- Debugging tips
- Performance tuning
- Known issues

### 3.7 Timeline

**Week 1-2: Prototype**
- Create basic dstorage.dll shim
- Implement skeleton COM interfaces
- Test with simple DirectStorage app

**Week 3-4: Type Mapping**
- Implement full type conversion
- Handle edge cases
- Vulkan device sharing

**Week 5-6: Testing**
- Test with real games
- Performance benchmarking
- Bug fixing

**Week 7-8: Polish**
- Documentation
- Error handling
- Wine upstreaming (if desired)

**Total Estimate**: **8 weeks**

---

## Part 4: Master Implementation Roadmap

### 4.1 Dependency Graph

```
GDeflate CPU ━━━┓
                ┃
Vulkan Compute ━╋━━> GDeflate GPU
                ┃
io_uring Multi  ┃
                ┃
Cancellation ━━━╋━━> GPU Workflows
                ┃
                ┗━━> Wine/Proton Integration
```

### 4.2 Phased Implementation

**Phase 1: Foundation** (Weeks 1-8)
- ✅ Initial assessment (complete)
- ⏩ GDeflate research (2 weeks)
- ⏩ Vulkan compute infrastructure (8 weeks, parallel)

**Phase 2: Core Features** (Weeks 9-18)
- ⏩ GDeflate CPU implementation (5 weeks)
- ⏩ io_uring multi-worker (6 weeks, parallel)
- ⏩ Request cancellation (3 weeks, parallel)

**Phase 3: Advanced Features** (Weeks 19-28)
- ⏩ GDeflate GPU implementation (6 weeks)
- ⏩ GPU workflow optimization (4 weeks)

**Phase 4: Integration** (Weeks 29-36)
- ⏩ Wine/Proton shim (8 weeks)
- ⏩ Real game testing
- ⏩ Performance tuning

**Total Timeline**: **36 weeks (9 months)**

### 4.3 Parallelization Opportunities

**Can Work in Parallel**:
- Vulkan compute + GDeflate research
- GDeflate CPU + io_uring enhancements
- GDeflate CPU + cancellation
- GPU workflows + Wine integration

**Must Be Sequential**:
- Vulkan compute → GDeflate GPU
- GDeflate CPU → GDeflate GPU
- Core features → Wine integration

### 4.4 Fast Track Option

**Goal**: Minimal viable product in 12 weeks

**Scope**:
- ✅ CPU backend (working)
- ⏩ GDeflate CPU (5 weeks)
- ⏩ Vulkan compute (8 weeks, start week 1)
- ⏩ Basic Wine shim (3 weeks)
- ❌ Skip: GPU GDeflate, io_uring multi-worker, advanced features

**Timeline**: **12 weeks**

---

## Part 5: Success Criteria

### 5.1 Functional Requirements

**Core**:
- ✅ All features work independently
- ✅ Integration tests pass
- ✅ No regressions in existing functionality
- ✅ Documentation complete

**Performance**:
- ✅ GDeflate CPU: ≥ 500 MB/s
- ✅ GDeflate GPU: ≥ 2 GB/s
- ✅ io_uring: ≥ 2x CPU backend
- ✅ Wine overhead: < 10%

**Quality**:
- ✅ No memory leaks
- ✅ Thread-safe
- ✅ Vulkan validation clean
- ✅ Works on CachyOS/Arch Linux

### 5.2 Wine/Proton Validation

**Required**:
- ✅ At least one DirectStorage game runs
- ✅ Asset loading works correctly
- ✅ Performance within 20% of Windows
- ✅ No crashes or hangs

---

## Part 6: Risk Assessment

### 6.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| GDeflate format unavailable | Medium | High | Reverse engineer, community collaboration |
| GPU compute too slow | Low | Medium | Optimize shaders, fallback to CPU |
| Wine integration complex | High | Medium | Start simple, iterate |
| Hardware incompatibility | Medium | High | Test on multiple GPUs, provide fallbacks |

### 6.2 Timeline Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| GDeflate research longer than expected | +4 weeks | Start GPU work in parallel |
| Wine upstreaming delays | +8 weeks | Maintain out-of-tree fork |
| Testing reveals bugs | +2-4 weeks | Allocate buffer time |

---

## Part 7: Next Actions

### Immediate (This Week)
1. ✅ Complete investigation documents
2. ⏩ Begin GDeflate format research
3. ⏩ Start Vulkan compute implementation
4. ⏩ Install liburing for io_uring testing

### Short Term (Weeks 2-4)
1. ⏩ Implement shader module loading
2. ⏩ Begin GDeflate CPU decoder
3. ⏩ Design cancellation API
4. ⏩ Test io_uring multi-worker prototype

### Medium Term (Weeks 5-12)
1. ⏩ Complete Vulkan compute pipelines
2. ⏩ Finish GDeflate CPU implementation
3. ⏩ Implement request cancellation
4. ⏩ Start Wine shim prototype

---

**Document Status**: Draft v1.0  
**Last Updated**: 2026-02-16  
**Next Review**: After Phase 1 milestones complete
