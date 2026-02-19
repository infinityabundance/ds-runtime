# Vulkan GPU Compute Pipeline Investigation and Implementation Plan

**Status:** Planning Phase  
**Priority:** High  
**Target:** Full GPU compute capability for decompression and data processing  
**Dependencies:** Existing Vulkan device/queue infrastructure

---

## Executive Summary

The current Vulkan backend in ds-runtime supports staging buffer copies but lacks compute pipeline functionality. This document outlines the investigation, design, and implementation plan for adding full GPU compute capabilities, primarily for GPU-accelerated decompression.

---

## 1. Current State

### 1.1 What Works

**Existing Vulkan Infrastructure** (`src/ds_runtime_vulkan.cpp`):
- ✅ Vulkan instance creation
- ✅ Physical device selection
- ✅ Logical device and queue creation
- ✅ Command pool management
- ✅ Staging buffer allocation (host-visible)
- ✅ GPU buffer allocation (device-local)
- ✅ `vkCmdCopyBuffer` for staging ↔ GPU transfers
- ✅ Synchronization via `vkDeviceWaitIdle`
- ✅ Memory type selection and allocation
- ✅ Request submission and completion tracking

**Capability**: File I/O → Staging buffer → GPU buffer (pure data transfer, no computation)

### 1.2 What's Missing

**Compute Pipeline Components**:
- ❌ Compute pipeline creation (`vkCreateComputePipelines`)
- ❌ Shader module loading (`vkCreateShaderModule`)
- ❌ Descriptor set layout creation
- ❌ Descriptor pool allocation
- ❌ Descriptor set updates (buffer bindings)
- ❌ Pipeline layout creation
- ❌ Compute command recording (`vkCmdBindPipeline`, `vkCmdDispatch`)
- ❌ Push constant support
- ❌ Compute-specific synchronization (barriers)

**Impact**: Cannot execute any GPU compute workloads (decompression, transforms, etc.)

### 1.3 Existing Assets (Unused)

**SPIR-V Shader**: `examples/vk-copy-test/copy.comp.spv` (256 bytes)
- Precompiled compute shader
- Currently not loaded or used by any code
- Likely a simple buffer copy/transform shader
- Should be examined and potentially reused

---

## 2. Vulkan Compute Architecture

### 2.1 Compute Pipeline Overview

```
Application
  ↓ (prepare compute work)
Descriptor Sets (bind buffers, uniforms)
  ↓
Pipeline Layout (descriptor layouts + push constants)
  ↓
Compute Pipeline (shader + configuration)
  ↓
Command Buffer (vkCmdBindPipeline, vkCmdDispatch)
  ↓
GPU Execution (workgroups → invocations)
  ↓
Synchronization (barriers, fences)
  ↓
Results in GPU buffers
```

### 2.2 Key Vulkan Objects

| Object | Purpose | Lifetime |
|--------|---------|----------|
| **VkShaderModule** | Compiled SPIR-V code | Per-shader, reusable |
| **VkDescriptorSetLayout** | Binding layout (types, stages) | Per-layout, reusable |
| **VkDescriptorPool** | Allocation pool for descriptor sets | Per-backend, managed |
| **VkDescriptorSet** | Actual buffer bindings | Per-dispatch, short-lived |
| **VkPipelineLayout** | Push constants + descriptor layouts | Per-pipeline, reusable |
| **VkPipeline** | Complete compute configuration | Per-shader, reusable |
| **VkCommandBuffer** | Recorded GPU commands | Per-submission, pooled |

### 2.3 Execution Model

**Workgroup Hierarchy**:
```
Global Work Size (e.g., 1024 elements)
  ↓ divide by local_size_x
Workgroups (e.g., 4 workgroups if local_size_x = 256)
  ↓ parallel execution
Invocations (256 invocations per workgroup)
  ↓
Each invocation processes one element
```

**Shader Invocation IDs**:
- `gl_GlobalInvocationID`: Unique ID across all invocations
- `gl_LocalInvocationID`: ID within the workgroup
- `gl_WorkGroupID`: Workgroup index

---

## 3. Implementation Plan

### 3.1 Phase 1: Shader Module Loading

**Goal**: Load and create VkShaderModule from SPIR-V bytecode

#### Tasks
1. Add SPIR-V loading utility function
2. Implement shader module creation
3. Add shader module caching (avoid redundant loads)
4. Validate shader compilation

#### API Design

**Location**: `src/ds_runtime_vulkan.cpp`

```cpp
class ShaderModuleCache {
public:
    VkShaderModule load_shader(
        VkDevice device,
        const std::string& path
    );
    
    void destroy_all(VkDevice device);
    
private:
    std::unordered_map<std::string, VkShaderModule> modules_;
};

// Add to VulkanBackend::Impl
ShaderModuleCache shader_cache_;
```

#### Implementation

```cpp
VkShaderModule ShaderModuleCache::load_shader(
    VkDevice device,
    const std::string& path
) {
    // Check cache
    auto it = modules_.find(path);
    if (it != modules_.end()) {
        return it->second;
    }
    
    // Read SPIR-V file
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open shader file");
    }
    
    size_t size = file.tellg();
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), size);
    
    // Create shader module
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = code.size();
    create_info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    
    VkShaderModule module;
    VkResult result = vkCreateShaderModule(
        device, &create_info, nullptr, &module
    );
    
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    
    modules_[path] = module;
    return module;
}
```

#### Testing
- Load existing `copy.comp.spv` shader
- Validate shader module creation succeeds
- Test error handling for missing files
- Verify shader module caching works

---

### 3.2 Phase 2: Descriptor Set Layout

**Goal**: Define buffer binding layouts for compute shaders

#### Descriptor Layouts for Decompression

**Example: GDeflate Decompression**
```cpp
// Binding 0: Input compressed buffer (read-only)
// Binding 1: Block metadata (read-only)
// Binding 2: Output decompressed buffer (write-only)
```

#### Implementation

**Location**: `src/ds_runtime_vulkan.cpp`

```cpp
struct ComputeDescriptorLayouts {
    VkDescriptorSetLayout decompression_layout;
    VkDescriptorSetLayout copy_layout;
    // Add more as needed
};

VkDescriptorSetLayout create_decompression_descriptor_layout(
    VkDevice device
) {
    VkDescriptorSetLayoutBinding bindings[3];
    
    // Binding 0: Input compressed data
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = nullptr;
    
    // Binding 1: Block metadata
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].pImmutableSamplers = nullptr;
    
    // Binding 2: Output decompressed data
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].pImmutableSamplers = nullptr;
    
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    
    VkDescriptorSetLayout layout;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &layout_info, nullptr, &layout
    );
    
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
    
    return layout;
}
```

#### Add to VulkanBackend

```cpp
// In VulkanBackend::Impl
ComputeDescriptorLayouts descriptor_layouts_;

// Initialize in constructor
descriptor_layouts_.decompression_layout = 
    create_decompression_descriptor_layout(device_);
```

---

### 3.3 Phase 3: Descriptor Pool

**Goal**: Allocate descriptor pool for runtime descriptor set allocation

#### Pool Sizing

**Strategy**: Pre-allocate pool large enough for concurrent dispatches

```cpp
// Estimate: 16 concurrent compute dispatches
// Each dispatch needs 3 storage buffer descriptors
// Total: 16 * 3 = 48 storage buffer descriptors
```

#### Implementation

```cpp
VkDescriptorPool create_compute_descriptor_pool(
    VkDevice device,
    uint32_t max_sets = 32
) {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = max_sets * 3; // 3 bindings per set
    
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = max_sets;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    
    VkDescriptorPool pool;
    VkResult result = vkCreateDescriptorPool(
        device, &pool_info, nullptr, &pool
    );
    
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
    
    return pool;
}

// Add to VulkanBackend::Impl
VkDescriptorPool descriptor_pool_;

// Initialize in constructor
descriptor_pool_ = create_compute_descriptor_pool(device_);
```

---

### 3.4 Phase 4: Pipeline Layout and Compute Pipeline

**Goal**: Create compute pipeline with shader and layout

#### Pipeline Layout

```cpp
VkPipelineLayout create_compute_pipeline_layout(
    VkDevice device,
    VkDescriptorSetLayout descriptor_layout
) {
    // Optional: push constants for dispatch parameters
    VkPushConstantRange push_constant{};
    push_constant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant.offset = 0;
    push_constant.size = sizeof(uint32_t) * 4; // Example: 4 uint32s
    
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;
    
    VkPipelineLayout layout;
    VkResult result = vkCreatePipelineLayout(
        device, &layout_info, nullptr, &layout
    );
    
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
    }
    
    return layout;
}
```

#### Compute Pipeline

```cpp
VkPipeline create_compute_pipeline(
    VkDevice device,
    VkPipelineLayout layout,
    VkShaderModule shader_module,
    const char* entry_point = "main"
) {
    VkPipelineShaderStageCreateInfo stage_info{};
    stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = shader_module;
    stage_info.pName = entry_point;
    
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage_info;
    pipeline_info.layout = layout;
    
    VkPipeline pipeline;
    VkResult result = vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline
    );
    
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline");
    }
    
    return pipeline;
}
```

#### Pipeline Management

```cpp
struct ComputePipeline {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout descriptor_layout;
    VkShaderModule shader;
};

// Add to VulkanBackend::Impl
std::unordered_map<std::string, ComputePipeline> compute_pipelines_;

ComputePipeline create_decompression_pipeline() {
    ComputePipeline result;
    
    // Load shader
    result.shader = shader_cache_.load_shader(
        device_, "shaders/decompress.comp.spv"
    );
    
    // Create descriptor layout
    result.descriptor_layout = 
        create_decompression_descriptor_layout(device_);
    
    // Create pipeline layout
    result.layout = create_compute_pipeline_layout(
        device_, result.descriptor_layout
    );
    
    // Create pipeline
    result.pipeline = create_compute_pipeline(
        device_, result.layout, result.shader
    );
    
    return result;
}
```

---

### 3.5 Phase 5: Descriptor Set Allocation and Updates

**Goal**: Bind buffers to descriptor sets for each dispatch

#### Allocation

```cpp
VkDescriptorSet allocate_descriptor_set(
    VkDevice device,
    VkDescriptorPool pool,
    VkDescriptorSetLayout layout
) {
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;
    
    VkDescriptorSet descriptor_set;
    VkResult result = vkAllocateDescriptorSets(
        device, &alloc_info, &descriptor_set
    );
    
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor set");
    }
    
    return descriptor_set;
}
```

#### Buffer Binding

```cpp
void update_decompression_descriptor_set(
    VkDevice device,
    VkDescriptorSet descriptor_set,
    VkBuffer input_buffer,
    VkBuffer metadata_buffer,
    VkBuffer output_buffer,
    VkDeviceSize input_size,
    VkDeviceSize metadata_size,
    VkDeviceSize output_size
) {
    VkDescriptorBufferInfo buffer_infos[3];
    
    // Input buffer
    buffer_infos[0].buffer = input_buffer;
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = input_size;
    
    // Metadata buffer
    buffer_infos[1].buffer = metadata_buffer;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = metadata_size;
    
    // Output buffer
    buffer_infos[2].buffer = output_buffer;
    buffer_infos[2].offset = 0;
    buffer_infos[2].range = output_size;
    
    VkWriteDescriptorSet writes[3];
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].pNext = nullptr;
        writes[i].dstSet = descriptor_set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_infos[i];
        writes[i].pImageInfo = nullptr;
        writes[i].pTexelBufferView = nullptr;
    }
    
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
}
```

---

### 3.6 Phase 6: Compute Dispatch

**Goal**: Record and execute compute commands

#### Command Buffer Recording

```cpp
void record_compute_dispatch(
    VkCommandBuffer cmd,
    VkPipeline pipeline,
    VkPipelineLayout layout,
    VkDescriptorSet descriptor_set,
    uint32_t workgroup_count_x,
    uint32_t workgroup_count_y = 1,
    uint32_t workgroup_count_z = 1
) {
    // Bind compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    
    // Bind descriptor set
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        layout,
        0,  // first set
        1,  // descriptor set count
        &descriptor_set,
        0,  // dynamic offset count
        nullptr
    );
    
    // Optional: push constants
    // vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, ...);
    
    // Dispatch compute work
    vkCmdDispatch(cmd, workgroup_count_x, workgroup_count_y, workgroup_count_z);
}
```

#### Integration with Request Processing

```cpp
void VulkanBackend::Impl::process_request_with_compute(Request& req) {
    // Begin command buffer
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(command_buffer_, &begin_info);
    
    // 1. Copy file data to staging buffer
    // (existing code for file I/O)
    
    // 2. Barrier: transfer → compute
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.buffer = staging_buffer_;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr
    );
    
    // 3. Dispatch compute (decompression)
    auto& pipeline = compute_pipelines_["decompress"];
    VkDescriptorSet desc_set = allocate_descriptor_set(
        device_, descriptor_pool_, pipeline.descriptor_layout
    );
    
    update_decompression_descriptor_set(
        device_, desc_set,
        staging_buffer_,      // compressed input
        metadata_buffer_,     // block info
        req.gpu_buffer,       // decompressed output
        req.size,
        metadata_size,
        req.size * 2  // assume 2x expansion
    );
    
    uint32_t workgroup_count = (req.size + 255) / 256;
    record_compute_dispatch(
        command_buffer_,
        pipeline.pipeline,
        pipeline.layout,
        desc_set,
        workgroup_count
    );
    
    // 4. Barrier: compute → host (if needed for readback)
    // ...
    
    // 5. End and submit
    vkEndCommandBuffer(command_buffer_);
    
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;
    
    vkQueueSubmit(queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    
    // Free descriptor set
    vkFreeDescriptorSets(device_, descriptor_pool_, 1, &desc_set);
}
```

---

### 3.7 Phase 7: Synchronization and Barriers

**Goal**: Proper memory synchronization between pipeline stages

#### Key Barriers

**Transfer → Compute**:
```cpp
VkBufferMemoryBarrier transfer_to_compute{};
transfer_to_compute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
transfer_to_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
// Use: After vkCmdCopyBuffer, before compute dispatch
```

**Compute → Transfer**:
```cpp
VkBufferMemoryBarrier compute_to_transfer{};
compute_to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
compute_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
// Use: After compute dispatch, before staging buffer readback
```

**Compute → Compute** (between dispatches):
```cpp
VkBufferMemoryBarrier compute_to_compute{};
compute_to_compute.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
compute_to_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
// Use: Between dependent compute passes
```

#### Synchronization Best Practices

1. **Minimize barriers**: Batch operations when possible
2. **Use appropriate stages**: Don't over-synchronize
3. **Consider queue ownership**: Handle queue family transfers if needed
4. **Validate with layers**: Enable `VK_LAYER_KHRONOS_validation`

---

## 4. Shader Development

### 4.1 Example: Simple Buffer Copy

**File**: `shaders/buffer_copy.comp`

```glsl
#version 450

layout(local_size_x = 256) in;

layout(binding = 0) readonly buffer InputBuffer {
    uint data[];
} input_buf;

layout(binding = 1) writeonly buffer OutputBuffer {
    uint data[];
} output_buf;

layout(push_constant) uniform PushConstants {
    uint element_count;
} push;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx < push.element_count) {
        output_buf.data[idx] = input_buf.data[idx];
    }
}
```

**Compilation**:
```bash
glslangValidator -V buffer_copy.comp -o buffer_copy.comp.spv
```

### 4.2 Example: Transform Shader

**File**: `shaders/uppercase.comp` (enhanced version of FakeUppercase)

```glsl
#version 450

layout(local_size_x = 256) in;

layout(binding = 0) readonly buffer InputBuffer {
    uint8_t data[];
} input_buf;

layout(binding = 1) writeonly buffer OutputBuffer {
    uint8_t data[];
} output_buf;

layout(push_constant) uniform PushConstants {
    uint byte_count;
} push;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx < push.byte_count) {
        uint8_t c = input_buf.data[idx];
        // Uppercase ASCII
        if (c >= 'a' && c <= 'z') {
            c = c - 32;
        }
        output_buf.data[idx] = c;
    }
}
```

### 4.3 Shader Build System

**Add to CMakeLists.txt**:
```cmake
# Find glslangValidator
find_program(GLSLANG_VALIDATOR glslangValidator)

if(GLSLANG_VALIDATOR)
    # Compile shaders
    file(GLOB SHADER_SOURCES shaders/*.comp)
    
    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
        
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${CMAKE_COMMAND} -E make_directory 
                    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SPIRV}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader ${SHADER_NAME}"
        )
        
        list(APPEND SPIRV_SHADERS ${SPIRV})
    endforeach()
    
    add_custom_target(shaders ALL DEPENDS ${SPIRV_SHADERS})
endif()
```

---

## 5. Testing Strategy

### 5.1 Unit Tests

**Test Suite**: `tests/vulkan_compute_test.cpp`

**Test Cases**:
1. **Pipeline creation**: Verify pipeline objects created successfully
2. **Shader loading**: Test shader module creation from SPIR-V
3. **Descriptor allocation**: Verify descriptor pool management
4. **Simple dispatch**: Buffer copy shader execution
5. **Transform shader**: Uppercase transform on GPU
6. **Error handling**: Invalid pipeline, missing shader, etc.

### 5.2 Integration Tests

**Scenarios**:
1. **File → GPU compute → GPU buffer**: Full decompression path
2. **Multiple dispatches**: Concurrent compute workloads
3. **Mixed operations**: Compute + transfer in same command buffer
4. **Large buffers**: Test with multi-MB data
5. **Synchronization**: Verify barriers work correctly

### 5.3 Validation

**Tools**:
- Vulkan Validation Layers (`VK_LAYER_KHRONOS_validation`)
- RenderDoc for command buffer inspection
- GPU profilers (Nsight, Radeon GPU Profiler)

**Check for**:
- Synchronization errors
- Memory leaks (descriptor sets, pipelines)
- Invalid API usage
- Performance bottlenecks

---

## 6. Performance Considerations

### 6.1 Optimization Strategies

**Shader Optimizations**:
- Coalesced memory access patterns
- Shared memory for temporary data
- Reduce divergent branches
- Optimize workgroup size for target GPU

**Pipeline Management**:
- Reuse pipelines across requests
- Minimize pipeline switches
- Batch similar compute work
- Pre-warm pipeline caches

**Memory Management**:
- Pool descriptor sets (avoid per-frame allocation)
- Reuse command buffers when possible
- Minimize host-device transfers
- Use push constants for small data

### 6.2 Performance Targets

**Goals**:
- Compute dispatch overhead < 100 µs
- Throughput ≥ 10 GB/s for simple transforms
- GPU utilization ≥ 80% during compute
- CPU overhead < 5% during GPU execution

---

## 7. Dependencies and Requirements

### 7.1 External Dependencies

**Required**:
- Vulkan SDK (already optional dependency)
- `glslangValidator` for shader compilation
- C++20 compiler (already required)

**Optional**:
- RenderDoc for debugging
- GPU profiling tools

### 7.2 Hardware Requirements

**Minimum**:
- Vulkan 1.0 support
- Compute queue support
- Storage buffer support

**Recommended**:
- Vulkan 1.3 support
- Dedicated compute queue
- ≥ 4GB VRAM for large asset processing

---

## 8. Timeline and Milestones

### 8.1 Implementation Phases

**Week 1-2: Foundation**
- Shader module loading
- Descriptor layouts
- Descriptor pool creation
- **Milestone**: Infrastructure ready

**Week 3-4: Pipeline Creation**
- Pipeline layout
- Compute pipeline creation
- Pipeline management
- **Milestone**: Simple copy shader works

**Week 5-6: Dispatch and Synchronization**
- Command buffer recording
- Compute dispatch
- Barriers and synchronization
- **Milestone**: Transform shader works

**Week 7-8: Integration and Testing**
- Backend integration
- Comprehensive testing
- Performance tuning
- **Milestone**: Production-ready compute support

### 8.2 Total Estimate

**8 weeks** for complete Vulkan compute implementation

**Dependencies**: None (can proceed independently)

---

## 9. Success Criteria

### 9.1 Functional Requirements
- ✅ Shader modules load from SPIR-V files
- ✅ Compute pipelines created successfully
- ✅ Descriptor sets allocated and bound correctly
- ✅ Compute dispatches execute on GPU
- ✅ Synchronization barriers work properly
- ✅ Existing Vulkan tests still pass
- ✅ New compute tests pass (100% coverage)

### 9.2 Performance Requirements
- ✅ Compute overhead < 100 µs per dispatch
- ✅ GPU utilization ≥ 80% during compute
- ✅ Throughput ≥ 10 GB/s for simple operations
- ✅ No performance regression in existing paths

### 9.3 Quality Requirements
- ✅ Vulkan validation layers pass (no errors)
- ✅ No memory leaks (descriptor sets, pipelines)
- ✅ Thread-safe pipeline management
- ✅ Documentation complete and accurate
- ✅ API stability maintained

---

## 10. Next Steps

### Immediate (This Week)
1. ✅ Complete investigation document
2. ⏩ Examine existing `copy.comp.spv` shader
3. ⏩ Set up shader build system
4. ⏩ Create simple test shader

### Short Term (Next 2 Weeks)
1. ⏩ Implement shader module loading
2. ⏩ Create descriptor layouts
3. ⏩ Set up descriptor pool
4. ⏩ Test infrastructure

### Medium Term (1-2 Months)
1. ⏩ Complete pipeline creation
2. ⏩ Implement compute dispatch
3. ⏩ Add synchronization
4. ⏩ Comprehensive testing

---

## 11. Open Questions

1. **Shader Language**: Should we support multiple shader languages (GLSL, HLSL via DXC)?
2. **Pipeline Caching**: Do we need VkPipelineCache for faster startup?
3. **Async Compute**: Should we use dedicated compute queue or graphics queue?
4. **Workgroup Size**: How to determine optimal local_size_x for different GPUs?
5. **Error Recovery**: How to handle GPU compute failures gracefully?
6. **Shader Hot-Reload**: Do we need runtime shader recompilation for development?

---

## 12. References

### Vulkan Specification
- Vulkan 1.3 Specification: Compute Shaders
- Khronos Vulkan Guide: Compute
- Vulkan Tutorial: Compute Shaders

### Best Practices
- Khronos Vulkan Best Practices
- AMD GPU Architecture
- NVIDIA Compute Best Practices
- Intel GPU Architecture Guide

### Tools
- glslangValidator documentation
- RenderDoc user guide
- Vulkan Validation Layers

---

**Document Status**: Draft v1.0  
**Last Updated**: 2026-02-16  
**Next Review**: After shader loading implementation complete
