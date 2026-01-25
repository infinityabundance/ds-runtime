// SPDX-License-Identifier: Apache-2.0
// Asset streaming demo for ds-runtime.
//
// This example demonstrates:
//  - Writing a "packed" asset file (two payloads back-to-back).
//  - Submitting concurrent read requests for both payloads.
//  - Using the error reporting callback for verbose diagnostics.
//  - Performing a basic transformation request (FakeUppercase).

#include "ds_runtime.hpp"

#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

void verbose_error_logger(const ds::ErrorContext& ctx) {
    std::cerr << "[asset_streaming][error]"
              << " subsystem=" << ctx.subsystem
              << " operation=" << ctx.operation
              << " errno=" << ctx.errno_value
              << " detail=\"" << ctx.detail << "\""
              << " file=" << ctx.file
              << " line=" << ctx.line
              << " function=" << ctx.function
              << std::endl;
}

} // namespace

int main() {
    using namespace ds;

    set_error_callback(verbose_error_logger);

    const char* filename = "streaming_assets.bin";
    const std::string payload_a = "texture:albedo.dds";
    const std::string payload_b = "shader:lighting.hlsl";

    // Build a packed file with two assets (A then B).
    {
        const int fd = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            report_error("demo",
                         "open",
                         "Failed to create asset pack file",
                         errno,
                         __FILE__,
                         __LINE__,
                         __func__);
            return 1;
        }

        if (::write(fd, payload_a.data(), payload_a.size()) < 0 ||
            ::write(fd, payload_b.data(), payload_b.size()) < 0) {
            report_error("demo",
                         "write",
                         "Failed to write asset payloads",
                         errno,
                         __FILE__,
                         __LINE__,
                         __func__);
            ::close(fd);
            return 1;
        }
        ::close(fd);
    }

    // Open the file for reading.
    const int fd = ::open(filename, O_RDONLY);
    if (fd < 0) {
        report_error("demo",
                     "open",
                     "Failed to open asset pack for reading",
                     errno,
                     __FILE__,
                     __LINE__,
                     __func__);
        return 1;
    }

    // Prepare destination buffers with room for a null terminator.
    std::vector<char> buffer_a(payload_a.size() + 1, '\0');
    std::vector<char> buffer_b(payload_b.size() + 1, '\0');

    auto backend = make_cpu_backend(/*worker_count=*/2);
    Queue queue(backend);

    // Request A: raw read at offset 0.
    Request req_a;
    req_a.fd = fd;
    req_a.offset = 0;
    req_a.size = payload_a.size();
    req_a.dst = buffer_a.data();
    req_a.compression = Compression::None;

    // Request B: uppercase transform at offset payload_a.size().
    Request req_b;
    req_b.fd = fd;
    req_b.offset = payload_a.size();
    req_b.size = payload_b.size();
    req_b.dst = buffer_b.data();
    req_b.compression = Compression::FakeUppercase;

    queue.enqueue(req_a);
    queue.enqueue(req_b);

    std::cout << "[asset_streaming] submitting 2 requests\n";
    queue.submit_all();
    queue.wait_all();

    const auto completed = queue.take_completed();
    for (const auto& done : completed) {
        std::cout << "[asset_streaming] completed op="
                  << (done.op == RequestOp::Write ? "write" : "read")
                  << " bytes=" << done.bytes_transferred
                  << " status=" << (done.status == RequestStatus::Ok ? "ok" : "error")
                  << "\n";
    }

    std::cout << "[asset_streaming] read A: \"" << buffer_a.data() << "\"\n";
    std::cout << "[asset_streaming] read B: \"" << buffer_b.data() << "\"\n";

    ::close(fd);
    set_error_callback(nullptr);
    return 0;
}
