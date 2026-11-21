// Requirements: UR-RES-9, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#include <slideio/processor/reporting/csv_writer.hpp>

#include "reporting/detail/columns.hpp"
#include "reporting/detail/csv_format.hpp"
#include "reporting/detail/wkt.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/types.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slideio::processor {
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

// Per-export lookups of names that repeat across rows.
class NameCache {
public:
    explicit NameCache(Store& store) : store_(store) {}

    const std::string& layer(LayerId id) {
        auto it = layers_.find(id.value());
        if (it == layers_.end()) it = layers_.emplace(id.value(), store_.layers().name(id)).first;
        return it->second;
    }
    const std::string& stage(StageId id) {
        auto it = stages_.find(id.value());
        if (it == stages_.end())
            it = stages_.emplace(id.value(), store_.stageStates().name(id)).first;
        return it->second;
    }
    // The label's escaped name and whether it is not a standard label (UR-RES-15).
    const std::pair<std::string, bool>& label(LabelId id) {
        auto it = labels_.find(id.value());
        if (it == labels_.end()) {
            std::string escaped;
            detail::appendEscapedLabel(escaped, store_.vocabulary().name(id));
            const bool local = store_.vocabulary().origin(id) != LabelOrigin::Standard;
            it = labels_.emplace(id.value(), std::make_pair(std::move(escaped), local)).first;
        }
        return it->second;
    }

private:
    Store& store_;
    std::map<int32_t, std::string> layers_;
    std::map<int32_t, std::string> stages_;
    std::map<int32_t, std::pair<std::string, bool>> labels_;
};

std::string objectIdText(ObjectId id) {
    const int32_t n = id.toString(nullptr, 0);
    std::string text(static_cast<size_t>(n) + 1, '\0');
    id.toString(&text[0], n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

void appendCommentLine(std::string& out, const char* key, const char* value) {
    out += "# ";
    out += key;
    out += ": ";
    detail::appendOneLine(out, value);
    out += '\n';
}

std::string provenanceLines(Store& store) {
    const Provenance provenance = store.provenance();
    std::string out = "# slideio-processor export\n";
    appendCommentLine(out, "slide_id", provenance.slideId());
    appendCommentLine(out, "scene_index", std::to_string(provenance.sceneIndex()).c_str());
    appendCommentLine(out, "library_version", provenance.libraryVersion());
    appendCommentLine(out, "created_at_unix_ms",
                      std::to_string(provenance.createdAtUnixMs()).c_str());
    StageStateReader& stages = store.stageStates();
    const int32_t count = stages.count();
    for (int32_t i = 0; i < count; ++i) {
        const StageId stage = stages.at(i);
        out += "# stage: ";
        detail::appendOneLine(out, stages.name(stage));
        const char* configuration = stages.configuration(stage);
        if (*configuration != '\0') {
            out += ' ';
            detail::appendOneLine(out, configuration);
        }
        out += '\n';
    }
    return out;
}

// Writes one object's cells into `line`, terminated by '\n'.
void appendRow(std::string& line, const ObjectRecord& record,
               const detail::ResolvedColumns& resolved, char delimiter, Store& store,
               NameCache& names) {
    const ObjectId id = record.id();
    std::string cell;
    // The effective label set is read once per row, and only when a column needs it.
    bool labelsRead = false;
    std::string labels;
    std::string locals;
    bool parentRead = false;
    bool hasParent = false;
    ObjectId parent;

    for (size_t c = 0; c < resolved.columns.size(); ++c) {
        const detail::ResolvedColumn& column = resolved.columns[c];
        cell.clear();
        switch (column.kind) {
        case detail::ColumnKind::LayerId:
            cell = std::to_string(id.layer().value());
            break;
        case detail::ColumnKind::AnchorId:
            cell = std::to_string(id.anchorPacked());
            break;
        case detail::ColumnKind::ObjectId:
            cell = objectIdText(id);
            break;
        case detail::ColumnKind::Layer:
            cell = names.layer(record.layer());
            break;
        case detail::ColumnKind::Stage:
            if (record.stage().isValid()) cell = names.stage(record.stage());
            break;
        case detail::ColumnKind::Labels:
        case detail::ColumnKind::LabelsLocal:
            if (!labelsRead) {
                labelsRead = true;
                AnnotationStore& annotations = store.annotations();
                const int32_t count = annotations.labelCount(id);
                for (int32_t i = 0; i < count; ++i) {
                    const auto& label = names.label(annotations.labelAt(id, i));
                    if (i > 0) {
                        labels += ';';
                        locals += ';';
                    }
                    labels += label.first;
                    locals += label.second ? '1' : '0';
                }
            }
            cell = column.kind == detail::ColumnKind::Labels ? labels : locals;
            break;
        case detail::ColumnKind::Label:
            if (record.hasLabel()) cell = names.label(record.label()).first;
            break;
        case detail::ColumnKind::LabelLocal:
            if (record.hasLabel()) cell = names.label(record.label()).second ? "1" : "0";
            break;
        case detail::ColumnKind::BboxX:
            cell = std::to_string(record.boundingBox().x);
            break;
        case detail::ColumnKind::BboxY:
            cell = std::to_string(record.boundingBox().y);
            break;
        case detail::ColumnKind::BboxWidth:
            cell = std::to_string(record.boundingBox().width);
            break;
        case detail::ColumnKind::BboxHeight:
            cell = std::to_string(record.boundingBox().height);
            break;
        case detail::ColumnKind::GeometryWkt:
            detail::appendWkt(cell, record.geometry());
            break;
        case detail::ColumnKind::ParentLayerId:
        case detail::ColumnKind::ParentAnchorId:
        case detail::ColumnKind::ParentId:
            if (!parentRead) {
                parentRead = true;
                hasParent = store.edges().parentOf(id, parent);
            }
            if (!hasParent) break;
            if (column.kind == detail::ColumnKind::ParentLayerId) {
                cell = std::to_string(parent.layer().value());
            } else if (column.kind == detail::ColumnKind::ParentAnchorId) {
                cell = std::to_string(parent.anchorPacked());
            } else {
                cell = objectIdText(parent);
            }
            break;
        case detail::ColumnKind::Measurement: {
            Measurement value;
            if (record.measurements().find(column.measurement.c_str(), value))
                detail::appendDouble(cell, value.value());
            break;
        }
        }
        if (c > 0) line += delimiter;
        detail::appendField(line, cell, delimiter);
    }
    line += '\n';
}

} // namespace

class CsvWriter::Impl {
public:
    std::string path;
    ColumnSchema schema;
    char delimiter = ',';
    bool provenance = true;
};

CsvWriter::CsvWriter(const char* path) {
    if (path == nullptr || *path == '\0')
        throw InvalidArgumentError("CsvWriter: path must not be null or empty");
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl_ = std::move(impl);
}

CsvWriter::~CsvWriter() = default;

CsvWriter& CsvWriter::schema(const ColumnSchema& schema) {
    impl_->schema = schema;
    return *this;
}

CsvWriter& CsvWriter::delimiter(char delimiter) {
    if (detail::isRefusedDelimiter(delimiter))
        throw InvalidArgumentError("CsvWriter::delimiter: the delimiter is not allowed");
    impl_->delimiter = delimiter;
    return *this;
}

CsvWriter& CsvWriter::provenanceHeader(bool include) {
    impl_->provenance = include;
    return *this;
}

ExportResult CsvWriter::write(Store& store, const ObjectQuery& query) {
    return write(store, query, nullptr, CancellationToken());
}

ExportResult CsvWriter::write(Store& store, const ObjectQuery& query, ProgressObserver* observer,
                              const CancellationToken& token) {
    const detail::ResolvedColumns resolved = detail::resolveColumns(impl_->schema, store);
    ObjectStore& objects = store.objects();
    const int64_t total = objects.count(query);

    const std::filesystem::path target = std::filesystem::u8path(impl_->path);
    std::filesystem::path partial = target;
    partial += ".partial";

    // Declared before the stream, so the stream is closed before the guard removes the file.
    PartialFileGuard guard(partial);
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) throw IoError("CsvWriter::write: cannot create the output file");

    std::string line;
    if (impl_->provenance) line = provenanceLines(store);
    for (size_t c = 0; c < resolved.columns.size(); ++c) {
        if (c > 0) line += impl_->delimiter;
        detail::appendField(line, resolved.columns[c].name, impl_->delimiter);
    }
    line += '\n';
    out.write(line.data(), static_cast<std::streamsize>(line.size()));

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
        line.clear();
        appendRow(line, cursor->current(), resolved, impl_->delimiter, store, names);
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!out) throw IoError("CsvWriter::write: writing the output file failed");
        ++written;
        if (written % kProgressEvery == 0) report(written);
    }
    cursor->close();

    out.close();
    if (!out) throw IoError("CsvWriter::write: writing the output file failed");
    std::error_code ec;
    std::filesystem::rename(partial, target, ec);
    if (ec) throw IoError("CsvWriter::write: replacing the output file failed");
    guard.release();
    const std::uintmax_t size = std::filesystem::file_size(target, ec);
    if (ec) throw IoError("CsvWriter::write: reading the output file size failed");

    if (written == 0 || written % kProgressEvery != 0) report(written);
    return ExportResult(written, static_cast<int64_t>(size), false);
}

} // namespace slideio::processor
