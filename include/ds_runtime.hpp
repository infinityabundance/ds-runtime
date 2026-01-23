// SPDX-License-Identifier: Apache-2.0
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
class Backend {
public:
    virtual ~Backend() = default;

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
