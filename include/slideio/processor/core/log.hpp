// Requirements: UR-EXE-5
#pragma once

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Severity of a log message, in increasing order. Off is never a message
 * level that is delivered; as the threshold it silences everything.
 */
enum class LogLevel : int32_t { Trace, Debug, Info, Warning, Error, Off };

/**
 * @brief Receives log messages. Implement it and install it with setLogSink().
 *
 * write() may be called concurrently from several threads, so an
 * implementation must be thread-safe. Exceptions it throws are swallowed.
 * @par Thread-safety
 * Defined by the implementation; see above.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS LogSink {
public:
    /** @brief Destroys the sink. */
    virtual ~LogSink();
    /**
     * @brief Receives one formatted message.
     * @param level The message level, never Off.
     * @param message A null-terminated UTF-8 string, valid for the call only.
     * @throws Anything; logMessage() swallows every exception a sink throws.
     */
    virtual void write(LogLevel level, const char* message) = 0;
};

/** @brief Shared ownership of a LogSink. */
using LogSinkPtr = std::shared_ptr<LogSink>;

/**
 * @brief Installs the process-wide sink, replacing any previous one.
 * @param sink The new sink; null removes it and discards messages (the default).
 * @throws Nothing. The swap is guarded by the library's lock; if acquiring that
 * lock fails (a std::system_error from std::mutex), the failure cannot be
 * reported from a noexcept function and is fatal: std::terminate is called.
 * @par Thread-safety
 * ThreadSafe. A message already being written to the previous sink completes
 * on it; the library keeps that sink alive until then.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS void setLogSink(LogSinkPtr sink) noexcept;

/**
 * @brief Sets the process-wide threshold.
 * @param level Messages below it are discarded; Off discards all. The default
 * is Warning.
 * @throws Nothing. The threshold is a lock-free atomic, so no lock can fail
 * here.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS void setLogLevel(LogLevel level) noexcept;

/**
 * @brief Returns the process-wide threshold.
 * @return The current level; Warning until changed.
 * @throws Nothing. The threshold is a lock-free atomic, so no lock can fail
 * here.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS LogLevel logLevel() noexcept;

/**
 * @brief Formats a message printf-style and delivers it to the sink.
 *
 * The message is delivered iff a sink is installed, level is not Off, and
 * level >= logLevel(). Otherwise nothing is formatted. Logging never fails
 * the caller: a null format, a formatting error, an allocation failure and
 * an exception thrown by the sink are all swallowed, and so is a failure of the
 * library's lock (a std::system_error). The sink is called
 * outside the library's lock, so a sink may itself log.
 * @param level The message level.
 * @param format A printf-style UTF-8 format string; null makes the call a no-op.
 * @param ... The format arguments.
 * @throws Nothing.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS void logMessage(LogLevel level, const char* format, ...);

} // namespace slideio::processor
