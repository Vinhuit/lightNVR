#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#define LOG_COMPONENT "FacesAPI"
#include "core/logger.h"
#include "database/db_detection_events.h"
#include "web/api_handlers_faces.h"
#include "web/httpd_utils.h"
#include "web/request_response.h"

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} face_buf_t;

static size_t face_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t bytes    = size * nmemb;
    face_buf_t *buf = (face_buf_t *)userp;
    if (buf->size + bytes + 1 > buf->capacity) {
        size_t new_cap = buf->capacity + bytes + 4096;
        char  *tmp     = realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data     = tmp;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, contents, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}

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

/* Build the upstream URL for a given sub-path, e.g. "list", "train", "recognize", "123" */
static int build_upstream_url(const char *sub_path, char *out, size_t out_sz) {
    const char *base = g_config.face_recognition_api_url[0]
                       ? g_config.face_recognition_api_url
                       : "http://light-object-detect:8000/api/v1/faces/recognize";

    /* Strip trailing path components after "recognize" or "train" to get base */
    /* We store the /recognize endpoint in config; derive root from it */
    /* e.g. http://host:8000/api/v1/faces/recognize -> http://host:8000/api/v1/faces */
    char api_base[512];
    strncpy(api_base, base, sizeof(api_base) - 1);
    api_base[sizeof(api_base) - 1] = '\0';

    /* Remove last path component if it ends with /recognize or /train */
    char *last_slash = strrchr(api_base, '/');
    if (last_slash && (strcmp(last_slash, "/recognize") == 0 || strcmp(last_slash, "/train") == 0)) {
        *last_slash = '\0';
    }

    int n = snprintf(out, out_sz, "%s/%s", api_base, sub_path);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static int is_safe_relative_snapshot_path(const char *path) {
    if (!path || path[0] == '\0' || path[0] == '/' || path[0] == '\\') {
        return 0;
    }
    if (strstr(path, "..") || strchr(path, '\\')) {
        return 0;
    }
    return 1;
}

static int read_file_bytes(const char *path, unsigned char **data, size_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long len = ftell(fp);
    if (len <= 0 || len > 8 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    rewind(fp);

    unsigned char *buf = malloc((size_t)len);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (nread != (size_t)len) {
        free(buf);
        return -1;
    }

    *data = buf;
    *size = nread;
    return 0;
}

static const char *json_string(cJSON *root, const char *name) {
    cJSON *item = cJSON_GetObjectItem(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static uint64_t json_u64(cJSON *root, const char *name) {
    cJSON *item = cJSON_GetObjectItem(root, name);
    return cJSON_IsNumber(item) ? (uint64_t)item->valuedouble : 0;
}

static cJSON *face_crop_to_json(const event_face_crop_t *crop) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id", (double)crop->id);
    cJSON_AddNumberToObject(obj, "event_id", (double)crop->event_id);
    cJSON_AddStringToObject(obj, "stream_name", crop->stream_name);
    cJSON_AddNumberToObject(obj, "event_time", (double)crop->event_time);
    cJSON_AddStringToObject(obj, "crop_path", crop->crop_path);
    char image_url[128];
    snprintf(image_url, sizeof(image_url),
             "/api/faces/crops/%llu/image",
             (unsigned long long)crop->id);
    cJSON_AddStringToObject(obj, "crop_url", image_url);
    cJSON *bbox = cJSON_AddObjectToObject(obj, "bbox");
    if (bbox) {
        cJSON_AddNumberToObject(bbox, "x", crop->bbox_x);
        cJSON_AddNumberToObject(bbox, "y", crop->bbox_y);
        cJSON_AddNumberToObject(bbox, "width", crop->bbox_w);
        cJSON_AddNumberToObject(bbox, "height", crop->bbox_h);
    }
    cJSON_AddNumberToObject(obj, "confidence", crop->confidence);
    cJSON_AddStringToObject(obj, "name", crop->name);
    cJSON_AddStringToObject(obj, "status", crop->status);
    cJSON_AddStringToObject(obj, "source", crop->source);
    cJSON_AddNumberToObject(obj, "created_at", (double)crop->created_at);
    cJSON_AddNumberToObject(obj, "updated_at", (double)crop->updated_at);
    return obj;
}

static int train_face_bytes(const char *name,
                            const char *filename,
                            const unsigned char *jpeg_data,
                            size_t jpeg_size,
                            long *http_code_out,
                            char **response_out) {
    if (!name || !filename || !jpeg_data || jpeg_size == 0 ||
        !http_code_out || !response_out) {
        return -1;
    }

    char upstream[1024];
    if (build_upstream_url("train", upstream, sizeof(upstream)) != 0) {
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "name");
    curl_mime_data(part, name, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, filename);
    curl_mime_type(part, "image/jpeg");
    curl_mime_data(part, (const char *)jpeg_data, jpeg_size);

    face_buf_t chunk = { .data = malloc(4096), .size = 0, .capacity = 4096 };
    if (!chunk.data) {
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return -1;
    }
    chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, upstream);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, face_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(chunk.data);
        *http_code_out = http_code;
        return -1;
    }

    *http_code_out = http_code;
    *response_out = chunk.data;
    return 0;
}

static bool upstream_train_success(long http_code, const char *response_json) {
    bool ok = http_code >= 200 && http_code < 300;
    if (ok && response_json && response_json[0]) {
        cJSON *json = cJSON_Parse(response_json);
        if (json) {
            cJSON *success = cJSON_GetObjectItem(json, "success");
            if (cJSON_IsFalse(success)) {
                ok = false;
            }
            cJSON_Delete(json);
        }
    }
    return ok;
}

/* ────────────────────────────────────────────────────────────────────────────
 * GET /api/faces/list  →  GET {base}/list
 * Returns the list of known faces from light-object-detect.
 * ────────────────────────────────────────────────────────────────────────── */
void handle_get_faces_list(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    if (!g_config.face_recognition_api_url[0]) {
        http_response_set_json_error(res, 503, "Face recognition API URL is not configured");
        return;
    }

    char upstream[1024];
    if (build_upstream_url("list", upstream, sizeof(upstream)) != 0) {
        http_response_set_json_error(res, 500, "Failed to build upstream URL");
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        http_response_set_json_error(res, 500, "CURL init failed");
        return;
    }

    face_buf_t chunk = { .data = malloc(4096), .size = 0, .capacity = 4096 };
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        http_response_set_json_error(res, 500, "Out of memory");
        return;
    }
    chunk.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, upstream);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, face_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode rc    = curl_easy_perform(curl);
    long      http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(chunk.data);
        http_response_set_json_error(res, 502, curl_easy_strerror(rc));
        return;
    }

    http_response_set_json(res, (int)http_code, chunk.data[0] ? chunk.data : "{\"faces\":[]}");
    free(chunk.data);
}

/* ────────────────────────────────────────────────────────────────────────────
 * POST /api/faces/train  →  POST {base}/train  (multipart passthrough)
 * ────────────────────────────────────────────────────────────────────────── */
void handle_post_faces_train(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    if (!g_config.face_recognition_api_url[0]) {
        http_response_set_json_error(res, 503, "Face recognition API URL is not configured");
        return;
    }

    char upstream[1024];
    if (build_upstream_url("train", upstream, sizeof(upstream)) != 0) {
        http_response_set_json_error(res, 500, "Failed to build upstream URL");
        return;
    }

    /* Get raw body and Content-Type from request to forward as-is */
    size_t body_len = req->body_len;
    const void *body = req->body;
    const char *content_type = http_request_get_header(req, "Content-Type");

    CURL *curl = curl_easy_init();
    if (!curl) {
        http_response_set_json_error(res, 500, "CURL init failed");
        return;
    }

    face_buf_t chunk = { .data = malloc(4096), .size = 0, .capacity = 4096 };
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        http_response_set_json_error(res, 500, "Out of memory");
        return;
    }
    chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    if (content_type && content_type[0]) {
        char ct_header[512];
        snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, ct_header);
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, upstream);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, face_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc    = curl_easy_perform(curl);
    long      http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(chunk.data);
        http_response_set_json_error(res, 502, curl_easy_strerror(rc));
        return;
    }

    http_response_set_json(res, (int)http_code, chunk.data[0] ? chunk.data : "{}");
    free(chunk.data);
}

/* ────────────────────────────────────────────────────────────────────────────
 * POST /api/faces/train-event
 * Trains a face from a saved camera event snapshot.
 * Body: { "event_id": 123, "name": "Alice" }
 * ────────────────────────────────────────────────────────────────────────── */
void handle_post_faces_train_event(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    if (!g_config.face_recognition_api_url[0]) {
        http_response_set_json_error(res, 503, "Face recognition API URL is not configured");
        return;
    }

    cJSON *body = httpd_parse_json_body(req);
    if (!body) {
        http_response_set_json_error(res, 400, "Invalid JSON body");
        return;
    }

    uint64_t event_id = json_u64(body, "event_id");
    const char *name = json_string(body, "name");
    if (event_id == 0 || !name || name[0] == '\0') {
        cJSON_Delete(body);
        http_response_set_json_error(res, 400, "event_id and name are required");
        return;
    }

    detection_event_t event;
    if (db_detection_event_get(event_id, &event) != 0) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 404, "Event not found");
        return;
    }

    if (strcmp(event.label, "person") != 0) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 400, "Only person events can train faces");
        return;
    }

    if (!is_safe_relative_snapshot_path(event.thumbnail_path)) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 404, "Event snapshot not found");
        return;
    }

    char snapshot_path[MAX_PATH_LENGTH];
    snprintf(snapshot_path, sizeof(snapshot_path), "%s/%s", g_config.storage_path, event.thumbnail_path);

    unsigned char *jpeg_data = NULL;
    size_t jpeg_size = 0;
    if (read_file_bytes(snapshot_path, &jpeg_data, &jpeg_size) != 0) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 404, "Failed to read event snapshot");
        return;
    }

    char upstream[1024];
    if (build_upstream_url("train", upstream, sizeof(upstream)) != 0) {
        free(jpeg_data);
        cJSON_Delete(body);
        http_response_set_json_error(res, 500, "Failed to build upstream URL");
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(jpeg_data);
        cJSON_Delete(body);
        http_response_set_json_error(res, 500, "CURL init failed");
        return;
    }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "name");
    curl_mime_data(part, name, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, "event-snapshot.jpg");
    curl_mime_type(part, "image/jpeg");
    curl_mime_data(part, (const char *)jpeg_data, jpeg_size);

    face_buf_t chunk = { .data = malloc(4096), .size = 0, .capacity = 4096 };
    if (!chunk.data) {
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        free(jpeg_data);
        cJSON_Delete(body);
        http_response_set_json_error(res, 500, "Out of memory");
        return;
    }
    chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, upstream);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, face_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    free(jpeg_data);

    if (rc != CURLE_OK) {
        free(chunk.data);
        cJSON_Delete(body);
        http_response_set_json_error(res, 502, curl_easy_strerror(rc));
        return;
    }

    bool train_ok = http_code >= 200 && http_code < 300;
    if (train_ok && chunk.data[0]) {
        cJSON *upstream_json = cJSON_Parse(chunk.data);
        if (upstream_json) {
            cJSON *success = cJSON_GetObjectItem(upstream_json, "success");
            if (cJSON_IsFalse(success)) {
                train_ok = false;
            }
            cJSON_Delete(upstream_json);
        }
    }

    if (train_ok) {
        db_detection_event_set_sub_label(event_id, name);
        db_event_enrichment_add(event_id, "face_recognition", "user", "completed",
                                1.0f, name, chunk.data[0] ? chunk.data : "{}", "");
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "success", train_ok);
    cJSON_AddNumberToObject(out, "event_id", (double)event_id);
    cJSON_AddStringToObject(out, "name", name);
    cJSON_AddNumberToObject(out, "upstream_status", http_code);
    if (chunk.data[0]) {
        cJSON_AddStringToObject(out, "upstream_response", chunk.data);
    }

    char *json = cJSON_PrintUnformatted(out);
    http_response_set_json(res, (int)http_code, json ? json : "{}");

    free(json);
    cJSON_Delete(out);
    free(chunk.data);
    cJSON_Delete(body);
}

/* ────────────────────────────────────────────────────────────────────────────
 * GET /api/faces/unknown-crops
 * Lists stored unknown face crops.
 * ────────────────────────────────────────────────────────────────────────── */
void handle_get_faces_unknown_crops(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    char limit_buf[16] = {0};
    http_request_get_query_param(req, "limit", limit_buf, sizeof(limit_buf));
    int limit = limit_buf[0] ? atoi(limit_buf) : 64;
    if (limit <= 0 || limit > MAX_EVENT_FACE_CROPS) {
        limit = MAX_EVENT_FACE_CROPS;
    }

    event_face_crop_t crops[MAX_EVENT_FACE_CROPS];
    int count = db_event_face_crops_list("unknown", crops, limit);
    if (count < 0) {
        http_response_set_json_error(res, 500, "Failed to list face crops");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, face_crop_to_json(&crops[i]));
    }
    cJSON_AddItemToObject(root, "crops", arr);
    cJSON_AddNumberToObject(root, "count", count);

    char *json = cJSON_PrintUnformatted(root);
    http_response_set_json(res, 200, json ? json : "{\"crops\":[]}");
    free(json);
    cJSON_Delete(root);
}

/* ────────────────────────────────────────────────────────────────────────────
 * GET /api/faces/crops/{id}/image
 * Serves a stored face crop.
 * ────────────────────────────────────────────────────────────────────────── */
void handle_get_face_crop_image(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    char id_buf[64] = {0};
    if (http_request_extract_path_param(req, "/api/faces/crops/", id_buf, sizeof(id_buf)) != 0) {
        http_response_set_json_error(res, 400, "Missing crop ID");
        return;
    }
    char *suffix = strstr(id_buf, "/image");
    if (suffix) *suffix = '\0';
    uint64_t crop_id = strtoull(id_buf, NULL, 10);
    if (crop_id == 0) {
        http_response_set_json_error(res, 400, "Invalid crop ID");
        return;
    }

    event_face_crop_t crop;
    if (db_event_face_crop_get(crop_id, &crop) != 0 ||
        !is_safe_relative_snapshot_path(crop.crop_path)) {
        http_response_set_json_error(res, 404, "Face crop not found");
        return;
    }

    char full_path[MAX_PATH_LENGTH];
    snprintf(full_path, sizeof(full_path), "%s/%s", g_config.storage_path, crop.crop_path);
    if (http_serve_file(req, res, full_path, "image/jpeg",
                        "Cache-Control: public, max-age=86400\r\n") != 0) {
        http_response_set_json_error(res, 404, "Face crop not found");
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * POST /api/faces/train-crop
 * Trains a face from a stored crop.
 * Body: { "crop_id": 123, "name": "Alice" }
 * ────────────────────────────────────────────────────────────────────────── */
void handle_post_faces_train_crop(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    if (!g_config.face_recognition_api_url[0]) {
        http_response_set_json_error(res, 503, "Face recognition API URL is not configured");
        return;
    }

    cJSON *body = httpd_parse_json_body(req);
    if (!body) {
        http_response_set_json_error(res, 400, "Invalid JSON body");
        return;
    }

    uint64_t crop_id = json_u64(body, "crop_id");
    const char *name = json_string(body, "name");
    if (crop_id == 0 || !name || name[0] == '\0') {
        cJSON_Delete(body);
        http_response_set_json_error(res, 400, "crop_id and name are required");
        return;
    }

    event_face_crop_t crop;
    if (db_event_face_crop_get(crop_id, &crop) != 0 ||
        !is_safe_relative_snapshot_path(crop.crop_path)) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 404, "Face crop not found");
        return;
    }

    char crop_path[MAX_PATH_LENGTH];
    snprintf(crop_path, sizeof(crop_path), "%s/%s", g_config.storage_path, crop.crop_path);

    unsigned char *jpeg_data = NULL;
    size_t jpeg_size = 0;
    if (read_file_bytes(crop_path, &jpeg_data, &jpeg_size) != 0) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 404, "Failed to read face crop");
        return;
    }

    long http_code = 0;
    char *upstream_response = NULL;
    int train_rc = train_face_bytes(name, "face-crop.jpg",
                                    jpeg_data, jpeg_size,
                                    &http_code, &upstream_response);
    free(jpeg_data);

    if (train_rc != 0) {
        cJSON_Delete(body);
        http_response_set_json_error(res, 502, "Face training request failed");
        return;
    }

    bool train_ok = upstream_train_success(http_code, upstream_response);
    if (train_ok) {
        db_event_face_crop_mark_named(crop_id, name);
        db_detection_event_set_sub_label(crop.event_id, name);
        db_event_enrichment_add(crop.event_id, "face_recognition", "user", "completed",
                                1.0f, name,
                                upstream_response && upstream_response[0] ? upstream_response : "{}",
                                "");
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "success", train_ok);
    cJSON_AddNumberToObject(out, "crop_id", (double)crop_id);
    cJSON_AddNumberToObject(out, "event_id", (double)crop.event_id);
    cJSON_AddStringToObject(out, "name", name);
    cJSON_AddNumberToObject(out, "upstream_status", http_code);
    if (upstream_response && upstream_response[0]) {
        cJSON_AddStringToObject(out, "upstream_response", upstream_response);
    }

    char *json = cJSON_PrintUnformatted(out);
    http_response_set_json(res, (int)http_code, json ? json : "{}");

    free(json);
    cJSON_Delete(out);
    free(upstream_response);
    cJSON_Delete(body);
}

/* ────────────────────────────────────────────────────────────────────────────
 * POST /api/faces/recognize  →  POST {base}/recognize  (multipart passthrough)
 * ────────────────────────────────────────────────────────────────────────── */
void handle_post_faces_recognize(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    if (!g_config.face_recognition_api_url[0]) {
        http_response_set_json_error(res, 503, "Face recognition API URL is not configured");
        return;
    }

    char upstream[1024];
    /* Directly use the configured URL (it already points to /recognize) */
    strncpy(upstream, g_config.face_recognition_api_url, sizeof(upstream) - 1);
    upstream[sizeof(upstream) - 1] = '\0';

    size_t body_len = req->body_len;
    const void *body = req->body;
    const char *content_type = http_request_get_header(req, "Content-Type");

    CURL *curl = curl_easy_init();
    if (!curl) {
        http_response_set_json_error(res, 500, "CURL init failed");
        return;
    }

    face_buf_t chunk = { .data = malloc(4096), .size = 0, .capacity = 4096 };
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        http_response_set_json_error(res, 500, "Out of memory");
        return;
    }
    chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    if (content_type && content_type[0]) {
        char ct_header[512];
        snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, ct_header);
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, upstream);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, face_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc    = curl_easy_perform(curl);
    long      http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(chunk.data);
        http_response_set_json_error(res, 502, curl_easy_strerror(rc));
        return;
    }

    http_response_set_json(res, (int)http_code, chunk.data[0] ? chunk.data : "{}");
    free(chunk.data);
}

/* ────────────────────────────────────────────────────────────────────────────
 * DELETE /api/faces/{id}  →  DELETE {base}/{id}
 * ────────────────────────────────────────────────────────────────────────── */
void handle_delete_face(const http_request_t *req, http_response_t *res) {
    if (!require_viewer(req, res)) return;

    if (!g_config.face_recognition_api_url[0]) {
        http_response_set_json_error(res, 503, "Face recognition API URL is not configured");
        return;
    }

    char id_buf[64] = {0};
    if (http_request_extract_path_param(req, "/api/faces/", id_buf, sizeof(id_buf)) != 0 || id_buf[0] == '\0') {
        http_response_set_json_error(res, 400, "Missing face ID");
        return;
    }

    char upstream[1024];
    if (build_upstream_url(id_buf, upstream, sizeof(upstream)) != 0) {
        http_response_set_json_error(res, 500, "Failed to build upstream URL");
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        http_response_set_json_error(res, 500, "CURL init failed");
        return;
    }

    face_buf_t chunk = { .data = malloc(4096), .size = 0, .capacity = 4096 };
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        http_response_set_json_error(res, 500, "Out of memory");
        return;
    }
    chunk.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, upstream);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, face_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode rc    = curl_easy_perform(curl);
    long      http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(chunk.data);
        http_response_set_json_error(res, 502, curl_easy_strerror(rc));
        return;
    }

    http_response_set_json(res, (int)http_code, chunk.data[0] ? chunk.data : "{\"success\":true}");
    free(chunk.data);
}
