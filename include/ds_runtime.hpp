// SPDX-License-Identifier: Apache-2.0
<<<<<<< HEAD
//
// ds-runtime public API
//
// This header declares:
//  - ds::Request and related enums
//  - ds::Backend (abstract execution backend)
//  - ds::Queue (front-end request queue)
//  - make_cpu_backend() factory for the CPU backend
//
// The focus is on clear semantics and portability rather than peak throughput.

#pragma once

#include <cstddef>      // std::size_t
#include <cstdint>      // std::uint64_t
#include <functional>   // std::function
#include <memory>       // std::shared_ptr, std::unique_ptr

namespace ds {

// -----------------------------------------------------------------------------
// Basic enums
// -----------------------------------------------------------------------------

/// Compression mode for a Request.
///
/// Only a trivial "fake" mode is currently supported for demo/testing.
/// Real backends could extend this enum with actual codecs (e.g. GDeflate).
enum class Compression {
    None,          ///< No compression; data is read as-is.
    FakeUppercase  ///< Demo mode: uppercase ASCII bytes after reading.
};

/// Status of a Request after execution by a Backend.
enum class RequestStatus {
    Pending,   ///< Not yet submitted or still in flight.
    Ok,        ///< Completed successfully.
    IoError    ///< I/O error; errno_value is set.
    // Additional statuses could be added later (e.g. Cancelled).
};

// -----------------------------------------------------------------------------
// Request
// -----------------------------------------------------------------------------

/// Description of a single I/O operation.
///
/// A Request describes a read from a POSIX file descriptor into a caller-
/// supplied buffer, optionally followed by a decompression step. The Request
/// object itself is passed by value into the backend; the caller retains
/// ownership of the underlying buffers (dst) and must keep them alive until
/// completion.
struct Request {
    int           fd          = -1;      ///< POSIX file descriptor.
    std::uint64_t offset      = 0;       ///< Byte offset within the file.
    std::size_t   size        = 0;       ///< Number of bytes to read into dst.
    void*         dst         = nullptr; ///< Destination buffer owned by caller.

    Compression   compression = Compression::None;   ///< Compression mode.
    RequestStatus status      = RequestStatus::Pending; ///< Result status.
    int           errno_value = 0;        ///< errno value on IoError, 0 otherwise.
};

// -----------------------------------------------------------------------------
// Backend
// -----------------------------------------------------------------------------

/// Completion callback type for backend submissions.
///
/// Called on a backend-owned worker thread when a Request has finished
/// executing (including any "decompression" step).
using CompletionCallback = std::function<void(Request&)>;

/// Abstract execution backend for ds-runtime.
///
/// A Backend implementation is responsible for actually executing Requests:
/// performing disk I/O, running any decompression, and invoking completion
/// callbacks. Backends may use CPU threads, GPU queues, io_uring, etc.
=======
#ifndef DS_RUNTIME_HPP
#define DS_RUNTIME_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ds {

/**
 * @brief Compression mode for a request.
 *
 * In a real implementation, this would include concrete formats
 * (e.g. GDeflate). For now we keep it simple and only distinguish
 * between "none" and "some transform".
 */
enum class Compression {
    None,
    FakeUppercase, ///< Demo-only "compression": transforms data to uppercase.
};

/**
 * @brief Status of an I/O request.
 */
enum class RequestStatus {
    Pending,
    Ok,
    IoError,
    Cancelled,
};

/**
 * @brief Description of a single I/O request.
 *
 * This is deliberately simple and POSIX-centric:
 *  - fd:     a Linux file descriptor (must remain valid while the request runs)
 *  - offset: byte offset in the file
 *  - size:   how many bytes to read
 *  - dst:    destination buffer in host memory (must be at least size bytes)
 */
struct Request {
    int                 fd           = -1;
    std::uint64_t       offset       = 0;
    std::size_t         size         = 0;
    void*               dst          = nullptr;
    Compression         compression  = Compression::None;
    RequestStatus       status       = RequestStatus::Pending;
    int                 errno_value  = 0;  ///< Set if status == IoError.
};

/**
 * @brief Completion callback type.
 *
 * Called once per request, from a worker thread, after the I/O and
 * any CPU-side post-processing (e.g. decompression) is finished.
 */
using CompletionCallback = std::function<void(Request&)>;

/**
 * @brief Abstract backend interface.
 *
 * Different implementations can route requests to:
 *  - a pure CPU path (pread + CPU decompression),
 *  - a Vulkan compute path for GPU decompression,
 *  - vendor-specific paths (CUDA, ROCm, etc.).
 */
>>>>>>> origin/main
class Backend {
public:
    virtual ~Backend() = default;

<<<<<<< HEAD
    /// Submit a Request for execution.
    ///
    /// The Request is passed by value into the backend implementation. The
    /// backend typically copies it into its own job structure. The caller
    /// remains responsible for the lifetime of any pointed-to buffers (dst).
    ///
    /// The completion callback is invoked on a backend-owned worker thread.
    virtual void submit(Request req, CompletionCallback on_complete) = 0;
};

// -----------------------------------------------------------------------------
// Queue
// -----------------------------------------------------------------------------

/// Front-end request queue.
///
/// A Queue collects Requests, batches them, and hands them off to a Backend
/// for execution. It tracks the number of in-flight requests and provides
/// a blocking wait_all() primitive.
///
/// All synchronization and bookkeeping are handled internally so that the
/// public API stays small and stable.
class Queue {
public:
    /// Construct a queue that uses the given backend for execution.
    ///
    /// The Queue takes shared ownership of @p backend.
    explicit Queue(std::shared_ptr<Backend> backend);

    /// Destroy the queue.
    ///
    /// Semantics: this destructor does *not* implicitly call wait_all().
    /// The caller is responsible for ensuring that any in-flight work does
    /// not outlive the buffers it references.
    ~Queue();

    /// Enqueue a request into the queue’s pending list.
    ///
    /// The Request object is moved into internal storage. The caller may
    /// reuse or destroy their original Request instance after this call,
    /// but must keep any referenced buffers (dst) alive until completion.
    void enqueue(Request req);

    /// Submit all currently pending requests to the backend.
    ///
    /// Requests enqueued so far are moved into a local batch and submitted
    /// to the backend without holding the internal mutex, to avoid blocking
    /// concurrent enqueue() calls or backend internals.
    void submit_all();

    /// Block until all in-flight requests have completed.
    ///
    /// This does not prevent new submissions from racing in from other
    /// threads; it only guarantees that at the moment of return, there
    /// are no requests currently in flight.
    void wait_all();

    /// Return the number of requests currently in flight.
    ///
    /// This is a snapshot and may be stale as soon as it is read, since
    /// other threads may be submitting or completing work concurrently.
    std::size_t in_flight() const;

private:
    /// Internal implementation type.
    ///
    /// Defined in src/ds_runtime.cpp as:
    ///   struct Queue::Impl { ... };
    struct Impl;

    /// Owning pointer to the internal implementation.
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// Backend factories
// -----------------------------------------------------------------------------

/// Create a CPU-only backend implementation.
///
/// @param worker_count  Requested number of worker threads. Zero is allowed
///                      and will be clamped up to at least 1 inside the
///                      implementation.
///
/// The CPU backend uses a small internal ThreadPool, POSIX pread(), and an
/// optional "fake decompression" step that uppercases ASCII bytes when
/// Compression::FakeUppercase is requested.
std::shared_ptr<Backend> make_cpu_backend(std::size_t worker_count = 1);

} // namespace ds
=======
    /**
     * @brief Submit a single request for asynchronous execution.
     *
     * Implementations must:
     *  - Not block the caller.
     *  - Eventually set req.status and (optionally) req.errno_value.
     *  - Invoke the provided completion callback exactly once.
     */
    virtual void submit(Request req, CompletionCallback on_complete) = 0;

    /**
     * @brief Poll for completions, if the backend needs it.
     *
     * CPU backend in this example does not need explicit polling;
     * it calls completion callbacks directly from worker threads.
     *
     * A Vulkan backend might choose to poll fences here, for example.
     */
    virtual void poll() {}
};

/**
 * @brief A simple I/O queue that uses a Backend to execute requests.
 *
 * This is the public object a caller would interact with. It owns:
 *  - A queue of pending Request objects.
 *  - A Backend implementation that actually performs the work.
 */
class Queue {
public:
    explicit Queue(std::shared_ptr<Backend> backend);
    ~Queue();
    /**
     * @brief Enqueue a request to be submitted later.
     *
     * This function is thread-safe.
     */
    void enqueue(Request req);

    /**
     * @brief Submit all currently enqueued requests to the backend.
     *
     * This function is thread-safe. It does not block on completion;
     * requests complete asynchronously.
     */
    void submit_all();

    /**
     * @brief Block until all submitted requests have completed.
     *
     * This is optional sugar for simple use cases. More complex
     * callers can track completion via their own callbacks.
     */
    void wait_all();

    /**
     * @brief Number of requests currently in flight (not yet completed).
     */
    std::size_t in_flight() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Factory for a CPU-only backend.
 *
 * This backend:
 *  - Runs pread() on worker threads.
 *  - Optionally applies a fake "decompression" transform
 *    (uppercase) if Compression::FakeUppercase is used.
 */
std::shared_ptr<Backend> make_cpu_backend(std::size_t worker_count = 1);

} // namespace ds

#endif // DS_RUNTIME_HPP
>>>>>>> origin/main
