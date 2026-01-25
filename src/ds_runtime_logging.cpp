// SPDX-License-Identifier: Apache-2.0
// Error reporting utilities for ds-runtime.

#include "ds_runtime.hpp"

#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace ds {

namespace {

std::mutex g_error_mutex;
ErrorCallback g_error_callback;

std::string format_timestamp(const std::chrono::system_clock::time_point& tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void default_reporter(const ErrorContext& ctx) {
    std::cerr << "[ds-runtime][error] " << format_timestamp(ctx.timestamp)
              << " subsystem=" << ctx.subsystem
              << " operation=" << ctx.operation
              << " errno=" << ctx.errno_value
              << " detail=\"" << ctx.detail << "\""
              << " request=" << (ctx.has_request ? "yes" : "no")
              << (ctx.has_request ? " fd=" : "")
              << (ctx.has_request ? std::to_string(ctx.fd) : "")
              << (ctx.has_request ? " offset=" : "")
              << (ctx.has_request ? std::to_string(ctx.offset) : "")
              << (ctx.has_request ? " size=" : "")
              << (ctx.has_request ? std::to_string(ctx.size) : "")
              << (ctx.has_request ? " op=" : "")
              << (ctx.has_request ? (ctx.op == RequestOp::Write ? "write" : "read") : "")
              << (ctx.has_request ? " src_mem=" : "")
              << (ctx.has_request ? (ctx.src_memory == RequestMemory::Gpu ? "gpu" : "host") : "")
              << (ctx.has_request ? " dst_mem=" : "")
              << (ctx.has_request ? (ctx.dst_memory == RequestMemory::Gpu ? "gpu" : "host") : "")
              << " at " << ctx.file << ":" << ctx.line
              << " (" << ctx.function << ")"
              << std::endl;
}

} // namespace

void set_error_callback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_error_callback = std::move(callback);
}

void report_error(const std::string& subsystem,
                  const std::string& operation,
                  const std::string& detail,
                  int errno_value,
                  const char* file,
                  int line,
                  const char* function) {
    ErrorContext ctx{};
    ctx.subsystem = subsystem;
    ctx.operation = operation;
    ctx.detail = detail;
    ctx.file = file ? file : "";
    ctx.function = function ? function : "";
    ctx.line = line;
    ctx.errno_value = errno_value;
    ctx.timestamp = std::chrono::system_clock::now();

    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        callback = g_error_callback;
    }

    if (callback) {
        callback(ctx);
    } else {
        default_reporter(ctx);
    }
}

void report_request_error(const std::string& subsystem,
                          const std::string& operation,
                          const std::string& detail,
                          const Request& request,
                          int errno_value,
                          const char* file,
                          int line,
                          const char* function) {
    ErrorContext ctx{};
    ctx.subsystem = subsystem;
    ctx.operation = operation;
    ctx.detail = detail;
    ctx.file = file ? file : "";
    ctx.function = function ? function : "";
    ctx.line = line;
    ctx.errno_value = errno_value;
    ctx.timestamp = std::chrono::system_clock::now();
    ctx.has_request = true;
    ctx.fd = request.fd;
    ctx.offset = request.offset;
    ctx.size = request.size;
    ctx.op = request.op;
    ctx.src_memory = request.src_memory;
    ctx.dst_memory = request.dst_memory;

    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        callback = g_error_callback;
    }

    if (callback) {
        callback(ctx);
    } else {
        default_reporter(ctx);
    }
}

} // namespace ds
