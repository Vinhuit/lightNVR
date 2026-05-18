#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE

#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#define LOG_COMPONENT "EventsAPI"
#include "core/logger.h"
#include "database/db_auth.h"
#include "database/db_detection_events.h"
#include "web/api_handlers_events.h"
#include "web/httpd_utils.h"
#include "web/request_response.h"

static int require_viewer(const http_request_t *req, http_response_t *res) {
    if (g_config.web_auth_enabled) {
        user_t user;
        if (!httpd_check_viewer_access(req, &user)) {
            http_response_set_json_error(res, 401, "Unauthorized");
            return 0;
        }
    }
    return 1;
}

static int require_admin(const http_request_t *req, http_response_t *res) {
    if (g_config.web_auth_enabled && !httpd_check_admin_privileges(req, res)) {
        return 0;
    }
    return 1;
}

static void add_best_box_to_event_json(cJSON *obj, const detection_event_t *event) {
    detection_t detection;
    if (!obj || !event ||
        db_detection_event_get_best_detection_box(event, &detection) != 0 ||
        detection.width <= 0.0f || detection.height <= 0.0f) {
        return;
    }

    cJSON *box = cJSON_CreateObject();
    if (!box) {
        return;
    }

    cJSON_AddStringToObject(box, "label", detection.label);
    cJSON_AddNumberToObject(box, "confidence", detection.confidence);
    cJSON_AddNumberToObject(box, "x", detection.x);
    cJSON_AddNumberToObject(box, "y", detection.y);
    cJSON_AddNumberToObject(box, "width", detection.width);
    cJSON_AddNumberToObject(box, "height", detection.height);
    cJSON_AddNumberToObject(box, "track_id", detection.track_id);
    if (detection.zone_id[0] != '\0') {
        cJSON_AddStringToObject(box, "zone_id", detection.zone_id);
    }
    cJSON_AddItemToObject(obj, "best_box", box);
}

static cJSON *event_to_json(const detection_event_t *event) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id", (double)event->id);
    cJSON_AddStringToObject(obj, "stream_name", event->stream_name);
    cJSON_AddStringToObject(obj, "label", event->label);
    cJSON_AddStringToObject(obj, "sub_label", event->sub_label);
    cJSON_AddStringToObject(obj, "recognized_text", event->recognized_text);
    cJSON_AddNumberToObject(obj, "start_time", (double)event->start_time);
    cJSON_AddNumberToObject(obj, "end_time", (double)event->end_time);
    cJSON_AddNumberToObject(obj, "best_time", (double)event->best_time);
    cJSON_AddNumberToObject(obj, "best_confidence", event->best_confidence);
    cJSON_AddNumberToObject(obj, "best_detection_id", (double)event->best_detection_id);
    cJSON_AddNumberToObject(obj, "recording_id", (double)event->recording_id);
    cJSON_AddNumberToObject(obj, "track_id", event->track_id);
    cJSON_AddStringToObject(obj, "zone_id", event->zone_id);
    cJSON_AddStringToObject(obj, "thumbnail_path", event->thumbnail_path);
    if (event->thumbnail_path[0] != '\0') {
        char snapshot_url[128];
        snprintf(snapshot_url, sizeof(snapshot_url),
                 "/api/events/%llu/snapshot",
                 (unsigned long long)event->id);
        cJSON_AddStringToObject(obj, "snapshot_url", snapshot_url);
    } else {
        cJSON_AddStringToObject(obj, "snapshot_url", "");
    }
    cJSON_AddStringToObject(obj, "status", event->status);
    cJSON_AddNumberToObject(obj, "created_at", (double)event->created_at);
    cJSON_AddNumberToObject(obj, "updated_at", (double)event->updated_at);
    add_best_box_to_event_json(obj, event);

    return obj;
}

static cJSON *enrichment_to_json(const event_enrichment_t *enrichment) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id", (double)enrichment->id);
    cJSON_AddNumberToObject(obj, "event_id", (double)enrichment->event_id);
    cJSON_AddStringToObject(obj, "type", enrichment->type);
    cJSON_AddStringToObject(obj, "provider", enrichment->provider);
    cJSON_AddStringToObject(obj, "status", enrichment->status);
    cJSON_AddNumberToObject(obj, "score", enrichment->score);
    cJSON_AddStringToObject(obj, "value", enrichment->value);
    cJSON_AddStringToObject(obj, "json", enrichment->json);
    cJSON_AddStringToObject(obj, "error", enrichment->error);
    cJSON_AddNumberToObject(obj, "created_at", (double)enrichment->created_at);
    cJSON_AddNumberToObject(obj, "updated_at", (double)enrichment->updated_at);

    return obj;
}

static cJSON *job_to_json(const enrichment_job_t *job) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id", (double)job->id);
    cJSON_AddNumberToObject(obj, "event_id", (double)job->event_id);
    cJSON_AddStringToObject(obj, "type", job->type);
    cJSON_AddStringToObject(obj, "status", job->status);
    cJSON_AddNumberToObject(obj, "attempts", job->attempts);
    cJSON_AddNumberToObject(obj, "next_attempt_at", (double)job->next_attempt_at);
    cJSON_AddStringToObject(obj, "payload_json", job->payload_json);
    cJSON_AddStringToObject(obj, "result_json", job->result_json);
    cJSON_AddStringToObject(obj, "error", job->error);
    cJSON_AddNumberToObject(obj, "created_at", (double)job->created_at);
    cJSON_AddNumberToObject(obj, "updated_at", (double)job->updated_at);

    return obj;
}

static uint64_t extract_event_id(const http_request_t *req, const char *suffix) {
    char id_buf[64] = {0};
    if (http_request_extract_path_param(req, "/api/events/", id_buf, sizeof(id_buf)) != 0) {
        return 0;
    }
    if (suffix) {
        char *suffix_pos = strstr(id_buf, suffix);
        if (suffix_pos) *suffix_pos = '\0';
    }
    return strtoull(id_buf, NULL, 10);
}

static uint64_t extract_job_id(const http_request_t *req, const char *suffix) {
    char id_buf[64] = {0};
    if (http_request_extract_path_param(req, "/api/enrichment/jobs/", id_buf, sizeof(id_buf)) != 0) {
        return 0;
    }
    if (suffix) {
        char *suffix_pos = strstr(id_buf, suffix);
        if (suffix_pos) *suffix_pos = '\0';
    }
    return strtoull(id_buf, NULL, 10);
}

static char *copy_json_payload(cJSON *json_payload) {
    if (!json_payload) {
        return NULL;
    }

    if (cJSON_IsString(json_payload)) {
        size_t len = strlen(json_payload->valuestring);
        char *copy = malloc(len + 1);
        if (!copy) {
            return NULL;
        }
        memcpy(copy, json_payload->valuestring, len + 1);
        return copy;
    }

    return cJSON_PrintUnformatted(json_payload);
}

void handle_get_events(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    char stream[MAX_STREAM_NAME] = {0};
    char label[MAX_EVENT_LABEL_LENGTH] = {0};
    char status[MAX_EVENT_STATUS_LENGTH] = {0};
    char start_buf[32] = {0};
    char end_buf[32] = {0};
    char limit_buf[16] = {0};

    http_request_get_query_param(req, "stream", stream, sizeof(stream));
    http_request_get_query_param(req, "label", label, sizeof(label));
    http_request_get_query_param(req, "status", status, sizeof(status));
    http_request_get_query_param(req, "start", start_buf, sizeof(start_buf));
    http_request_get_query_param(req, "end", end_buf, sizeof(end_buf));
    http_request_get_query_param(req, "limit", limit_buf, sizeof(limit_buf));

    int limit = limit_buf[0] ? atoi(limit_buf) : 100;
    if (limit <= 0 || limit > MAX_DETECTION_EVENTS) limit = MAX_DETECTION_EVENTS;

    detection_event_t events[MAX_DETECTION_EVENTS];
    int count = db_detection_events_list(stream[0] ? stream : NULL,
                                         label[0] ? label : NULL,
                                         status[0] ? status : NULL,
                                         start_buf[0] ? (time_t)strtoll(start_buf, NULL, 10) : 0,
                                         end_buf[0] ? (time_t)strtoll(end_buf, NULL, 10) : 0,
                                         events,
                                         limit);
    if (count < 0) {
        http_response_set_json_error(res, 500, "Failed to list events");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, event_to_json(&events[i]));
    }
    cJSON_AddItemToObject(root, "events", arr);
    cJSON_AddNumberToObject(root, "count", count);

    char *json = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 200, json);
    free(json);
    cJSON_Delete(root);
}

void handle_get_event(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    uint64_t event_id = extract_event_id(req, NULL);
    if (event_id == 0) {
        http_response_set_json_error(res, 400, "Invalid event ID");
        return;
    }

    detection_event_t event;
    if (db_detection_event_get(event_id, &event) != 0) {
        http_response_set_json_error(res, 404, "Event not found");
        return;
    }

    cJSON *root = event_to_json(&event);
    char *json = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 200, json);
    free(json);
    cJSON_Delete(root);
}

static int is_safe_event_thumbnail_path(const char *path) {
    if (!path || path[0] == '\0' || path[0] == '/' || path[0] == '\\') {
        return 0;
    }
    if (strstr(path, "..") || strchr(path, '\\')) {
        return 0;
    }
    return 1;
}

void handle_get_event_snapshot(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    uint64_t event_id = extract_event_id(req, "/snapshot");
    if (event_id == 0) {
        http_response_set_json_error(res, 400, "Invalid event ID");
        return;
    }

    detection_event_t event;
    if (db_detection_event_get(event_id, &event) != 0) {
        http_response_set_json_error(res, 404, "Event not found");
        return;
    }

    if (!is_safe_event_thumbnail_path(event.thumbnail_path)) {
        http_response_set_json_error(res, 404, "Event snapshot not found");
        return;
    }

    char full_path[MAX_PATH_LENGTH];
    snprintf(full_path, sizeof(full_path), "%s/%s", g_config.storage_path, event.thumbnail_path);
    if (http_serve_file(req, res, full_path, "image/jpeg",
                        "Cache-Control: public, max-age=86400\r\n") != 0) {
        http_response_set_json_error(res, 404, "Event snapshot not found");
    }
}

void handle_get_event_enrichments(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    uint64_t event_id = extract_event_id(req, "/enrichments");
    if (event_id == 0) {
        http_response_set_json_error(res, 400, "Invalid event ID");
        return;
    }

    event_enrichment_t enrichments[MAX_EVENT_ENRICHMENTS];
    int count = db_event_enrichments_list(event_id, enrichments, MAX_EVENT_ENRICHMENTS);
    if (count < 0) {
        http_response_set_json_error(res, 500, "Failed to list enrichments");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, enrichment_to_json(&enrichments[i]));
    }
    cJSON_AddNumberToObject(root, "event_id", (double)event_id);
    cJSON_AddItemToObject(root, "enrichments", arr);
    cJSON_AddNumberToObject(root, "count", count);

    char *json = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 200, json);
    free(json);
    cJSON_Delete(root);
}

void handle_post_event_enrichment(const http_request_t *req, http_response_t *res) {
    if (!require_admin(req, res)) return;

    uint64_t event_id = extract_event_id(req, "/enrichments");
    if (event_id == 0) {
        http_response_set_json_error(res, 400, "Invalid event ID");
        return;
    }

    cJSON *body = httpd_parse_json_body(req);
    if (!body) {
        http_response_set_json_error(res, 400, "Invalid JSON body");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(body, "type");
    cJSON *provider = cJSON_GetObjectItem(body, "provider");
    cJSON *status = cJSON_GetObjectItem(body, "status");
    cJSON *score = cJSON_GetObjectItem(body, "score");
    cJSON *value = cJSON_GetObjectItem(body, "value");
    cJSON *json_payload = cJSON_GetObjectItem(body, "json");
    cJSON *error = cJSON_GetObjectItem(body, "error");

    if (!cJSON_IsString(type) || !cJSON_IsString(provider)) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 400, "Fields 'type' and 'provider' are required");
        return;
    }

    char *json_str = copy_json_payload(json_payload);

    uint64_t id = db_event_enrichment_add(event_id,
                                          type->valuestring,
                                          provider->valuestring,
                                          cJSON_IsString(status) ? status->valuestring : "completed",
                                          cJSON_IsNumber(score) ? (float)score->valuedouble : 0.0f,
                                          cJSON_IsString(value) ? value->valuestring : "",
                                          json_str ? json_str : "",
                                          cJSON_IsString(error) ? error->valuestring : "");
    free(json_str);
    cJSON_Delete(body);

    if (id == 0) {
        http_response_set_json_error(res, 500, "Failed to add enrichment");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddNumberToObject(root, "event_id", (double)event_id);
    char *response = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 201, response);
    free(response);
    cJSON_Delete(root);
}

void handle_get_enrichment_jobs(const http_request_t *req, http_response_t *res) {
    if (!require_admin(req, res)) return;

    char status[MAX_EVENT_STATUS_LENGTH] = "queued";
    char type[MAX_EVENT_ENRICHMENT_TYPE] = {0};
    char limit_buf[16] = {0};

    http_request_get_query_param(req, "status", status, sizeof(status));
    http_request_get_query_param(req, "type", type, sizeof(type));
    http_request_get_query_param(req, "limit", limit_buf, sizeof(limit_buf));

    int limit = limit_buf[0] ? atoi(limit_buf) : MAX_ENRICHMENT_JOBS;
    if (limit <= 0 || limit > MAX_ENRICHMENT_JOBS) limit = MAX_ENRICHMENT_JOBS;

    enrichment_job_t jobs[MAX_ENRICHMENT_JOBS];
    int count = db_enrichment_jobs_list(status[0] ? status : "queued",
                                        type[0] ? type : NULL,
                                        jobs,
                                        limit);
    if (count < 0) {
        http_response_set_json_error(res, 500, "Failed to list enrichment jobs");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, job_to_json(&jobs[i]));
    }
    cJSON_AddItemToObject(root, "jobs", arr);
    cJSON_AddNumberToObject(root, "count", count);

    char *json = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 200, json);
    free(json);
    cJSON_Delete(root);
}

void handle_post_enrichment_job_claim(const http_request_t *req, http_response_t *res) {
    if (!require_admin(req, res)) return;

    uint64_t job_id = extract_job_id(req, "/claim");
    if (job_id == 0) {
        http_response_set_json_error(res, 400, "Invalid job ID");
        return;
    }

    enrichment_job_t job;
    if (db_enrichment_job_claim(job_id, &job) != 0) {
        http_response_set_json_error(res, 409, "Job is not available to claim");
        return;
    }

    cJSON *root = job_to_json(&job);
    char *json = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 200, json);
    free(json);
    cJSON_Delete(root);
}

void handle_post_enrichment_job_complete(const http_request_t *req, http_response_t *res) {
    if (!require_admin(req, res)) return;

    uint64_t job_id = extract_job_id(req, "/complete");
    if (job_id == 0) {
        http_response_set_json_error(res, 400, "Invalid job ID");
        return;
    }

    cJSON *body = httpd_parse_json_body(req);
    if (!body) {
        http_response_set_json_error(res, 400, "Invalid JSON body");
        return;
    }

    cJSON *provider = cJSON_GetObjectItem(body, "provider");
    cJSON *score = cJSON_GetObjectItem(body, "score");
    cJSON *value = cJSON_GetObjectItem(body, "value");
    cJSON *json_payload = cJSON_GetObjectItem(body, "json");
    cJSON *error = cJSON_GetObjectItem(body, "error");

    if (!cJSON_IsString(provider)) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 400, "Field 'provider' is required");
        return;
    }

    char *json_str = copy_json_payload(json_payload);
    int rc = db_enrichment_job_complete(job_id,
                                        provider->valuestring,
                                        cJSON_IsNumber(score) ? (float)score->valuedouble : 0.0f,
                                        cJSON_IsString(value) ? value->valuestring : "",
                                        json_str ? json_str : "",
                                        cJSON_IsString(error) ? error->valuestring : "");
    free(json_str);
    cJSON_Delete(body);

    if (rc != 0) {
        http_response_set_json_error(res, 500, "Failed to complete enrichment job");
        return;
    }

    http_response_set_json(res, 200, "{\"success\":true}");
}

void handle_post_enrichment_job_fail(const http_request_t *req, http_response_t *res) {
    if (!require_admin(req, res)) return;

    uint64_t job_id = extract_job_id(req, "/fail");
    if (job_id == 0) {
        http_response_set_json_error(res, 400, "Invalid job ID");
        return;
    }

    cJSON *body = httpd_parse_json_body(req);
    if (!body) {
        http_response_set_json_error(res, 400, "Invalid JSON body");
        return;
    }

    cJSON *error = cJSON_GetObjectItem(body, "error");
    cJSON *retry_after = cJSON_GetObjectItem(body, "retry_after_seconds");
    int retry_after_seconds = cJSON_IsNumber(retry_after) ? retry_after->valueint : 0;

    int rc = db_enrichment_job_fail(job_id,
                                    cJSON_IsString(error) ? error->valuestring : "",
                                    retry_after_seconds);
    cJSON_Delete(body);

    if (rc != 0) {
        http_response_set_json_error(res, 500, "Failed to update enrichment job");
        return;
    }

    http_response_set_json(res, 200, "{\"success\":true}");
}
