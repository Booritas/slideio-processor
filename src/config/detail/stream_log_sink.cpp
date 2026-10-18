#include "config/detail/stream_log_sink.hpp"

#include <slideio/processor/core/error.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>

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
    char buffer[40];
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
    bool opened = false;
    try {
        file_.open(std::filesystem::u8path(path), std::ios::app | std::ios::binary);
        opened = file_.is_open();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        opened = false;
    }
    if (!opened) {
        throw IoError((std::string("cannot open log file: ") + path).c_str());
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
