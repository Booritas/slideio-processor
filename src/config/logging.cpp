#include <slideio/processor/config/logging.hpp>
#include <slideio/processor/core/error.hpp>

#include "config/detail/stream_log_sink.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <new>

namespace slideio::processor {

namespace {

// Serialises reconfigurations against each other only; readers never take it.
std::mutex& configureMutex() {
    static std::mutex mutex;
    return mutex;
}

// The sink and the level cannot change atomically for a concurrent logMessage, so
// a raised threshold goes in before the sink changes and a lowered one after: the
// new sink is never handed a level below its own, only a message that had already
// passed the old check when the switch began. The caller holds configureMutex()
// and releases the returned previous sink only after unlocking it: that sink's
// destructor may call configureLogging(), which would otherwise re-enter the lock.
LogSinkPtr install(LogSinkPtr sink, LogLevel level) noexcept {
    LogSinkPtr previous;
    if (level > logLevel()) {
        setLogLevel(level);
        previous = exchangeLogSink(std::move(sink));
    } else {
        previous = exchangeLogSink(std::move(sink));
        setLogLevel(level);
    }
    return previous;
}

} // namespace

void configureLogging(const Configuration& configuration) {
    try {
        // Build the sink first: if the file will not open, nothing has changed.
        LogSinkPtr sink = std::make_shared<detail::StreamLogSink>(configuration.logFilePath());
        LogSinkPtr previous; // declared before the lock, so released after it
        std::lock_guard<std::mutex> lock(configureMutex());
        previous = install(std::move(sink), configuration.logLevel());
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
        LogSinkPtr previous; // declared before the lock, so released after it
        std::lock_guard<std::mutex> lock(configureMutex());
        previous = install(std::move(sink), level);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) { // a failed lock (std::system_error)
        throw InternalError(e.what());
    }
}

} // namespace slideio::processor
