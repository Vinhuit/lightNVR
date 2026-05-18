#ifndef LIGHTNVR_DB_DETECTION_EVENTS_H
#define LIGHTNVR_DB_DETECTION_EVENTS_H

#include <stdint.h>
#include <time.h>

#include "core/config.h"
#include "video/detection_result.h"

#define MAX_DETECTION_EVENTS 128
#define MAX_EVENT_LABEL_LENGTH 64
#define MAX_EVENT_SUB_LABEL_LENGTH 128
#define MAX_EVENT_TEXT_LENGTH 128
#define MAX_EVENT_STATUS_LENGTH 16
#define MAX_EVENT_THUMBNAIL_PATH 256
#define MAX_EVENT_ENRICHMENTS 64
#define MAX_EVENT_ENRICHMENT_TYPE 32
#define MAX_EVENT_ENRICHMENT_PROVIDER 64
#define MAX_EVENT_ENRICHMENT_VALUE 256
#define MAX_EVENT_ENRICHMENT_JSON 2048
#define MAX_EVENT_ENRICHMENT_ERROR 512
#define MAX_ENRICHMENT_JOBS 64
#define MAX_ENRICHMENT_JOB_PAYLOAD 2048
#define MAX_EVENT_FACE_CROPS 128
#define MAX_FACE_CROP_STATUS_LENGTH 16
#define MAX_FACE_CROP_SOURCE_LENGTH 64

typedef struct {
    uint64_t id;
    char stream_name[MAX_STREAM_NAME];
    char label[MAX_EVENT_LABEL_LENGTH];
    char sub_label[MAX_EVENT_SUB_LABEL_LENGTH];
    char recognized_text[MAX_EVENT_TEXT_LENGTH];
    time_t start_time;
    time_t end_time;
    time_t best_time;
    float best_confidence;
    uint64_t best_detection_id;
    uint64_t recording_id;
    int track_id;
    char zone_id[MAX_ZONE_ID_LENGTH];
    char thumbnail_path[MAX_EVENT_THUMBNAIL_PATH];
    char status[MAX_EVENT_STATUS_LENGTH];
    time_t created_at;
    time_t updated_at;
} detection_event_t;

typedef struct {
    uint64_t id;
    uint64_t event_id;
    char type[MAX_EVENT_ENRICHMENT_TYPE];
    char provider[MAX_EVENT_ENRICHMENT_PROVIDER];
    char status[MAX_EVENT_STATUS_LENGTH];
    float score;
    char value[MAX_EVENT_ENRICHMENT_VALUE];
    char json[MAX_EVENT_ENRICHMENT_JSON];
    char error[MAX_EVENT_ENRICHMENT_ERROR];
    time_t created_at;
    time_t updated_at;
} event_enrichment_t;

typedef struct {
    uint64_t id;
    uint64_t event_id;
    char type[MAX_EVENT_ENRICHMENT_TYPE];
    char status[MAX_EVENT_STATUS_LENGTH];
    int attempts;
    time_t next_attempt_at;
    char payload_json[MAX_ENRICHMENT_JOB_PAYLOAD];
    char result_json[MAX_EVENT_ENRICHMENT_JSON];
    char error[MAX_EVENT_ENRICHMENT_ERROR];
    time_t created_at;
    time_t updated_at;
} enrichment_job_t;

typedef struct {
    uint64_t id;
    uint64_t event_id;
    char stream_name[MAX_STREAM_NAME];
    time_t event_time;
    char crop_path[MAX_EVENT_THUMBNAIL_PATH];
    float bbox_x;
    float bbox_y;
    float bbox_w;
    float bbox_h;
    float confidence;
    char name[MAX_EVENT_SUB_LABEL_LENGTH];
    char status[MAX_FACE_CROP_STATUS_LENGTH];
    char source[MAX_FACE_CROP_SOURCE_LENGTH];
    time_t created_at;
    time_t updated_at;
} event_face_crop_t;

int db_detection_events_observe(const char *stream_name,
                                const detection_result_t *result,
                                time_t timestamp,
                                uint64_t recording_id);

int db_detection_events_list(const char *stream_name,
                             const char *label,
                             const char *status,
                             time_t start_time,
                             time_t end_time,
                             detection_event_t *events,
                             int max_events);

int db_detection_event_get(uint64_t event_id, detection_event_t *event);

int db_detection_event_get_best_detection_box(const detection_event_t *event,
                                              detection_t *detection);

int db_detection_event_set_thumbnail(uint64_t event_id,
                                     const char *thumbnail_path);

int db_detection_event_set_sub_label(uint64_t event_id,
                                     const char *sub_label);

int db_event_enrichments_list(uint64_t event_id,
                              event_enrichment_t *enrichments,
                              int max_enrichments);

uint64_t db_event_enrichment_add(uint64_t event_id,
                                 const char *type,
                                 const char *provider,
                                 const char *status,
                                 float score,
                                 const char *value,
                                 const char *json,
                                 const char *error);

uint64_t db_enrichment_job_enqueue(uint64_t event_id,
                                   const char *type,
                                   const char *payload_json);

int db_enrichment_jobs_list(const char *status,
                            const char *type,
                            enrichment_job_t *jobs,
                            int max_jobs);

int db_enrichment_job_claim(uint64_t job_id, enrichment_job_t *job);

int db_enrichment_job_complete(uint64_t job_id,
                               const char *provider,
                               float score,
                               const char *value,
                               const char *json,
                               const char *error);

int db_enrichment_job_fail(uint64_t job_id,
                           const char *error,
                           int retry_after_seconds);

uint64_t db_event_face_crop_add(uint64_t event_id,
                                const char *crop_path,
                                float bbox_x,
                                float bbox_y,
                                float bbox_w,
                                float bbox_h,
                                float confidence,
                                const char *status,
                                const char *source);

int db_event_face_crop_get(uint64_t crop_id,
                           event_face_crop_t *crop);

int db_event_face_crops_list(const char *status,
                             event_face_crop_t *crops,
                             int max_crops);

int db_event_face_crops_list_for_event(uint64_t event_id,
                                       event_face_crop_t *crops,
                                       int max_crops);

int db_event_face_crop_mark_named(uint64_t crop_id,
                                  const char *name);

#endif // LIGHTNVR_DB_DETECTION_EVENTS_H
