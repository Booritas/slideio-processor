#include <slideio/processor/config/logging.hpp>
#include <slideio/processor/core/error.hpp>

#include "config/detail/stream_log_sink.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <new>

namespace slideio::processor {

namespace {

// Serialises reconfigurations against each other only. It does not make the sink
// and level change atomically for readers: a concurrent logMessage can briefly
// see the new sink with the old level.
std::mutex& configureMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

void configureLogging(const Configuration& configuration) {
    try {
        // Build the sink first: if the file will not open, nothing has changed.
        LogSinkPtr sink = std::make_shared<detail::StreamLogSink>(configuration.logFilePath());
        std::lock_guard<std::mutex> lock(configureMutex());
        setLogSink(std::move(sink));
        setLogLevel(configuration.logLevel());
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        throw InternalError(e.what());
    }
}

void configureLogging(LogSinkPtr sink, LogLevel level) {
    if (level < LogLevel::Trace || level > LogLevel::Off) {
        throw InvalidArgumentError("configureLogging: level must be Trace..Off");
    }
    try {
        std::lock_guard<std::mutex> lock(configureMutex());
        setLogSink(std::move(sink));
        setLogLevel(level);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) { // a failed lock (std::system_error)
        throw InternalError(e.what());
    }
}

} // namespace slideio::processor
