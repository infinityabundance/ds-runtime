// SPDX-License-Identifier: Apache-2.0
// Vulkan backend implementation for ds-runtime.

#include "ds_runtime_vulkan.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace ds {

namespace {

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

class VulkanBackend final : public Backend {
public:
    explicit VulkanBackend(const VulkanBackendConfig& config)
        : pool_(config.worker_count)
    {
        init(config);
    }

    ~VulkanBackend() override {
        cleanup();
    }

    void submit(Request req, CompletionCallback on_complete) override {
        pool_.submit([this, req, on_complete]() mutable {
            handle_request(req);
            if (on_complete) {
                on_complete(req);
            }
        });
    }

private:
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
                return;
            }
            owns_instance_ = true;

            uint32_t device_count = 0;
            vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
            if (device_count == 0) {
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
                return;
            }
            owns_device_ = true;
            vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
            owns_command_pool_ = true;
        }

        if (command_pool_ == VK_NULL_HANDLE) {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = queue_family_index_;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) == VK_SUCCESS) {
                owns_command_pool_ = true;
            }
        }

        if (physical_device_ != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_props_);
        }
    }

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

    void handle_request(Request& req) {
        if (device_ == VK_NULL_HANDLE || physical_device_ == VK_NULL_HANDLE) {
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            return;
        }

        if (req.op == RequestOp::Write && req.src_memory == RequestMemory::Gpu) {
            handle_gpu_to_file(req);
        } else if (req.op == RequestOp::Read && req.dst_memory == RequestMemory::Gpu) {
            handle_file_to_gpu(req);
        } else {
            handle_host_io(req);
        }
    }

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
            req.status = RequestStatus::IoError;
            req.errno_value = errno;
        } else {
            req.status = RequestStatus::Ok;
            req.errno_value = 0;
        }
    }

    void handle_file_to_gpu(Request& req) {
        VkBuffer gpu_buffer = reinterpret_cast<VkBuffer>(req.gpu_buffer);
        if (gpu_buffer == VK_NULL_HANDLE) {
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            return;
        }

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        if (!create_staging_buffer(req.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   staging_buffer, staging_memory)) {
            req.status = RequestStatus::IoError;
            req.errno_value = ENOMEM;
            return;
        }

        void* mapped = nullptr;
        vkMapMemory(device_, staging_memory, 0, req.size, 0, &mapped);
        const ssize_t rd = ::pread(
            req.fd,
            mapped,
            req.size,
            static_cast<off_t>(req.offset)
        );
        vkUnmapMemory(device_, staging_memory);

        if (rd < 0) {
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = errno;
            return;
        }

        if (!submit_copy(staging_buffer, gpu_buffer, req.size, 0, req.gpu_offset)) {
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = EIO;
            return;
        }

        destroy_buffer(staging_buffer, staging_memory);
        req.status = RequestStatus::Ok;
        req.errno_value = 0;
    }

    void handle_gpu_to_file(Request& req) {
        VkBuffer gpu_buffer = reinterpret_cast<VkBuffer>(req.gpu_buffer);
        if (gpu_buffer == VK_NULL_HANDLE) {
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            return;
        }

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        if (!create_staging_buffer(req.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   staging_buffer, staging_memory)) {
            req.status = RequestStatus::IoError;
            req.errno_value = ENOMEM;
            return;
        }

        if (!submit_copy(gpu_buffer, staging_buffer, req.size, req.gpu_offset, 0)) {
            destroy_buffer(staging_buffer, staging_memory);
            req.status = RequestStatus::IoError;
            req.errno_value = EIO;
            return;
        }

        void* mapped = nullptr;
        vkMapMemory(device_, staging_memory, 0, req.size, 0, &mapped);
        const ssize_t wr = ::pwrite(
            req.fd,
            mapped,
            req.size,
            static_cast<off_t>(req.offset)
        );
        vkUnmapMemory(device_, staging_memory);

        destroy_buffer(staging_buffer, staging_memory);

        if (wr < 0) {
            req.status = RequestStatus::IoError;
            req.errno_value = errno;
            return;
        }

        req.status = RequestStatus::Ok;
        req.errno_value = 0;
    }

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
            vkDestroyBuffer(device_, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex = type_index;

        if (vkAllocateMemory(device_, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyBuffer(device_, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        vkBindBufferMemory(device_, buffer, memory, 0);
        return true;
    }

    void destroy_buffer(VkBuffer buffer, VkDeviceMemory memory) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory, nullptr);
        }
    }

    bool submit_copy(VkBuffer src,
                     VkBuffer dst,
                     VkDeviceSize size,
                     VkDeviceSize src_offset,
                     VkDeviceSize dst_offset) {
        std::lock_guard<std::mutex> lock(vk_mutex_);

        if (command_pool_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE) {
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool_;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &alloc_info, &cmd) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(cmd, &begin_info);

        VkBufferCopy region{};
        region.srcOffset = src_offset;
        region.dstOffset = dst_offset;
        region.size = size;
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);

        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(device_, &fence_info, nullptr, &fence) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
            return false;
        }

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;

        if (vkQueueSubmit(queue_, 1, &submit_info, fence) != VK_SUCCESS) {
            vkDestroyFence(device_, fence, nullptr);
            vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
            return false;
        }

        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_C(1'000'000'000));

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
