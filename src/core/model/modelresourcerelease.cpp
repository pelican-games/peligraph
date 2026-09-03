#include "modelresourcerelease.hpp"

#include "../log.hpp"

namespace Pelican {
namespace {
std::atomic_uint64_t release_error_count{0};
std::atomic_uint64_t release_overflow_count{0};
} // namespace

void ReleaseErrorSink::record(std::string_view context,
                              const ReleaseReport &report) noexcept {
    if (report.empty()) return;
    release_error_count.fetch_add(report.error_count, std::memory_order_relaxed);
    release_overflow_count.fetch_add(report.overflow_count,
                                     std::memory_order_relaxed);
    for (const auto &error : report.view()) {
        if (logger) LOG_ERROR(logger, "{}: stage={} code={} index={}", context,
                              releaseStageName(error.stage),
                              releaseErrorCodeName(error.code), error.index);
    }
    if (report.overflow_count != 0 && logger) {
        LOG_ERROR(logger, "{}: overflow_count={}", context,
                  report.overflow_count);
    }
}

ReleaseErrorSinkStatus ReleaseErrorSink::status() noexcept {
    return {release_error_count.load(std::memory_order_relaxed),
            release_overflow_count.load(std::memory_order_relaxed)};
}
} // namespace Pelican
