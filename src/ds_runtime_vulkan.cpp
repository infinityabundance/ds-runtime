// SPDX-License-Identifier: Apache-2.0
// Vulkan backend implementation for ds-runtime.

#include "ds_runtime_vulkan.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace ds {

namespace {

// Load SPIR-V bytecode from a file.
// Returns the bytecode as a vector of uint32_t words.
// Throws std::runtime_error if the file cannot be read or is invalid.
std::vector<uint32_t> load_spirv_from_file(const std::string& path) {
    // Open file in binary mode, seek to end to get size
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open SPIR-V file: " + path);
    }

    // Get file size
    std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("SPIR-V file is empty: " + path);
    }

    // SPIR-V must be a multiple of 4 bytes (32-bit words)
    if (size % 4 != 0) {
        throw std::runtime_error("SPIR-V file size is not a multiple of 4 bytes: " + path);
    }

    // Seek back to beginning
    file.seekg(0, std::ios::beg);

    // Read the file into a buffer
    std::vector<char> buffer(static_cast<std::size_t>(size));
    if (!file.read(buffer.data(), size)) {
        throw std::runtime_error("Failed to read SPIR-V file: " + path);
    }

    // Validate SPIR-V magic number (0x07230203 in little-endian)
    if (buffer.size() >= 4) {
        uint32_t magic = *reinterpret_cast<const uint32_t*>(buffer.data());
        if (magic != 0x07230203) {
            throw std::runtime_error("Invalid SPIR-V magic number in file: " + path);
        }
    }

    // Convert to uint32_t vector
    std::vector<uint32_t> spirv(buffer.size() / 4);
    std::memcpy(spirv.data(), buffer.data(), buffer.size());

    return spirv;
}

// Wrapper for VkShaderModule with RAII lifecycle management.
class ShaderModule {
public:
    // Create a shader module from SPIR-V bytecode.
    // Throws std::runtime_error if creation fails.
    ShaderModule(VkDevice device, const std::vector<uint32_t>& spirv_code)
        : device_(device), module_(VK_NULL_HANDLE)
    {
        if (device == VK_NULL_HANDLE) {
            throw std::runtime_error("Invalid VkDevice for shader module creation");
        }

        if (spirv_code.empty()) {
            throw std::runtime_error("Empty SPIR-V code");
        }

        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = spirv_code.size() * sizeof(uint32_t);
        create_info.pCode = spirv_code.data();

        VkResult result = vkCreateShaderModule(device_, &create_info, nullptr, &module_);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create VkShaderModule (VkResult: " + 
                                   std::to_string(static_cast<int>(result)) + ")");
        }
    }

    // Destructor cleans up the Vulkan shader module.
    ~ShaderModule() {
        if (module_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, module_, nullptr);
        }
    }

    // Delete copy operations (shader modules should not be copied)
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    // Allow move operations
    ShaderModule(ShaderModule&& other) noexcept
        : device_(other.device_), module_(other.module_)
    {
        other.module_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
    }

    ShaderModule& operator=(ShaderModule&& other) noexcept {
        if (this != &other) {
            // Clean up our current module
            if (module_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, module_, nullptr);
            }
            
            // Take ownership of other's module
            device_ = other.device_;
            module_ = other.module_;
            other.module_ = VK_NULL_HANDLE;
            other.device_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    // Get the underlying VkShaderModule handle.
    VkShaderModule get() const { return module_; }

    // Check if the module is valid.
    bool is_valid() const { return module_ != VK_NULL_HANDLE; }

private:
    VkDevice device_;
    VkShaderModule module_;
};

// Cache for shader modules to avoid reloading/recompiling the same shaders.
// Maps shader file paths to loaded shader modules.
class ShaderModuleCache {
public:
    explicit ShaderModuleCache(VkDevice device) : device_(device) {}

    // Load a shader from file, returning a cached module if already loaded.
    // Throws std::runtime_error if the shader cannot be loaded.
    VkShaderModule load_shader(const std::string& path) {
        // Check if already cached
        auto it = cache_.find(path);
        if (it != cache_.end()) {
            return it->second.get();
        }

        // Load SPIR-V from file
        std::vector<uint32_t> spirv = load_spirv_from_file(path);

        // Create shader module
        ShaderModule module(device_, spirv);

        // Cache it (move into cache)
        VkShaderModule handle = module.get();
        cache_.emplace(path, std::move(module));

        return handle;
    }

    // Clear all cached shader modules.
    void clear() {
        cache_.clear();
    }

    // Get number of cached shaders.
    std::size_t size() const {
        return cache_.size();
    }

    // Check if a shader is cached.
    bool has_shader(const std::string& path) const {
        return cache_.find(path) != cache_.end();
    }

private:
    VkDevice device_;
    std::unordered_map<std::string, ShaderModule> cache_;
};

// Descriptor set layout for compute shaders.
// Defines the bindings used by a compute pipeline.
struct DescriptorLayoutInfo {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    
    // Create the Vulkan descriptor set layout from bindings
    void create(VkDevice device) {
        if (layout != VK_NULL_HANDLE) {
            return;  // Already created
        }
        
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        
        VkResult result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &layout);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create descriptor set layout (VkResult: " +
                                   std::to_string(static_cast<int>(result)) + ")");
        }
    }
    
    // Destroy the layout
    void destroy(VkDevice device) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    }
};

// Factory functions to create common descriptor layouts
namespace descriptor_layouts {

// Layout for simple buffer copy: 2 storage buffers (input, output)
inline DescriptorLayoutInfo create_buffer_copy_layout() {
    DescriptorLayoutInfo info;
    info.bindings.resize(2);
    
    // Binding 0: Input buffer (read-only)
    info.bindings[0].binding = 0;
    info.bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    info.bindings[0].descriptorCount = 1;
    info.bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    info.bindings[0].pImmutableSamplers = nullptr;
    
    // Binding 1: Output buffer (write-only)
    info.bindings[1].binding = 1;
    info.bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    info.bindings[1].descriptorCount = 1;
    info.bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    info.bindings[1].pImmutableSamplers = nullptr;
    
    return info;
}

// Layout for decompression: 3 storage buffers (compressed, metadata, decompressed)
inline DescriptorLayoutInfo create_decompression_layout() {
    DescriptorLayoutInfo info;
    info.bindings.resize(3);
    
    // Binding 0: Compressed input buffer
    info.bindings[0].binding = 0;
    info.bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    info.bindings[0].descriptorCount = 1;
    info.bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    info.bindings[0].pImmutableSamplers = nullptr;
    
    // Binding 1: Metadata buffer (block info)
    info.bindings[1].binding = 1;
    info.bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    info.bindings[1].descriptorCount = 1;
    info.bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    info.bindings[1].pImmutableSamplers = nullptr;
    
    // Binding 2: Decompressed output buffer
    info.bindings[2].binding = 2;
    info.bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    info.bindings[2].descriptorCount = 1;
    info.bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    info.bindings[2].pImmutableSamplers = nullptr;
    
    return info;
}

} // namespace descriptor_layouts

// Descriptor pool for allocating descriptor sets.
// Pre-allocates a pool of descriptors that can be used by compute pipelines.
class DescriptorPool {
public:
    explicit DescriptorPool(VkDevice device, uint32_t max_sets = 32)
        : device_(device), pool_(VK_NULL_HANDLE)
    {
        // Size the pool for storage buffers (most common for compute)
        // Each set needs up to 3 storage buffers (for decompression layout)
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = max_sets * 3;  // 3 buffers per set max
        
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        pool_info.maxSets = max_sets;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        
        VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create descriptor pool (VkResult: " +
                                   std::to_string(static_cast<int>(result)) + ")");
        }
    }
    
    ~DescriptorPool() {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
    }
    
    // Delete copy operations
    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;
    
    // Allow move operations
    DescriptorPool(DescriptorPool&& other) noexcept
        : device_(other.device_), pool_(other.pool_)
    {
        other.pool_ = VK_NULL_HANDLE;
    }
    
    DescriptorPool& operator=(DescriptorPool&& other) noexcept {
        if (this != &other) {
            if (pool_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, pool_, nullptr);
            }
            device_ = other.device_;
            pool_ = other.pool_;
            other.pool_ = VK_NULL_HANDLE;
        }
        return *this;
    }
    
    // Allocate a descriptor set from this pool
    VkDescriptorSet allocate(VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &layout;
        
        VkDescriptorSet descriptor_set;
        VkResult result = vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate descriptor set (VkResult: " +
                                   std::to_string(static_cast<int>(result)) + ")");
        }
        
        return descriptor_set;
    }
    
    // Free a descriptor set back to the pool
    void free(VkDescriptorSet descriptor_set) {
        vkFreeDescriptorSets(device_, pool_, 1, &descriptor_set);
    }
    
    // Reset the entire pool (frees all allocated sets)
    void reset() {
        vkResetDescriptorPool(device_, pool_, 0);
    }
    
    VkDescriptorPool get() const { return pool_; }

private:
    VkDevice device_;
    VkDescriptorPool pool_;
};

// Simple fixed-size thread pool to keep backend execution async.
// This mirrors the CPU backend model but is local to the Vulkan backend
// to keep responsibilities self-contained.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count)
        : stop_(false)
    {
        if (thread_count == 0) {
            thread_count = 1;
        }

        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();

        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop();
            }

            job();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    bool                              stop_;
};

// Find a memory type index that satisfies the requested properties.
// Returns UINT32_MAX if no matching type is found.
uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props,
                          uint32_t type_bits,
                          VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

// Vulkan backend implementation that can:
//  - Read file data into host-visible staging buffers.
//  - Copy staging buffers into GPU buffers (file -> GPU).
//  - Copy GPU buffers into staging buffers and write to disk (GPU -> file).
//
// This backend is intentionally minimal. It focuses on correctness and a clear
// data flow that is suitable for integration into Wine/Proton without shims.
class VulkanBackend final : public Backend {
public:
    // Construct the backend. If external Vulkan objects are provided, this
    // backend will borrow them without taking ownership.
    explicit VulkanBackend(const VulkanBackendConfig& config)
        : pool_(config.worker_count)
    {
        init(config);
    }

    // Destroy the backend and release only the resources we created.
    ~VulkanBackend() override {
        cleanup();
    }

    // Submit work asynchronously. The Request is copied into the worker
    // lambda to decouple lifetime from the caller.
    void submit(Request req, CompletionCallback on_complete) override {
        pool_.submit([this, req, on_complete]() mutable {
            // Validate the request before performing any GPU operations.
            if (req.fd < 0) {
                report_request_error("vulkan",
                                     "submit",
                                     "Invalid file descriptor",
                                     req,
                                     EBADF,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EBADF;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if (req.size == 0) {
                report_request_error("vulkan",
                                     "submit",
                                     "Zero-length request is not allowed",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if (req.op == RequestOp::Read && req.dst_memory == RequestMemory::Host &&
                req.dst == nullptr) {
                report_request_error("vulkan",
                                     "submit",
                                     "Read request missing destination buffer",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if (req.op == RequestOp::Write && req.src_memory == RequestMemory::Host &&
                req.src == nullptr) {
                report_request_error("vulkan",
                                     "submit",
                                     "Write request missing source buffer",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            handle_request(req);
            if (on_complete) {
                on_complete(req);
            }
        });
    }

private:
    // Initialize Vulkan context. Either borrow existing objects from config
    // or create a minimal Vulkan instance/device/queue/pool.
    void init(const VulkanBackendConfig& config) {
        if (config.device != VK_NULL_HANDLE) {
            instance_ = config.instance;
            physical_device_ = config.physical_device;
            device_ = config.device;
            queue_ = config.queue;
            queue_family_index_ = config.queue_family_index;
            command_pool_ = config.command_pool;
            owns_instance_ = false;
            owns_device_ = false;
            owns_command_pool_ = (command_pool_ == VK_NULL_HANDLE);
        } else {
            VkApplicationInfo app_info{};
            app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName = "ds-runtime";
            app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
            app_info.pEngineName = "ds-runtime";
            app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
            app_info.apiVersion = VK_API_VERSION_1_1;

            VkInstanceCreateInfo instance_info{};
            instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instance_info.pApplicationInfo = &app_info;

            if (vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
                report_error("vulkan",
                             "vkCreateInstance",
                             "Failed to create Vulkan instance",
                             EIO,
                             __FILE__,
                             __LINE__,
                             __func__);
                return;
            }
            owns_instance_ = true;

            uint32_t device_count = 0;
            vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
            if (device_count == 0) {
                report_error("vulkan",
                             "vkEnumeratePhysicalDevices",
                             "No Vulkan devices available",
                             ENODEV,
                             __FILE__,
                             __LINE__,
                             __func__);
                return;
            }
            std::vector<VkPhysicalDevice> devices(device_count);
            vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
            physical_device_ = devices[0];

            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());
            for (uint32_t i = 0; i < family_count; ++i) {
                if (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                    queue_family_index_ = i;
                    break;
                }
            }

            float priority = 1.0f;
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = queue_family_index_;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;

            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;

            if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
                report_error("vulkan",
                             "vkCreateDevice",
                             "Failed to create Vulkan device",
                             EIO,
                             __FILE__,
                             __LINE__,
                             __func__);
                return;
            }
            owns_device_ = true;
            vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
            owns_command_pool_ = true;
        }

        // Ensure a command pool exists for transient copy commands.
        if (command_pool_ == VK_NULL_HANDLE) {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = queue_family_index_;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) == VK_SUCCESS) {
                owns_command_pool_ = true;
            } else {
                report_error("vulkan",
                             "vkCreateCommandPool",
                             "Failed to create command pool",
                             EIO,
                             __FILE__,
                             __LINE__,
                             __func__);
            }
        }

        if (physical_device_ != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_props_);
        }
    }

    // Clean up only the Vulkan resources we own.
    void cleanup() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        if (owns_command_pool_ && command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        if (owns_device_ && device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }
        if (owns_instance_ && instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    // Route the request to the appropriate data path based on memory targets.
    void handle_request(Request& req) {
        if (device_ == VK_NULL_HANDLE || physical_device_ == VK_NULL_HANDLE) {
            report_request_error("vulkan",
                                 "handle_request",
                                 "Vulkan device not initialized",
                                 req,
                                 EINVAL,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            return;
        }

        // GPU -> file path (write).
        if (req.op == RequestOp::Write && req.src_memory == RequestMemory::Gpu) {
            handle_gpu_to_file(req);
        // File -> GPU path (read).
        } else if (req.op == RequestOp::Read && req.dst_memory == RequestMemory::Gpu) {
            handle_file_to_gpu(req);
        } else {
            // Host-only path (read/write).
            handle_host_io(req);
        }
    }

    // Host-only I/O fallback path (no GPU buffers involved).
    // Host-only I/O fallback path (no GPU buffers involved).
    void handle_host_io(Request& req) {
        ssize_t io_bytes = 0;
        if (req.op == RequestOp::Write) {
            io_bytes = ::pwrite(
                req.fd,
                req.src,
                req.size,
                static_cast<off_t>(req.offset)
            );
        } else {
            io_bytes = ::pread(
                req.fd,
                req.dst,
                req.size,
                static_cast<off_t>(req.offset)
            );
        }

        if (io_bytes < 0) {
            report_request_error("vulkan",
                                 req.op == RequestOp::Write ? "pwrite" : "pread",
                                 "Host I/O failed",
                                 req,
                                 errno,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = errno;
        } else {
            req.status = RequestStatus::Ok;
            req.errno_value = 0;
        }
    }

    // Read file data into a staging buffer, then copy into the GPU buffer.
    void handle_file_to_gpu(Request& req) {
        VkBuffer gpu_buffer = reinterpret_cast<VkBuffer>(req.gpu_buffer);
        if (gpu_buffer == VK_NULL_HANDLE) {
            report_request_error("vulkan",
                                 "file_to_gpu",
                                 "GPU buffer handle is null",
                                 req,
                                 EINVAL,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            return;
        }

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        if (!create_staging_buffer(req.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   staging_buffer, staging_memory)) {
            report_request_error("vulkan",
                                 "create_staging_buffer",
                                 "Failed to allocate staging buffer",
                                 req,
                                 ENOMEM,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = ENOMEM;
            return;
        }

        // Stage file contents in a host-visible buffer.
        void* mapped = nullptr;
        if (vkMapMemory(device_, staging_memory, 0, req.size, 0, &mapped) != VK_SUCCESS) {
            report_request_error("vulkan",
                                 "vkMapMemory",
                                 "Failed to map staging buffer memory",
                                 req,
                                 EIO,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = EIO;
            return;
        }
        const ssize_t rd = ::pread(
            req.fd,
            mapped,
            req.size,
            static_cast<off_t>(req.offset)
        );
        vkUnmapMemory(device_, staging_memory);

        if (rd < 0) {
            report_request_error("vulkan",
                                 "pread",
                                 "Failed to read file into staging buffer",
                                 req,
                                 errno,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = errno;
            return;
        }

        // Copy staged contents into the GPU buffer.
        if (!submit_copy(staging_buffer, gpu_buffer, req.size, 0, req.gpu_offset)) {
            report_request_error("vulkan",
                                 "vkCmdCopyBuffer",
                                 "Failed to copy staging buffer to GPU buffer",
                                 req,
                                 EIO,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = EIO;
            return;
        }

        destroy_buffer(staging_buffer, staging_memory);
        req.status = RequestStatus::Ok;
        req.errno_value = 0;
    }

    // Copy GPU buffer contents into a staging buffer, then write to disk.
    void handle_gpu_to_file(Request& req) {
        VkBuffer gpu_buffer = reinterpret_cast<VkBuffer>(req.gpu_buffer);
        if (gpu_buffer == VK_NULL_HANDLE) {
            report_request_error("vulkan",
                                 "gpu_to_file",
                                 "GPU buffer handle is null",
                                 req,
                                 EINVAL,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            return;
        }

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        if (!create_staging_buffer(req.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   staging_buffer, staging_memory)) {
            report_request_error("vulkan",
                                 "create_staging_buffer",
                                 "Failed to allocate staging buffer",
                                 req,
                                 ENOMEM,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = ENOMEM;
            return;
        }

        // Copy GPU buffer contents into staging.
        if (!submit_copy(gpu_buffer, staging_buffer, req.size, req.gpu_offset, 0)) {
            report_request_error("vulkan",
                                 "vkCmdCopyBuffer",
                                 "Failed to copy GPU buffer to staging buffer",
                                 req,
                                 EIO,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = EIO;
            return;
        }

        // Write staged contents to disk.
        void* mapped = nullptr;
        if (vkMapMemory(device_, staging_memory, 0, req.size, 0, &mapped) != VK_SUCCESS) {
            report_request_error("vulkan",
                                 "vkMapMemory",
                                 "Failed to map staging buffer memory",
                                 req,
                                 EIO,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = EIO;
            return;
        }
        const ssize_t wr = ::pwrite(
            req.fd,
            mapped,
            req.size,
            static_cast<off_t>(req.offset)
        );
        vkUnmapMemory(device_, staging_memory);

        destroy_buffer(staging_buffer, staging_memory);

        if (wr < 0) {
            report_request_error("vulkan",
                                 "pwrite",
                                 "Failed to write staging buffer to file",
                                 req,
                                 errno,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = errno;
            return;
        }

        req.status = RequestStatus::Ok;
        req.errno_value = 0;
    }

    // Allocate a host-visible staging buffer for file transfers.
    bool create_staging_buffer(std::size_t size,
                               VkBufferUsageFlags usage,
                               VkBuffer& buffer,
                               VkDeviceMemory& memory) {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkCreateBuffer",
                         "Failed to create staging buffer",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
            return false;
        }

        VkMemoryRequirements mem_req{};
        vkGetBufferMemoryRequirements(device_, buffer, &mem_req);

        const uint32_t type_index = find_memory_type(
            memory_props_,
            mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        if (type_index == UINT32_MAX) {
            report_error("vulkan",
                         "find_memory_type",
                         "No suitable memory type for staging buffer",
                         ENOMEM,
                         __FILE__,
                         __LINE__,
                         __func__);
            vkDestroyBuffer(device_, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex = type_index;

        if (vkAllocateMemory(device_, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkAllocateMemory",
                         "Failed to allocate staging buffer memory",
                         ENOMEM,
                         __FILE__,
                         __LINE__,
                         __func__);
            vkDestroyBuffer(device_, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        vkBindBufferMemory(device_, buffer, memory, 0);
        return true;
    }

    // Free a staging buffer and its memory.
    void destroy_buffer(VkBuffer buffer, VkDeviceMemory memory) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory, nullptr);
        }
    }

    // Submit a synchronous copy command and wait for completion.
    // This is intentionally simple and safe; future versions can
    // migrate to timeline semaphores or batched submissions.
    bool submit_copy(VkBuffer src,
                     VkBuffer dst,
                     VkDeviceSize size,
                     VkDeviceSize src_offset,
                     VkDeviceSize dst_offset) {
        std::lock_guard<std::mutex> lock(vk_mutex_);

        if (command_pool_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE) {
            report_error("vulkan",
                         "submit_copy",
                         "Command pool or queue not initialized",
                         EINVAL,
                         __FILE__,
                         __LINE__,
                         __func__);
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool_;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &alloc_info, &cmd) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkAllocateCommandBuffers",
                         "Failed to allocate command buffer",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkBeginCommandBuffer",
                         "Failed to begin command buffer",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
            vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
            return false;
        }

        VkBufferCopy region{};
        region.srcOffset = src_offset;
        region.dstOffset = dst_offset;
        region.size = size;
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkEndCommandBuffer",
                         "Failed to end command buffer",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
            vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
            return false;
        }

        // Fence ensures the copy completes before we read/write staging memory.
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(device_, &fence_info, nullptr, &fence) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkCreateFence",
                         "Failed to create fence",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
            vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
            return false;
        }

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;

        if (vkQueueSubmit(queue_, 1, &submit_info, fence) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkQueueSubmit",
                         "Queue submission failed",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
            vkDestroyFence(device_, fence, nullptr);
            vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
            return false;
        }

        if (vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_C(1'000'000'000)) != VK_SUCCESS) {
            report_error("vulkan",
                         "vkWaitForFences",
                         "Fence wait failed",
                         EIO,
                         __FILE__,
                         __LINE__,
                         __func__);
        }

        vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        return true;
    }

    ThreadPool pool_;
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    uint32_t queue_family_index_{0};
    VkPhysicalDeviceMemoryProperties memory_props_{};
    bool owns_instance_{false};
    bool owns_device_{false};
    bool owns_command_pool_{false};
    std::mutex vk_mutex_;
};

} // namespace

std::shared_ptr<Backend> make_vulkan_backend(const VulkanBackendConfig& config) {
    return std::make_shared<VulkanBackend>(config);
}

} // namespace ds
