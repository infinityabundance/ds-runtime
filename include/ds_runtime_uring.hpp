// SPDX-License-Identifier: Apache-2.0
// io_uring backend interface for ds-runtime.

#pragma once

#include "ds_runtime.hpp"

#ifdef DS_RUNTIME_HAS_IO_URING
#include <liburing.h>
#endif

namespace ds {

/// Configuration for the io_uring backend.
struct IoUringBackendConfig {
    unsigned entries = 256;   ///< SQ/CQ size. Must be >= 1.
    std::size_t worker_count = 1; ///< Reserved for future use.
};

/// Create an io_uring-backed implementation.
std::shared_ptr<Backend> make_io_uring_backend(const IoUringBackendConfig& config);

} // namespace ds
