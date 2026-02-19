# io_uring Backend Investigation and Enhancement Plan

**Status:** Planning Phase  
**Priority:** Medium  
**Target:** Production-ready io_uring backend with full feature support  
**Dependencies:** liburing library

---

## Executive Summary

The ds-runtime project includes an experimental io_uring backend that provides kernel-level asynchronous I/O. This document outlines the investigation, design, and implementation plan for enhancing the io_uring backend to production quality.

---

## 1. Current State

### 1.1 What Exists

**Implementation**: `src/ds_runtime_uring.cpp`

**Current Capabilities**:
- ✅ `io_uring_queue_init()` with 256 queue entries
- ✅ SQE (Submission Queue Entry) submission
- ✅ CQE (Completion Queue Entry) handling
- ✅ Single worker thread
- ✅ Batch submission (`io_uring_submit`)
- ✅ Completion polling (`io_uring_wait_cqe`)
- ✅ Read operations (`io_uring_prep_read`)
- ✅ Write operations (`io_uring_prep_write`)
- ✅ Host memory support

**Configuration**:
```cpp
struct IoUringBackendConfig {
    uint32_t queue_entries = 256;
    uint32_t worker_count = 1;  // Currently unused!
};
```

### 1.2 What's Missing

**Incomplete Features**:
- ❌ **Multi-worker support**: `worker_count` field exists but ignored
- ❌ **GPU memory**: Explicitly rejected with `EINVAL` (host-only)
- ❌ **Decompression**: No compression/decompression handling
- ❌ **Advanced features**: No linked ops, timeouts, or fixed files
- ❌ **Error recovery**: Limited error handling
- ❌ **Performance tuning**: No SQPOLL or IOPOLL modes

**Known Limitations**:
- Host memory only (GPU buffers not supported by io_uring design)
- Single-threaded (no parallelism despite worker_count field)
- Basic error reporting
- No request prioritization

### 1.3 Build Status

**Dependency**: liburing (optional)
- Not built by default (requires `-DDS_RUNTIME_HAS_IO_URING`)
- CMake checks for liburing via pkg-config
- Test suite requires liburing to run

**Current Build**:
```bash
# In sandbox (liburing not found):
cmake .. # io_uring backend NOT built
```

---

## 2. io_uring Background

### 2.1 What is io_uring?

**io_uring** is a Linux kernel asynchronous I/O interface introduced in Linux 5.1.

**Key Benefits**:
- **Zero-copy**: Shared memory between kernel and userspace
- **Batching**: Submit multiple operations at once
- **Low overhead**: Minimal system call overhead
- **Flexible**: Supports many operation types (read, write, fsync, etc.)
- **Modern**: Designed for modern SSD and NVMe performance

**Architecture**:
```
Application
  ↓ (prepare SQEs)
Submission Queue (SQ) - shared memory ring
  ↓ (submit)
Kernel (process I/O asynchronously)
  ↓ (complete)
Completion Queue (CQ) - shared memory ring
  ↓ (reap CQEs)
Application (handle completions)
```

### 2.2 io_uring vs Traditional I/O

| Feature | io_uring | pread/pwrite | POSIX AIO |
|---------|----------|--------------|-----------|
| **Overhead** | Very Low | High (syscall per op) | Medium |
| **Batching** | ✅ Yes | ❌ No | ⚠️ Limited |
| **Zero-copy** | ✅ Yes | ❌ No | ❌ No |
| **Flexibility** | ✅ High | ⚠️ Limited | ⚠️ Limited |
| **Kernel support** | ✅ 5.1+ | ✅ All | ⚠️ Poor |

**Verdict**: io_uring is superior for high-performance async I/O on modern Linux

### 2.3 DirectStorage Relevance

**Alignment with DirectStorage**:
- ✅ Batched I/O submission (queue-based model)
- ✅ Asynchronous completion (callback-driven)
- ✅ Low CPU overhead (kernel handles scheduling)
- ✅ High throughput (optimized for NVMe SSDs)

**Differences**:
- ❌ No GPU memory support (host-only by design)
- ❌ No built-in decompression (must be done in userspace)
- ✅ Linux-native (no Windows compatibility layer)

---

## 3. Enhancement Plan

### 3.1 Phase 1: Multi-Worker Architecture

**Goal**: Honor `worker_count` configuration for parallel I/O

#### Current Architecture

```
Single Worker Thread
  ↓
io_uring instance (256 entries)
  ↓
Kernel I/O processing
```

#### Proposed Architecture

```
Worker Thread 1              Worker Thread N
  ↓                            ↓
io_uring instance 1          io_uring instance N
  ↓                            ↓
Kernel I/O processing (parallel)
```

#### Design Decisions

**Option 1: Multiple io_uring Instances** (Recommended)
- Each worker has own io_uring
- Load balance requests round-robin
- Independent polling threads
- Pros: True parallelism, simple synchronization
- Cons: More kernel resources

**Option 2: Shared io_uring with Thread Pool**
- Single io_uring, multiple polling threads
- Workers compete for CQEs
- Pros: Fewer kernel resources
- Cons: Contention, complex synchronization

**Recommendation**: Option 1 (multiple instances)

#### Implementation

**Data Structure**:
```cpp
struct IoUringWorker {
    std::thread thread;
    io_uring ring;
    std::atomic<bool> running;
    std::queue<Request*> pending;
    std::mutex pending_mutex;
    std::condition_variable pending_cv;
};

class IoUringBackend::Impl {
    std::vector<IoUringWorker> workers_;
    std::atomic<uint32_t> next_worker_; // Round-robin counter
};
```

**Worker Thread**:
```cpp
void worker_loop(IoUringWorker& worker) {
    while (worker.running) {
        // 1. Submit pending requests
        {
            std::unique_lock<std::mutex> lock(worker.pending_mutex);
            while (!worker.pending.empty()) {
                Request* req = worker.pending.front();
                worker.pending.pop();
                
                // Prepare SQE
                io_uring_sqe* sqe = io_uring_get_sqe(&worker.ring);
                if (req->op == RequestOp::Read) {
                    io_uring_prep_read(sqe, req->fd, req->dst, 
                                      req->size, req->offset);
                } else {
                    io_uring_prep_write(sqe, req->fd, req->src, 
                                       req->size, req->offset);
                }
                io_uring_sqe_set_data(sqe, req);
            }
        }
        
        io_uring_submit(&worker.ring);
        
        // 2. Wait for completions
        io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe_timeout(&worker.ring, &cqe, 
                                           &timeout);
        if (ret == 0) {
            // Process completion
            Request* req = static_cast<Request*>(
                io_uring_cqe_get_data(cqe)
            );
            req->status = (cqe->res < 0) ? 
                RequestStatus::Failed : RequestStatus::Complete;
            req->bytes_transferred = (cqe->res > 0) ? cqe->res : 0;
            
            // Invoke callback
            req->callback(req);
            
            io_uring_cqe_seen(&worker.ring, cqe);
        }
    }
}
```

**Load Balancing**:
```cpp
void IoUringBackend::submit_request(Request& req) {
    // Round-robin worker selection
    uint32_t worker_idx = next_worker_.fetch_add(1) % workers_.size();
    IoUringWorker& worker = workers_[worker_idx];
    
    {
        std::lock_guard<std::mutex> lock(worker.pending_mutex);
        worker.pending.push(&req);
    }
    worker.pending_cv.notify_one();
}
```

**Testing**:
- Submit requests to multiple workers
- Verify parallel execution
- Check completion order independence
- Measure throughput scaling with worker count

---

### 3.2 Phase 2: Advanced io_uring Features

**Goal**: Leverage advanced io_uring capabilities for performance

#### Feature 1: SQPOLL Mode

**Description**: Kernel-side submission queue polling (eliminates submit syscall)

**Configuration**:
```cpp
struct io_uring_params params = {};
params.flags = IORING_SETUP_SQPOLL;
params.sq_thread_idle = 1000; // 1 second idle before sleep

io_uring_queue_init_params(256, &ring, &params);
```

**Benefits**:
- No `io_uring_submit()` syscall needed
- Lower latency for high-frequency submissions
- Kernel thread handles polling

**Tradeoffs**:
- Extra kernel thread (CPU overhead when idle)
- Requires CAP_SYS_NICE or io_uring_register_iowq_max_workers

**Use Case**: High-throughput streaming with many small I/Os

#### Feature 2: IOPOLL Mode

**Description**: Kernel polls completion directly from device (bypass interrupts)

**Configuration**:
```cpp
params.flags = IORING_SETUP_IOPOLL;
```

**Benefits**:
- Lower latency on fast NVMe devices
- Reduced interrupt overhead

**Requirements**:
- O_DIRECT file I/O
- Polling-capable storage device

**Use Case**: Ultra-low-latency I/O on NVMe SSDs

#### Feature 3: Fixed Files

**Description**: Register file descriptors to avoid fd lookup overhead

**API**:
```cpp
// Register FDs once
int fds[MAX_FILES];
io_uring_register_files(&ring, fds, MAX_FILES);

// Use registered FD index in SQEs
io_uring_prep_read(sqe, fd_index, buf, size, offset);
sqe->flags |= IOSQE_FIXED_FILE;
```

**Benefits**:
- Eliminates fd table lookup
- ~10-15% latency reduction

**Use Case**: Repeatedly accessing same set of files

#### Feature 4: Linked Operations

**Description**: Chain dependent operations (e.g., read → decompress → write)

**API**:
```cpp
// Read operation
io_uring_prep_read(sqe1, fd, buf, size, offset);
sqe1->flags |= IOSQE_IO_LINK;

// Write operation (only if read succeeds)
io_uring_prep_write(sqe2, fd_out, buf, size, 0);
```

**Benefits**:
- Atomic operation sequences
- Reduced round-trips

**Use Case**: Complex I/O workflows (read-modify-write)

#### Implementation Priority

1. **Fixed Files** (High): Easy to implement, measurable benefit
2. **SQPOLL** (Medium): Good for high throughput, adds complexity
3. **Linked Ops** (Low): Complex, requires workflow redesign
4. **IOPOLL** (Low): Requires O_DIRECT, hardware-specific

---

### 3.3 Phase 3: Compression Integration

**Goal**: Add decompression support to io_uring backend

#### Design Challenge

**Problem**: io_uring is host-only, decompression needs CPU/GPU

**Solutions**:

**Option 1: Hybrid Approach** (Recommended)
```
io_uring (read compressed data)
  ↓
CPU decompression (in worker thread)
  ↓
Completion callback
```

**Option 2: Separate Decompression Queue**
```
io_uring (read compressed data)
  ↓
Enqueue to decompression thread pool
  ↓ (parallel decompression)
Completion callback
```

**Option 3: Reject Compression**
```
if (req.compression != Compression::None) {
    return error(EINVAL, "io_uring backend does not support compression");
}
```

**Recommendation**: Option 1 for CPU compression, Option 3 for GPU (hand off to Vulkan backend)

#### Implementation (Option 1)

```cpp
void worker_loop(IoUringWorker& worker) {
    // After io_uring read completion
    if (cqe->res > 0) {
        Request* req = static_cast<Request*>(
            io_uring_cqe_get_data(cqe)
        );
        
        // Decompress if needed
        if (req->compression != Compression::None) {
            bool success = decompress(req);
            if (!success) {
                req->status = RequestStatus::Failed;
                req->errno_value = EIO;
            }
        }
        
        req->status = RequestStatus::Complete;
        req->callback(req);
    }
}

bool decompress(Request* req) {
    switch (req->compression) {
        case Compression::FakeUppercase:
            return fake_uppercase_transform(req->dst, req->size);
        case Compression::GDeflate:
            return gdeflate_decompress(req->dst, req->size);
        default:
            return true; // No compression
    }
}
```

**Testing**:
- Read compressed file via io_uring
- Verify decompression occurs
- Check performance vs CPU backend
- Ensure no blocking in completion path

---

### 3.4 Phase 4: Error Handling and Resilience

**Goal**: Robust error handling for production use

#### Error Scenarios

1. **EAGAIN** (queue full): Retry or backpressure
2. **EINTR** (interrupted): Retry operation
3. **EIO** (device error): Report to application
4. **EBADF** (bad fd): Validate before submission
5. **Ring setup failure**: Fallback to CPU backend

#### Enhanced Error Handling

```cpp
void handle_cqe_error(Request* req, io_uring_cqe* cqe) {
    int err = -cqe->res;
    
    switch (err) {
        case EAGAIN:
            // Retry operation
            resubmit_request(req);
            break;
            
        case EINTR:
            // Interrupted, retry
            resubmit_request(req);
            break;
            
        case EIO:
        case EBADF:
        case EFAULT:
            // Fatal error, report to application
            req->status = RequestStatus::Failed;
            req->errno_value = err;
            report_request_error("io_uring", "completion", err,
                               strerror(err), *req);
            req->callback(req);
            break;
            
        default:
            // Unknown error
            req->status = RequestStatus::Failed;
            req->errno_value = err;
            report_request_error("io_uring", "completion", err,
                               "Unknown io_uring error", *req);
            req->callback(req);
            break;
    }
}
```

**Retry Logic**:
```cpp
void resubmit_request(Request* req) {
    if (req->retry_count++ >= MAX_RETRIES) {
        req->status = RequestStatus::Failed;
        req->errno_value = ETIMEDOUT;
        req->callback(req);
        return;
    }
    
    // Exponential backoff
    std::this_thread::sleep_for(
        std::chrono::milliseconds(1 << req->retry_count)
    );
    
    // Re-enqueue
    submit_request(*req);
}
```

---

### 3.5 Phase 5: Performance Tuning

**Goal**: Optimize io_uring backend for maximum throughput

#### Tuning Parameters

**Queue Depth**:
```cpp
// Current: 256 entries
// Consider: Configurable based on workload
// - Small files: 128-256
// - Large files: 512-1024
// - Streaming: 2048+
```

**Batch Size**:
```cpp
// Submit multiple SQEs at once
// Amortizes syscall overhead
uint32_t batch_size = 16; // Tune based on request rate
```

**Worker Count**:
```cpp
// Heuristic: Number of CPU cores or storage devices
uint32_t optimal_workers = std::min(
    std::thread::hardware_concurrency(),
    num_storage_devices
);
```

**Polling Interval**:
```cpp
// Balance latency vs CPU usage
struct __kernel_timespec timeout = {
    .tv_sec = 0,
    .tv_nsec = 1000000  // 1ms (tune based on workload)
};
```

#### Benchmarking

**Metrics to Track**:
- Throughput (MB/s, IOPS)
- Latency (p50, p95, p99)
- CPU utilization
- Queue depth utilization
- Syscall count (should be minimal with batching)

**Test Scenarios**:
- Sequential reads (large files)
- Random reads (small files)
- Mixed read/write
- Concurrent requests (stress test)

**Comparison**:
- io_uring vs CPU backend (pread/pwrite)
- Single worker vs multi-worker
- SQPOLL vs non-SQPOLL

---

## 4. GPU Memory Limitation

### 4.1 Why GPU Buffers Don't Work

**Fundamental Limitation**: io_uring operates on host virtual memory
- Kernel I/O subsystem writes to host memory only
- GPU memory (VRAM) is not directly accessible to kernel I/O
- DMA transfers require device-specific drivers

**Workarounds** (Not Implemented):
1. **GPU memory mapping**: Expose GPU memory to host address space (driver-dependent, slow)
2. **Staging buffers**: io_uring → host buffer → GPU copy (defeats purpose)
3. **GPU Direct Storage**: Requires specialized hardware/drivers (NVIDIA GPUDirect Storage)

### 4.2 Recommended Approach

**Strategy**: Reject GPU memory requests, hand off to Vulkan backend

```cpp
void IoUringBackend::submit_request(Request& req) {
    // Check for GPU memory
    if (req.dst_memory == RequestMemory::Gpu || 
        req.src_memory == RequestMemory::Gpu) {
        report_request_error("io_uring", "submit", EINVAL,
            "io_uring backend does not support GPU memory (use Vulkan backend)",
            req);
        req.status = RequestStatus::Failed;
        req.errno_value = EINVAL;
        req.callback(&req);
        return;
    }
    
    // Proceed with host memory I/O
    // ...
}
```

**Documentation**: Clearly state io_uring is host-only backend

---

## 5. Testing Strategy

### 5.1 Unit Tests

**Test Suite**: `tests/io_uring_backend_test.cpp` (already exists)

**Additional Test Cases**:
1. **Multi-worker**: Submit to multiple workers, verify parallelism
2. **High load**: 1000+ concurrent requests
3. **Error injection**: Simulate EAGAIN, EIO, EINTR
4. **Compression**: Read compressed files, verify decompression
5. **Batching**: Submit batches, measure throughput
6. **Cancellation**: Cancel in-flight requests (if supported)

### 5.2 Integration Tests

**Scenarios**:
1. **Asset streaming demo**: Use io_uring backend
2. **Mixed backends**: io_uring for host, Vulkan for GPU
3. **Stress test**: Sustained high I/O rate
4. **Error recovery**: Handle disk full, permission denied

### 5.3 Performance Tests

**Benchmarks**:
- Throughput vs CPU backend
- Scalability with worker count
- Latency distribution
- CPU overhead

**Test Assets**:
- 1MB, 10MB, 100MB files
- Sequential vs random access
- Compressed vs uncompressed

---

## 6. Dependencies

### 6.1 Build System

**liburing Detection**:
```cmake
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBURING liburing)
endif()

if (LIBURING_FOUND)
    list(APPEND DS_RUNTIME_SOURCES src/ds_runtime_uring.cpp)
    target_link_libraries(ds_runtime PUBLIC ${LIBURING_LIBRARIES})
    target_include_directories(ds_runtime PUBLIC ${LIBURING_INCLUDE_DIRS})
    target_compile_definitions(ds_runtime PUBLIC DS_RUNTIME_HAS_IO_URING)
endif()
```

**Installation** (Arch Linux, CachyOS):
```bash
sudo pacman -S liburing
```

### 6.2 Runtime Requirements

**Kernel Version**: Linux 5.1+ (5.10+ recommended for stability)
**Capabilities**: None required for basic use, CAP_SYS_NICE for SQPOLL

---

## 7. Timeline and Milestones

### 7.1 Implementation Phases

**Week 1-2: Multi-Worker**
- Implement worker pool
- Load balancing
- Testing
- **Milestone**: Multi-worker backend functional

**Week 3: Advanced Features**
- Fixed files support
- SQPOLL mode (optional)
- Testing
- **Milestone**: Advanced features working

**Week 4: Compression**
- Integrate decompression
- Testing
- **Milestone**: Compression support

**Week 5-6: Error Handling & Tuning**
- Robust error handling
- Performance tuning
- Benchmarking
- **Milestone**: Production-ready backend

### 7.2 Total Estimate

**6 weeks** for complete io_uring backend enhancement

---

## 8. Success Criteria

### 8.1 Functional Requirements
- ✅ Multi-worker support working
- ✅ All request types supported (read, write)
- ✅ Compression/decompression integrated
- ✅ Error handling robust
- ✅ Existing tests pass
- ✅ New tests pass (100% coverage)

### 8.2 Performance Requirements
- ✅ Throughput ≥ 2x CPU backend (large files)
- ✅ Latency ≤ 0.5x CPU backend
- ✅ CPU overhead ≤ 20% of CPU backend
- ✅ Scales linearly with worker count (up to core count)

### 8.3 Quality Requirements
- ✅ No memory leaks
- ✅ Thread-safe
- ✅ Graceful degradation on error
- ✅ Documentation complete
- ✅ API stability maintained

---

## 9. Next Steps

### Immediate (This Week)
1. ✅ Complete investigation document
2. ⏩ Install liburing in development environment
3. ⏩ Examine current implementation
4. ⏩ Design multi-worker architecture

### Short Term (Next 2 Weeks)
1. ⏩ Implement multi-worker support
2. ⏩ Test parallelism and load balancing
3. ⏩ Benchmark vs CPU backend

### Medium Term (1-2 Months)
1. ⏩ Add advanced features
2. ⏩ Integrate compression
3. ⏩ Performance tuning
4. ⏩ Production-ready release

---

## 10. Open Questions

1. **Worker Count Default**: What's optimal default for `worker_count`?
2. **SQPOLL**: Is SQPOLL worth the complexity for our use case?
3. **Fixed Files**: Should we auto-register frequently accessed files?
4. **GPU Fallback**: Should we automatically hand off GPU requests to Vulkan backend?
5. **Compression**: CPU-only or thread pool for decompression?
6. **Batching**: What's optimal batch size for different workloads?

---

## 11. References

### Documentation
- liburing documentation: https://github.com/axboe/liburing
- io_uring manpages: `man io_uring`
- Kernel documentation: Documentation/io_uring.txt
- Efficient IO with io_uring (Jens Axboe)

### Performance
- io_uring performance analysis
- NVMe optimization guides
- Linux I/O stack deep dive

---

**Document Status**: Draft v1.0  
**Last Updated**: 2026-02-16  
**Next Review**: After liburing installation and multi-worker implementation
