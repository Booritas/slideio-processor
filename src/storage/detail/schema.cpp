#include "storage/detail/schema.hpp"

#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/config/standard_vocabulary.hpp>
#include <slideio/processor/storage/store_format.hpp>
#include <slideio/processor/storage/types.hpp>

namespace slideio::processor::detail {

namespace {

// Bounding boxes are int64 and half-open: [min_x, max_x) x [min_y, max_y).
// Edge, annotation and raster tables belong to plans B and C.
constexpr const char* kDdl = R"sql(
CREATE TABLE meta(
    key   TEXT PRIMARY KEY,
    value
) WITHOUT ROWID;

CREATE TABLE layers(
    id             INTEGER PRIMARY KEY,
    name           TEXT    NOT NULL UNIQUE,
    kind           INTEGER NOT NULL,
    parent_id      INTEGER REFERENCES layers(id),
    depth          INTEGER NOT NULL,
    dropped        INTEGER NOT NULL DEFAULT 0,
    analysis_level INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE labels(
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL,
    origin       INTEGER NOT NULL,
    ontology_ref TEXT    NOT NULL DEFAULT ''
);

CREATE TABLE stages(
    id                INTEGER PRIMARY KEY,
    name              TEXT    NOT NULL UNIQUE,
    status            INTEGER NOT NULL DEFAULT 0,
    stale             INTEGER NOT NULL DEFAULT 0,
    configuration     TEXT    NOT NULL DEFAULT '',
    completed_at_ms   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE stage_deps(
    stage_id      INTEGER NOT NULL REFERENCES stages(id),
    depends_on_id INTEGER NOT NULL REFERENCES stages(id),
    PRIMARY KEY(stage_id, depends_on_id)
) WITHOUT ROWID;

CREATE TABLE stage_layers(
    layer_id INTEGER NOT NULL REFERENCES layers(id),
    stage_id INTEGER NOT NULL REFERENCES stages(id),
    PRIMARY KEY(layer_id, stage_id)
) WITHOUT ROWID;

CREATE TABLE objects(
    rowid     INTEGER PRIMARY KEY,
    layer_id  INTEGER NOT NULL REFERENCES layers(id),
    anchor_id INTEGER NOT NULL,
    stage_id  INTEGER NOT NULL REFERENCES stages(id),
    label_id  INTEGER REFERENCES labels(id),
    min_x     INTEGER NOT NULL,
    min_y     INTEGER NOT NULL,
    max_x     INTEGER NOT NULL,
    max_y     INTEGER NOT NULL,
    geometry  BLOB    NOT NULL,
    UNIQUE(layer_id, anchor_id)
);
CREATE INDEX objects_stage ON objects(stage_id);

CREATE VIRTUAL TABLE object_rtree USING rtree(rowid, min_x, max_x, min_y, max_y);

CREATE TABLE measurement_names(
    id   INTEGER PRIMARY KEY,
    name TEXT    NOT NULL UNIQUE
);

CREATE TABLE measurements(
    object_rowid INTEGER NOT NULL REFERENCES objects(rowid),
    ordinal      INTEGER NOT NULL,
    name_id      INTEGER NOT NULL REFERENCES measurement_names(id),
    value        REAL    NOT NULL,
    unit         INTEGER NOT NULL,
    PRIMARY KEY(object_rowid, ordinal)
) WITHOUT ROWID;

CREATE TABLE events(
    seq          INTEGER PRIMARY KEY AUTOINCREMENT,
    kind         INTEGER NOT NULL,
    stage_id     INTEGER,
    layer_id     INTEGER,
    label_id     INTEGER,
    object_count INTEGER NOT NULL DEFAULT 0,
    min_x        INTEGER NOT NULL DEFAULT 0,
    min_y        INTEGER NOT NULL DEFAULT 0,
    max_x        INTEGER NOT NULL DEFAULT 0,
    max_y        INTEGER NOT NULL DEFAULT 0,
    stage_status INTEGER,
    timestamp_ms INTEGER NOT NULL
);
)sql";

} // namespace

void createSchema(sqlite3* db, const char* provenanceJson, const char* storeUuid) {
    exec(db, kDdl);

    Statement pixel(db, "INSERT INTO layers(id, name, kind, parent_id, depth) VALUES (?, ?, ?, NULL, 0)");
    pixel.bindInt64(1, kPixelLayerId);
    pixel.bindText(2, kPixelLayerName);
    pixel.bindInt64(3, static_cast<int64_t>(LayerKind::Semantic));
    pixel.run();

    StoreImpl::setMetaInt64(db, kMetaFormatVersion, storeFormatVersion());
    StoreImpl::setMetaInt64(db, kMetaVocabularyVersion, standardVocabularyVersion());
    StoreImpl::setMetaText(db, kMetaProvenance, provenanceJson);
    StoreImpl::setMetaInt64(db, kMetaImportPolicy, static_cast<int64_t>(ImportPolicy::Strict));
    // Nothing is truncated yet: every sequence (they start at 1) is retained.
    StoreImpl::setMetaInt64(db, kMetaJournalTruncatedBelow, 1);
    StoreImpl::setMetaText(db, kMetaStoreUuid, storeUuid);
}

} // namespace slideio::processor::detail
