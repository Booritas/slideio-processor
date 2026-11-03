// Test-only: runs store_observer_helper, the storage tests' other process.
//
// Rule: on Linux this test executable carries its own SQLite copy beside the hidden one
// inside slideio-processor-data. POSIX locks are per process and each copy tracks them on
// its own, so a raw sqlite3 connection (the test's copy) that is open on, or is closed on,
// a file a Store (the library's copy) has open can checkpoint and delete the WAL under
// the Store. A foreign writer on a live store therefore runs in another process, through
// execInAnotherProcess; a raw connection in this process touches a store file only
// while no Store has it open.
#pragma once

#include "common/temp_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifndef SLIDEIO_PROCESSOR_TEST_OBSERVER_HELPER
#error "SLIDEIO_PROCESSOR_TEST_OBSERVER_HELPER must name the store_observer_helper executable"
#endif

namespace slideio::processor {

// Runs the helper with `args` through std::system and returns its exit code.
inline int runHelper(const std::vector<std::string>& args) {
    std::string command = std::string("\"") + SLIDEIO_PROCESSOR_TEST_OBSERVER_HELPER + "\"";
    for (const std::string& arg : args) command += " \"" + arg + "\"";
#ifdef _WIN32
    // cmd /c strips the outermost quotes; wrap the whole line so the quoted paths survive.
    command = "\"" + command + "\"";
    return std::system(command.c_str());
#else
    const int status = std::system(command.c_str());
    return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Runs `sql` on the store file at `storePath` (UTF-8) through a raw connection in another
// process, as a foreign writer would, and returns the helper's exit code (0 on success).
// The raw connection posts no wake-up signal.
inline int execInAnotherProcess(const std::string& storePath, const std::string& sql) {
    const std::filesystem::path sqlFile = uniqueTempPath("helper_sql", ".sql");
    TempFileGuard removeSql(sqlFile);
    writeFile(sqlFile, sql);
    return runHelper({storePath, "exec", sqlFile.u8string()});
}

} // namespace slideio::processor
