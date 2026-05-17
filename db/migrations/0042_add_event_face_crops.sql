-- Store detected face crop candidates for camera events.

-- migrate:up

CREATE TABLE IF NOT EXISTS event_face_crops (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id INTEGER NOT NULL,
    crop_path TEXT NOT NULL,
    bbox_x REAL DEFAULT 0.0,
    bbox_y REAL DEFAULT 0.0,
    bbox_w REAL DEFAULT 0.0,
    bbox_h REAL DEFAULT 0.0,
    confidence REAL DEFAULT 0.0,
    name TEXT DEFAULT '',
    status TEXT DEFAULT 'unknown',
    source TEXT DEFAULT 'light-object-detect',
    created_at INTEGER DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER DEFAULT (strftime('%s', 'now')),
    FOREIGN KEY (event_id) REFERENCES detection_events(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_event_face_crops_event
    ON event_face_crops(event_id);
CREATE INDEX IF NOT EXISTS idx_event_face_crops_status
    ON event_face_crops(status, created_at);

-- migrate:down

DROP INDEX IF EXISTS idx_event_face_crops_status;
DROP INDEX IF EXISTS idx_event_face_crops_event;
DROP TABLE IF EXISTS event_face_crops;
