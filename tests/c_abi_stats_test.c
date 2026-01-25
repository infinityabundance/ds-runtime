// SPDX-License-Identifier: Apache-2.0
// C ABI stats test for ds-runtime.
//
// This test validates the C API queue statistics helpers:
//  - ds_queue_total_completed
//  - ds_queue_total_failed
//  - ds_queue_total_bytes_transferred

#include "ds_runtime_c.h"

#include <assert.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

int main(void) {
    const char* filename = "c_abi_stats_test.bin";
    const char* payload = "c-abi-stats";

    const int fd_write = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);
    const ssize_t wr = write(fd_write, payload, strlen(payload));
    assert(wr == (ssize_t)strlen(payload));
    close(fd_write);

    const int fd_read = open(filename, O_RDONLY);
    assert(fd_read >= 0);

    char buffer[64] = {0};

    ds_backend_t* backend = ds_make_cpu_backend(1);
    assert(backend != NULL);

    ds_queue_t* queue = ds_queue_create(backend);
    assert(queue != NULL);

    ds_request req = {0};
    req.fd = fd_read;
    req.offset = 0;
    req.size = strlen(payload);
    req.dst = buffer;
    req.compression = DS_COMPRESSION_NONE;
    req.op = DS_REQUEST_OP_READ;
    req.dst_memory = DS_REQUEST_MEMORY_HOST;
    req.src_memory = DS_REQUEST_MEMORY_HOST;

    ds_queue_enqueue(queue, &req);
    ds_queue_submit_all(queue, NULL, NULL);
    ds_queue_wait_all(queue);

    assert(ds_queue_total_completed(queue) == 1);
    assert(ds_queue_total_failed(queue) == 0);
    assert(ds_queue_total_bytes_transferred(queue) == strlen(payload));

    assert(strcmp(buffer, payload) == 0);

    ds_queue_release(queue);
    ds_backend_release(backend);
    close(fd_read);
    unlink(filename);
    return 0;
}
