// Requirements: UR-RES-9, UR-RES-11, UR-RES-15, UR-QA-3
#include "reporting/detail/export_driver.hpp"
#include "core/detail/diagnostics.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace slideio::processor::detail {
namespace {

constexpr int64_t kProgressEvery = 1000;
constexpr const char* kProgressName = "export";

// Removes the partial file on every exit path unless released after the rename.
class PartialFileGuard {
public:
    explicit PartialFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    PartialFileGuard(const PartialFileGuard&) = delete;
    PartialFileGuard& operator=(const PartialFileGuard&) = delete;
    ~PartialFileGuard() {
        if (armed_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }
    void release() noexcept { armed_ = false; }

private:
    std::filesystem::path path_;
    bool armed_ = true;
};

// Closes the sink's file on every exit path.
class SinkCloser {
public:
    explicit SinkCloser(ExportSink& sink) : sink_(sink) {}
    SinkCloser(const SinkCloser&) = delete;
    SinkCloser& operator=(const SinkCloser&) = delete;
    ~SinkCloser() { sink_.close(); }

private:
    ExportSink& sink_;
};

} // namespace

void throwReplaceFailed(const char* who, const std::filesystem::path& target,
                        const std::filesystem::path& partial, const std::error_code& ec) {
    const std::string replacing = withPath(std::string(who) + ": replacing the output file", target.u8string());
    const std::string message = withPath(replacing + " with the partial file", partial.u8string()) + " failed";
    const std::string cause = ec.message();
    ErrorContext context;
    context.cause = cause.c_str();
    throw IoError(message.c_str(), context);
}

const std::string& NameCache::layer(LayerId id) {
    auto it = layers_.find(id.value());
    if (it == layers_.end()) it = layers_.emplace(id.value(), store_.layers().name(id)).first;
    return it->second;
}

const std::string& NameCache::stage(StageId id) {
    auto it = stages_.find(id.value());
    if (it == stages_.end())
        it = stages_.emplace(id.value(), store_.stageStates().name(id)).first;
    return it->second;
}

const NameCache::LabelInfo& NameCache::label(LabelId id) {
    auto it = labels_.find(id.value());
    if (it == labels_.end()) {
        LabelInfo info;
        info.name = store_.vocabulary().name(id);
        info.isWorkspaceLocal = store_.vocabulary().origin(id) != LabelOrigin::Standard;
        it = labels_.emplace(id.value(), std::move(info)).first;
    }
    return it->second;
}

ExportResult runExport(Store& store, const ObjectQuery& query,
                       const std::filesystem::path& target, const char* who, ExportSink& sink,
                       ProgressObserver* observer, const CancellationToken& token) {
    ObjectStore& objects = store.objects();
    const int64_t total = objects.count(query);

    std::filesystem::path partial = target;
    partial += ".partial";

    // The guard is declared before the closer, so the file is closed before it is removed.
    PartialFileGuard guard(partial);
    SinkCloser closer(sink);
    sink.begin(partial);

    auto report = [&](int64_t completed) {
        if (observer == nullptr) return;
        const double fraction =
            total > 0 ? std::min(1.0, static_cast<double>(completed) / static_cast<double>(total))
                      : 1.0;
        observer->onProgress(kProgressName, fraction, completed, total);
    };

    NameCache names(store);
    int64_t written = 0;
    CursorPtr cursor = objects.query(query);
    while (cursor->next()) {
        if (token.isCancelled()) {
            cursor->close();
            return ExportResult(written, 0, true);  // the guard removes the partial file
        }
        sink.writeObject(cursor->current(), names);
        ++written;
        if (written % kProgressEvery == 0) report(written);
    }
    cursor->close();

    // The final progress comes before the file is finished and renamed, so an observer
    // that throws leaves the target untouched.
    if (written == 0 || written % kProgressEvery != 0) report(written);

    const int64_t bytes = sink.finish();
    std::error_code ec;
    std::filesystem::rename(partial, target, ec);
    if (ec) throwReplaceFailed(who, target, partial, ec);
    guard.release();
    return ExportResult(written, bytes, false);
}

} // namespace slideio::processor::detail
