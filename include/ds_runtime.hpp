// SPDX-License-Identifier: Apache-2.0
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

#include <cstddef>    // std::size_t
#include <cstdint>    // std::uint64_t
#include <functional> // std::function
#include <memory>     // std::shared_ptr, std::unique_ptr

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

/// Operation type for a Request.
enum class RequestOp {
    Read,  ///< Read data from the file descriptor into dst.
    Write  ///< Write data from src into the file descriptor.
};

/// Memory location for request buffers.
enum class RequestMemory {
    Host, ///< Buffer resides in host memory.
    Gpu   ///< Buffer resides in a GPU buffer (Vulkan backend).
};

// -----------------------------------------------------------------------------
// Request
// -----------------------------------------------------------------------------

/// Description of a single I/O operation.
///
/// A Request describes a read or write on a POSIX file descriptor, optionally
/// followed by a decompression step for reads. The Request
/// object itself is passed by value into the backend; the caller retains
/// ownership of the underlying buffers (dst/src) and must keep them alive until
/// completion.
struct Request {
    int           fd          = -1;      ///< POSIX file descriptor.
    std::uint64_t offset      = 0;       ///< Byte offset within the file.
    std::size_t   size        = 0;       ///< Number of bytes to read into dst.
    void*         dst         = nullptr; ///< Destination buffer for host reads.
    const void*   src         = nullptr; ///< Source buffer for host writes.
    void*         gpu_buffer  = nullptr; ///< Vulkan VkBuffer handle for GPU transfers.
    std::uint64_t gpu_offset  = 0;       ///< Byte offset into gpu_buffer.

    RequestOp     op          = RequestOp::Read;       ///< Read or write operation.
    RequestMemory dst_memory  = RequestMemory::Host;   ///< Destination memory location.
    RequestMemory src_memory  = RequestMemory::Host;   ///< Source memory location.
    Compression   compression = Compression::None;       ///< Compression mode.
    RequestStatus status      = RequestStatus::Pending;  ///< Result status.
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
class Backend {
public:
    virtual ~Backend() = default;
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
