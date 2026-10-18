// Requirements: UR-EXE-5
#pragma once

#include <slideio/processor/core/log.hpp>

#include <fstream>
#include <mutex>

namespace slideio::processor::detail {

// A LogSink writing timestamped lines to an append-mode file, or to stderr when
// the path is empty. One mutex guards each write; every line is flushed.
class StreamLogSink final : public LogSink {
public:
    // path is UTF-8; empty selects stderr.
    // Throws IoError (naming the path) when the file cannot be opened.
    explicit StreamLogSink(const char* path);
    void write(LogLevel level, const char* message) override;

private:
    std::mutex mutex_;
    std::ofstream file_;
    bool toStderr_;
};

} // namespace slideio::processor::detail
