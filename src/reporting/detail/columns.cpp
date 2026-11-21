// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#include "reporting/detail/columns.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/store.hpp>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
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

namespace {
// The base of a twin name ("n.um" or "n.um2" gives "n"); any other name unchanged.
std::string baseName(const std::string& name) {
    for (const char* suffix : {".um2", ".um"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0)
            return name.substr(0, name.size() - n);
    }
    return name;
}
} // namespace

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

ResolvedColumns resolveColumns(const ColumnSchema& schema, Store& store, ColumnLayout layout) {
    const ColumnSchema::Impl& s = ColumnSchemaAccess::impl(schema);
    ResolvedColumns result;
    if (layout == ColumnLayout::Table) result.columns = fixedColumns(s);

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
    std::set<std::string> assertedNames;
    for (const auto& pair : assertedPairs) assertedNames.insert(pair.second);

    // `held` is the store's whole name list, shared with assertions (ruling C): a name
    // only later stages assert is also a row name, and its row column is empty.

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
            // A row twin no base name accompanies (an importer may write "len.um" alone)
            // counts as its base, as an asserted one does below: the unit policy then
            // picks the twin, the pixel column or both. A twin with its base is reached
            // through the base.
            const std::string base = baseName(name);
            if (base != name && held.count(base) > 0) continue;
            // In a table a fixed column name would give the header two columns of that
            // name; nested measurements (GeoJSON) cannot clash with the fixed fields.
            if (layout == ColumnLayout::Table && isFixedColumnName(base)) continue;
            if (!isWanted(std::string(), base)) wanted.push_back({std::string(), StageId(), base});
        }
        for (const auto& [stage, name] : assertedPairs) {
            // A twin the stage asserts without its base still gets its column, through
            // the base: the unit policy then picks the twin, the pixel column or both.
            const std::string base = baseName(name);
            std::string stageName = stages.name(stage);
            if (!isWanted(stageName, base)) wanted.push_back({std::move(stageName), stage, base});
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
            // A row twin name some stage asserts may be held by no row at all (the name
            // list is shared): the rows' pixel column is written instead, so no row value
            // is hidden behind an empty twin column.
            if (!asserted && twin != nullptr && assertedNames.count(*twin) > 0) twin = nullptr;
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

const Measurement* findAsserted(const AssertedValues& values, const ResolvedColumn& column) {
    if (!column.stage.isValid()) return nullptr;
    const auto it = values.find(AssertedKeyView{column.stage.value(), column.measurement});
    return it == values.end() ? nullptr : &it->second;
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
