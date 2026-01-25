// SPDX-License-Identifier: Apache-2.0
// GDeflate stub test.
//
// This test verifies that the runtime returns a clear, structured error
// when a GDeflate compression mode is requested.

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

void test_error_logger(const ds::ErrorContext& ctx) {
    ++g_error_count;
    std::cerr << "[gdeflate_stub_test][error]"
              << " subsystem=" << ctx.subsystem
              << " operation=" << ctx.operation
              << " errno=" << ctx.errno_value
              << " detail=\"" << ctx.detail << "\""
              << std::endl;
}

} // namespace

int main() {
    using namespace ds;

    set_error_callback(test_error_logger);

    const char* filename = "gdeflate_stub_test.bin";
    const char* payload = "gdeflate-stub";

    const int fd_write = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);
    const ssize_t wr = ::write(fd_write, payload, std::strlen(payload));
    assert(wr == static_cast<ssize_t>(std::strlen(payload)));
    ::close(fd_write);

    const int fd_read = ::open(filename, O_RDONLY);
    assert(fd_read >= 0);

    std::vector<char> buffer(std::strlen(payload) + 1, '\0');
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

    assert(g_error_count.load() > 0);

    ::close(fd_read);
    set_error_callback(nullptr);
    return 0;
}
