// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#include "reporting/detail/columns.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/store.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace slideio::processor::detail {

const ColumnSchema::Impl& ColumnSchemaAccess::impl(const ColumnSchema& schema) noexcept {
    return *schema.impl_;
}

bool isTwinName(const std::string& name) noexcept {
    auto ends = [&](const char* suffix, size_t n) {
        return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
    };
    return ends(".um", 3) || ends(".um2", 4);
}

bool isFixedColumnName(const std::string& name) noexcept {
    static const char* const kNames[] = {
        "layer_id",    "anchor_id",    "object_id",       "layer",
        "stage",       "labels",       "labels_local",    "label",
        "label_local", "bbox_x",       "bbox_y",          "bbox_width",
        "bbox_height", "geometry_wkt", "parent_layer_id", "parent_anchor_id",
        "parent_id"};
    for (const char* fixed : kNames) {
        if (name == fixed) return true;
    }
    return false;
}

std::vector<ResolvedColumn> fixedColumns(const ColumnSchema::Impl& s) {
    std::vector<ResolvedColumn> out;
    auto add = [&](const char* name, ColumnKind kind) {
        out.push_back({name, kind, std::string(), StageId()});
    };
    if (s.idFormat == IdFormat::TwoColumns) {
        add("layer_id", ColumnKind::LayerId);
        add("anchor_id", ColumnKind::AnchorId);
    } else {
        add("object_id", ColumnKind::ObjectId);
    }
    if (s.layer) add("layer", ColumnKind::Layer);
    if (s.stage) add("stage", ColumnKind::Stage);
    if (s.label) {
        if (s.labelPolicy == LabelPolicy::EffectiveSet) {
            add("labels", ColumnKind::Labels);
            add("labels_local", ColumnKind::LabelsLocal);
        } else {
            add("label", ColumnKind::Label);
            add("label_local", ColumnKind::LabelLocal);
        }
    }
    if (s.boundingBox) {
        add("bbox_x", ColumnKind::BboxX);
        add("bbox_y", ColumnKind::BboxY);
        add("bbox_width", ColumnKind::BboxWidth);
        add("bbox_height", ColumnKind::BboxHeight);
    }
    if (s.geometryWkt) add("geometry_wkt", ColumnKind::GeometryWkt);
    if (s.parent) {
        if (s.idFormat == IdFormat::TwoColumns) {
            add("parent_layer_id", ColumnKind::ParentLayerId);
            add("parent_anchor_id", ColumnKind::ParentAnchorId);
        } else {
            add("parent_id", ColumnKind::ParentId);
        }
    }
    return out;
}

ResolvedColumns resolveColumns(const ColumnSchema& schema, Store& store) {
    const ColumnSchema::Impl& s = ColumnSchemaAccess::impl(schema);
    ResolvedColumns result;
    result.columns = fixedColumns(s);

    ObjectStore& objects = store.objects();
    // The store lists names in ascending byte order; std::set keeps that order.
    std::set<std::string> held;
    const int32_t count = objects.measurementNameCount();
    for (int32_t i = 0; i < count; ++i) held.insert(objects.measurementNameAt(i));

    // The asserted names of each stage, from the listing ordered by stage id, then name.
    AnnotationStore& annotations = store.annotations();
    std::vector<std::pair<StageId, std::string>> assertedPairs;
    std::map<int32_t, std::set<std::string>> assertedHeld;
    const int32_t assertedCount = annotations.assertedMeasurementColumnCount();
    for (int32_t i = 0; i < assertedCount; ++i) {
        StageId stage;
        const char* name = nullptr;
        annotations.assertedMeasurementColumnAt(i, stage, name);
        assertedPairs.emplace_back(stage, name);
        assertedHeld[stage.value()].insert(name);
    }

    // The name list is shared with assertions. A name only ever asserted has its
    // stage-qualified columns and is neither a row column nor a row twin, so it leaves
    // `held`. Only asserted names are checked, and only when row columns are resolved,
    // since each check may scan every row measurement.
    const bool rowColumns =
        s.allMeasurements || std::any_of(s.measurements.begin(), s.measurements.end(),
                                         [](const ColumnSchema::Impl::Explicit& m) { return m.stage.empty(); });
    if (rowColumns) {
        std::set<std::string> assertedNames;
        for (const auto& pair : assertedPairs) assertedNames.insert(pair.second);
        for (const std::string& name : assertedNames) {
            if (held.count(name) > 0 && !objects.hasRowMeasurementName(name.c_str())) held.erase(name);
        }
    }

    // One measurement to resolve: a row base name (stageName empty) or a stage's.
    struct Wanted {
        std::string stageName;
        StageId stage;  // invalid for a row name or a stage the store lacks
        std::string base;
    };
    StageStateReader& stages = store.stageStates();
    std::vector<Wanted> wanted;
    auto isWanted = [&](const std::string& stageName, const std::string& base) {
        return std::any_of(wanted.begin(), wanted.end(), [&](const Wanted& w) {
            return w.stageName == stageName && w.base == base;
        });
    };
    for (const ColumnSchema::Impl::Explicit& m : s.measurements) {
        StageId stage;
        if (!m.stage.empty() && !stages.find(m.stage.c_str(), stage)) stage = StageId();
        wanted.push_back({m.stage, stage, m.name});
    }
    if (s.allMeasurements) {
        for (const std::string& name : held) {
            // A fixed column name would give the header two columns of that name.
            if (isTwinName(name) || isFixedColumnName(name)) continue;
            if (!isWanted(std::string(), name)) wanted.push_back({std::string(), StageId(), name});
        }
        for (const auto& [stage, name] : assertedPairs) {
            if (isTwinName(name)) continue;
            std::string stageName = stages.name(stage);
            if (!isWanted(stageName, name)) wanted.push_back({std::move(stageName), stage, name});
        }
    }

    static const std::set<std::string> kNone;
    for (const Wanted& w : wanted) {
        const bool asserted = !w.stageName.empty();
        const std::set<std::string>* names = &held;
        if (asserted) {
            const auto it = w.stage.isValid() ? assertedHeld.find(w.stage.value()) : assertedHeld.end();
            names = it != assertedHeld.end() ? &it->second : &kNone;
        }
        auto addColumn = [&](const std::string& name) {
            if (asserted) {
                result.columns.push_back({assertedColumnName(w.stageName, name),
                                          ColumnKind::AssertedMeasurement, name, w.stage});
            } else {
                result.columns.push_back({name, ColumnKind::Measurement, name, StageId()});
            }
        };
        // A base name has at most one twin; ".um2" is checked first (areas).
        const std::string um2 = w.base + ".um2";
        const std::string um = w.base + ".um";
        const std::string* twin = names->count(um2) ? &um2 : (names->count(um) ? &um : nullptr);
        switch (s.units) {
        case UnitPolicy::PixelsOnly:
            addColumn(w.base);
            break;
        case UnitPolicy::Calibrated:
            addColumn(twin != nullptr ? *twin : w.base);
            break;
        case UnitPolicy::BothColumns:
            addColumn(w.base);
            if (twin != nullptr) addColumn(*twin);
            break;
        }
    }
    return result;
}

std::string assertedColumnName(const std::string& stageName, const std::string& name) {
    return stageName + '/' + name;
}

void readAssertedValues(Store& store, ObjectId object, AssertedValues& out) {
    out.clear();
    AnnotationStore& annotations = store.annotations();
    const int32_t count = annotations.assertedMeasurementCount(object);
    for (int32_t i = 0; i < count; ++i) {
        const Measurement value = annotations.assertedMeasurementAt(object, i);
        const StageId stage = annotations.measurementAssertedBy(object, i);
        out[{stage.value(), std::string(value.name())}] = value;
    }
}

void requireDistinctNames(const ResolvedColumns& resolved, const char* who) {
    std::set<std::string> seen;
    for (const ResolvedColumn& column : resolved.columns) {
        if (!seen.insert(column.name).second) {
            const std::string message =
                std::string(who) + ": two columns would have the same name";
            throw InvalidArgumentError(message.c_str());
        }
    }
}

} // namespace slideio::processor::detail
