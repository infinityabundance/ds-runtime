# GDeflate Compression Investigation and Implementation Plan

**Status:** Research Phase  
**Priority:** High  
**Target:** CPU and GPU GDeflate decompression support  
**Dependencies:** None for CPU; Vulkan compute pipeline for GPU

---

## Executive Summary

GDeflate is Microsoft's custom compression format for DirectStorage, designed for efficient GPU decompression. This document outlines the investigation, design, and implementation plan for adding GDeflate support to ds-runtime.

---

## 1. Background

### 1.1 What is GDeflate?

GDeflate is a GPU-friendly variant of DEFLATE compression used in Microsoft DirectStorage. Key characteristics:

- **Block-based structure**: Data is compressed in independent blocks for parallel GPU decompression
- **Modified DEFLATE**: Based on standard DEFLATE but optimized for GPU compute
- **Metadata format**: Includes block headers with size information
- **Designed for parallelism**: Each block can be decompressed independently

### 1.2 Current State

- **Status**: Intentionally stubbed - returns `ENOTSUP` error
- **Test**: `tests/compression_gdeflate_stub_test.cpp` verifies failure
- **API**: `Compression::GDeflate` enum value exists
- **Path**: Error callback triggered when requested

---

## 2. Research Requirements

### 2.1 Format Specification

**Primary Goal**: Obtain or reverse-engineer GDeflate format specification

**Approaches**:

1. **Official Documentation**
   - Check Microsoft DirectStorage SDK documentation
   - Review DirectStorage headers and samples
   - Search for format specifications in MSDN

2. **Reverse Engineering**
   - Analyze DirectStorage DLL behavior
   - Examine compressed asset samples
   - Study existing decompression implementations

3. **Community Resources**
   - Wine/Proton community investigations
   - Graphics programming forums
   - Open-source game engine implementations

**Deliverable**: GDeflate format specification document

### 2.2 Existing Implementations

**Research Goals**:
- Identify existing open-source GDeflate decoders
- Review Wine/Proton DirectStorage implementation status
- Study GPU decompression implementations (if any)

**Potential Sources**:
- Wine project (dstorage.dll implementation)
- Game engine implementations (Unreal, Unity plugins)
- Graphics libraries with DirectStorage support
- Academic papers on GPU decompression

---

## 3. CPU Implementation Plan

### 3.1 Architecture

```
Input: Compressed buffer + size
  ↓
Block Header Parser
  ↓ (block metadata: offset, compressed_size, uncompressed_size)
Per-Block Decompression Loop
  ↓ (DEFLATE decode per block)
Output: Decompressed buffer
```

### 3.2 Implementation Phases

#### Phase 3.1: Format Understanding
**Tasks**:
- Document GDeflate file/stream structure
- Identify block header format
- Document compression parameters
- Understand dictionary handling

**Deliverables**:
- Format specification document
- Test asset creation tool

#### Phase 3.2: Block Header Parser
**Tasks**:
- Implement GDeflate header parsing
- Extract block metadata (offsets, sizes)
- Validate header checksums (if present)
- Error handling for corrupted headers

**Location**: `src/gdeflate_decoder.cpp`

**API**:
```cpp
struct GDeflateBlockInfo {
    uint64_t offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

std::vector<GDeflateBlockInfo> parse_gdeflate_header(
    const void* data, 
    size_t size
);
```

#### Phase 3.3: DEFLATE Decoder Integration
**Tasks**:
- Choose DEFLATE library (zlib, miniz, or custom)
- Integrate block decompression
- Handle partial decompression
- Add streaming support

**Library Options**:
1. **zlib** (standard, widely available)
   - Pros: Mature, well-tested, optimized
   - Cons: May need modification for block format
   
2. **miniz** (single-file, public domain)
   - Pros: Easy integration, no dependencies
   - Cons: May be slower than zlib
   
3. **Custom implementation**
   - Pros: Full control, GDeflate-specific optimizations
   - Cons: High development effort, testing burden

**Recommendation**: Start with zlib, consider optimization later

#### Phase 3.4: CPU Backend Integration
**Tasks**:
- Remove ENOTSUP stub from `ds_runtime.cpp`
- Wire GDeflate decoder into decompression pipeline
- Add error handling for decompression failures
- Update tests to verify successful decompression

**Location**: `src/ds_runtime.cpp` (CpuBackend::decompress)

**Changes**:
```cpp
// Replace stub:
if (req.compression == Compression::GDeflate) {
    report_error("cpu", "decompression", ENOTSUP,
                 "GDeflate compression is not yet implemented (ENOTSUP)");
    return;
}

// With implementation:
if (req.compression == Compression::GDeflate) {
    if (!gdeflate_decompress(req.dst, req.size, ...)) {
        report_request_error("cpu", "decompression", errno, 
                           "GDeflate decompression failed", req);
        return;
    }
}
```

### 3.3 Testing Strategy

#### Test Assets
- Create compressed test files with known content
- Various block sizes (1KB, 4KB, 16KB, 64KB)
- Edge cases: empty blocks, maximum compression, random data

#### Test Cases
1. **Basic decompression**: Simple compressed buffer → original data
2. **Multi-block**: File with multiple independent blocks
3. **Error handling**: Corrupted header, truncated data, invalid checksums
4. **Performance**: Benchmark against uncompressed I/O
5. **Partial reads**: Decompression with offset/size parameters

**New Test File**: `tests/gdeflate_cpu_test.cpp`

### 3.4 Performance Considerations

**Metrics to Track**:
- Decompression throughput (MB/s)
- CPU utilization per thread
- Memory overhead during decompression
- Comparison vs uncompressed I/O

**Optimization Opportunities**:
- Parallel block decompression (thread pool)
- SIMD optimizations for DEFLATE decode
- Memory pool for temporary buffers
- Block prefetching for streaming

---

## 4. GPU Implementation Plan

### 4.1 Architecture

```
CPU: Parse block headers → metadata to GPU
  ↓
GPU: Parallel block decompression (compute shader)
  ↓ (one workgroup per block or chunk)
GPU: Output to GPU buffer
```

### 4.2 Prerequisites

**Required Infrastructure** (from Vulkan compute investigation):
- Compute pipeline creation ✅ (planned)
- Shader module loading ✅ (planned)
- Descriptor set management ✅ (planned)
- GPU buffer management ✅ (exists)

**Dependency**: Vulkan GPU compute pipeline implementation must be complete

### 4.3 Implementation Phases

#### Phase 4.1: Compute Shader Design
**Tasks**:
- Design GLSL compute shader for DEFLATE decode
- Implement block-parallel decompression
- Handle LZ77 back-references efficiently
- Optimize for GPU wavefront/warp sizes

**File**: `shaders/gdeflate_decompress.comp`

**Key Challenges**:
1. **Shared memory management**: LZ77 history buffer
2. **Divergent execution**: Huffman decoding branches
3. **Synchronization**: Block-independent decompression
4. **Memory access patterns**: Coalesced reads/writes

**Shader Structure**:
```glsl
#version 450

// Input: compressed blocks buffer
layout(binding = 0) readonly buffer CompressedData {
    uint data[];
} compressed;

// Input: block metadata
layout(binding = 1) readonly buffer BlockInfo {
    uint offset[];
    uint compressed_size[];
    uint uncompressed_size[];
} blocks;

// Output: decompressed data
layout(binding = 2) writeonly buffer DecompressedData {
    uint data[];
} decompressed;

layout(local_size_x = 256) in;

void main() {
    uint block_id = gl_GlobalInvocationID.x;
    // Decompress block[block_id]
    // ...
}
```

#### Phase 4.2: GPU Backend Integration
**Tasks**:
- Add GDeflate compute pipeline to VulkanBackend
- Create descriptor sets for compression buffers
- Dispatch compute workload for decompression
- Handle synchronization (barriers, fences)

**Location**: `src/ds_runtime_vulkan.cpp`

**Changes**:
- Add `gdeflate_pipeline_` member to VulkanBackend
- Create compression-specific descriptor layout
- Dispatch compute before/after staging buffer copies

#### Phase 4.3: CPU-GPU Hybrid Strategy
**Goal**: Choose CPU vs GPU decompression based on data characteristics

**Decision Factors**:
1. **Data size**: Small files → CPU, Large files → GPU
2. **Block count**: Few blocks → CPU, Many blocks → GPU
3. **Request destination**: Host memory → CPU, GPU buffer → GPU
4. **GPU availability**: Fallback to CPU if GPU busy

**Configuration**:
```cpp
struct CompressionConfig {
    size_t gpu_threshold_bytes = 1024 * 1024; // 1MB
    size_t min_blocks_for_gpu = 16;
    bool prefer_gpu_for_gpu_memory = true;
};
```

### 4.4 Performance Targets

**Goals**:
- GPU decompression ≥ 5x faster than CPU (for large files)
- Minimal CPU involvement during GPU path
- Efficient for small files (CPU path competitive)

**Benchmarks**:
- 1MB, 10MB, 100MB compressed assets
- Various compression ratios (1.5x, 3x, 5x)
- CPU vs GPU throughput comparison
- GPU occupancy and utilization metrics

---

## 5. Dependencies

### 5.1 External Libraries

**CPU Path**:
- zlib or miniz (DEFLATE decompression)
- No additional system dependencies

**GPU Path**:
- Vulkan SDK (already optional dependency)
- SPIR-V compiler for shaders (glslangValidator)
- Vulkan compute pipeline support (from separate investigation)

### 5.2 Internal Dependencies

**Build System**:
- Add GDeflate source files to CMakeLists.txt
- Optional dependency on compression library
- Shader compilation step in build

**API Changes**:
- No breaking changes (GDeflate enum already exists)
- Remove ENOTSUP stub behavior
- Update documentation to reflect support

---

## 6. Testing and Validation

### 6.1 Unit Tests

**Test Coverage**:
1. Block header parsing (valid/invalid headers)
2. Single-block decompression (various sizes)
3. Multi-block decompression (independent blocks)
4. Error handling (corrupted data, invalid sizes)
5. Compression ratio validation (known test vectors)

**Test Files**:
- `tests/gdeflate_format_test.cpp` - Header parsing
- `tests/gdeflate_cpu_test.cpp` - CPU decompression
- `tests/gdeflate_gpu_test.cpp` - GPU decompression (if Vulkan available)

### 6.2 Integration Tests

**Scenarios**:
1. CPU backend with GDeflate compression
2. Vulkan backend with GDeflate GPU decompression
3. Mixed requests (compressed + uncompressed)
4. Error cases (invalid compression, missing data)

**Update Existing Test**:
- Modify `tests/compression_gdeflate_stub_test.cpp`
- Change from "verify failure" to "verify success"
- Add decompression correctness checks

### 6.3 Real-World Testing

**Asset Types**:
- Game textures (DDS, KTX)
- Mesh data (binary vertex buffers)
- Shader bytecode (SPIR-V, DXBC)
- Mixed asset packs

**Performance Testing**:
- Asset streaming demo with GDeflate assets
- Benchmark vs uncompressed baseline
- CPU vs GPU comparison
- DirectStorage parity testing (if possible on Windows)

---

## 7. Documentation Requirements

### 7.1 Format Documentation

**Document**: `docs/gdeflate_format.md`

**Contents**:
- GDeflate file structure
- Block header format
- Compression parameters
- Differences from standard DEFLATE
- Decoder state machine

### 7.2 Implementation Documentation

**Updates Required**:
- `README.md`: Change GDeflate status from ⚠️ to ✅
- `MISSING_FEATURES.md`: Mark GDeflate items as complete
- `docs/design.md`: Add GDeflate pipeline description
- API documentation: Update Compression enum docs

### 7.3 Usage Guide

**Document**: `docs/gdeflate_usage.md`

**Contents**:
- How to compress assets for ds-runtime
- Choosing CPU vs GPU decompression
- Performance tuning recommendations
- Troubleshooting guide

---

## 8. Risks and Mitigations

### 8.1 Technical Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Format spec unavailable | High | Reverse engineer from samples, collaborate with Wine community |
| GPU decode too slow | Medium | Optimize shader, fallback to CPU |
| Memory overhead too high | Medium | Streaming decompression, memory pools |
| Compatibility issues | Medium | Comprehensive testing, multiple test assets |
| Patent/licensing concerns | Low | Use open compression algorithms, document format |

### 8.2 Timeline Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Format research takes longer than expected | +2-4 weeks | Start with CPU implementation, add GPU later |
| GPU optimization difficult | +2-3 weeks | Accept lower performance initially, iterate |
| Integration issues with existing code | +1 week | Incremental integration, thorough testing |

---

## 9. Timeline and Milestones

### 9.1 Research Phase (2-3 weeks)
- Week 1: Format investigation and documentation
- Week 2: Library evaluation and prototyping
- Week 3: Design review and planning finalization

**Milestone**: Format specification complete

### 9.2 CPU Implementation (3-4 weeks)
- Week 4: Header parser implementation
- Week 5: DEFLATE integration and testing
- Week 6: Backend integration
- Week 7: Testing and optimization

**Milestone**: CPU GDeflate decompression working

### 9.3 GPU Implementation (4-6 weeks)
- Week 8-9: Compute shader development
- Week 10-11: GPU backend integration
- Week 12-13: Testing and optimization

**Milestone**: GPU GDeflate decompression working

### 9.4 Total Estimate
**9-13 weeks** for complete CPU + GPU implementation

**Fast Track Option** (CPU only): **5-7 weeks**

---

## 10. Success Criteria

### 10.1 Functional Requirements
- ✅ CPU decoder successfully decompresses GDeflate assets
- ✅ GPU decoder works on Vulkan-supported hardware
- ✅ Error handling for corrupted/invalid data
- ✅ All existing tests still pass
- ✅ New GDeflate tests pass (100% coverage)

### 10.2 Performance Requirements
- ✅ CPU: Decompression throughput ≥ 500 MB/s (uncompressed equivalent)
- ✅ GPU: Decompression throughput ≥ 2 GB/s (for large files)
- ✅ GPU overhead < 10% for file ≥ 1MB
- ✅ CPU fallback for small files performs acceptably

### 10.3 Quality Requirements
- ✅ No memory leaks (valgrind clean)
- ✅ Thread-safe (no data races)
- ✅ Vulkan validation layers pass (no errors)
- ✅ Documentation complete and accurate
- ✅ API stability maintained (no breaking changes)

---

## 11. Next Steps

### Immediate Actions (This Week)
1. ✅ Complete this investigation document
2. ⏩ Research GDeflate format (begin literature search)
3. ⏩ Evaluate DEFLATE libraries (zlib vs miniz vs custom)
4. ⏩ Create prototype test assets

### Short Term (Next 2 Weeks)
1. ⏩ Finalize format specification
2. ⏩ Begin header parser implementation
3. ⏩ Set up testing infrastructure
4. ⏩ Create CPU implementation plan

### Medium Term (1-2 Months)
1. ⏩ Complete CPU implementation
2. ⏩ Integrate with existing backend
3. ⏩ Begin GPU shader development
4. ⏩ Performance benchmarking

---

## 12. Open Questions

1. **Format Availability**: Is GDeflate format publicly documented? If not, can we reverse engineer it legally?
2. **Patent Concerns**: Are there any patents covering GDeflate that we need to be aware of?
3. **Library Choice**: Which DEFLATE library best fits our needs (zlib, miniz, custom)?
4. **GPU Priority**: Should we implement CPU first and GPU later, or develop in parallel?
5. **Compression Tool**: Do we need to provide a GDeflate compression tool, or only decompression?
6. **Block Size**: What are optimal block sizes for CPU vs GPU decompression?
7. **Streaming**: Do we need streaming decompression, or block-at-a-time is sufficient?
8. **Validation**: How do we validate correctness without official test vectors?

---

## 13. References and Resources

### Documentation
- Microsoft DirectStorage documentation
- Wine DirectStorage implementation (if available)
- DEFLATE RFC 1951
- GPU compression research papers

### Libraries
- zlib: https://www.zlib.net/
- miniz: https://github.com/richgel999/miniz
- Vulkan GPU compute examples

### Community
- Wine development mailing list
- Proton GitHub discussions
- Graphics programming forums

---

**Document Status**: Draft v1.0  
**Last Updated**: 2026-02-16  
**Next Review**: After format specification research complete
