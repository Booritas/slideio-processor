// Requirements: UR-EXE-1
#pragma once

// The workspace's run slot (SDS 04 4.4): one StageRunner at a time is inside run(). The
// flag is a std::atomic<bool> in Workspace::Impl, reached through detail::takeRunSlot and
// detail::releaseRunSlot (workspace.hpp), because public headers carry no std::atomic.
// Library-internal, not installed.

#include <slideio/processor/workspace/workspace.hpp>

namespace slideio::processor::detail {

// Takes the workspace's run slot for its lifetime. The constructor throws StageError,
// naming `stageName`, when another runner of the workspace holds the slot; the slot is
// released on every path out of the holder's scope, by the destructor.
class RunSlot {
public:
    RunSlot(Workspace& workspace, const char* stageName);
    ~RunSlot();
    RunSlot(const RunSlot&) = delete;
    RunSlot& operator=(const RunSlot&) = delete;

private:
    Workspace& workspace_;
};

} // namespace slideio::processor::detail
