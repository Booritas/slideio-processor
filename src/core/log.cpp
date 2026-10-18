#include <slideio/processor/core/log.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

LogSink::~LogSink() = default;

namespace {

// setLogLevel() and logLevel() are noexcept on the strength of this.
static_assert(std::atomic<LogLevel>::is_always_lock_free, "the log threshold must be lock-free");

struct LogState {
    std::mutex mutex;
    LogSinkPtr sink;
    std::atomic<LogLevel> level{LogLevel::Warning};
};

// Function-local so it is usable during static initialisation of other units.
LogState& state() {
    static LogState s;
    return s;
}

LogSinkPtr currentSink() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.sink;
}

} // namespace

void setLogSink(LogSinkPtr sink) noexcept {
    LogState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.sink.swap(sink);
    }
    // The previous sink is released here, outside the lock, so its destructor
    // may log.
}

void setLogLevel(LogLevel level) noexcept { state().level.store(level); }

LogLevel logLevel() noexcept { return state().level.load(); }

void logMessage(LogLevel level, const char* format, ...) {
    try {
        if (format == nullptr || level == LogLevel::Off || level < logLevel()) {
            return;
        }
        LogSinkPtr sink = currentSink();
        if (!sink) {
            return;
        }
        std::va_list args;
        va_start(args, format);
        std::va_list measure;
        va_copy(measure, args);
        const int length = std::vsnprintf(nullptr, 0, format, measure);
        va_end(measure);
        std::vector<char> text;
        if (length >= 0) {
            text.resize(static_cast<size_t>(length) + 1);
            std::vsnprintf(text.data(), text.size(), format, args);
        }
        va_end(args);
        if (length < 0) {
            return;
        }
        sink->write(level, text.data());
    } catch (...) {
        // Logging must never fail the caller.
    }
}

} // namespace slideio::processor
