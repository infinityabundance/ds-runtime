// SPDX-License-Identifier: Apache-2.0
// io_uring backend implementation for ds-runtime.

#include "ds_runtime_uring.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <unistd.h>

namespace ds {

namespace {

// Simple io_uring backend that offloads POSIX read/write to the kernel.
// This backend is host-memory only and rejects GPU-targeted requests.
class IoUringBackend final : public Backend {
public:
    explicit IoUringBackend(const IoUringBackendConfig& config)
        : entries_(config.entries ? config.entries : 1u)
        , stop_(false)
    {
        const int init_rc = io_uring_queue_init(entries_, &ring_, 0);
        if (init_rc != 0) {
            report_error("io_uring",
                         "io_uring_queue_init",
                         "Failed to initialize io_uring ring",
                         -init_rc,
                         __FILE__,
                         __LINE__,
                         __func__);
            init_failed_ = true;
        } else {
            worker_ = std::thread([this]() { worker_loop(); });
        }
    }

    ~IoUringBackend() override {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }

        if (!init_failed_) {
            io_uring_queue_exit(&ring_);
        }
    }

    // Submit a host-memory-only request to the ring worker thread.
    void submit(Request req, CompletionCallback on_complete) override {
        if (init_failed_) {
            report_request_error("io_uring",
                                 "submit",
                                 "Backend initialization failed",
                                 req,
                                 EINVAL,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            req.bytes_transferred = 0;
            if (on_complete) {
                on_complete(req);
            }
            return;
        }

        if ((req.op == RequestOp::Read && req.dst_memory == RequestMemory::Gpu) ||
            (req.op == RequestOp::Write && req.src_memory == RequestMemory::Gpu)) {
            report_request_error("io_uring",
                                 "submit",
                                 "GPU memory requested on io_uring backend",
                                 req,
                                 EINVAL,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = EINVAL;
            req.bytes_transferred = 0;
            if (on_complete) {
                on_complete(req);
            }
            return;
        }

        if (req.op == RequestOp::Write && req.compression != Compression::None) {
            report_request_error("io_uring",
                                 "submit",
                                 "Compression is not supported for write requests",
                                 req,
                                 ENOTSUP,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = ENOTSUP;
            req.bytes_transferred = 0;
            if (on_complete) {
                on_complete(req);
            }
            return;
        }

        if (req.op == RequestOp::Read && req.compression == Compression::GDeflate) {
            report_request_error("io_uring",
                                 "submit",
                                 "GDeflate is not implemented yet",
                                 req,
                                 ENOTSUP,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
            req.status = RequestStatus::IoError;
            req.errno_value = ENOTSUP;
            req.bytes_transferred = 0;
            if (on_complete) {
                on_complete(req);
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx_);
            pending_.push({std::move(req), std::move(on_complete)});
        }
        cv_.notify_one();
    }

private:
    struct PendingOp {
        Request req;
        CompletionCallback callback;
    };

    // Worker thread loop:
    // 1) Drain pending requests into a local batch.
    // 2) Prepare SQEs and submit to io_uring.
    // 3) Wait for CQEs and invoke callbacks.
    void worker_loop() {
        while (true) {
            std::queue<PendingOp> batch;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
                if (stop_ && pending_.empty()) {
                    break;
                }
                std::swap(batch, pending_);
            }

            while (!batch.empty()) {
                auto op = std::make_unique<PendingOp>(std::move(batch.front()));
                batch.pop();

                io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                if (!sqe) {
                    report_request_error("io_uring",
                                         "io_uring_get_sqe",
                                         "Submission queue is full",
                                         op->req,
                                         EBUSY,
                                         __FILE__,
                                         __LINE__,
                                         __func__);
                    op->req.status = RequestStatus::IoError;
                    op->req.errno_value = EBUSY;
                    op->req.bytes_transferred = 0;
                    if (op->callback) {
                        op->callback(op->req);
                    }
                    continue;
                }

                if (op->req.op == RequestOp::Write) {
                    io_uring_prep_write(
                        sqe,
                        op->req.fd,
                        op->req.src,
                        op->req.size,
                        static_cast<off_t>(op->req.offset)
                    );
                } else {
                    io_uring_prep_read(
                        sqe,
                        op->req.fd,
                        op->req.dst,
                        op->req.size,
                        static_cast<off_t>(op->req.offset)
                    );
                }

                io_uring_sqe_set_data(sqe, op.release());
            }

            const int submitted = io_uring_submit(&ring_);
            if (submitted <= 0) {
                report_error("io_uring",
                             "io_uring_submit",
                             "Submission failed",
                             submitted,
                             __FILE__,
                             __LINE__,
                             __func__);
                continue;
            }
            unsigned completed = 0;
            while (completed < static_cast<unsigned>(submitted)) {
                io_uring_cqe* cqe = nullptr;
                const int wait_rc = io_uring_wait_cqe(&ring_, &cqe);
                if (wait_rc != 0 || !cqe) {
                    report_error("io_uring",
                                 "io_uring_wait_cqe",
                                 "Failed waiting for completion",
                                 wait_rc,
                                 __FILE__,
                                 __LINE__,
                                 __func__);
                    break;
                }
                auto* op = static_cast<PendingOp*>(io_uring_cqe_get_data(cqe));
                if (op) {
                    if (cqe->res < 0) {
                        op->req.status = RequestStatus::IoError;
                        op->req.errno_value = -cqe->res;
                        op->req.bytes_transferred = 0;
                    } else {
                        op->req.status = RequestStatus::Ok;
                        op->req.errno_value = 0;
                        op->req.bytes_transferred = static_cast<std::size_t>(cqe->res);
                    }
                    if (op->callback) {
                        op->callback(op->req);
                    }
                    delete op;
                }
                io_uring_cqe_seen(&ring_, cqe);
                ++completed;
            }
        }
    }

    unsigned entries_;
    io_uring ring_{};
    std::atomic<bool> init_failed_{false};
    std::atomic<bool> stop_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<PendingOp> pending_;
    std::thread worker_;
};

} // namespace

std::shared_ptr<Backend> make_io_uring_backend(const IoUringBackendConfig& config) {
    return std::make_shared<IoUringBackend>(config);
}

} // namespace ds
