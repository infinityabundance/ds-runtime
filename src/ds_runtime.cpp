// SPDX-License-Identifier: Apache-2.0
#include "ds_runtime.hpp"

#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace ds {

// -------------------------
// Internal thread pool
// -------------------------

namespace {

/**
 * @brief Very small, fixed-size thread pool.
 *
 *  - Jobs are std::function<void()>.
 *  - Threads run until destruction.
 *  - No dynamic resizing or fancy features; this is intentionally minimal.
 */
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count)
        : stop_(false)
    {
        if (thread_count == 0) {
            thread_count = 1;
        }

        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();

        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });

                if (stop_ && jobs_.empty()) {
                    return;
                }

                job = std::move(jobs_.front());
                jobs_.pop();
            }

            job();
        }
    }

    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  jobs_;
    std::mutex                         mtx_;
    std::condition_variable            cv_;
    bool                               stop_;
};

} // anonymous namespace

// -------------------------
// CPU backend
// -------------------------

namespace {

/**
 * @brief CPU-only backend implementation.
 *
 * Uses:
 *  - ThreadPool for concurrency.
 *  - pread() for POSIX file I/O.
 *  - Optional "fake decompression" (uppercase transformation).
 */
class CpuBackend final : public Backend {
public:
    explicit CpuBackend(std::size_t worker_count)
        : pool_(worker_count)
    {}

    void submit(Request req, CompletionCallback on_complete) override {
        // Copy req by value into the job; the user-owned Request is distinct.
        pool_.submit([req, on_complete]() mutable {
            // Perform POSIX read
            const ssize_t rd = ::pread(req.fd, req.dst, req.size, static_cast<off_t>(req.offset));
            if (rd < 0) {
                req.status      = RequestStatus::IoError;
                req.errno_value = errno;
            } else {
                req.status      = RequestStatus::Ok;
                req.errno_value = 0;
                // For safety, zero-terminate if we're treating it as a string
                // in simple demos. This is optional and not needed for binary data.
                if (static_cast<std::size_t>(rd) < req.size) {
                    auto* c = static_cast<char*>(req.dst);
                    c[rd] = '\0';
                }
            }

            // "Decompression" pass. In real code this would be a true codec
            // like GDeflate. Here we just uppercase ASCII for demo/testing.
            if (req.status == RequestStatus::Ok &&
                req.compression == Compression::FakeUppercase) {

                char* c = static_cast<char*>(req.dst);
                for (std::size_t i = 0; i < req.size && c[i] != '\0'; ++i) {
                    c[i] = static_cast<char>(std::toupper(
                        static_cast<unsigned char>(c[i])));
                }
            }

            // Invoke completion callback
            if (on_complete) {
                on_complete(req);
            }
        });
    }

private:
    ThreadPool pool_;
};

} // anonymous namespace

std::shared_ptr<Backend> make_cpu_backend(std::size_t worker_count) {
    return std::make_shared<CpuBackend>(worker_count);
}

// -------------------------
// Queue implementation
// -------------------------

struct Queue::Impl {
    explicit Impl(std::shared_ptr<Backend> backend)
        : backend_(std::move(backend))
        , in_flight_(0)
    {}

    void enqueue(Request req) {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.push_back(std::move(req));
    }

    void submit_all() {
        std::vector<Request> to_submit;

        {
            std::lock_guard<std::mutex> lock(mtx_);
            to_submit.swap(pending_);
        }

        for (auto& req : to_submit) {
            in_flight_.fetch_add(1, std::memory_order_relaxed);

            backend_->submit(
                std::move(req),
                [this](Request& completed_req) {
                    // User could hook into this later; for now we just
                    // track in-flight count and stash the completed requests.
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        completed_.push_back(completed_req);
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

    void wait_all() {
        std::unique_lock<std::mutex> lock(wait_mtx_);
        wait_cv_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_acquire) == 0;
        });
    }

    std::size_t in_flight() const {
        return in_flight_.load(std::memory_order_acquire);
    }

    // (Optional) You could expose access to completed_ later to let users
    // inspect statuses, aggregate stats, etc.

    std::shared_ptr<Backend> backend_;

    mutable std::mutex       mtx_;
    std::vector<Request>     pending_;
    std::vector<Request>     completed_;

    std::atomic<std::size_t> in_flight_;

    mutable std::mutex       wait_mtx_;
    std::condition_variable  wait_cv_;
};

Queue::Queue(std::shared_ptr<Backend> backend)
    : impl_(std::make_unique<Impl>(std::move(backend)))
{}
// Define destructor out-of-line so Impl is complete here
Queue::~Queue() = default;

void Queue::enqueue(Request req) {
    impl_->enqueue(std::move(req));
}

void Queue::submit_all() {
    impl_->submit_all();
}

void Queue::wait_all() {
    impl_->wait_all();
}

std::size_t Queue::in_flight() const {
    return impl_->in_flight();
}

} // namespace ds
