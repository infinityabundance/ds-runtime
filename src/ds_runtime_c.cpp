// SPDX-License-Identifier: Apache-2.0
// C-compatible wrapper layer for ds-runtime.

#include "ds_runtime_c.h"

#include "ds_runtime.hpp"

#ifdef DS_RUNTIME_HAS_VULKAN
#include "ds_runtime_vulkan.hpp"
#endif
#ifdef DS_RUNTIME_HAS_IO_URING
#include "ds_runtime_uring.hpp"
#endif

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// Map C enum to C++ enum for compression modes.
ds::Compression to_cpp_compression(ds_compression compression) {
    switch (compression) {
        case DS_COMPRESSION_FAKE_UPPERCASE:
            return ds::Compression::FakeUppercase;
        case DS_COMPRESSION_NONE:
        default:
            return ds::Compression::None;
    }
}

// Map C enum to C++ enum for read/write operations.
ds::RequestOp to_cpp_op(ds_request_op op) {
    switch (op) {
        case DS_REQUEST_OP_WRITE:
            return ds::RequestOp::Write;
        case DS_REQUEST_OP_READ:
        default:
            return ds::RequestOp::Read;
    }
}

// Map C enum to C++ enum for memory location (host/GPU).
ds::RequestMemory to_cpp_memory(ds_request_memory memory) {
    switch (memory) {
        case DS_REQUEST_MEMORY_GPU:
            return ds::RequestMemory::Gpu;
        case DS_REQUEST_MEMORY_HOST:
        default:
            return ds::RequestMemory::Host;
    }
}

// Map C++ request status back to C enum for ABI consumers.
ds_request_status to_c_status(ds::RequestStatus status) {
    switch (status) {
        case ds::RequestStatus::Ok:
            return DS_REQUEST_OK;
        case ds::RequestStatus::IoError:
            return DS_REQUEST_IO_ERROR;
        case ds::RequestStatus::Pending:
        default:
            return DS_REQUEST_PENDING;
    }
}

// Translate a C request struct into the C++ Request type.
// This keeps the C ABI stable while allowing internal evolution.
ds::Request to_cpp_request(const ds_request& request) {
    ds::Request cpp{};
    cpp.fd = request.fd;
    cpp.offset = request.offset;
    cpp.size = request.size;
    cpp.dst = request.dst;
    cpp.src = request.src;
    cpp.gpu_buffer = request.gpu_buffer;
    cpp.gpu_offset = request.gpu_offset;
    cpp.op = to_cpp_op(request.op);
    cpp.dst_memory = to_cpp_memory(request.dst_memory);
    cpp.src_memory = to_cpp_memory(request.src_memory);
    cpp.compression = to_cpp_compression(request.compression);
    cpp.status = ds::RequestStatus::Pending;
    cpp.errno_value = 0;
    return cpp;
}

// Update the C request struct with completion status/error.
void update_c_request(ds_request& c_req, const ds::Request& cpp_req) {
    c_req.status = to_c_status(cpp_req.status);
    c_req.errno_value = cpp_req.errno_value;
}

// Track a C request alongside its C++ equivalent so we can
// update the C struct on completion.
struct PendingRequest {
    ds::Request cpp_request;
    ds_request* c_request = nullptr;
};

// C ABI queue wrapper. Owns a C++ backend and manages
// pending submission and in-flight tracking.
class CQueue {
public:
    explicit CQueue(std::shared_ptr<ds::Backend> backend)
        : backend_(std::move(backend))
        , in_flight_(0)
    {}

    // Enqueue a request. Ownership of the C struct remains with the caller.
    void enqueue(ds_request* request) {
        if (!request) {
            return;
        }
        request->status = DS_REQUEST_PENDING;
        request->errno_value = 0;
        PendingRequest pending{to_cpp_request(*request), request};
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.push_back(std::move(pending));
    }

    // Submit all enqueued requests to the backend.
    // The callback is invoked once per request completion.
    void submit_all(ds_completion_callback callback, void* user_data) {
        std::vector<PendingRequest> to_submit;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            to_submit.swap(pending_);
        }

        for (auto& pending : to_submit) {
            in_flight_.fetch_add(1, std::memory_order_relaxed);
            ds_request* c_request = pending.c_request;

            backend_->submit(
                std::move(pending.cpp_request),
                [this, c_request, callback, user_data](ds::Request& completed) {
                    if (c_request) {
                        update_c_request(*c_request, completed);
                    }

                    if (callback) {
                        callback(c_request, user_data);
                    }

                    const auto remaining =
                        in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (remaining == 0) {
                        std::lock_guard<std::mutex> lock(wait_mtx_);
                        wait_cv_.notify_all();
                    }
                }
            );
        }
    }

    // Block until all in-flight requests have completed.
    void wait_all() {
        std::unique_lock<std::mutex> lock(wait_mtx_);
        wait_cv_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_acquire) == 0;
        });
    }

    // Return a snapshot of the in-flight request count.
    size_t in_flight() const {
        return in_flight_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<ds::Backend> backend_;
    mutable std::mutex mtx_;
    std::vector<PendingRequest> pending_;
    std::atomic<size_t> in_flight_;
    mutable std::mutex wait_mtx_;
    std::condition_variable wait_cv_;
};

} // namespace

struct ds_backend {
    std::shared_ptr<ds::Backend> backend;
};

struct ds_queue {
    std::unique_ptr<CQueue> queue;
};

ds_backend_t* ds_make_cpu_backend(size_t worker_count) {
    return new ds_backend_t{ds::make_cpu_backend(worker_count)};
}

void ds_backend_release(ds_backend_t* backend) {
    delete backend;
}

ds_queue_t* ds_queue_create(ds_backend_t* backend) {
    if (!backend) {
        return nullptr;
    }
    auto queue = std::make_unique<CQueue>(backend->backend);
    return new ds_queue_t{std::move(queue)};
}

void ds_queue_release(ds_queue_t* queue) {
    delete queue;
}

void ds_queue_enqueue(ds_queue_t* queue, ds_request* request) {
    if (!queue) {
        return;
    }
    queue->queue->enqueue(request);
}

void ds_queue_submit_all(ds_queue_t* queue, ds_completion_callback callback, void* user_data) {
    if (!queue) {
        return;
    }
    queue->queue->submit_all(callback, user_data);
}

void ds_queue_wait_all(ds_queue_t* queue) {
    if (!queue) {
        return;
    }
    queue->queue->wait_all();
}

size_t ds_queue_in_flight(const ds_queue_t* queue) {
    if (!queue) {
        return 0;
    }
    return queue->queue->in_flight();
}

#ifdef DS_RUNTIME_HAS_VULKAN
ds_backend_t* ds_make_vulkan_backend(const ds_vulkan_backend_config* config) {
    if (!config) {
        return nullptr;
    }
    ds::VulkanBackendConfig cpp_config{};
    cpp_config.instance = reinterpret_cast<VkInstance>(config->instance);
    cpp_config.physical_device = reinterpret_cast<VkPhysicalDevice>(config->physical_device);
    cpp_config.device = reinterpret_cast<VkDevice>(config->device);
    cpp_config.queue = reinterpret_cast<VkQueue>(config->queue);
    cpp_config.queue_family_index = config->queue_family_index;
    cpp_config.command_pool = reinterpret_cast<VkCommandPool>(config->command_pool);
    cpp_config.worker_count = config->worker_count;
    return new ds_backend_t{ds::make_vulkan_backend(cpp_config)};
}
#endif

#ifdef DS_RUNTIME_HAS_IO_URING
ds_backend_t* ds_make_io_uring_backend(const ds_io_uring_backend_config* config) {
    ds::IoUringBackendConfig cpp_config{};
    if (config) {
        cpp_config.entries = config->entries;
    }
    return new ds_backend_t{ds::make_io_uring_backend(cpp_config)};
}
#endif
