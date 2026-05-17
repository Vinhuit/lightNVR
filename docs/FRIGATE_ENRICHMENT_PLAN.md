# Frigate-Inspired Enrichment Plan

Research date: 2026-05-16

This plan compares LightNVR's current detection architecture with Frigate's enrichment features and defines a path for adding the useful ideas without losing LightNVR's lightweight design.

## Current LightNVR Baseline

LightNVR already has the right low-level primitives for event enrichment:

- C core with go2rtc as the streaming backbone.
- Optional object detection through SOD/TFLite or an external API such as `light-object-detect`.
- Per-stream detection interval, confidence threshold, object include/exclude filters, and per-stream detection API URL.
- Polygon detection zones with class filters, confidence thresholds, and zone IDs.
- `detections` table with `stream_name`, `timestamp`, `label`, `confidence`, normalized box coordinates, `recording_id`, `track_id`, and `zone_id`.
- Annotation mode where detections link to continuous recordings through `recording_id`.
- Recording filters by detection labels, recording tags, timeline playback, detection overlays, MQTT detection publishing, ONVIF motion triggers, PTZ fields, and tag-based RBAC.

The missing layer is not raw detection. The missing layer is a higher-level event/enrichment model that can group detections into useful objects/events, attach metadata, and run optional AI enrichment only when it is valuable.

## Frigate Features Checked

Frigate's current public docs and repo describe these relevant features:

- Object detection, tracking, motion-aware processing, Home Assistant/MQTT integration, and recording retention based on detected objects.
- Face recognition: runs after `person` detection, can use a small CPU FaceNet model or larger GPU/NPU-oriented ArcFace model. Requires AVX + AVX2.
- License plate recognition: runs after `car` or `motorcycle` detection in normal mode, uses plate detection plus OCR, and requires at least 4GB RAM and AVX + AVX2.
- Semantic search: local image/text embeddings for tracked objects. Requires at least 8GB RAM; 16GB+ and GPU are recommended.
- Generative AI object descriptions and review summaries: event/object thumbnails or frames are sent to an AI provider after an object/review lifecycle.
- Audio events and audio transcription: transcription is not intended to run continuously; Frigate initiates transcriptions manually from UI/API because continuous transcription is expensive.
- Hardware acceleration for enrichments is separate from object detection acceleration. Coral helps object detection, not enrichment models.

Primary sources:

- Frigate repo: https://github.com/blakeblackshear/frigate
- Face recognition: https://docs.frigate.video/configuration/face_recognition/
- LPR: https://docs.frigate.video/configuration/license_plate_recognition/
- Semantic search: https://docs.frigate.video/configuration/semantic_search/
- GenAI object descriptions: https://docs.frigate.video/configuration/genai/genai_objects/
- GenAI configuration: https://docs.frigate.video/configuration/genai/genai_config/
- Audio detectors/transcription: https://docs.frigate.video/configuration/audio_detectors/
- Hardware acceleration for enrichments: https://docs.frigate.video/configuration/hardware_acceleration_enrichments/

## Design Principle

Do not embed heavy AI libraries into the LightNVR core.

LightNVR should keep the core as:

```text
C core + SQLite + go2rtc + recording/timeline/detection metadata
```

Optional enrichment should run as external services:

```text
LightNVR event/crop/thumbnail
        |
        +--> lpr-api
        +--> face-recognition-api
        +--> genai-description-api
        +--> semantic-index-api
```

The core owns scheduling, metadata, retention, permissions, UI, and failure handling. External services own expensive inference. All enrichment features should be disabled by default, event-triggered, bounded by rate limits, and configurable per stream/zone/object class.

## Priority Decision

| Priority | Feature | Decision | Reason |
| --- | --- | --- | --- |
| P0 | Event/object model | Build first | Required foundation for every enrichment. |
| P1 | Webhooks | Build early | Useful, low CPU, complements MQTT and external services. |
| P1 | LPR | Add as external service | Strong NVR value and can be gated to vehicles/zones. |
| P2 | GenAI descriptions | Add as external service | Useful if run only after event end from thumbnails/crops. |
| P2 | Face recognition | Add after event model | Valuable but privacy-sensitive and harder to tune. |
| P3 | Semantic search | Optional advanced module | Conflicts with low-memory target unless isolated. |
| P3 | Audio transcription | Manual/API-triggered only | Continuous transcription is too expensive. |
| P3 | Classification plugins | Optional later | Useful but not core security workflow. |
| Avoid | Always-on enrichments in C core | Do not build | Would remove LightNVR's lightweight advantage. |

## Phase 1: Event Model Foundation

Goal: convert raw detections into stable, queryable event/object records.

Add tables:

```sql
CREATE TABLE detection_events (
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

CREATE INDEX idx_detection_events_stream_time
    ON detection_events(stream_name, start_time, end_time);
CREATE INDEX idx_detection_events_label
    ON detection_events(label);
CREATE INDEX idx_detection_events_sub_label
    ON detection_events(sub_label);
CREATE INDEX idx_detection_events_recording
    ON detection_events(recording_id);
```

Add enrichment metadata:

```sql
CREATE TABLE event_enrichments (
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

CREATE INDEX idx_event_enrichments_event_type
    ON event_enrichments(event_id, type);
```

Core behavior:

- Group detections by `stream_name`, `label`, `track_id` when available, and time/box overlap when `track_id` is absent.
- Maintain an active event until no matching detection appears for a configurable close timeout.
- Keep the highest-confidence detection as `best_detection_id` and `best_time`.
- Generate or reference a best thumbnail/crop only at event close or after a meaningful confidence improvement.
- Keep raw `detections` unchanged for overlays and debugging.

New APIs:

- `GET /api/events`
- `GET /api/events/{id}`
- `GET /api/events/{id}/detections`
- `GET /api/events/{id}/thumbnail`
- `PUT /api/events/{id}/sub-label`
- `PUT /api/events/{id}/tags`

UI:

- Add an Events/Review view or extend Recordings with event grouping.
- Add filters for object label, sub-label, recognized text, zone, stream, date, confidence, tag, and enrichment status.
- Let timeline segments link to event details.

## Phase 2: Webhooks and Enrichment Job Queue

Goal: make external services reliable without blocking detection threads.

Add tables:

```sql
CREATE TABLE enrichment_jobs (
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

CREATE INDEX idx_enrichment_jobs_status_next
    ON enrichment_jobs(status, next_attempt_at);
```

Add settings:

- Global enable/disable for enrichment jobs.
- Per-type worker concurrency.
- Per-type timeout, retry count, retry backoff, and max queue size.
- Per-camera and per-zone enable/disable.
- Per-object filters such as `person` for face recognition and `car,motorcycle` for LPR.

Webhook events:

- `detection.created`
- `event.started`
- `event.updated`
- `event.ended`
- `enrichment.completed`
- `enrichment.failed`

Webhook payloads should include IDs, stream, label, sub-label, recognized text, confidence, zone, timestamps, recording ID, and optional thumbnail URL. They should not include full image bytes by default.

## Phase 3: LPR

Goal: add plate recognition while preserving low CPU.

Architecture:

- External `lpr-api` endpoint.
- Trigger only when an event label is `car` or `motorcycle`, or when a stream is explicitly marked as a dedicated LPR stream.
- Use event crop/thumbnail or a go2rtc snapshot around `best_time`.
- Store plate result in `detection_events.recognized_text` and `event_enrichments`.
- Optional known-plate mapping updates `sub_label`.

Settings:

- `enabled`
- `api_url`
- `min_vehicle_confidence`
- `min_plate_area`
- `recognition_threshold`
- `min_plate_length`
- `format_regex`
- `known_plates`
- `required_zones`
- `debug_save_crops`

UI:

- Recognized Plate filter.
- Known Plates settings.
- Plate result and crop on event detail.

Guardrails:

- Disabled by default.
- Per-stream and per-zone gating.
- One job per event by default, with optional retry if a better crop arrives.
- Dedicated LPR mode must be explicit because it may raise CPU.

## Phase 4: GenAI Descriptions

Goal: add useful event summaries without local heavy models in the core.

Architecture:

- External OpenAI-compatible or generic HTTP provider.
- Trigger after event close, not during every detection frame.
- Send one thumbnail/crop by default; optionally send a small sequence of event thumbnails.
- Store description and model metadata in `event_enrichments`.

Settings:

- `enabled`
- `provider`: `external`, `openai`, `gemini`, `ollama`, or `openai_compatible`
- `api_url`: for example `https://api.openai.com/v1/responses`, `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent`, or `http://localhost:11434/api/chat`
- `api_key_env`
- `model`
- `prompt_template`
- `required_objects`
- `required_zones`
- `max_images`
- `max_events_per_hour`

UI:

- Description shown in event details.
- Filter/search over generated description using SQLite FTS later if needed.

Guardrails:

- Never require cloud AI for core NVR behavior.
- Redact credentials in logs.
- Show clear local/cloud provider status.

## Phase 5: Face Recognition

Goal: support known-person labels while keeping privacy and CPU costs controlled.

Architecture:

- External `face-recognition-api`.
- Trigger only for `person` events.
- Require a minimum person box size or face crop size.
- Store recognized person in `sub_label`; store raw matches in `event_enrichments`.

Data model additions:

```sql
CREATE TABLE known_faces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    image_path TEXT NOT NULL,
    embedding_ref TEXT DEFAULT '',
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);
```

UI:

- Face Library page for known people.
- Recent unknown faces queue.
- Train/assign/delete flow.
- Per-camera enable/disable.

Guardrails:

- Disabled by default.
- Admin-only.
- Clear privacy warning in docs.
- No bulk import workflow in the first version.

## Phase 6: Semantic Search

Goal: make this an optional advanced feature, not part of the lightweight baseline.

Architecture:

- External `semantic-index-api`, not in the C core.
- Index event thumbnails/descriptions asynchronously.
- Store only references and small metadata in LightNVR unless a vector extension is intentionally adopted.

Initial API:

- `POST /api/search/events` with text query.
- Return event IDs, scores, thumbnails, labels, and timestamps.

Guardrails:

- Separate Docker profile or companion service.
- Disabled by default.
- Document RAM/GPU expectations clearly.

## Phase 7: Audio Events and Transcription

Goal: support useful audio workflows without continuous heavy transcription.

Suggested order:

1. Add audio event webhook ingestion first, so external systems can create audio events.
2. Add manual/API-triggered transcription for selected recordings/events.
3. Consider lightweight audio event detection only if a service can do it externally.

Avoid continuous transcription in the LightNVR core.

## Implementation Order

1. Add `detection_events` and `event_enrichments` schema plus migration tests. (Implemented in migration 0041.)
2. Add event aggregator in the unified detection path without changing recording behavior. (Implemented at detection storage time.)
3. Add Events API and recording/timeline linking. (Initial `/api/events`, `/api/events/{id}`, and enrichment endpoints implemented.)
4. Add minimal Events UI.
5. Add webhook delivery with retries and signing.
6. Add enrichment job queue. (Initial queue table, enqueue hooks, and worker claim/complete/fail API implemented.)
7. Add LPR external API integration.
8. Add GenAI external API integration. (Settings UI/API now store provider URL, model, and API-key environment variable for external workers.)
9. Add face recognition external API integration.
10. Add optional semantic search companion integration.
11. Add optional audio transcription integration.

## Success Criteria

- With all enrichments disabled, CPU and memory are effectively unchanged from current LightNVR.
- Detection threads never block on LPR, face recognition, GenAI, semantic search, or webhooks.
- Enrichment queue failures do not break recording, streaming, or detection.
- Every enrichment can be disabled globally and per stream.
- Event metadata is queryable without requiring external services to be online.
- LightNVR keeps raw detections for overlays and debug views.

## Non-Goals

- Do not copy Frigate's implementation into LightNVR.
- Do not require Python, PyTorch, PaddleOCR, Whisper, CLIP/Jina, FaceNet, ArcFace, or LLM clients in the C core.
- Do not make semantic search or GenAI part of the default install.
- Do not run face recognition, LPR, or transcription continuously on every frame.
- Do not remove existing raw detection overlays and recording filters.
