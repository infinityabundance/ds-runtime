// SPDX-License-Identifier: Apache-2.0
<<<<<<< HEAD
//
// ds-runtime
//
// Implementation of the public API declared in ds_runtime.hpp.
//
// This file contains:
//  - the concrete CPU backend implementation (make_cpu_backend)
//  - the Queue::Impl definition and queue orchestration logic
//
// The focus here is correctness and clear semantics rather than
// maximum I/O throughput.

=======
>>>>>>> origin/main
#include "ds_runtime.hpp"

#include <atomic>
#include <cerrno>
#include <cctype>
<<<<<<< HEAD
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h> // pread, close, etc.
=======
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

#include <fcntl.h>
#include <unistd.h>
>>>>>>> origin/main

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
<<<<<<< HEAD
    /**
     * @brief Construct a thread pool with @p thread_count workers.
     *
     * If @p thread_count is 0, it is clamped up to 1 to avoid
     * a degenerate pool with no workers.
     */
=======
>>>>>>> origin/main
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

<<<<<<< HEAD
    /**
     * @brief Join all worker threads and destroy the pool.
     *
     * Any jobs still in the queue are discarded after @ref stop_ is set.
     * Workers finish at job boundaries; there is no preemption.
     */
=======
>>>>>>> origin/main
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

<<<<<<< HEAD
    /**
     * @brief Submit a job to be executed by the pool.
     *
     * The job is queued and executed by the next available worker thread.
     * This function is thread-safe.
     */
=======
>>>>>>> origin/main
    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
<<<<<<< HEAD
    /// Worker thread main loop.
    ///
    /// Each worker waits for jobs, executes them, and terminates only when:
    ///  - @ref stop_ is true *and*
    ///  - the job queue is empty.
=======
>>>>>>> origin/main
    void worker_loop() {
        for (;;) {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });

<<<<<<< HEAD
                // If we're asked to stop and there's no more work, exit.
=======
>>>>>>> origin/main
                if (stop_ && jobs_.empty()) {
                    return;
                }

                job = std::move(jobs_.front());
                jobs_.pop();
            }

            job();
        }
    }

<<<<<<< HEAD
    std::vector<std::thread>          workers_; ///< Worker threads owned by the pool.
    std::queue<std::function<void()>> jobs_;    ///< FIFO queue of pending jobs.
    std::mutex                        mtx_;     ///< Protects jobs_ and stop_.
    std::condition_variable           cv_;      ///< Signals workers when work is available or stop_ changes.
    bool                              stop_;    ///< Set to true during destruction to shut workers down.
=======
    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  jobs_;
    std::mutex                         mtx_;
    std::condition_variable            cv_;
    bool                               stop_;
>>>>>>> origin/main
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
<<<<<<< HEAD
 *
 * This backend is intended as a simple, correct reference implementation
 * and semantic baseline, not a high-throughput I/O engine.
 */

// -----------------------------------------------------------------------------
// CpuBackend
//
// Concrete Backend implementation that executes requests using POSIX pread()
// on a small worker thread pool.
//
// Responsibilities:
//  - perform blocking file I/O (pread)
//  - update Request::status and Request::errno_value
//  - optionally apply a "fake" decompression transform
//    (uppercase) when Compression::FakeUppercase is used
// -----------------------------------------------------------------------------
class CpuBackend final : public Backend {
public:
    /**
     * @brief Construct a CPU backend with @p worker_count worker threads.
     *
     * @param worker_count  Desired worker thread count. Zero is allowed and
     *                      is clamped up to 1 by the internal ThreadPool.
     */
    explicit CpuBackend(std::size_t worker_count)
        : pool_(worker_count) // ThreadPool itself clamps zero to 1
    {}

    /**
     * @brief Submit a Request for execution.
     *
     * The Request is copied by value into the job lambda. The caller
     * remains responsible for the lifetime of any pointed-to data (dst).
     *
     * @param req          Request describing the I/O operation.
     * @param on_complete  Completion callback invoked on a worker thread
     *                     when the operation (including "decompression")
     *                     has finished.
     */
    void submit(Request req, CompletionCallback on_complete) override {
        // Copy req by value into the job; the user-owned Request is distinct.
        pool_.submit([req, on_complete]() mutable {
            // Perform POSIX read.
            const ssize_t rd = ::pread(
                req.fd,
                req.dst,
                req.size,
                static_cast<off_t>(req.offset)
            );

            if (rd < 0) {
                // I/O error: capture errno and mark the request as failed.
                req.status      = RequestStatus::IoError;
                req.errno_value = errno;
            } else {
                // Successful read.
                req.status      = RequestStatus::Ok;
                req.errno_value = 0;

                // For safety in string-based demos: if we read fewer bytes
                // than the buffer size, zero-terminate.
                //
                // NOTE: This is *not* suitable as a general binary I/O
                // policy; callers should not rely on it for non-text data.
=======
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
>>>>>>> origin/main
                if (static_cast<std::size_t>(rd) < req.size) {
                    auto* c = static_cast<char*>(req.dst);
                    c[rd] = '\0';
                }
            }

<<<<<<< HEAD
            // "Decompression" pass.
            //
            // In real DirectStorage-style pipelines, this would be a true
            // codec (e.g., GDeflate) running on CPU or GPU. Here we simply
            // uppercase ASCII characters for demonstration and testing.
=======
            // "Decompression" pass. In real code this would be a true codec
            // like GDeflate. Here we just uppercase ASCII for demo/testing.
>>>>>>> origin/main
            if (req.status == RequestStatus::Ok &&
                req.compression == Compression::FakeUppercase) {

                char* c = static_cast<char*>(req.dst);
                for (std::size_t i = 0; i < req.size && c[i] != '\0'; ++i) {
<<<<<<< HEAD
                    c[i] = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(c[i]))
                    );
                }
            }

            // Invoke completion callback.
            //
            // Note: this is called on a worker thread. Callers must ensure
            // that any captured state is thread-safe.
=======
                    c[i] = static_cast<char>(std::toupper(
                        static_cast<unsigned char>(c[i])));
                }
            }

            // Invoke completion callback
>>>>>>> origin/main
            if (on_complete) {
                on_complete(req);
            }
        });
    }

private:
<<<<<<< HEAD
    ThreadPool pool_; ///< Worker pool used to execute I/O and post-processing work.
=======
    ThreadPool pool_;
>>>>>>> origin/main
};

} // anonymous namespace

<<<<<<< HEAD
/**
 * @brief Factory function for the CPU backend implementation.
 *
 * @param worker_count  Requested number of worker threads. Zero is allowed and
 *                      will be clamped to at least 1 inside CpuBackend.
 *
 * @return Shared pointer to a new CpuBackend instance.
 */
std::shared_ptr<Backend> make_cpu_backend(std::size_t worker_count) {
    // Using std::make_shared keeps allocation overhead low.
=======
std::shared_ptr<Backend> make_cpu_backend(std::size_t worker_count) {
>>>>>>> origin/main
    return std::make_shared<CpuBackend>(worker_count);
}

// -------------------------
// Queue implementation
// -------------------------

<<<<<<< HEAD
/**
 * @brief Internal implementation for ds::Queue.
 *
 * Responsibilities:
 *  - store enqueued Request objects until submission
 *  - hand them off to the Backend for execution
 *  - track how many requests are currently in flight
 *  - provide a blocking wait_all() primitive
 *
 * All synchronization and bookkeeping live here so that the public Queue
 * interface in ds_runtime.hpp can remain small and stable.
 */
struct Queue::Impl {
    /**
     * @brief Construct a Queue::Impl with a given backend.
     *
     * @param backend  Backend used to execute submitted requests.
     */
=======
struct Queue::Impl {
>>>>>>> origin/main
    explicit Impl(std::shared_ptr<Backend> backend)
        : backend_(std::move(backend))
        , in_flight_(0)
    {}

<<<<<<< HEAD
    /// Enqueue a request into the pending list.
    ///
    /// The Request object is moved into internal storage. The caller may
    /// reuse or destroy their original Request instance after this call,
    /// but must keep any referenced buffers (dst) alive until completion.
=======
>>>>>>> origin/main
    void enqueue(Request req) {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.push_back(std::move(req));
    }

<<<<<<< HEAD
    /// Submit all currently pending requests to the backend.
    ///
    /// The requests are moved out of the pending_ vector under lock and then
    /// submitted without holding mtx_, to avoid blocking other enqueue() calls
    /// or backend internals.
=======
>>>>>>> origin/main
    void submit_all() {
        std::vector<Request> to_submit;

        {
<<<<<<< HEAD
            // Move all pending requests into a local buffer so we can release
            // the lock before calling into the backend.
=======
>>>>>>> origin/main
            std::lock_guard<std::mutex> lock(mtx_);
            to_submit.swap(pending_);
        }

        for (auto& req : to_submit) {
<<<<<<< HEAD
            // Mark this request as in flight. Relaxed ordering is sufficient
            // for the increment; we use stronger ordering on decrement/loads
            // where we synchronize with wait_all().
=======
>>>>>>> origin/main
            in_flight_.fetch_add(1, std::memory_order_relaxed);

            backend_->submit(
                std::move(req),
                [this](Request& completed_req) {
<<<<<<< HEAD
                    // Completion callback runs on a worker thread owned by
                    // the backend. For now we:
                    //  - stash the completed Request for potential future use
                    //  - update the in-flight count and notify waiters.
=======
                    // User could hook into this later; for now we just
                    // track in-flight count and stash the completed requests.
>>>>>>> origin/main
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        completed_.push_back(completed_req);
                    }

                    const auto remaining =
                        in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;

<<<<<<< HEAD
                    // If this was the last in-flight request, wake any
                    // threads blocked in wait_all().
=======
>>>>>>> origin/main
                    if (remaining == 0) {
                        std::lock_guard<std::mutex> lock(wait_mtx_);
                        wait_cv_.notify_all();
                    }
                }
            );
        }
    }

<<<<<<< HEAD
    /// Block until all in-flight requests have completed.
    ///
    /// This does not prevent new submissions from racing in from other
    /// threads; it only guarantees that at the moment of return, there
    /// are no requests currently in flight.
=======
>>>>>>> origin/main
    void wait_all() {
        std::unique_lock<std::mutex> lock(wait_mtx_);
        wait_cv_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_acquire) == 0;
        });
    }

<<<<<<< HEAD
    /// Return the number of requests currently in flight.
    ///
    /// This is a snapshot and may be stale as soon as it is read, since
    /// other threads may be submitting or completing work concurrently.
=======
>>>>>>> origin/main
    std::size_t in_flight() const {
        return in_flight_.load(std::memory_order_acquire);
    }

<<<<<<< HEAD
    // (You already have the member fields below; just an example of how
    // you might comment them for clarity.)
    //
    // std::shared_ptr<Backend> backend_;   // execution engine
    // mutable std::mutex        mtx_;      // protects pending_ and completed_
    // std::vector<Request>      pending_;  // queued but not yet submitted
    // std::vector<Request>      completed_;// completed requests (for future use)
    // std::atomic<std::size_t>  in_flight_;// number of in-flight requests
    // mutable std::mutex        wait_mtx_; // guards condition variable
    // std::condition_variable   wait_cv_;  // used by wait_all()

    // (Optional) You could expose access to completed_ later to let users
    // inspect statuses, aggregate stats, etc.
    std::shared_ptr<Backend> backend_;   ///< Backend used to execute submitted requests.

    mutable std::mutex       mtx_;       ///< Protects pending_ and completed_.
    std::vector<Request>     pending_;   ///< Requests enqueued but not yet submitted.
    std::vector<Request>     completed_; ///< Requests that have completed (not yet surfaced).

    std::atomic<std::size_t> in_flight_; ///< Number of requests currently in flight.

    mutable std::mutex       wait_mtx_;  ///< Guards wait_cv_ for wait_all().
    std::condition_variable  wait_cv_;   ///< Used to block/wake threads in wait_all().
};

// -------------------------
// Queue public API
// -------------------------

/**
 * @brief Construct a Queue that uses the given backend.
 *
 * The Queue takes shared ownership of @p backend and uses it to execute
 * all submitted requests.
 */
Queue::Queue(std::shared_ptr<Backend> backend)
    : impl_(std::make_unique<Impl>(std::move(backend)))
{}

/**
 * @brief Destroy the Queue.
 *
 * Semantics: this destructor does *not* implicitly call wait_all().
 * It assumes the user has already ensured that any in-flight work
 * will not outlive the buffers it references. This mirrors the
 * "you own your buffers" philosophy of DirectStorage-style APIs.
 */
Queue::~Queue() = default;

void Queue::enqueue(Request req) {
    // Thin forwarding wrapper around Impl::enqueue().
    // See Impl::enqueue() for synchronization details.
=======
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
>>>>>>> origin/main
    impl_->enqueue(std::move(req));
}

void Queue::submit_all() {
<<<<<<< HEAD
    // Forward to Impl::submit_all(), which handles:
    //  - moving pending requests under lock
    //  - incrementing in-flight count
    //  - delegating to the backend with completion callbacks
=======
>>>>>>> origin/main
    impl_->submit_all();
}

void Queue::wait_all() {
<<<<<<< HEAD
    // Forward to Impl::wait_all(), which uses in_flight_ and
    // wait_cv_ to block until there are no in-flight requests.
=======
>>>>>>> origin/main
    impl_->wait_all();
}

std::size_t Queue::in_flight() const {
<<<<<<< HEAD
    // Snapshot of the current in-flight counter.
=======
>>>>>>> origin/main
    return impl_->in_flight();
}

} // namespace ds
