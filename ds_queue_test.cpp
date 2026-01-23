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
#include <cctype>


// ---- DSRequest definition ----
struct DSRequest {
    int      fd;         // file descriptor
    size_t   offset;     // where to start reading
    size_t   size;       // how many bytes to read
    void*    dst;        // destination buffer (must be at least 'size' bytes)
    bool     compressed; // not used yet; placeholder for future decompression
};

// ---- Simple 1-thread thread pool ----
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

// ---- DSQueue implementation ----
class DSQueue {
public:
    DSQueue() : in_flight(0) {}

    // Add a request to the queue (does NOT start it yet)
    void enqueue(DSRequest req) {
        std::lock_guard<std::mutex> lock(mtx);
        pending.push_back(req);
    }

    // Submit all queued requests to the thread pool
    void submit() {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& req : pending) {
            in_flight++;
            // Copy req by value into the lambda so it's safe
            pool.submit([this, req]() {
            // Do the actual read
            ssize_t rd = pread(req.fd, req.dst, req.size, req.offset);
        if (rd < 0) {
        perror("[DSQueue worker] pread");
        } else {
        std::cout << "[DSQueue worker] read " << rd
                  << " bytes at offset " << req.offset << "\n";
        }

        if (req.compressed) {
        std::cout << "[DSQueue worker] fake decompress (uppercase)\n";
        char* c = static_cast<char*>(req.dst);
        for (size_t i = 0; i < req.size && c[i] != '\0'; ++i) {
        c[i] = std::toupper((unsigned char)c[i]);
    }
}


        // Mark this request as done
        {
        std::lock_guard<std::mutex> lock(this->mtx);
        this->in_flight--;
        if (this->in_flight == 0) {
            this->cv.notify_all();
        }
        }
    });
        }
        pending.clear();
    }

    // Block until all submitted requests complete
    void wait_all() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return in_flight == 0; });
    }

private:
    ThreadPool pool;

    std::mutex mtx;
    std::condition_variable cv;

    std::vector<DSRequest> pending;  // queued but not yet submitted
    size_t in_flight;                // # of requests currently running
};

// ---- Test harness ----
int main() {
    std::cout << "[main] DSQueue test starting\n";

    const char* filename = "test_asset.bin";

    // 1. Create a test file with some content
    {
        std::cout << "[main] creating file " << filename << "\n";
        int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open for write");
            return 1;
        }

        const char* text = "Hello DirectStorage-style queue on Linux!";
        ssize_t w = write(fd, text, strlen(text));
        if (w < 0) {
            perror("write");
            close(fd);
            return 1;
        }
        close(fd);
    }

    // 2. Open the file for reading
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open for read");
        return 1;
    }

    std::cout << "[main] opened file fd=" << fd << "\n";

    // 3. Prepare destination buffers
    std::vector<char> buf1(64, 0);
    std::vector<char> buf2(32, 0);

    // 4. Create requests
    DSRequest r1;
    r1.fd = fd;
    r1.offset = 0;
    r1.size = buf1.size();
    r1.dst = buf1.data();
    r1.compressed = false; // raw

    // Second request reads from an offset (just as an example)
    DSRequest r2;
    r2.fd = fd;
    r2.offset = 6;  // skip "Hello "
    r2.size = buf2.size();
    r2.dst = buf2.data();
    r2.compressed = true; // "compressed"

    // 5. Create queue and enqueue requests
    DSQueue queue;
    std::cout << "[main] enqueueing 2 requests\n";
    queue.enqueue(r1);
    queue.enqueue(r2);

    // 6. Submit and wait
    std::cout << "[main] submitting queue\n";
    queue.submit();

    std::cout << "[main] waiting for all requests to finish\n";
    queue.wait_all();

    // 7. Print results
    std::cout << "[main] all requests completed\n";
    std::cout << "buf1: \"" << buf1.data() << "\"\n";
    std::cout << "buf2: \"" << buf2.data() << "\"\n";

    close(fd);
    return 0;
}
