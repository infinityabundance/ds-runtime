#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <chrono>

// Simple thread pool (minimal, not production-ready)
class ThreadPool {
public:
    ThreadPool() : stop(false) {
        worker = std::thread([this]() { loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_one();
        if (worker.joinable())
            worker.join();
    }

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            jobs.push(std::move(job));
        }
        cv.notify_one();
    }

private:
    void loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return stop || !jobs.empty(); });
                if (stop && jobs.empty()) return;
                job = std::move(jobs.front());
                jobs.pop();
            }
            job();
        }
    }

    std::thread worker;
    std::queue<std::function<void()>> jobs;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop;
};

struct Request {
    int fd;
    size_t offset;
    size_t size;
    char* dst;
    std::atomic<bool> completed{false};
};

int main() {
    std::cout << "[main] starting test\n";

    // 1. Create a test file
    const char* filename = "test_asset.bin";
    {
        std::cout << "[main] creating file " << filename << "\n";
        int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open for write");
            return 1;
        }
        const char* text = "Hello DirectStorage on Linux!";
        ssize_t w = write(fd, text, strlen(text));
        if (w < 0) {
            perror("write");
            close(fd);
            return 1;
        }
        close(fd);
    }

    // 2. Open file for reading
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open for read");
        return 1;
    }
    std::cout << "[main] opened file for read, fd=" << fd << "\n";

    // 3. Prepare destination buffer
    std::vector<char> buffer(64, 0);

    // 4. Create request
    Request req;
    req.fd = fd;
    req.offset = 0;
    req.size = buffer.size();
    req.dst = buffer.data();

    // 5. Create thread pool and enqueue read
    ThreadPool pool;
    std::cout << "[main] submitting async read request\n";

    pool.submit([&req]() {
        std::cout << "[worker] starting pread\n";
        ssize_t rd = pread(req.fd, req.dst, req.size, req.offset);
        if (rd < 0) {
            perror("[worker] pread");
        } else {
            std::cout << "[worker] pread read " << rd << " bytes\n";
        }
        req.completed.store(true, std::memory_order_release);
    });

    // 6. Wait for completion
    std::cout << "[main] waiting for completion...\n";
    while (!req.completed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "[main] request completed\n";

    // 7. Print result
    std::cout << "Read data: " << buffer.data() << std::endl;

    close(fd);
    return 0;
}
