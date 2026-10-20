// Requirements: UR-OBS-1
#pragma once

// The exclusive writer lock (ruling S3): a file at "<store>-writer.lock" holding the
// owner's process id as decimal text, locked with LockFileEx on Windows and
// flock(LOCK_EX | LOCK_NB) on POSIX. Both lock per handle, so a second acquire in the
// same process fails as one from another process does.

#include <string>

namespace slideio::processor::detail {

class WriterLock {
public:
    WriterLock() noexcept = default;
    WriterLock(WriterLock&& other) noexcept;
    WriterLock& operator=(WriterLock&& other) noexcept;
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    ~WriterLock();

    // Takes the lock for the store at `storePath` (UTF-8), without waiting. Throws
    // IoError naming the store path and, when the lock file's text parses,
    // "process <pid>" when another handle holds it; IoError naming the lock file when
    // it cannot be created or written.
    static WriterLock acquire(const std::string& storePath);

    bool held() const noexcept;
    // Releases the lock; a no-op when it is not held. The file is left in place:
    // removing it would race with a process that has just opened it.
    void release() noexcept;
    // Removes the lock file of the store at `storePath` while the lock is still held,
    // then releases it: for a store whose creation failed, so nothing is left behind.
    // Removing it first means no opener can lock the old file by name in between.
    void removeFileAndRelease(const std::string& storePath) noexcept;

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace slideio::processor::detail
