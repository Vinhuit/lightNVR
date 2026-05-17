#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "core/config.h"
#include "core/logger.h"
#include "database/db_core.h"
#include "database/db_detection_events.h"
#include "utils/strings.h"

#define EVENT_ACTIVE_GAP_SECONDS 30

static void copy_sql_text(sqlite3_stmt *stmt, int col, char *dst, size_t dst_size) {
    const char *text = (const char *)sqlite3_column_text(stmt, col);
    if (text) {
        safe_strcpy(dst, text, dst_size, 0);
    } else if (dst_size > 0) {
        dst[0] = '\0';
    }
}

static void row_to_event(sqlite3_stmt *stmt, detection_event_t *event) {
    event->id = (uint64_t)sqlite3_column_int64(stmt, 0);
    copy_sql_text(stmt, 1, event->stream_name, sizeof(event->stream_name));
    copy_sql_text(stmt, 2, event->label, sizeof(event->label));
    copy_sql_text(stmt, 3, event->sub_label, sizeof(event->sub_label));
    copy_sql_text(stmt, 4, event->recognized_text, sizeof(event->recognized_text));
    event->start_time = (time_t)sqlite3_column_int64(stmt, 5);
    event->end_time = (time_t)sqlite3_column_int64(stmt, 6);
    event->best_time = (time_t)sqlite3_column_int64(stmt, 7);
    event->best_confidence = (float)sqlite3_column_double(stmt, 8);
    event->best_detection_id = (uint64_t)sqlite3_column_int64(stmt, 9);
    event->recording_id = (uint64_t)sqlite3_column_int64(stmt, 10);
    event->track_id = sqlite3_column_int(stmt, 11);
    copy_sql_text(stmt, 12, event->zone_id, sizeof(event->zone_id));
    copy_sql_text(stmt, 13, event->thumbnail_path, sizeof(event->thumbnail_path));
    copy_sql_text(stmt, 14, event->status, sizeof(event->status));
    event->created_at = (time_t)sqlite3_column_int64(stmt, 15);
    event->updated_at = (time_t)sqlite3_column_int64(stmt, 16);
}

static void row_to_enrichment(sqlite3_stmt *stmt, event_enrichment_t *enrichment) {
    enrichment->id = (uint64_t)sqlite3_column_int64(stmt, 0);
    enrichment->event_id = (uint64_t)sqlite3_column_int64(stmt, 1);
    copy_sql_text(stmt, 2, enrichment->type, sizeof(enrichment->type));
    copy_sql_text(stmt, 3, enrichment->provider, sizeof(enrichment->provider));
    copy_sql_text(stmt, 4, enrichment->status, sizeof(enrichment->status));
    enrichment->score = (float)sqlite3_column_double(stmt, 5);
    copy_sql_text(stmt, 6, enrichment->value, sizeof(enrichment->value));
    copy_sql_text(stmt, 7, enrichment->json, sizeof(enrichment->json));
    copy_sql_text(stmt, 8, enrichment->error, sizeof(enrichment->error));
    enrichment->created_at = (time_t)sqlite3_column_int64(stmt, 9);
    enrichment->updated_at = (time_t)sqlite3_column_int64(stmt, 10);
}

static void row_to_job(sqlite3_stmt *stmt, enrichment_job_t *job) {
    job->id = (uint64_t)sqlite3_column_int64(stmt, 0);
    job->event_id = (uint64_t)sqlite3_column_int64(stmt, 1);
    copy_sql_text(stmt, 2, job->type, sizeof(job->type));
    copy_sql_text(stmt, 3, job->status, sizeof(job->status));
    job->attempts = sqlite3_column_int(stmt, 4);
    job->next_attempt_at = (time_t)sqlite3_column_int64(stmt, 5);
    copy_sql_text(stmt, 6, job->payload_json, sizeof(job->payload_json));
    copy_sql_text(stmt, 7, job->result_json, sizeof(job->result_json));
    copy_sql_text(stmt, 8, job->error, sizeof(job->error));
    job->created_at = (time_t)sqlite3_column_int64(stmt, 9);
    job->updated_at = (time_t)sqlite3_column_int64(stmt, 10);
}

static void row_to_face_crop(sqlite3_stmt *stmt, event_face_crop_t *crop) {
    crop->id = (uint64_t)sqlite3_column_int64(stmt, 0);
    crop->event_id = (uint64_t)sqlite3_column_int64(stmt, 1);
    copy_sql_text(stmt, 2, crop->stream_name, sizeof(crop->stream_name));
    crop->event_time = (time_t)sqlite3_column_int64(stmt, 3);
    copy_sql_text(stmt, 4, crop->crop_path, sizeof(crop->crop_path));
    crop->bbox_x = (float)sqlite3_column_double(stmt, 5);
    crop->bbox_y = (float)sqlite3_column_double(stmt, 6);
    crop->bbox_w = (float)sqlite3_column_double(stmt, 7);
    crop->bbox_h = (float)sqlite3_column_double(stmt, 8);
    crop->confidence = (float)sqlite3_column_double(stmt, 9);
    copy_sql_text(stmt, 10, crop->name, sizeof(crop->name));
    copy_sql_text(stmt, 11, crop->status, sizeof(crop->status));
    copy_sql_text(stmt, 12, crop->source, sizeof(crop->source));
    crop->created_at = (time_t)sqlite3_column_int64(stmt, 13);
    crop->updated_at = (time_t)sqlite3_column_int64(stmt, 14);
}

static uint64_t find_active_event(sqlite3 *db,
                                  const char *stream_name,
                                  const detection_t *detection,
                                  time_t timestamp,
                                  float *best_confidence) {
    sqlite3_stmt *stmt = NULL;
    uint64_t event_id = 0;
    const char *zone_id = detection->zone_id[0] ? detection->zone_id : "";

    const char *sql =
        "SELECT id, best_confidence "
        "FROM detection_events "
        "WHERE stream_name = ? AND label = ? AND status = 'active' "
        "  AND end_time >= ? "
        "  AND ((? >= 0 AND track_id = ?) OR (? < 0 AND track_id < 0 AND COALESCE(zone_id, '') = ?)) "
        "ORDER BY updated_at DESC LIMIT 1;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare active event lookup: %s", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, stream_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, detection->label, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)(timestamp - EVENT_ACTIVE_GAP_SECONDS));
    sqlite3_bind_int(stmt, 4, detection->track_id);
    sqlite3_bind_int(stmt, 5, detection->track_id);
    sqlite3_bind_int(stmt, 6, detection->track_id);
    sqlite3_bind_text(stmt, 7, zone_id, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        event_id = (uint64_t)sqlite3_column_int64(stmt, 0);
        if (best_confidence) {
            *best_confidence = (float)sqlite3_column_double(stmt, 1);
        }
    }

    sqlite3_finalize(stmt);
    return event_id;
}

static void close_stale_events_locked(sqlite3 *db, time_t timestamp) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE detection_events "
        "SET status = 'ended', updated_at = strftime('%s', 'now') "
        "WHERE status = 'active' AND end_time < ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare stale detection event close: %s", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)(timestamp - EVENT_ACTIVE_GAP_SECONDS));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        log_error("Failed to close stale detection events: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
}

static uint64_t insert_event(sqlite3 *db,
                             const char *stream_name,
                             const detection_t *detection,
                             time_t timestamp,
                             uint64_t recording_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO detection_events "
        "(stream_name, label, start_time, end_time, best_time, best_confidence, recording_id, track_id, zone_id, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'active');";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare detection event insert: %s", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, stream_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, detection->label, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)timestamp);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)timestamp);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)timestamp);
    sqlite3_bind_double(stmt, 6, detection->confidence);
    if (recording_id > 0) {
        sqlite3_bind_int64(stmt, 7, (sqlite3_int64)recording_id);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    sqlite3_bind_int(stmt, 8, detection->track_id);
    sqlite3_bind_text(stmt, 9, detection->zone_id, -1, SQLITE_STATIC);

    uint64_t event_id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        event_id = (uint64_t)sqlite3_last_insert_rowid(db);
    } else {
        log_error("Failed to insert detection event: %s", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    return event_id;
}

static int update_event(sqlite3 *db,
                        uint64_t event_id,
                        const detection_t *detection,
                        time_t timestamp,
                        uint64_t recording_id,
                        float previous_best) {
    sqlite3_stmt *stmt = NULL;
    const bool is_best = detection->confidence >= previous_best;
    const char *sql =
        "UPDATE detection_events "
        "SET end_time = ?, "
        "    best_time = CASE WHEN ? THEN ? ELSE best_time END, "
        "    best_confidence = CASE WHEN ? THEN ? ELSE best_confidence END, "
        "    recording_id = CASE WHEN ? > 0 THEN ? ELSE recording_id END, "
        "    zone_id = CASE WHEN COALESCE(zone_id, '') = '' THEN ? ELSE zone_id END, "
        "    updated_at = strftime('%s', 'now') "
        "WHERE id = ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare detection event update: %s", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)timestamp);
    sqlite3_bind_int(stmt, 2, is_best ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)timestamp);
    sqlite3_bind_int(stmt, 4, is_best ? 1 : 0);
    sqlite3_bind_double(stmt, 5, detection->confidence);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)recording_id);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)recording_id);
    sqlite3_bind_text(stmt, 8, detection->zone_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)event_id);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    if (rc != 0) {
        log_error("Failed to update detection event: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    return rc;
}

static uint64_t enqueue_job_locked(sqlite3 *db,
                                   uint64_t event_id,
                                   const char *type,
                                   const char *payload_json) {
    sqlite3_stmt *stmt = NULL;
    uint64_t id = 0;

    const char *exists_sql =
        "SELECT id FROM enrichment_jobs WHERE event_id = ? AND type = ? "
        "AND status IN ('queued', 'running') LIMIT 1;";
    if (sqlite3_prepare_v2(db, exists_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
        sqlite3_bind_text(stmt, 2, type, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = (uint64_t)sqlite3_column_int64(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (id == 0) {
        const char *insert_sql =
            "INSERT INTO enrichment_jobs (event_id, type, payload_json) VALUES (?, ?, ?);";
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
            sqlite3_bind_text(stmt, 2, type, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, payload_json ? payload_json : "", -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                id = (uint64_t)sqlite3_last_insert_rowid(db);
            }
        }
    }
    sqlite3_finalize(stmt);
    return id;
}

static void apply_enrichment_to_event_locked(sqlite3 *db,
                                             const enrichment_job_t *job,
                                             const char *value) {
    if (!job || !value || value[0] == '\0') {
        return;
    }

    const char *field = NULL;
    if (strcmp(job->type, "face_recognition") == 0) {
        field = "sub_label";
    } else if (strcmp(job->type, "license_plate") == 0) {
        field = "recognized_text";
    }
    if (!field) {
        return;
    }

    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE detection_events SET %s = ?, updated_at = strftime('%%s', 'now') WHERE id = ?;",
             field);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, value, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)job->event_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void enqueue_default_jobs_locked(sqlite3 *db, uint64_t event_id, const detection_t *detection) {
    char base_payload[256];
    snprintf(base_payload, sizeof(base_payload), "{\"label\":\"%s\",\"confidence\":%.3f}",
             detection->label, detection->confidence);

    if (g_config.genai_enabled) {
        char genai_payload[1536];
        snprintf(genai_payload, sizeof(genai_payload),
                 "{\"label\":\"%s\",\"confidence\":%.3f,"
                 "\"provider\":\"%s\",\"api_url\":\"%s\",\"model\":\"%s\",\"api_key_env\":\"%s\"}",
                 detection->label,
                 detection->confidence,
                 g_config.genai_provider,
                 g_config.genai_api_url,
                 g_config.genai_model,
                 g_config.genai_api_key_env);
        enqueue_job_locked(db, event_id, "genai_description", genai_payload);
    }

    if (g_config.face_recognition_enabled && strcmp(detection->label, "person") == 0) {
        enqueue_job_locked(db, event_id, "face_recognition", base_payload);
    }
}

int db_detection_events_observe(const char *stream_name,
                                const detection_result_t *result,
                                time_t timestamp,
                                uint64_t recording_id) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();

    if (!db || !db_mutex || !stream_name || !result) {
        return -1;
    }
    if (timestamp == 0) {
        timestamp = time(NULL);
    }

    pthread_mutex_lock(db_mutex);
    int observed = 0;
    close_stale_events_locked(db, timestamp);

    for (int i = 0; i < result->count; i++) {
        const detection_t *det = &result->detections[i];
        if (det->label[0] == '\0') {
            continue;
        }

        float previous_best = 0.0f;
        uint64_t event_id = find_active_event(db, stream_name, det, timestamp, &previous_best);
        bool created = false;

        if (event_id == 0) {
            event_id = insert_event(db, stream_name, det, timestamp, recording_id);
            created = event_id != 0;
        } else if (update_event(db, event_id, det, timestamp, recording_id, previous_best) != 0) {
            continue;
        }

        if (event_id != 0) {
            observed++;
            if (created) {
                enqueue_default_jobs_locked(db, event_id, det);
            }
        }
    }

    pthread_mutex_unlock(db_mutex);
    return observed;
}

int db_detection_events_list(const char *stream_name,
                             const char *label,
                             const char *status,
                             time_t start_time,
                             time_t end_time,
                             detection_event_t *events,
                             int max_events) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (!db || !db_mutex || !events || max_events <= 0) {
        return -1;
    }

    char sql[1024];
    safe_strcpy(sql,
        "SELECT id, stream_name, label, sub_label, recognized_text, start_time, end_time, best_time, "
        "best_confidence, COALESCE(best_detection_id, 0), COALESCE(recording_id, 0), track_id, "
        "zone_id, thumbnail_path, status, created_at, updated_at "
        "FROM detection_events WHERE 1=1",
        sizeof(sql), 0);

    if (stream_name && stream_name[0]) safe_strcat(sql, " AND stream_name = ?", sizeof(sql));
    if (label && label[0]) safe_strcat(sql, " AND label = ?", sizeof(sql));
    if (status && status[0]) safe_strcat(sql, " AND status = ?", sizeof(sql));
    if (start_time > 0) safe_strcat(sql, " AND COALESCE(end_time, start_time) >= ?", sizeof(sql));
    if (end_time > 0) safe_strcat(sql, " AND start_time <= ?", sizeof(sql));
    safe_strcat(sql, " ORDER BY start_time DESC LIMIT ?;", sizeof(sql));

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare detection events list: %s", sqlite3_errmsg(db));
        pthread_mutex_unlock(db_mutex);
        return -1;
    }

    int idx = 1;
    if (stream_name && stream_name[0]) sqlite3_bind_text(stmt, idx++, stream_name, -1, SQLITE_STATIC);
    if (label && label[0]) sqlite3_bind_text(stmt, idx++, label, -1, SQLITE_STATIC);
    if (status && status[0]) sqlite3_bind_text(stmt, idx++, status, -1, SQLITE_STATIC);
    if (start_time > 0) sqlite3_bind_int64(stmt, idx++, (sqlite3_int64)start_time);
    if (end_time > 0) sqlite3_bind_int64(stmt, idx++, (sqlite3_int64)end_time);
    sqlite3_bind_int(stmt, idx, max_events);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_events) {
        row_to_event(stmt, &events[count++]);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return count;
}

int db_detection_event_get(uint64_t event_id, detection_event_t *event) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || !event || event_id == 0) {
        return -1;
    }

    const char *sql =
        "SELECT id, stream_name, label, sub_label, recognized_text, start_time, end_time, best_time, "
        "best_confidence, COALESCE(best_detection_id, 0), COALESCE(recording_id, 0), track_id, "
        "zone_id, thumbnail_path, status, created_at, updated_at "
        "FROM detection_events WHERE id = ?;";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row_to_event(stmt, event);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return rc;
}

int db_detection_event_set_thumbnail(uint64_t event_id,
                                     const char *thumbnail_path) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || event_id == 0 || !thumbnail_path) {
        return -1;
    }

    const char *sql =
        "UPDATE detection_events "
        "SET thumbnail_path = ?, updated_at = strftime('%s', 'now') "
        "WHERE id = ?;";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare detection event thumbnail update: %s", sqlite3_errmsg(db));
        pthread_mutex_unlock(db_mutex);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, thumbnail_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id);

    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    if (rc != 0) {
        log_error("Failed to update detection event thumbnail: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return rc;
}

int db_detection_event_set_sub_label(uint64_t event_id,
                                     const char *sub_label) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || event_id == 0 || !sub_label) {
        return -1;
    }

    const char *sql =
        "UPDATE detection_events "
        "SET sub_label = ?, updated_at = strftime('%s', 'now') "
        "WHERE id = ?;";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare detection event sub-label update: %s", sqlite3_errmsg(db));
        pthread_mutex_unlock(db_mutex);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, sub_label, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id);

    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    if (rc != 0) {
        log_error("Failed to update detection event sub-label: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return rc;
}

int db_event_enrichments_list(uint64_t event_id,
                              event_enrichment_t *enrichments,
                              int max_enrichments) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (!db || !db_mutex || !enrichments || max_enrichments <= 0 || event_id == 0) {
        return -1;
    }

    const char *sql =
        "SELECT id, event_id, type, provider, status, score, value, json, error, created_at, updated_at "
        "FROM event_enrichments WHERE event_id = ? ORDER BY created_at DESC LIMIT ?;";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
    sqlite3_bind_int(stmt, 2, max_enrichments);
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_enrichments) {
        row_to_enrichment(stmt, &enrichments[count++]);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return count;
}

uint64_t db_event_enrichment_add(uint64_t event_id,
                                 const char *type,
                                 const char *provider,
                                 const char *status,
                                 float score,
                                 const char *value,
                                 const char *json,
                                 const char *error) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    uint64_t id = 0;

    if (!db || !db_mutex || event_id == 0 || !type || !provider) {
        return 0;
    }

    const char *sql =
        "INSERT INTO event_enrichments (event_id, type, provider, status, score, value, json, error) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
        sqlite3_bind_text(stmt, 2, type, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, provider, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, status ? status : "pending", -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 5, score);
        sqlite3_bind_text(stmt, 6, value ? value : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, json ? json : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 8, error ? error : "", -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            id = (uint64_t)sqlite3_last_insert_rowid(db);
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return id;
}

uint64_t db_enrichment_job_enqueue(uint64_t event_id,
                                   const char *type,
                                   const char *payload_json) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    uint64_t id = 0;

    if (!db || !db_mutex || event_id == 0 || !type || type[0] == '\0') {
        return 0;
    }

    pthread_mutex_lock(db_mutex);
    id = enqueue_job_locked(db, event_id, type, payload_json);
    pthread_mutex_unlock(db_mutex);
    return id;
}

int db_enrichment_jobs_list(const char *status,
                            const char *type,
                            enrichment_job_t *jobs,
                            int max_jobs) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (!db || !db_mutex || !jobs || max_jobs <= 0) {
        return -1;
    }

    char sql[768];
    safe_strcpy(sql,
        "SELECT id, event_id, type, status, attempts, next_attempt_at, payload_json, "
        "result_json, error, created_at, updated_at "
        "FROM enrichment_jobs WHERE 1=1",
        sizeof(sql), 0);
    if (status && status[0]) safe_strcat(sql, " AND status = ?", sizeof(sql));
    if (type && type[0]) safe_strcat(sql, " AND type = ?", sizeof(sql));
    if (!status || strcmp(status, "queued") == 0) {
        safe_strcat(sql, " AND next_attempt_at <= strftime('%s', 'now')", sizeof(sql));
    }
    safe_strcat(sql, " ORDER BY next_attempt_at ASC, id ASC LIMIT ?;", sizeof(sql));

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_error("Failed to prepare enrichment jobs list: %s", sqlite3_errmsg(db));
        pthread_mutex_unlock(db_mutex);
        return -1;
    }

    int idx = 1;
    if (status && status[0]) sqlite3_bind_text(stmt, idx++, status, -1, SQLITE_STATIC);
    if (type && type[0]) sqlite3_bind_text(stmt, idx++, type, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, idx, max_jobs);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_jobs) {
        row_to_job(stmt, &jobs[count++]);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return count;
}

static int get_job_locked(sqlite3 *db, uint64_t job_id, enrichment_job_t *job) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT id, event_id, type, status, attempts, next_attempt_at, payload_json, "
        "result_json, error, created_at, updated_at "
        "FROM enrichment_jobs WHERE id = ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)job_id);

    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row_to_job(stmt, job);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    return rc;
}

int db_enrichment_job_claim(uint64_t job_id, enrichment_job_t *job) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || job_id == 0 || !job) {
        return -1;
    }

    const char *sql =
        "UPDATE enrichment_jobs "
        "SET status = 'running', attempts = attempts + 1, updated_at = strftime('%s', 'now') "
        "WHERE id = ? AND status = 'queued' AND next_attempt_at <= strftime('%s', 'now');";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)job_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(stmt);

    if (rc == 0) {
        rc = get_job_locked(db, job_id, job);
    }
    pthread_mutex_unlock(db_mutex);
    return rc;
}

int db_enrichment_job_complete(uint64_t job_id,
                               const char *provider,
                               float score,
                               const char *value,
                               const char *json,
                               const char *error) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    enrichment_job_t job;

    if (!db || !db_mutex || job_id == 0 || !provider) {
        return -1;
    }

    pthread_mutex_lock(db_mutex);
    if (get_job_locked(db, job_id, &job) != 0) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    pthread_mutex_unlock(db_mutex);

    uint64_t enrichment_id = db_event_enrichment_add(job.event_id,
                                                     job.type,
                                                     provider,
                                                     error && error[0] ? "failed" : "completed",
                                                     score,
                                                     value ? value : "",
                                                     json ? json : "",
                                                     error ? error : "");
    if (enrichment_id == 0) {
        return -1;
    }

    const char *sql =
        "UPDATE enrichment_jobs "
        "SET status = 'completed', result_json = ?, error = ?, updated_at = strftime('%s', 'now') "
        "WHERE id = ?;";
    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, json ? json : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, error ? error : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)job_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    if (rc == 0 && (!error || error[0] == '\0')) {
        apply_enrichment_to_event_locked(db, &job, value);
    }
    pthread_mutex_unlock(db_mutex);
    return rc;
}

int db_enrichment_job_fail(uint64_t job_id,
                           const char *error,
                           int retry_after_seconds) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || job_id == 0) {
        return -1;
    }

    const char *status = retry_after_seconds > 0 ? "queued" : "failed";
    const char *sql =
        "UPDATE enrichment_jobs "
        "SET status = ?, error = ?, next_attempt_at = strftime('%s', 'now') + ?, "
        "    updated_at = strftime('%s', 'now') "
        "WHERE id = ? AND status IN ('queued', 'running');";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, error ? error : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, retry_after_seconds > 0 ? retry_after_seconds : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)job_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return rc;
}

uint64_t db_event_face_crop_add(uint64_t event_id,
                                const char *crop_path,
                                float bbox_x,
                                float bbox_y,
                                float bbox_w,
                                float bbox_h,
                                float confidence,
                                const char *status,
                                const char *source) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    uint64_t id = 0;

    if (!db || !db_mutex || event_id == 0 || !crop_path || crop_path[0] == '\0') {
        return 0;
    }

    const char *sql =
        "INSERT INTO event_face_crops "
        "(event_id, crop_path, bbox_x, bbox_y, bbox_w, bbox_h, confidence, status, source) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
        sqlite3_bind_text(stmt, 2, crop_path, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 3, bbox_x);
        sqlite3_bind_double(stmt, 4, bbox_y);
        sqlite3_bind_double(stmt, 5, bbox_w);
        sqlite3_bind_double(stmt, 6, bbox_h);
        sqlite3_bind_double(stmt, 7, confidence);
        sqlite3_bind_text(stmt, 8, status && status[0] ? status : "unknown", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 9, source && source[0] ? source : "light-object-detect", -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            id = (uint64_t)sqlite3_last_insert_rowid(db);
        } else {
            log_error("Failed to insert event face crop: %s", sqlite3_errmsg(db));
        }
    } else {
        log_error("Failed to prepare event face crop insert: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return id;
}

static int face_crops_select(sqlite3 *db,
                             sqlite3_stmt **stmt,
                             const char *where_clause) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT c.id, c.event_id, e.stream_name, COALESCE(e.best_time, e.start_time), "
             "c.crop_path, c.bbox_x, c.bbox_y, c.bbox_w, c.bbox_h, c.confidence, "
             "c.name, c.status, c.source, c.created_at, c.updated_at "
             "FROM event_face_crops c "
             "JOIN detection_events e ON e.id = c.event_id "
             "%s "
             "ORDER BY c.created_at DESC LIMIT ?;",
             where_clause ? where_clause : "");
    return sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
}

int db_event_face_crop_get(uint64_t crop_id,
                           event_face_crop_t *crop) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || crop_id == 0 || !crop) {
        return -1;
    }

    pthread_mutex_lock(db_mutex);
    if (face_crops_select(db, &stmt, "WHERE c.id = ?") != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)crop_id);
    sqlite3_bind_int(stmt, 2, 1);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row_to_face_crop(stmt, crop);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return rc;
}

int db_event_face_crops_list(const char *status,
                             event_face_crop_t *crops,
                             int max_crops) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (!db || !db_mutex || !crops || max_crops <= 0) {
        return -1;
    }

    pthread_mutex_lock(db_mutex);
    const char *where_clause = status && status[0] ? "WHERE c.status = ?" : "";
    if (face_crops_select(db, &stmt, where_clause) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    int idx = 1;
    if (status && status[0]) {
        sqlite3_bind_text(stmt, idx++, status, -1, SQLITE_STATIC);
    }
    sqlite3_bind_int(stmt, idx, max_crops);
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_crops) {
        row_to_face_crop(stmt, &crops[count++]);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return count;
}

int db_event_face_crops_list_for_event(uint64_t event_id,
                                       event_face_crop_t *crops,
                                       int max_crops) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (!db || !db_mutex || event_id == 0 || !crops || max_crops <= 0) {
        return -1;
    }

    pthread_mutex_lock(db_mutex);
    if (face_crops_select(db, &stmt, "WHERE c.event_id = ?") != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id);
    sqlite3_bind_int(stmt, 2, max_crops);
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_crops) {
        row_to_face_crop(stmt, &crops[count++]);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return count;
}

int db_event_face_crop_mark_named(uint64_t crop_id,
                                  const char *name) {
    sqlite3 *db = get_db_handle();
    pthread_mutex_t *db_mutex = get_db_mutex();
    sqlite3_stmt *stmt = NULL;

    if (!db || !db_mutex || crop_id == 0 || !name || name[0] == '\0') {
        return -1;
    }

    const char *sql =
        "UPDATE event_face_crops "
        "SET name = ?, status = 'named', updated_at = strftime('%s', 'now') "
        "WHERE id = ?;";

    pthread_mutex_lock(db_mutex);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(db_mutex);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)crop_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(db_mutex);
    return rc;
}
