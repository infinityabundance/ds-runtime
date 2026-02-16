// SPDX-License-Identifier: Apache-2.0
// Error handling test.
//
// This test verifies:
//  - Invalid file descriptor errors are reported correctly
//  - Error callback system works
//  - Request error context is populated correctly

#include "ds_runtime.hpp"

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

std::atomic<int> g_error_count{0};
ds::ErrorContext g_last_error;

void test_error_logger(const ds::ErrorContext& ctx) {
    ++g_error_count;
    g_last_error = ctx;
    std::cerr << "[error_test][error]"
              << " subsystem=" << ctx.subsystem
              << " operation=" << ctx.operation
              << " errno=" << ctx.errno_value
              << " detail=\"" << ctx.detail << "\""
              << std::endl;
}

void test_invalid_fd() {
    using namespace ds;

    g_error_count = 0;
    set_error_callback(test_error_logger);

    std::vector<char> buffer(100, '\0');
    Request req;
    req.fd = -1;  // Invalid file descriptor
    req.offset = 0;
    req.size = 100;
    req.dst = buffer.data();

    Queue queue(make_cpu_backend(1));
    queue.enqueue(req);
    queue.submit_all();
    queue.wait_all();

    // Should have triggered error callback
    assert(g_error_count.load() > 0);
    assert(g_last_error.subsystem == "cpu");
    assert(g_last_error.errno_value == EBADF);

    // Check completed request status
    auto completed = queue.take_completed();
    assert(completed.size() == 1);
    assert(completed[0].status == RequestStatus::IoError);
    assert(completed[0].errno_value == EBADF);
    assert(completed[0].bytes_transferred == 0);

    set_error_callback(nullptr);
    std::cout << "[error_test] test_invalid_fd PASSED\n";
}

void test_read_from_nonexistent_file() {
    using namespace ds;

    g_error_count = 0;
    set_error_callback(test_error_logger);

    // Try to open a file that doesn't exist
    const int fd = ::open("/tmp/nonexistent_file_12345.bin", O_RDONLY);
    if (fd >= 0) {
        ::close(fd);
        std::cerr << "[error_test] WARNING: test file unexpectedly exists, skipping test\n";
        return;
    }

    std::vector<char> buffer(100, '\0');
    Request req;
    req.fd = fd;  // Will be -1
    req.offset = 0;
    req.size = 100;
    req.dst = buffer.data();

    Queue queue(make_cpu_backend(1));
    queue.enqueue(req);
    queue.submit_all();
    queue.wait_all();

    // Should have error
    assert(g_error_count.load() > 0);

    auto completed = queue.take_completed();
    assert(completed.size() == 1);
    assert(completed[0].status == RequestStatus::IoError);
    assert(completed[0].bytes_transferred == 0);

    set_error_callback(nullptr);
    std::cout << "[error_test] test_read_from_nonexistent_file PASSED\n";
}

void test_gdeflate_error() {
    using namespace ds;

    g_error_count = 0;
    set_error_callback(test_error_logger);

    const char* filename = "error_test_gdeflate.bin";
    const char* payload = "test data";
    
    // Write file
    const int fd_write = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);
    ::write(fd_write, payload, std::strlen(payload));
    ::close(fd_write);

    // Try to read with GDeflate (not implemented)
    const int fd_read = ::open(filename, O_RDONLY);
    assert(fd_read >= 0);

    std::vector<char> buffer(100, '\0');
    Request req;
    req.fd = fd_read;
    req.offset = 0;
    req.size = std::strlen(payload);
    req.dst = buffer.data();
    req.compression = Compression::GDeflate;

    Queue queue(make_cpu_backend(1));
    queue.enqueue(req);
    queue.submit_all();
    queue.wait_all();

    // Should have error for unsupported compression
    assert(g_error_count.load() > 0);
    assert(g_last_error.subsystem == "cpu");
    assert(g_last_error.operation == "decompression");
    assert(g_last_error.errno_value == ENOTSUP);

    auto completed = queue.take_completed();
    assert(completed.size() == 1);
    assert(completed[0].status == RequestStatus::IoError);
    assert(completed[0].errno_value == ENOTSUP);

    ::close(fd_read);
    ::unlink(filename);

    set_error_callback(nullptr);
    std::cout << "[error_test] test_gdeflate_error PASSED\n";
}

void test_error_context_has_request_info() {
    using namespace ds;

    g_error_count = 0;
    set_error_callback(test_error_logger);

    std::vector<char> buffer(100, '\0');
    Request req;
    req.fd = -1;  // Invalid
    req.offset = 12345;
    req.size = 100;
    req.dst = buffer.data();
    req.op = RequestOp::Read;
    req.dst_memory = RequestMemory::Host;

    Queue queue(make_cpu_backend(1));
    queue.enqueue(req);
    queue.submit_all();
    queue.wait_all();

    // Verify error context has request information
    assert(g_error_count.load() > 0);
    assert(g_last_error.has_request);
    assert(g_last_error.fd == -1);
    assert(g_last_error.offset == 12345);
    assert(g_last_error.size == 100);
    assert(g_last_error.op == RequestOp::Read);
    assert(g_last_error.dst_memory == RequestMemory::Host);

    set_error_callback(nullptr);
    std::cout << "[error_test] test_error_context_has_request_info PASSED\n";
}

} // namespace

int main() {
    test_invalid_fd();
    test_read_from_nonexistent_file();
    test_gdeflate_error();
    test_error_context_has_request_info();

    std::cout << "[error_test] ALL TESTS PASSED\n";
    return 0;
}
