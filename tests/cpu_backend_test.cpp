// SPDX-License-Identifier: Apache-2.0
// Comprehensive CPU backend test.
//
// This test verifies:
//  - Basic read/write operations
//  - Partial reads are handled correctly
//  - bytes_transferred is set correctly
//  - FakeUppercase compression works
//  - Multiple concurrent requests work

#include "ds_runtime.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

void test_basic_read_write() {
    using namespace ds;

    const char* filename = "cpu_backend_test_rw.bin";
    const char* payload = "test-read-write-data";
    const size_t payload_len = std::strlen(payload);

    // Write using queue
    const int fd_write = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);

    Request write_req;
    write_req.fd = fd_write;
    write_req.offset = 0;
    write_req.size = payload_len;
    write_req.src = payload;
    write_req.op = RequestOp::Write;

    Queue write_queue(make_cpu_backend(2));
    write_queue.enqueue(write_req);
    write_queue.submit_all();
    write_queue.wait_all();

    ::close(fd_write);

    // Read back using queue
    const int fd_read = ::open(filename, O_RDONLY);
    assert(fd_read >= 0);

    std::vector<char> buffer(payload_len + 1, '\0');
    Request read_req;
    read_req.fd = fd_read;
    read_req.offset = 0;
    read_req.size = payload_len;
    read_req.dst = buffer.data();

    Queue read_queue(make_cpu_backend(2));
    read_queue.enqueue(read_req);
    read_queue.submit_all();
    read_queue.wait_all();

    // Verify using take_completed
    auto completed = read_queue.take_completed();
    assert(completed.size() == 1);
    assert(completed[0].status == RequestStatus::Ok);
    assert(completed[0].bytes_transferred == payload_len);

    assert(std::strncmp(buffer.data(), payload, payload_len) == 0);

    ::close(fd_read);
    ::unlink(filename);

    std::cout << "[cpu_backend_test] test_basic_read_write PASSED\n";
}

void test_partial_read() {
    using namespace ds;

    const char* filename = "cpu_backend_test_partial.bin";
    const char* payload = "short";
    const size_t payload_len = std::strlen(payload);

    // Write file
    const int fd_write = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);
    ::write(fd_write, payload, payload_len);
    ::close(fd_write);

    // Try to read more bytes than available
    const int fd_read = ::open(filename, O_RDONLY);
    assert(fd_read >= 0);

    std::vector<char> buffer(100, '\0');
    Request req;
    req.fd = fd_read;
    req.offset = 0;
    req.size = 100;  // More than file size
    req.dst = buffer.data();

    Queue queue(make_cpu_backend(1));
    queue.enqueue(req);
    queue.submit_all();
    queue.wait_all();

    auto completed = queue.take_completed();
    assert(completed.size() == 1);
    assert(completed[0].status == RequestStatus::Ok);
    // Should have read exactly payload_len bytes
    assert(completed[0].bytes_transferred == payload_len);

    ::close(fd_read);
    ::unlink(filename);

    std::cout << "[cpu_backend_test] test_partial_read PASSED\n";
}

void test_fake_uppercase() {
    using namespace ds;

    const char* filename = "cpu_backend_test_upper.bin";
    const char* payload = "lowercase text";
    const size_t payload_len = std::strlen(payload);

    // Write file
    const int fd_write = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);
    ::write(fd_write, payload, payload_len);
    ::close(fd_write);

    // Read with uppercase compression
    const int fd_read = ::open(filename, O_RDONLY);
    assert(fd_read >= 0);

    std::vector<char> buffer(payload_len + 1, '\0');
    Request req;
    req.fd = fd_read;
    req.offset = 0;
    req.size = payload_len;
    req.dst = buffer.data();
    req.compression = Compression::FakeUppercase;

    Queue queue(make_cpu_backend(1));
    queue.enqueue(req);
    queue.submit_all();
    queue.wait_all();

    // Verify uppercase
    const char* expected = "LOWERCASE TEXT";
    assert(std::strncmp(buffer.data(), expected, payload_len) == 0);

    ::close(fd_read);
    ::unlink(filename);

    std::cout << "[cpu_backend_test] test_fake_uppercase PASSED\n";
}

void test_multiple_requests() {
    using namespace ds;

    const char* filename = "cpu_backend_test_multi.bin";
    const char* payload = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const size_t payload_len = std::strlen(payload);

    // Write file
    const int fd_write = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd_write >= 0);
    ::write(fd_write, payload, payload_len);
    ::close(fd_write);

    // Submit multiple reads at different offsets
    const int fd_read = ::open(filename, O_RDONLY);
    assert(fd_read >= 0);

    std::vector<char> buffer1(10, '\0');
    std::vector<char> buffer2(10, '\0');
    std::vector<char> buffer3(10, '\0');

    Request req1, req2, req3;
    req1.fd = fd_read;
    req1.offset = 0;
    req1.size = 10;
    req1.dst = buffer1.data();

    req2.fd = fd_read;
    req2.offset = 10;
    req2.size = 10;
    req2.dst = buffer2.data();

    req3.fd = fd_read;
    req3.offset = 26;
    req3.size = 10;
    req3.dst = buffer3.data();

    Queue queue(make_cpu_backend(4));
    queue.enqueue(req1);
    queue.enqueue(req2);
    queue.enqueue(req3);
    queue.submit_all();
    queue.wait_all();

    auto completed = queue.take_completed();
    assert(completed.size() == 3);

    // Check all completed successfully
    for (const auto& req : completed) {
        assert(req.status == RequestStatus::Ok);
        assert(req.bytes_transferred == 10);
    }

    // Verify data
    assert(std::strncmp(buffer1.data(), "0123456789", 10) == 0);
    assert(std::strncmp(buffer2.data(), "ABCDEFGHIJ", 10) == 0);
    assert(std::strncmp(buffer3.data(), "QRSTUVWXYZ", 10) == 0);

    ::close(fd_read);
    ::unlink(filename);

    std::cout << "[cpu_backend_test] test_multiple_requests PASSED\n";
}

} // namespace

int main() {
    test_basic_read_write();
    test_partial_read();
    test_fake_uppercase();
    test_multiple_requests();

    std::cout << "[cpu_backend_test] ALL TESTS PASSED\n";
    return 0;
}
