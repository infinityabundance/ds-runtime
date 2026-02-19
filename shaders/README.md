# DS-Runtime Compute Shaders

This directory contains GLSL compute shaders for GPU-accelerated operations in ds-runtime.

## Shaders

### copy.comp
Basic buffer copy shader. Copies data from source buffer to destination buffer using GPU compute.
- Local workgroup size: 16
- Bindings:
  - 0: Source buffer (read-only)
  - 1: Destination buffer (write-only)

## Building

Shaders are automatically compiled to SPIR-V during the CMake build process using `glslangValidator`.

```bash
# Manual compilation (for testing):
glslangValidator -V copy.comp -o copy.comp.spv
```

## Adding New Shaders

1. Create your shader file (e.g., `my_shader.comp`)
2. CMake will automatically compile it to SPIR-V
3. The compiled `.spv` file will be available at build time
4. Load the shader in code using the shader module system

## Shader Conventions

- Use `#version 450` for Vulkan 1.0+ compatibility
- Declare local workgroup size with `layout(local_size_x = N) in;`
- Use storage buffers for data: `layout(binding = N) buffer BufferName { ... };`
- Optimize for coalesced memory access patterns
- Keep workgroup sizes as multiples of 32 (warp/wavefront size)
