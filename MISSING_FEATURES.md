# DS-Runtime: Missing Features Checklist

**Quick reference for what needs to be implemented**

---

## 🚨 Build-Breaking Issues (MUST FIX FIRST)

- [ ] **Missing Field:** Add `std::size_t bytes_transferred = 0;` to `Request` struct in `include/ds_runtime.hpp`
  - Referenced in: `src/ds_runtime.cpp:298`, `src/ds_runtime.cpp:441`
  - Impact: **Compilation fails**

- [ ] **Missing Method:** Implement `Queue::Impl::take_completed()` in `src/ds_runtime.cpp`
  - Called from: `Queue::take_completed()` at line 546
  - Should: Lock mutex, swap completed_ vector, return snapshot
  - Impact: **Compilation fails**

- [ ] **Missing Update:** Set `req.bytes_transferred = static_cast<std::size_t>(io_bytes);` after successful I/O
  - Location: `src/ds_runtime.cpp` after line 301 (success path)
  - Impact: Stats tracking incomplete

---

## ⚠️ Incomplete Features (Advertised but Partial)

### Vulkan Backend
- [ ] **Compute Pipeline Creation**
  - Missing: `vkCreateComputePipelines()`
  - Status: Only copy operations work, no GPU compute
  
- [ ] **Shader Module Loading**
  - Missing: `vkCreateShaderModule()` to load `copy.comp.spv`
  - File exists: `examples/vk-copy-test/copy.comp.spv` (unused)
  
- [ ] **Descriptor Management**
  - Missing: `vkCreateDescriptorSetLayout()`, `vkCreateDescriptorPool()`
  - Needed for: Binding buffers to compute shaders
  
- [ ] **Compute Dispatch**
  - Missing: `vkCmdBindPipeline()` (compute), `vkCmdDispatch()`
  - Impact: Cannot execute GPU workloads

- [ ] **GPU Decompression**
  - Missing: Any decompression logic in compute shaders
  - Status: Not started

### GDeflate Compression
- [ ] **CPU Decoder Implementation**
  - Status: `Compression::GDeflate` triggers error callback (intentional stub)
  - Test: `tests/compression_gdeflate_stub_test.cpp` verifies it fails
  - Needs: Real decompression algorithm
  
- [ ] **GPU Decoder Implementation**
  - Status: Not started
  - Depends on: Vulkan compute pipeline completion
  
- [ ] **Format Specification**
  - Needs: Microsoft DirectStorage GDeflate format documentation
  - Status: Research required

### io_uring Backend
- [ ] **Multi-Worker Support**
  - Current: Single worker thread only
  - Field exists: `IoUringBackendConfig::worker_count` (unused)
  - Impact: No parallelism
  
- [ ] **GPU Memory Support**
  - Current: Explicitly rejected with `EINVAL`
  - Status: May be impossible (io_uring is host-only by design)
  - Decision needed: Document as limitation?
  
- [ ] **Decompression Support**
  - Current: No compression handling
  - Status: Not started

### Request Management
- [ ] **Request Cancellation**
  - Missing: `RequestStatus::Cancelled` enum value
  - Missing: Cancel method on Queue
  - Impact: Cannot abort in-flight requests

- [ ] **Partial Read Handling**
  - Current: Zero-terminates if fewer bytes read than requested
  - Issue: Only suitable for text, not binary
  - Needs: Proper partial read signaling

---

## ❌ Missing Features (Documented but No Code)

### Wine/Proton Integration
- [ ] **dstorage.dll Shim**
  - Location: Not in repository (documented in `docs/wine_proton.md`)
  - Needs: PE DLL that forwards to `libds_runtime.so`
  - Status: Design documented, no implementation
  
- [ ] **DSTORAGE_REQUEST_DESC Mapping**
  - Needs: Translation layer between Windows and Linux types
  - Status: Not started
  
- [ ] **Wine Integration Testing**
  - Needs: Actual DirectStorage apps tested under Wine
  - Status: Not attempted

### Test Coverage
- [ ] **CPU Backend Happy Path Test**
  - What: End-to-end test with real file I/O
  - Missing: `tests/cpu_backend_test.cpp`
  
- [ ] **Error Handling Test**
  - What: Verify error callbacks fire correctly
  - Missing: `tests/error_reporting_test.cpp`
  
- [ ] **Concurrent Queue Operations Test**
  - What: Multi-threaded enqueue/submit/wait
  - Missing: `tests/queue_concurrent_test.cpp`
  
- [ ] **take_completed() API Test**
  - What: Verify completed request retrieval
  - Missing: Should be added to `basic_queue_test.cpp`
  
- [ ] **bytes_transferred Tracking Test**
  - What: Verify stats are correct
  - Missing: Should be added to `c_abi_stats_test.c`
  
- [ ] **Vulkan Copy Test** (in test suite)
  - What: GPU transfer verification
  - Exists: `examples/vk-copy-test/vk_copy_test.cpp` (not in tests/)
  - Needs: Move to `tests/` directory
  
- [ ] **Request Validation Test**
  - What: Invalid fd, null buffers, zero size
  - Missing: `tests/validation_test.cpp`

### Performance & Benchmarking
- [ ] **Throughput Benchmarks**
  - What: MB/s for CPU, Vulkan, io_uring backends
  - Missing: No benchmark infrastructure
  
- [ ] **Latency Measurements**
  - What: Submission → completion time
  - Missing: No timing code
  
- [ ] **Comparison vs DirectStorage**
  - What: Windows vs Linux performance
  - Missing: No baseline data

### CI/CD
- [ ] **GitHub Actions Workflow**
  - What: Automated build + test on push
  - Missing: `.github/workflows/ci.yml`
  
- [ ] **Multiple Compiler Testing**
  - What: GCC + Clang builds
  - Missing: CI infrastructure
  
- [ ] **Optional Dependency Testing**
  - What: With/without Vulkan, with/without liburing
  - Missing: Build matrix

### Documentation
- [ ] **Known Issues / Limitations**
  - What: List of current bugs and design limits
  - Missing: KNOWN_ISSUES.md or README section
  
- [ ] **API Stability Promise**
  - What: SemVer guarantees, breaking change policy
  - Missing: Documentation
  
- [ ] **Performance Characteristics**
  - What: When to use CPU vs GPU backend
  - Missing: Usage guide
  
- [ ] **Troubleshooting Guide**
  - What: Common errors and solutions
  - Missing: TROUBLESHOOTING.md
  
- [ ] **Changelog**
  - What: Version history
  - Missing: CHANGELOG.md
  
- [ ] **Build Troubleshooting**
  - What: Common build errors and fixes
  - Missing: README section

---

## 🔬 Verification Needed (Unknown Status)

Once build is fixed, these need verification:

- [ ] **CPU Backend End-to-End**
  - Run: `./build/examples/ds_demo`
  - Verify: Both raw and uppercase reads work
  
- [ ] **Asset Streaming Demo**
  - Run: `./build/examples/ds_asset_streaming`
  - Verify: Concurrent reads with offsets work
  
- [ ] **Error Reporting**
  - Test: Trigger various errors (bad fd, null buffer)
  - Verify: Callback fires with correct context
  
- [ ] **FakeUppercase Compression**
  - Test: Request with `Compression::FakeUppercase`
  - Verify: ASCII bytes uppercased
  
- [ ] **in_flight() Tracking**
  - Test: Check counter during async operations
  - Verify: Increments/decrements correctly
  
- [ ] **wait_all() Blocking**
  - Test: Submit requests, wait_all(), check completion
  - Verify: Blocks until all done
  
- [ ] **C ABI Wrapper**
  - Run: `./build/tests/c_abi_stats_test` (if it compiles)
  - Verify: C functions work correctly

---

## 📊 Statistics

### Implementation Completeness

| Component | Complete | Incomplete | Missing | Total |
|-----------|----------|------------|---------|-------|
| Core API | 12 | 2 | 1 | 15 |
| CPU Backend | 8 | 2 | 0 | 10 |
| Vulkan Backend | 8 | 4 | 4 | 16 |
| io_uring Backend | 5 | 3 | 2 | 10 |
| Compression | 2 | 0 | 2 | 4 |
| Test Suite | 4 | 0 | 8 | 12 |
| Documentation | 5 | 2 | 6 | 13 |
| **TOTAL** | **44** | **13** | **23** | **80** |

**Completeness: 55% (44/80)**  
**Needs Work: 45% (36/80)**

### Critical Path

To achieve "working CPU backend":
1. Fix 2 build-breaking issues (P0) ← **BLOCKING**
2. Verify 7 core features (verification)
3. Add 5 missing tests (P1)

**Estimated effort:** 1-2 days for one developer

To achieve "working Vulkan backend with compute":
1. Fix build issues (prerequisite)
2. Implement 4 Vulkan features (P1)
3. Add GPU decompression (P2)
4. Add tests

**Estimated effort:** 2-4 weeks for one developer

To achieve "production-ready":
1. Fix all build issues
2. Complete all backends
3. Implement GDeflate
4. Add comprehensive tests (80%+ coverage)
5. Add CI/CD
6. Wine integration

**Estimated effort:** 3-6 months for small team

---

## 🎯 Recommended Priorities

### Phase 1: Make it Build (1 day)
1. Add `bytes_transferred` field
2. Implement `take_completed()` method
3. Update bytes on successful I/O
4. Verify compilation succeeds

### Phase 2: Make it Work (1 week)
1. Run all existing tests
2. Fix any discovered bugs
3. Add take_completed() test
4. Add bytes_transferred test
5. Run both demo programs
6. Document actual status

### Phase 3: Make it Complete (1 month)
1. Implement Vulkan compute pipelines
2. Add comprehensive test suite
3. Implement CPU GDeflate (basic)
4. Add CI/CD
5. Update documentation

### Phase 4: Make it Production (3 months)
1. GPU GDeflate implementation
2. Wine/Proton shim
3. Performance benchmarking
4. Real-world testing
5. API stability guarantees

---

## 📝 Notes

- All checkboxes represent **actual code changes** needed
- Items marked ❌ may be design decisions (e.g., "should io_uring support GPU?")
- Priorities assume goal is working DirectStorage-like runtime
- Estimates assume experienced C++/Vulkan developer
- Some features may be intentionally excluded (out of scope)

---

**Last Updated:** 2026-02-15  
**Based on:** Comprehensive code inspection + documentation review  
**Maintainer:** Update this as features are implemented

---

## Quick Commands

### To fix build:
```bash
# 1. Edit include/ds_runtime.hpp - add field to Request struct
# 2. Edit src/ds_runtime.cpp - add take_completed() method to Queue::Impl
# 3. Edit src/ds_runtime.cpp - set bytes_transferred after successful I/O
cmake --build build
```

### To verify fix:
```bash
cd build
ctest --verbose
./examples/ds_demo
./examples/ds_asset_streaming
```

### To add tests:
```bash
# Add test files to tests/ directory
# Update CMakeLists.txt to build them
# Run with: ctest --test-dir build
```
