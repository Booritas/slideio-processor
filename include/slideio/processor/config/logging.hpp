// Requirements: UR-EXE-5
#pragma once

/**
 * @file logging.hpp
 * @brief configureLogging: chooses where the library's log messages go.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/log.hpp>

namespace slideio::processor {

/**
 * @brief Installs a file or stderr sink behind core's log facade, per the
 * configuration's logFilePath() and logLevel().
 *
 * An empty logFilePath() selects stderr; otherwise the file is opened in append
 * mode, as bytes (a line ends in a bare newline on every platform). Each
 * message is one line, flushed at once:
 * `<UTC time as YYYY-MM-DDTHH:MM:SS.mmmZ> <LEVEL> <message>`, where LEVEL is one
 * of TRACE, DEBUG, INFO, WARNING, ERROR. The sink is built before anything is
 * changed, so on failure the sink and level already installed stay as they were.
 * @param configuration Supplies the log file path and the threshold.
 * @throws IoError When the log file cannot be opened; the message names the path.
 * @throws InternalError When an unexpected standard exception other than
 * std::bad_alloc is raised; its message is the cause.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS void configureLogging(const Configuration& configuration);

/**
 * @brief Installs a caller-supplied sink instead.
 *
 * The sink may be null, which installs core's discarding sink; level is the
 * threshold, Trace to Off. LogSinkPtr is a std::shared_ptr, so the library
 * shares ownership of the sink and keeps it alive until logging is reconfigured and any message in flight has
 * been written; the caller need not keep it alive.
 * @param sink The sink; may be null.
 * @param level The threshold, Trace..Off.
 * @throws InvalidArgumentError When level is outside Trace..Off; nothing is changed.
 * @throws InternalError When the library's lock cannot be acquired.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-5 (diagnostics)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS void configureLogging(LogSinkPtr sink, LogLevel level);

} // namespace slideio::processor
