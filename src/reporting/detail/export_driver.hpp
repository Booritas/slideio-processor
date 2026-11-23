// Requirements: UR-RES-9, UR-RES-11, UR-RES-15, UR-QA-3
#pragma once

// Internal: the export loop shared by every writer. Not installed.

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/reporting/export_result.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace slideio::processor::detail {

/**
 * Per-export lookups of layer, stage and label names that repeat across objects.
 * Owned by one export run and used from one thread. Names are the stored UTF-8 text,
 * unescaped; a writer that needs an escaped form keeps its own cache of it.
 */
class NameCache {
public:
    /** A label's stored name and whether it is not a standard label (UR-RES-15). */
    struct LabelInfo {
        std::string name;
        bool isWorkspaceLocal = false;
    };

    explicit NameCache(Store& store) : store_(store) {}

    /** The layer's name; the reference stays valid for the cache's lifetime. */
    const std::string& layer(LayerId id);
    /** The stage's name; the reference stays valid for the cache's lifetime. */
    const std::string& stage(StageId id);
    /** The label's name and local flag; the reference stays valid for the cache's lifetime. */
    const LabelInfo& label(LabelId id);

private:
    Store& store_;
    std::map<int32_t, std::string> layers_;
    std::map<int32_t, std::string> stages_;
    std::map<int32_t, LabelInfo> labels_;
};

/**
 * What a writer supplies to runExport. The sink need not own a stream: it may stream
 * into its own file or let another component write the file at the given path.
 * Methods other than close() report failure by throwing IoError.
 * A sink is single-use: create one per runExport call and do not pass it to another.
 */
class ExportSink {
public:
    virtual ~ExportSink() = default;

    /** Creates the file at `partialPath` and writes everything before the first object. */
    virtual void begin(const std::filesystem::path& partialPath) = 0;
    /** Writes one object. */
    virtual void writeObject(const ObjectRecord& record, NameCache& names) = 0;
    /**
     * Writes everything after the last object, closes the file and returns the file's
     * size in bytes. Nothing may fail after it returns: the driver renames next.
     */
    virtual int64_t finish() = 0;
    /**
     * Releases the file without finishing it. Safe to call at any time and more than
     * once; the driver calls it on every exit path before removing the partial file.
     */
    virtual void close() noexcept = 0;
};

/**
 * Throws IoError "<who>: replacing the output file (<target>) with the partial file
 * (<partial>) failed", with the message of `ec` (the rename's failure) as the cause.
 */
[[noreturn]] void throwReplaceFailed(const char* who, const std::filesystem::path& target,
                                     const std::filesystem::path& partial, const std::error_code& ec);

/**
 * Runs one export: writes through `sink` to "<target>.partial", then renames it onto
 * `target`. Progress is reported to `observer` (stage "export") every 1000 objects and
 * once at the end, before the file is finished and renamed, so an observer that throws
 * leaves the target untouched; cancellation is checked before each object. On cancellation or any
 * failure the partial file is removed and an existing `target` is left untouched.
 * @param who Prefix of the failure messages, for example "CsvWriter::write".
 * @return The result; a cancelled run reports 0 bytes.
 * @throws IoError When the file cannot be created or written (naming the partial file), or
 *         cannot replace the target (naming both files, the rename's error as the cause).
 */
ExportResult runExport(Store& store, const ObjectQuery& query,
                       const std::filesystem::path& target, const char* who, ExportSink& sink,
                       ProgressObserver* observer, const CancellationToken& token);

} // namespace slideio::processor::detail
