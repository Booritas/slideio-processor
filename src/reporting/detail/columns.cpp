// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#include "reporting/detail/columns.hpp"

#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <algorithm>
#include <set>

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
        out.push_back({name, kind, std::string()});
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

    std::vector<std::string> bases = s.measurements;
    if (s.allMeasurements) {
        for (const std::string& name : held) {
            // A fixed column name would give the header two columns of that name.
            if (isTwinName(name) || isFixedColumnName(name)) continue;
            if (std::find(bases.begin(), bases.end(), name) == bases.end()) bases.push_back(name);
        }
    }

    auto addColumn = [&](const std::string& name) {
        result.columns.push_back({name, ColumnKind::Measurement, name});
    };
    for (const std::string& base : bases) {
        // A base name has at most one twin; ".um2" is checked first (areas).
        const std::string um2 = base + ".um2";
        const std::string um = base + ".um";
        const std::string* twin = held.count(um2) ? &um2 : (held.count(um) ? &um : nullptr);
        switch (s.units) {
        case UnitPolicy::PixelsOnly:
            addColumn(base);
            break;
        case UnitPolicy::Calibrated:
            addColumn(twin != nullptr ? *twin : base);
            break;
        case UnitPolicy::BothColumns:
            addColumn(base);
            if (twin != nullptr) addColumn(*twin);
            break;
        }
    }
    return result;
}

} // namespace slideio::processor::detail
