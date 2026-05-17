-- Add higher-level detection events and optional enrichment job metadata.

-- migrate:up

CREATE TABLE IF NOT EXISTS detection_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    stream_name TEXT NOT NULL,
    label TEXT NOT NULL,
    sub_label TEXT DEFAULT '',
    recognized_text TEXT DEFAULT '',
    start_time INTEGER NOT NULL,
    end_time INTEGER,
    best_time INTEGER,
    best_confidence REAL DEFAULT 0.0,
    best_detection_id INTEGER,
    recording_id INTEGER,
    track_id INTEGER DEFAULT -1,
    zone_id TEXT DEFAULT '',
    thumbnail_path TEXT DEFAULT '',
    status TEXT DEFAULT 'active',
    created_at INTEGER DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER DEFAULT (strftime('%s', 'now')),
    FOREIGN KEY (recording_id) REFERENCES recordings(id),
    FOREIGN KEY (best_detection_id) REFERENCES detections(id)
);

CREATE INDEX IF NOT EXISTS idx_detection_events_stream_time
    ON detection_events(stream_name, start_time, end_time);
CREATE INDEX IF NOT EXISTS idx_detection_events_label
    ON detection_events(label);
CREATE INDEX IF NOT EXISTS idx_detection_events_sub_label
    ON detection_events(sub_label);
CREATE INDEX IF NOT EXISTS idx_detection_events_recording
    ON detection_events(recording_id);
CREATE INDEX IF NOT EXISTS idx_detection_events_status
    ON detection_events(status);

CREATE TABLE IF NOT EXISTS event_enrichments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id INTEGER NOT NULL,
    type TEXT NOT NULL,
    provider TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    score REAL DEFAULT 0.0,
    value TEXT DEFAULT '',
    json TEXT DEFAULT '',
    error TEXT DEFAULT '',
    created_at INTEGER DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER DEFAULT (strftime('%s', 'now')),
    FOREIGN KEY (event_id) REFERENCES detection_events(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_event_enrichments_event_type
    ON event_enrichments(event_id, type);

CREATE TABLE IF NOT EXISTS enrichment_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id INTEGER NOT NULL,
    type TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'queued',
    attempts INTEGER DEFAULT 0,
    next_attempt_at INTEGER DEFAULT (strftime('%s', 'now')),
    payload_json TEXT DEFAULT '',
    result_json TEXT DEFAULT '',
    error TEXT DEFAULT '',
    created_at INTEGER DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER DEFAULT (strftime('%s', 'now')),
    FOREIGN KEY (event_id) REFERENCES detection_events(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_enrichment_jobs_status_next
    ON enrichment_jobs(status, next_attempt_at);
CREATE INDEX IF NOT EXISTS idx_enrichment_jobs_event_type
    ON enrichment_jobs(event_id, type);

CREATE TABLE IF NOT EXISTS known_faces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    image_path TEXT NOT NULL,
    embedding_ref TEXT DEFAULT '',
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_known_faces_name ON known_faces(name);

-- migrate:down

DROP INDEX IF EXISTS idx_known_faces_name;
DROP TABLE IF EXISTS known_faces;
DROP INDEX IF EXISTS idx_enrichment_jobs_event_type;
DROP INDEX IF EXISTS idx_enrichment_jobs_status_next;
DROP TABLE IF EXISTS enrichment_jobs;
DROP INDEX IF EXISTS idx_event_enrichments_event_type;
DROP TABLE IF EXISTS event_enrichments;
DROP INDEX IF EXISTS idx_detection_events_status;
DROP INDEX IF EXISTS idx_detection_events_recording;
DROP INDEX IF EXISTS idx_detection_events_sub_label;
DROP INDEX IF EXISTS idx_detection_events_label;
DROP INDEX IF EXISTS idx_detection_events_stream_time;
DROP TABLE IF EXISTS detection_events;
