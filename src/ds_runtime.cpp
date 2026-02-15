// SPDX-License-Identifier: Apache-2.0
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

#include "ds_runtime.hpp"

#include <atomic>
#include <cerrno>
#include <cctype>
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
#include <unistd.h> // pread, pwrite, close, etc.

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
    /**
     * @brief Construct a thread pool with @p thread_count workers.
     *
     * If @p thread_count is 0, it is clamped up to 1 to avoid
     * a degenerate pool with no workers.
     */
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

    /**
     * @brief Join all worker threads and destroy the pool.
     *
     * Any jobs still in the queue are discarded after @ref stop_ is set.
     * Workers finish at job boundaries; there is no preemption.
     */
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

    /**
     * @brief Submit a job to be executed by the pool.
     *
     * The job is queued and executed by the next available worker thread.
     * This function is thread-safe.
     */
    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    /// Worker thread main loop.
    ///
    /// Each worker waits for jobs, executes them, and terminates only when:
    ///  - @ref stop_ is true *and*
    ///  - the job queue is empty.
    void worker_loop() {
        for (;;) {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });

                // If we're asked to stop and there's no more work, exit.
                if (stop_ && jobs_.empty()) {
                    return;
                }

                job = std::move(jobs_.front());
                jobs_.pop();
            }

            job();
        }
    }

    std::vector<std::thread>          workers_; ///< Worker threads owned by the pool.
    std::queue<std::function<void()>> jobs_;    ///< FIFO queue of pending jobs.
    std::mutex                        mtx_;     ///< Protects jobs_ and stop_.
    std::condition_variable           cv_;      ///< Signals workers when work is available or stop_ changes.
    bool                              stop_;    ///< Set to true during destruction to shut workers down.
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
 *
 * This backend is intended as a simple, correct reference implementation
 * and semantic baseline, not a high-throughput I/O engine.
 */
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
            // Validate the request before attempting any I/O.
            if (req.fd < 0) {
                report_request_error("cpu",
                                     "submit",
                                     "Invalid file descriptor",
                                     req,
                                     EBADF,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EBADF;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if (req.size == 0) {
                report_request_error("cpu",
                                     "submit",
                                     "Zero-length request is not allowed",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if (req.op == RequestOp::Read && req.dst == nullptr) {
                report_request_error("cpu",
                                     "submit",
                                     "Read request missing destination buffer",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if (req.op == RequestOp::Write && req.src == nullptr) {
                report_request_error("cpu",
                                     "submit",
                                     "Write request missing source buffer",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            if ((req.op == RequestOp::Read && req.dst_memory == RequestMemory::Gpu) ||
                (req.op == RequestOp::Write && req.src_memory == RequestMemory::Gpu)) {
                report_request_error("cpu",
                                     "submit",
                                     "GPU memory requested on CPU backend",
                                     req,
                                     EINVAL,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status = RequestStatus::IoError;
                req.errno_value = EINVAL;
                if (on_complete) {
                    on_complete(req);
                }
                return;
            }

            ssize_t io_bytes = 0;

            if (req.op == RequestOp::Write) {
                io_bytes = ::pwrite(
                    req.fd,
                    req.src,
                    req.size,
                    static_cast<off_t>(req.offset)
                );
            } else {
                io_bytes = ::pread(
                    req.fd,
                    req.dst,
                    req.size,
                    static_cast<off_t>(req.offset)
                );
            }

            if (io_bytes < 0) {
                // I/O error: capture errno and mark the request as failed.
                report_request_error("cpu",
                                     req.op == RequestOp::Write ? "pwrite" : "pread",
                                     "POSIX I/O failed",
                                     req,
                                     errno,
                                     __FILE__,
                                     __LINE__,
                                     __func__);
                req.status      = RequestStatus::IoError;
                req.errno_value = errno;
                req.bytes_transferred = 0;
            } else {
                // Successful read/write.
                req.status      = RequestStatus::Ok;
                req.errno_value = 0;
                req.bytes_transferred = static_cast<std::size_t>(io_bytes);

                if (req.op == RequestOp::Read) {
                    // For safety in string-based demos: if we read fewer bytes
                    // than the buffer size, zero-terminate.
                    //
                    // NOTE: This is *not* suitable as a general binary I/O
                    // policy; callers should not rely on it for non-text data.
                    if (static_cast<std::size_t>(io_bytes) < req.size) {
                        auto* c = static_cast<char*>(req.dst);
                        c[io_bytes] = '\0';
                    }
                }
            }

            // "Decompression" pass.
            //
            // In real DirectStorage-style pipelines, this would be a true
            // codec (e.g., GDeflate) running on CPU or GPU. Here we simply
            // uppercase ASCII characters for demonstration and testing.
            if (req.op == RequestOp::Read &&
                req.status == RequestStatus::Ok &&
                req.compression == Compression::FakeUppercase) {

                char* c = static_cast<char*>(req.dst);
                for (std::size_t i = 0; i < req.size && c[i] != '\0'; ++i) {
                    c[i] = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(c[i]))
                    );
                }
            }

            // Invoke completion callback.
            //
            // Note: this is called on a worker thread. Callers must ensure
            // that any captured state is thread-safe.
            if (on_complete) {
                on_complete(req);
            }
        });
    }

private:
    ThreadPool pool_; ///< Worker pool used to execute I/O and post-processing work.
};

} // anonymous namespace

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
    return std::make_shared<CpuBackend>(worker_count);
}

// -------------------------
// Queue implementation
// -------------------------

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
    explicit Impl(std::shared_ptr<Backend> backend)
        : backend_(std::move(backend))
        , in_flight_(0)
        , total_completed_(0)
        , total_failed_(0)
        , total_bytes_transferred_(0)
    {}

    /// Enqueue a request into the pending list.
    ///
    /// The Request object is moved into internal storage. The caller may
    /// reuse or destroy their original Request instance after this call,
    /// but must keep any referenced buffers (dst) alive until completion.
    void enqueue(Request req) {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_.push_back(std::move(req));
    }

    /// Submit all currently pending requests to the backend.
    ///
    /// The requests are moved out of the pending_ vector under lock and then
    /// submitted without holding mtx_, to avoid blocking other enqueue() calls
    /// or backend internals.
    void submit_all() {
        std::vector<Request> to_submit;

        {
            // Move all pending requests into a local buffer so we can release
            // the lock before calling into the backend.
            std::lock_guard<std::mutex> lock(mtx_);
            to_submit.swap(pending_);
        }

        for (auto& req : to_submit) {
            // Mark this request as in flight. Relaxed ordering is sufficient
            // for the increment; we use stronger ordering on decrement/loads
            // where we synchronize with wait_all().
            in_flight_.fetch_add(1, std::memory_order_relaxed);

            backend_->submit(
                std::move(req),
                [this](Request& completed_req) {
                    // Completion callback runs on a worker thread owned by
                    // the backend. For now we:
                    //  - stash the completed Request for potential future use
                    //  - update the in-flight count and notify waiters.
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        completed_.push_back(completed_req);
                    }

                    total_completed_.fetch_add(1, std::memory_order_relaxed);
                    if (completed_req.status != RequestStatus::Ok) {
                        total_failed_.fetch_add(1, std::memory_order_relaxed);
                    }
                    total_bytes_transferred_.fetch_add(
                        completed_req.bytes_transferred,
                        std::memory_order_relaxed
                    );

                    const auto remaining =
                        in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;

                    // If this was the last in-flight request, wake any
                    // threads blocked in wait_all().
                    if (remaining == 0) {
                        std::lock_guard<std::mutex> lock(wait_mtx_);
                        wait_cv_.notify_all();
                    }
                }
            );
        }
    }

    /// Block until all in-flight requests have completed.
    ///
    /// This does not prevent new submissions from racing in from other
    /// threads; it only guarantees that at the moment of return, there
    /// are no requests currently in flight.
    void wait_all() {
        std::unique_lock<std::mutex> lock(wait_mtx_);
        wait_cv_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_acquire) == 0;
        });
    }

    /// Return the number of requests currently in flight.
    ///
    /// This is a snapshot and may be stale as soon as it is read, since
    /// other threads may be submitting or completing work concurrently.
    std::size_t in_flight() const {
        return in_flight_.load(std::memory_order_acquire);
    }

    /// Retrieve and clear the list of completed requests.
    ///
    /// This returns a snapshot of completed requests accumulated since the
    /// last call. The caller can inspect status, bytes_transferred, etc.
    std::vector<Request> take_completed() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Request> result;
        result.swap(completed_);
        return result;
    }

    // (Optional) You could expose access to completed_ later to let users
    // inspect statuses, aggregate stats, etc.
    std::shared_ptr<Backend> backend_;   ///< Backend used to execute submitted requests.

    mutable std::mutex       mtx_;       ///< Protects pending_ and completed_.
    std::vector<Request>     pending_;   ///< Requests enqueued but not yet submitted.
    std::vector<Request>     completed_; ///< Requests that have completed (not yet surfaced).

    std::atomic<std::size_t> in_flight_; ///< Number of requests currently in flight.
    std::atomic<std::size_t> total_completed_; ///< Total completed requests.
    std::atomic<std::size_t> total_failed_; ///< Total failed requests.
    std::atomic<std::size_t> total_bytes_transferred_; ///< Total bytes transferred.

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
    impl_->enqueue(std::move(req));
}

void Queue::submit_all() {
    // Forward to Impl::submit_all(), which handles:
    //  - moving pending requests under lock
    //  - incrementing in-flight count
    //  - delegating to the backend with completion callbacks
    impl_->submit_all();
}

void Queue::wait_all() {
    // Forward to Impl::wait_all(), which uses in_flight_ and
    // wait_cv_ to block until there are no in-flight requests.
    impl_->wait_all();
}

std::size_t Queue::in_flight() const {
    // Snapshot of the current in-flight counter.
    return impl_->in_flight();
}

std::vector<Request> Queue::take_completed() {
    return impl_->take_completed();
}

} // namespace ds
