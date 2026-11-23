#include "config/detail/stream_log_sink.hpp"
#include "core/detail/diagnostics.hpp"

#include <slideio/processor/core/error.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>

namespace slideio::processor::detail {

namespace {

const char* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Off:
        break;
    }
    return "OFF";
}

// "YYYY-MM-DDTHH:MM:SS.mmmZ" in UTC.
std::string timestampNow() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t seconds = system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    // Every %d field is printed in full whatever its value, so size for the worst case: seven
    // ints of up to 11 characters each, the separators, the Z and the terminator.
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(millis.count()));
    return buffer;
}

} // namespace

StreamLogSink::StreamLogSink(const char* path) : toStderr_(path == nullptr || *path == '\0') {
    if (toStderr_) {
        return;
    }
    // The open's own failure is the cause: the exception forming or opening the path
    // threw, or else what the C library reports through errno.
    bool opened = false;
    std::string cause;
    try {
        errno = 0;
        file_.open(std::filesystem::u8path(path), std::ios::app | std::ios::binary);
        opened = file_.is_open();
        if (!opened && errno != 0) {
            cause = std::generic_category().message(errno);
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        opened = false;
        cause = diagnosticText(e);
    }
    if (!opened) {
        ErrorContext context;
        context.cause = cause.c_str();
        throw IoError(withPath("cannot open the log file", path).c_str(), context);
    }
}

void StreamLogSink::write(LogLevel level, const char* message) {
    std::string line = timestampNow();
    line += ' ';
    line += levelName(level);
    line += ' ';
    line += message;
    line += '\n';
    std::lock_guard<std::mutex> lock(mutex_);
    if (toStderr_) {
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    } else {
        // A failed write leaves the stream's error state set, which would drop
        // every later line; clear it so logging resumes once the file recovers.
        file_.clear();
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.flush();
    }
}

} // namespace slideio::processor::detail
