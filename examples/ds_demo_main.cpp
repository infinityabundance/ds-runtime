// SPDX-License-Identifier: Apache-2.0
#include "ds_runtime.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

int main() {
    using namespace ds;

    std::cout << "[demo] starting DirectStorage-style CPU demo\n";

    const char* filename = "demo_asset.bin";

    // 1. Create a small test file
    {
        const char* text = "Hello DirectStorage-style queue on Linux!";
        const int fd = ::open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open for write");
            return 1;
        }

        const ssize_t w = ::write(fd, text, std::strlen(text));
        if (w < 0) {
            perror("write");
            ::close(fd);
            return 1;
        }
        ::close(fd);
        std::cout << "[demo] wrote " << w << " bytes to " << filename << "\n";
    }

    // 2. Open the file for reading
    const int fd = ::open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open for read");
        return 1;
    }

    // 3. Prepare destination buffers
    std::vector<char> buf_raw(64, 0);
    std::vector<char> buf_upper(64, 0);

    // 4. Build a CPU backend and a queue
    auto backend = make_cpu_backend(/*worker_count=*/2);
    ds::Queue queue(backend);

    // 5. Enqueue two requests: one raw, one "compressed" (uppercase transform)
    Request r1;
    r1.fd          = fd;
    r1.offset      = 0;
    r1.size        = buf_raw.size() - 1; // leave space for '\0'
    r1.dst         = buf_raw.data();
    r1.compression = Compression::None;

    Request r2;
    r2.fd          = fd;
    r2.offset      = 0;
    r2.size        = buf_upper.size() - 1;
    r2.dst         = buf_upper.data();
    r2.compression = Compression::FakeUppercase;

    queue.enqueue(r1);
    queue.enqueue(r2);

    // 6. Submit and wait
    std::cout << "[demo] submitting 2 requests\n";
    queue.submit_all();

    std::cout << "[demo] waiting for completion (in-flight=" << queue.in_flight() << ")\n";
    queue.wait_all();

    std::cout << "[demo] all requests completed (in-flight=" << queue.in_flight() << ")\n";

    ::close(fd);

    // 7. Print results
    std::cout << "raw   : \"" << buf_raw.data() << "\"\n";
    std::cout << "upper : \"" << buf_upper.data() << "\"\n";

    return 0;
}
