-- sqlite_master: type|name|tbl_name|sql, ordered by type, name
index|derived_from_primitive|derived_from|CREATE INDEX derived_from_primitive ON derived_from(primitive_layer, primitive_anchor)
index|objects_stage|objects|CREATE INDEX objects_stage ON objects(stage_id)
index|parent_edges_parent|parent_edges|CREATE INDEX parent_edges_parent ON parent_edges(parent_rowid)
index|rasters_writer|rasters|CREATE INDEX rasters_writer ON rasters(writer_stage_id)
index|sqlite_autoindex_layers_1|layers|
index|sqlite_autoindex_measurement_names_1|measurement_names|
index|sqlite_autoindex_objects_1|objects|
index|sqlite_autoindex_rasters_1|rasters|
index|sqlite_autoindex_stages_1|stages|
table|derived_from|derived_from|CREATE TABLE derived_from(
    derived_layer    INTEGER NOT NULL,
    derived_anchor   INTEGER NOT NULL,
    primitive_layer  INTEGER NOT NULL,
    primitive_anchor INTEGER NOT NULL,
    stage_id         INTEGER NOT NULL REFERENCES stages(id),
    PRIMARY KEY(derived_layer, derived_anchor, primitive_layer, primitive_anchor, stage_id)
) WITHOUT ROWID
table|events|events|CREATE TABLE events(
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
    timestamp_ms INTEGER NOT NULL,
    raster_id    INTEGER
)
table|label_assertions|label_assertions|CREATE TABLE label_assertions(
    object_rowid INTEGER NOT NULL REFERENCES objects(rowid),
    stage_id     INTEGER NOT NULL REFERENCES stages(id),
    label_id     INTEGER NOT NULL REFERENCES labels(id),
    PRIMARY KEY(object_rowid, stage_id, label_id)
) WITHOUT ROWID
table|labels|labels|CREATE TABLE labels(
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL,
    origin       INTEGER NOT NULL,
    ontology_ref TEXT    NOT NULL DEFAULT ''
)
table|layers|layers|CREATE TABLE layers(
    id             INTEGER PRIMARY KEY,
    name           TEXT    NOT NULL UNIQUE,
    kind           INTEGER NOT NULL,
    parent_id      INTEGER REFERENCES layers(id),
    depth          INTEGER NOT NULL,
    dropped        INTEGER NOT NULL DEFAULT 0,
    analysis_level INTEGER NOT NULL DEFAULT 0,
    -- Set by the first ObjectWriter commit into the layer, never cleared (ruling S16).
    ever_populated       INTEGER NOT NULL DEFAULT 0,
    dropped_object_count INTEGER NOT NULL DEFAULT 0
)
table|measurement_assertions|measurement_assertions|CREATE TABLE measurement_assertions(
    object_rowid INTEGER NOT NULL REFERENCES objects(rowid),
    stage_id     INTEGER NOT NULL REFERENCES stages(id),
    name_id      INTEGER NOT NULL REFERENCES measurement_names(id),
    value        REAL    NOT NULL,
    unit         INTEGER NOT NULL,
    PRIMARY KEY(object_rowid, stage_id, name_id)
) WITHOUT ROWID
table|measurement_names|measurement_names|CREATE TABLE measurement_names(
    id   INTEGER PRIMARY KEY,
    name TEXT    NOT NULL UNIQUE
)
table|measurements|measurements|CREATE TABLE measurements(
    object_rowid INTEGER NOT NULL REFERENCES objects(rowid),
    ordinal      INTEGER NOT NULL,
    name_id      INTEGER NOT NULL REFERENCES measurement_names(id),
    value        REAL    NOT NULL,
    unit         INTEGER NOT NULL,
    PRIMARY KEY(object_rowid, ordinal)
) WITHOUT ROWID
table|meta|meta|CREATE TABLE meta(
    key   TEXT PRIMARY KEY,
    value
) WITHOUT ROWID
table|object_rtree|object_rtree|CREATE VIRTUAL TABLE object_rtree USING rtree(rowid, min_x, max_x, min_y, max_y)
table|object_rtree_node|object_rtree_node|CREATE TABLE "object_rtree_node"(nodeno INTEGER PRIMARY KEY,data)
table|object_rtree_parent|object_rtree_parent|CREATE TABLE "object_rtree_parent"(nodeno INTEGER PRIMARY KEY,parentnode)
table|object_rtree_rowid|object_rtree_rowid|CREATE TABLE "object_rtree_rowid"(rowid INTEGER PRIMARY KEY,nodeno)
table|objects|objects|CREATE TABLE objects(
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
)
table|parent_edges|parent_edges|CREATE TABLE parent_edges(
    child_rowid  INTEGER PRIMARY KEY REFERENCES objects(rowid),
    parent_rowid INTEGER NOT NULL REFERENCES objects(rowid),
    stage_id     INTEGER NOT NULL REFERENCES stages(id)
) WITHOUT ROWID
table|rasters|rasters|CREATE TABLE rasters(
    id              INTEGER PRIMARY KEY,
    name            TEXT    NOT NULL UNIQUE,
    data_type       INTEGER NOT NULL,
    plane_count     INTEGER NOT NULL,
    compression     INTEGER NOT NULL,
    description     TEXT    NOT NULL DEFAULT '',
    writer_stage_id INTEGER REFERENCES stages(id),
    level           INTEGER NOT NULL DEFAULT 0,
    scale_x         REAL    NOT NULL DEFAULT 0,
    scale_y         REAL    NOT NULL DEFAULT 0,
    width           INTEGER NOT NULL DEFAULT 0,
    height          INTEGER NOT NULL DEFAULT 0,
    file_name       TEXT,
    bytes           INTEGER NOT NULL DEFAULT 0,
    populated       INTEGER NOT NULL DEFAULT 0
)
table|sqlite_sequence|sqlite_sequence|CREATE TABLE sqlite_sequence(name,seq)
table|stage_deps|stage_deps|CREATE TABLE stage_deps(
    stage_id      INTEGER NOT NULL REFERENCES stages(id),
    depends_on_id INTEGER NOT NULL REFERENCES stages(id),
    PRIMARY KEY(stage_id, depends_on_id)
) WITHOUT ROWID
table|stage_layers|stage_layers|CREATE TABLE stage_layers(
    layer_id INTEGER NOT NULL REFERENCES layers(id),
    stage_id INTEGER NOT NULL REFERENCES stages(id),
    PRIMARY KEY(layer_id, stage_id)
) WITHOUT ROWID
table|stages|stages|CREATE TABLE stages(
    id                INTEGER PRIMARY KEY,
    name              TEXT    NOT NULL UNIQUE,
    status            INTEGER NOT NULL DEFAULT 0,
    stale             INTEGER NOT NULL DEFAULT 0,
    configuration     TEXT    NOT NULL DEFAULT '',
    completed_at_ms   INTEGER NOT NULL DEFAULT 0
)
-- meta keys, sorted
format_version
import_policy
journal_truncated_below
layers_revision
provenance
rasters_revision
store_uuid
vocabulary_version
