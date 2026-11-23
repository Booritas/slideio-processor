// Requirements: UR-RES-9, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#include <slideio/processor/reporting/csv_writer.hpp>

#include "core/detail/diagnostics.hpp"
#include "reporting/detail/columns.hpp"
#include "reporting/detail/csv_format.hpp"
#include "reporting/detail/export_driver.hpp"
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
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {
namespace {

// The shared name cache plus this writer's escaped form of each label name.
class CsvNames {
public:
    struct Label {
        const std::string& name;  // escaped for a labels cell
        bool isWorkspaceLocal;
    };

    explicit CsvNames(detail::NameCache& names) : names_(names) {}

    const std::string& layer(LayerId id) { return names_.layer(id); }
    const std::string& stage(StageId id) { return names_.stage(id); }
    Label label(LabelId id) {
        const detail::NameCache::LabelInfo& info = names_.label(id);
        auto it = escaped_.find(id.value());
        if (it == escaped_.end()) {
            std::string text;
            detail::appendEscapedLabel(text, info.name.c_str());
            it = escaped_.emplace(id.value(), std::move(text)).first;
        }
        return Label{it->second, info.isWorkspaceLocal};
    }

private:
    detail::NameCache& names_;
    std::map<int32_t, std::string> escaped_;
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

// The provenance lines; `slideId`, when not null, replaces the store's slide identifier.
std::string provenanceLines(Store& store, const std::string* slideId) {
    const Provenance provenance = store.provenance();
    std::string out = "# slideio-processor export\n";
    appendCommentLine(out, "slide_id", slideId != nullptr ? slideId->c_str() : provenance.slideId());
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
               CsvNames& names) {
    const ObjectId id = record.id();
    std::string cell;
    // The effective label set is read once per row, and only when a column needs it.
    bool labelsRead = false;
    std::string labels;
    std::string locals;
    bool parentRead = false;
    bool hasParent = false;
    ObjectId parent;
    // So are the visible asserted measurements.
    bool assertedRead = false;
    detail::AssertedValues asserted;

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
                    const CsvNames::Label label = names.label(annotations.labelAt(id, i));
                    if (i > 0) {
                        labels += ';';
                        locals += ';';
                    }
                    labels += label.name;
                    locals += label.isWorkspaceLocal ? '1' : '0';
                }
            }
            cell = column.kind == detail::ColumnKind::Labels ? labels : locals;
            break;
        case detail::ColumnKind::Label:
            if (record.hasLabel()) cell = names.label(record.label()).name;
            break;
        case detail::ColumnKind::LabelLocal:
            if (record.hasLabel())
                cell = names.label(record.label()).isWorkspaceLocal ? "1" : "0";
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
        case detail::ColumnKind::AssertedMeasurement: {
            if (!column.stage.isValid()) break;  // a stage the store lacks
            if (!assertedRead) {
                assertedRead = true;
                detail::readAssertedValues(store, id, asserted);
            }
            if (const Measurement* value = detail::findAsserted(asserted, column))
                detail::appendDouble(cell, value->value());
            break;
        }
        }
        if (c > 0) line += delimiter;
        detail::appendField(line, cell, delimiter);
    }
    line += '\n';
}

// Streams rows into its own file; the driver owns the partial path and the rename.
class CsvSink : public detail::ExportSink {
public:
    CsvSink(Store& store, const detail::ResolvedColumns& resolved, char delimiter,
            bool provenance, const std::string* slideId)
        : store_(store), resolved_(resolved), delimiter_(delimiter), provenance_(provenance),
          slideId_(slideId) {}

    void begin(const std::filesystem::path& partialPath) override {
        partial_ = partialPath.u8string();
        out_.open(partialPath, std::ios::binary | std::ios::trunc);
        if (!out_.is_open()) throwAboutTheFile("cannot create the output file");

        std::string line;
        if (provenance_) line = provenanceLines(store_, slideId_);
        for (size_t c = 0; c < resolved_.columns.size(); ++c) {
            if (c > 0) line += delimiter_;
            detail::appendField(line, resolved_.columns[c].name, delimiter_);
        }
        line += '\n';
        out_.write(line.data(), static_cast<std::streamsize>(line.size()));
        // Counted as written, so nothing can fail once the finished file is in place.
        bytes_ = static_cast<int64_t>(line.size());
    }

    void writeObject(const ObjectRecord& record, detail::NameCache& names) override {
        if (!csvNames_) csvNames_.emplace(names);
        line_.clear();
        appendRow(line_, record, resolved_, delimiter_, store_, *csvNames_);
        out_.write(line_.data(), static_cast<std::streamsize>(line_.size()));
        if (!out_) throwAboutTheFile("writing the output file failed");
        bytes_ += static_cast<int64_t>(line_.size());
    }

    int64_t finish() override {
        out_.close();
        if (!out_) throwAboutTheFile("writing the output file failed");
        return bytes_;
    }

    void close() noexcept override {
        if (out_.is_open()) out_.close();
    }

private:
    // Throws "CsvWriter::write: <message> (<path>)", naming the partial file by its path.
    [[noreturn]] void throwAboutTheFile(const char* message) const {
        throw IoError(detail::withPath(std::string("CsvWriter::write: ") + message, partial_).c_str());
    }

    Store& store_;
    const detail::ResolvedColumns& resolved_;
    char delimiter_;
    bool provenance_;
    const std::string* slideId_;
    std::ofstream out_;
    std::string partial_;  // the partial file's path (UTF-8), named in failures
    std::string line_;
    int64_t bytes_ = 0;
    std::optional<CsvNames> csvNames_;
};

} // namespace

class CsvWriter::Impl {
public:
    std::string path;
    ColumnSchema schema;
    char delimiter = ',';
    bool provenance = true;
    std::optional<std::string> slideId; // empty: the store's slide identifier
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

CsvWriter& CsvWriter::slideId(const char* id) {
    if (id == nullptr) {
        impl_->slideId.reset();
    } else {
        impl_->slideId = std::string(id);
    }
    return *this;
}

ExportResult CsvWriter::write(Store& store, const ObjectQuery& query) {
    return write(store, query, nullptr, CancellationToken());
}

ExportResult CsvWriter::write(Store& store, const ObjectQuery& query, ProgressObserver* observer,
                              const CancellationToken& token) {
    const detail::ResolvedColumns resolved = detail::resolveColumns(impl_->schema, store);
    detail::requireDistinctNames(resolved, "CsvWriter::write");
    CsvSink sink(store, resolved, impl_->delimiter, impl_->provenance,
                 impl_->slideId ? &*impl_->slideId : nullptr);
    return detail::runExport(store, query, std::filesystem::u8path(impl_->path),
                             "CsvWriter::write", sink, observer, token);
}

} // namespace slideio::processor
