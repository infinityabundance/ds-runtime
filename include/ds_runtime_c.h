// SPDX-License-Identifier: Apache-2.0
// C-compatible API surface for ds-runtime shared library.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds_backend ds_backend_t;
typedef struct ds_queue ds_queue_t;

typedef enum ds_compression {
    DS_COMPRESSION_NONE = 0,
    DS_COMPRESSION_FAKE_UPPERCASE = 1
} ds_compression;

typedef enum ds_request_status {
    DS_REQUEST_PENDING = 0,
    DS_REQUEST_OK = 1,
    DS_REQUEST_IO_ERROR = 2
} ds_request_status;

typedef enum ds_request_op {
    DS_REQUEST_OP_READ = 0,
    DS_REQUEST_OP_WRITE = 1
} ds_request_op;

typedef enum ds_request_memory {
    DS_REQUEST_MEMORY_HOST = 0,
    DS_REQUEST_MEMORY_GPU = 1
} ds_request_memory;

typedef struct ds_request {
    int                fd;
    uint64_t           offset;
    size_t             size;
    void*              dst;
    const void*        src;
    void*              gpu_buffer;
    uint64_t           gpu_offset;
    ds_request_op      op;
    ds_request_memory  dst_memory;
    ds_request_memory  src_memory;
    ds_compression     compression;
    ds_request_status  status;
    int                errno_value;
} ds_request;

typedef void (*ds_completion_callback)(ds_request* request, void* user_data);

ds_backend_t* ds_make_cpu_backend(size_t worker_count);
void ds_backend_release(ds_backend_t* backend);

ds_queue_t* ds_queue_create(ds_backend_t* backend);
void ds_queue_release(ds_queue_t* queue);

void ds_queue_enqueue(ds_queue_t* queue, ds_request* request);
void ds_queue_submit_all(ds_queue_t* queue, ds_completion_callback callback, void* user_data);
void ds_queue_wait_all(ds_queue_t* queue);
size_t ds_queue_in_flight(const ds_queue_t* queue);

#ifdef DS_RUNTIME_HAS_VULKAN
typedef struct ds_vulkan_backend_config {
    void*    instance;
    void*    physical_device;
    void*    device;
    void*    queue;
    uint32_t queue_family_index;
    void*    command_pool;
    size_t   worker_count;
} ds_vulkan_backend_config;

ds_backend_t* ds_make_vulkan_backend(const ds_vulkan_backend_config* config);
#endif

#ifdef DS_RUNTIME_HAS_IO_URING
typedef struct ds_io_uring_backend_config {
    unsigned entries;
} ds_io_uring_backend_config;

ds_backend_t* ds_make_io_uring_backend(const ds_io_uring_backend_config* config);
#endif

#ifdef __cplusplus
}
#endif
