#define _POSIX_C_SOURCE 200809L

#include "core/enrichment_worker.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/config.h"
#include "core/path_utils.h"
#define LOG_COMPONENT "EnrichmentWorker"
#include "core/logger.h"
#include "core/shutdown_coordinator.h"
#include "database/db_detection_events.h"
#include "utils/strings.h"
#include "video/go2rtc/go2rtc_snapshot.h"

#define ENRICHMENT_WORKER_POLL_SECONDS 5
#define ENRICHMENT_WORKER_MAX_JOBS 4
#define GEMINI_TIMEOUT_SECONDS 30
#define OPENAI_TIMEOUT_SECONDS 30
#define OPENAI_DEFAULT_URL "https://api.openai.com/v1/responses"
#define OPENAI_DEFAULT_MODEL "gpt-5.4-mini"
#define OPENAI_COMPAT_DEFAULT_MODEL "qw/vision-model"
#define GEMINI_MAX_RESPONSE_BYTES (128 * 1024)
#define ENRICHMENT_SNAPSHOT_MAX_BYTES (2 * 1024 * 1024)

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} http_buffer_t;

static pthread_t g_enrichment_thread;
static pthread_mutex_t g_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_worker_running = false;
static bool g_worker_stop_requested = false;

static bool should_stop_worker(void);

static size_t http_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t bytes = size * nmemb;
    http_buffer_t *buffer = (http_buffer_t *)userp;

    if (bytes == 0) {
        return 0;
    }
    if (buffer->size + bytes + 1 > GEMINI_MAX_RESPONSE_BYTES) {
        return 0;
    }

    size_t needed = buffer->size + bytes + 1;
    if (needed > buffer->capacity) {
        size_t next_capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
        while (next_capacity < needed) {
            next_capacity *= 2;
        }
        char *next_data = realloc(buffer->data, next_capacity);
        if (!next_data) {
            return 0;
        }
        buffer->data = next_data;
        buffer->capacity = next_capacity;
    }

    memcpy(buffer->data + buffer->size, contents, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';
    return bytes;
}

static const char *base64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void sleep_milliseconds(long milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && !should_stop_worker()) {
        continue;
    }
}

static char *base64_encode(const unsigned char *data, size_t input_length) {
    if (!data || input_length == 0) {
        return NULL;
    }

    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded = malloc(output_length + 1);
    if (!encoded) {
        return NULL;
    }

    size_t in_index = 0;
    size_t out_index = 0;
    while (in_index < input_length) {
        uint32_t octet_a = in_index < input_length ? data[in_index++] : 0;
        uint32_t octet_b = in_index < input_length ? data[in_index++] : 0;
        uint32_t octet_c = in_index < input_length ? data[in_index++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded[out_index++] = base64_table[(triple >> 18) & 0x3F];
        encoded[out_index++] = base64_table[(triple >> 12) & 0x3F];
        encoded[out_index++] = base64_table[(triple >> 6) & 0x3F];
        encoded[out_index++] = base64_table[triple & 0x3F];
    }

    size_t padding = (3 - (input_length % 3)) % 3;
    for (size_t i = 0; i < padding; i++) {
        encoded[output_length - 1 - i] = '=';
    }
    encoded[output_length] = '\0';
    return encoded;
}

static bool append_bytes(unsigned char **buffer,
                         size_t *size,
                         size_t *capacity,
                         const unsigned char *data,
                         size_t bytes) {
    if (!buffer || !size || !capacity || !data || bytes == 0) {
        return false;
    }
    if (bytes > ENRICHMENT_SNAPSHOT_MAX_BYTES - *size) {
        return false;
    }

    size_t needed = *size + bytes;
    if (needed > *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 65536;
        while (next_capacity < needed) {
            next_capacity *= 2;
        }
        if (next_capacity > ENRICHMENT_SNAPSHOT_MAX_BYTES) {
            next_capacity = ENRICHMENT_SNAPSHOT_MAX_BYTES;
        }
        unsigned char *next = realloc(*buffer, next_capacity);
        if (!next) {
            return false;
        }
        *buffer = next;
        *capacity = next_capacity;
    }

    memcpy(*buffer + *size, data, bytes);
    *size += bytes;
    return true;
}

static bool capture_rtsp_snapshot_with_ffmpeg(const char *stream_name,
                                              unsigned char **jpeg_data,
                                              size_t *jpeg_size) {
    if (!stream_name || stream_name[0] == '\0' || !jpeg_data || !jpeg_size) {
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        log_warn("Failed to create pipe for FFmpeg snapshot fallback");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        log_warn("Failed to fork FFmpeg snapshot fallback");
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        char rtsp_url[512];
        snprintf(rtsp_url, sizeof(rtsp_url), "rtsp://localhost:8554/%s", stream_name);

        execlp("timeout", "timeout", "12s",
               "ffmpeg",
               "-hide_banner",
               "-loglevel", "error",
               "-rtsp_transport", "tcp",
               "-i", rtsp_url,
               "-an",
               "-frames:v", "1",
               "-f", "image2pipe",
               "-vcodec", "mjpeg",
               "-",
               (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    unsigned char *buffer = NULL;
    size_t size = 0;
    size_t capacity = 0;
    unsigned char chunk[8192];
    ssize_t nread;
    bool ok = true;
    while ((nread = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        if (!append_bytes(&buffer, &size, &capacity, chunk, (size_t)nread)) {
            ok = false;
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (!ok || size < 4 || buffer[0] != 0xFF || buffer[1] != 0xD8) {
        free(buffer);
        return false;
    }

    *jpeg_data = buffer;
    *jpeg_size = size;
    return true;
}

static bool get_event_snapshot(const char *stream_name,
                               unsigned char **jpeg_data,
                               size_t *jpeg_size) {
    if (go2rtc_get_snapshot(stream_name, jpeg_data, jpeg_size)) {
        log_info("Gemini enrichment snapshot captured from go2rtc for stream %s (%zu bytes)",
                 stream_name,
                 *jpeg_size);
        return true;
    }

    log_warn("go2rtc snapshot failed for stream %s; trying FFmpeg RTSP fallback", stream_name);
    if (capture_rtsp_snapshot_with_ffmpeg(stream_name, jpeg_data, jpeg_size)) {
        log_info("Gemini enrichment snapshot captured with FFmpeg fallback for stream %s (%zu bytes)",
                 stream_name,
                 *jpeg_size);
        return true;
    }

    return false;
}

static bool persist_event_snapshot(uint64_t event_id,
                                   const unsigned char *jpeg_data,
                                   size_t jpeg_size,
                                   char *relative_path,
                                   size_t relative_path_size) {
    if (event_id == 0 || !jpeg_data || jpeg_size == 0 ||
        !relative_path || relative_path_size == 0) {
        return false;
    }

    char full_path[MAX_PATH_LENGTH];
    snprintf(relative_path, relative_path_size,
             "event_snapshots/event_%llu.jpg",
             (unsigned long long)event_id);
    snprintf(full_path, sizeof(full_path), "%s/%s", g_config.storage_path, relative_path);

    if (ensure_path(full_path) != 0) {
        log_warn("Failed to create event snapshot directory for %s: %s",
                 full_path,
                 strerror(errno));
        return false;
    }

    FILE *file = fopen(full_path, "wb");
    if (!file) {
        log_warn("Failed to open event snapshot file %s: %s", full_path, strerror(errno));
        return false;
    }

    size_t written = fwrite(jpeg_data, 1, jpeg_size, file);
    int close_rc = fclose(file);
    if (written != jpeg_size || close_rc != 0) {
        unlink(full_path);
        log_warn("Failed to write complete event snapshot %s", full_path);
        return false;
    }

    return true;
}

static bool persist_face_crop(uint64_t event_id,
                              int face_index,
                              const unsigned char *jpeg_data,
                              size_t jpeg_size,
                              char *relative_path,
                              size_t relative_path_size) {
    if (event_id == 0 || face_index < 0 || !jpeg_data || jpeg_size == 0 ||
        !relative_path || relative_path_size == 0) {
        return false;
    }

    char full_path[MAX_PATH_LENGTH];
    snprintf(relative_path, relative_path_size,
             "face_crops/event_%llu_face_%d.jpg",
             (unsigned long long)event_id,
             face_index);
    snprintf(full_path, sizeof(full_path), "%s/%s", g_config.storage_path, relative_path);

    if (ensure_path(full_path) != 0) {
        log_warn("Failed to create face crop directory for %s: %s",
                 full_path,
                 strerror(errno));
        return false;
    }

    FILE *file = fopen(full_path, "wb");
    if (!file) {
        log_warn("Failed to open face crop file %s: %s", full_path, strerror(errno));
        return false;
    }

    size_t written = fwrite(jpeg_data, 1, jpeg_size, file);
    int close_rc = fclose(file);
    if (written != jpeg_size || close_rc != 0) {
        unlink(full_path);
        log_warn("Failed to write complete face crop %s", full_path);
        return false;
    }

    return true;
}

static bool is_safe_event_snapshot_path(const char *path) {
    if (!path || path[0] == '\0' || path[0] == '/' || path[0] == '\\') {
        return false;
    }
    if (strstr(path, "..") || strchr(path, '\\')) {
        return false;
    }
    return true;
}

static bool read_event_snapshot_file(const detection_event_t *event,
                                     unsigned char **jpeg_data,
                                     size_t *jpeg_size) {
    if (!event || !jpeg_data || !jpeg_size ||
        !is_safe_event_snapshot_path(event->thumbnail_path)) {
        return false;
    }

    char full_path[MAX_PATH_LENGTH];
    snprintf(full_path, sizeof(full_path), "%s/%s",
             g_config.storage_path, event->thumbnail_path);

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size <= 0 || size > ENRICHMENT_SNAPSHOT_MAX_BYTES) {
        fclose(file);
        return false;
    }
    rewind(file);

    unsigned char *buffer = malloc((size_t)size);
    if (!buffer) {
        fclose(file);
        return false;
    }
    size_t read_bytes = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_bytes != (size_t)size) {
        free(buffer);
        return false;
    }

    *jpeg_data = buffer;
    *jpeg_size = read_bytes;
    return true;
}

static bool get_or_create_event_snapshot(detection_event_t *event,
                                         unsigned char **jpeg_data,
                                         size_t *jpeg_size) {
    if (!event || !jpeg_data || !jpeg_size) {
        return false;
    }

    if (read_event_snapshot_file(event, jpeg_data, jpeg_size)) {
        return true;
    }

    if (event->stream_name[0] == '\0' ||
        !get_event_snapshot(event->stream_name, jpeg_data, jpeg_size)) {
        return false;
    }

    if (event->thumbnail_path[0] == '\0') {
        char relative_path[MAX_EVENT_THUMBNAIL_PATH] = {0};
        if (persist_event_snapshot(event->id, *jpeg_data, *jpeg_size,
                                   relative_path, sizeof(relative_path))) {
            db_detection_event_set_thumbnail(event->id, relative_path);
            safe_strcpy(event->thumbnail_path, relative_path,
                        sizeof(event->thumbnail_path), 0);
        }
    }

    return true;
}

static const char *json_string_value(cJSON *object, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static bool base64_decode_alloc(const char *input,
                                unsigned char **output,
                                size_t *output_size) {
    if (!input || !output || !output_size || input[0] == '\0') {
        return false;
    }

    size_t input_len = strlen(input);
    size_t needed = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &needed,
                                   (const unsigned char *)input,
                                   input_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0 ||
        needed > ENRICHMENT_SNAPSHOT_MAX_BYTES) {
        return false;
    }

    unsigned char *buffer = malloc(needed);
    if (!buffer) {
        return false;
    }
    rc = mbedtls_base64_decode(buffer, needed, output_size,
                               (const unsigned char *)input,
                               input_len);
    if (rc != 0) {
        free(buffer);
        return false;
    }

    *output = buffer;
    return true;
}

static void build_face_detect_url(const char *recognize_url,
                                  char *out,
                                  size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    const char *base = recognize_url && recognize_url[0]
                       ? recognize_url
                       : "http://light-object-detect:8000/api/v1/faces/recognize";
    char api_base[512];
    safe_strcpy(api_base, base, sizeof(api_base), 0);

    char *last_slash = strrchr(api_base, '/');
    if (last_slash && (strcmp(last_slash, "/recognize") == 0 ||
                       strcmp(last_slash, "/train") == 0 ||
                       strcmp(last_slash, "/detect") == 0)) {
        *last_slash = '\0';
    }

    snprintf(out, out_size, "%s/detect", api_base);
}

static void save_face_crops_from_response(uint64_t event_id,
                                          const char *response_json) {
    if (event_id == 0 || !response_json || response_json[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_Parse(response_json);
    if (!root) {
        return;
    }
    cJSON *faces = cJSON_GetObjectItem(root, "faces");
    int count = cJSON_IsArray(faces) ? cJSON_GetArraySize(faces) : 0;

    for (int i = 0; i < count && i < 8; i++) {
        cJSON *face = cJSON_GetArrayItem(faces, i);
        cJSON *crop_b64 = face ? cJSON_GetObjectItem(face, "crop_jpeg_base64") : NULL;
        cJSON *bbox = face ? cJSON_GetObjectItem(face, "bbox") : NULL;
        if (!cJSON_IsString(crop_b64) || !crop_b64->valuestring) {
            continue;
        }

        unsigned char *crop_data = NULL;
        size_t crop_size = 0;
        if (!base64_decode_alloc(crop_b64->valuestring, &crop_data, &crop_size)) {
            continue;
        }

        char relative_path[MAX_EVENT_THUMBNAIL_PATH] = {0};
        if (persist_face_crop(event_id, i, crop_data, crop_size,
                              relative_path, sizeof(relative_path))) {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, confidence = 0.0f;
            cJSON *x_val = bbox ? cJSON_GetObjectItem(bbox, "x") : NULL;
            cJSON *y_val = bbox ? cJSON_GetObjectItem(bbox, "y") : NULL;
            cJSON *w_val = bbox ? cJSON_GetObjectItem(bbox, "width") : NULL;
            cJSON *h_val = bbox ? cJSON_GetObjectItem(bbox, "height") : NULL;
            cJSON *conf_val = cJSON_GetObjectItem(face, "confidence");
            if (cJSON_IsNumber(x_val)) x = (float)x_val->valuedouble;
            if (cJSON_IsNumber(y_val)) y = (float)y_val->valuedouble;
            if (cJSON_IsNumber(w_val)) w = (float)w_val->valuedouble;
            if (cJSON_IsNumber(h_val)) h = (float)h_val->valuedouble;
            if (cJSON_IsNumber(conf_val)) confidence = (float)conf_val->valuedouble;

            db_event_face_crop_add(event_id, relative_path,
                                   x, y, w, h, confidence,
                                   "unknown", "light-object-detect");
        }

        free(crop_data);
    }

    cJSON_Delete(root);
}

static void detect_and_save_face_crops(uint64_t event_id,
                                       const unsigned char *jpeg_data,
                                       size_t jpeg_size) {
    if (event_id == 0 || !jpeg_data || jpeg_size == 0 ||
        !g_config.face_recognition_api_url[0]) {
        return;
    }

    char detect_url[1024];
    build_face_detect_url(g_config.face_recognition_api_url,
                          detect_url,
                          sizeof(detect_url));

    CURL *curl = curl_easy_init();
    if (!curl) {
        return;
    }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, "snapshot.jpg");
    curl_mime_type(part, "image/jpeg");
    curl_mime_data(part, (const char *)jpeg_data, jpeg_size);

    http_buffer_t chunk = {0};
    chunk.capacity = 4096;
    chunk.data = malloc(chunk.capacity);
    if (!chunk.data) {
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return;
    }
    chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, detect_url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (res == CURLE_OK && http_code == 200 && chunk.data[0]) {
        save_face_crops_from_response(event_id, chunk.data);
    } else {
        log_debug("Face crop detection skipped/failed for event %llu: %s (HTTP %ld)",
                  (unsigned long long)event_id,
                  curl_easy_strerror(res),
                  http_code);
    }

    free(chunk.data);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
}

static const char *resolve_api_key(const char *api_key_ref) {
    if (!api_key_ref || api_key_ref[0] == '\0') {
        return NULL;
    }

    const char *env_value = getenv(api_key_ref);
    if (env_value && env_value[0] != '\0') {
        return env_value;
    }

    /*
     * Backward compatibility for early test builds where the WebUI accepted a
     * raw provider key in the "API key env" field. Keep supporting it so old
     * queued jobs can complete, but do not log the value.
     */
    if (strlen(api_key_ref) > 24 && strchr(api_key_ref, '_') != NULL) {
        log_warn("GenAI API key appears to be stored directly; use an environment variable name instead");
        return api_key_ref;
    }

    return NULL;
}

static char *extract_gemini_text(const char *response_json) {
    cJSON *root = cJSON_Parse(response_json);
    if (!root) {
        return NULL;
    }

    cJSON *candidates = cJSON_GetObjectItemCaseSensitive(root, "candidates");
    cJSON *candidate = cJSON_IsArray(candidates) ? cJSON_GetArrayItem(candidates, 0) : NULL;
    cJSON *content = candidate ? cJSON_GetObjectItemCaseSensitive(candidate, "content") : NULL;
    cJSON *parts = content ? cJSON_GetObjectItemCaseSensitive(content, "parts") : NULL;
    char *result = NULL;
    if (cJSON_IsArray(parts)) {
        int part_count = cJSON_GetArraySize(parts);
        for (int i = 0; i < part_count; i++) {
            cJSON *part = cJSON_GetArrayItem(parts, i);
            cJSON *thought = part ? cJSON_GetObjectItemCaseSensitive(part, "thought") : NULL;
            cJSON *text = part ? cJSON_GetObjectItemCaseSensitive(part, "text") : NULL;
            if (cJSON_IsTrue(thought)) {
                continue;
            }
            if (cJSON_IsString(text) && text->valuestring && text->valuestring[0] != '\0') {
                result = strdup(text->valuestring);
                break;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

static char *extract_openai_text(const char *response_json) {
    cJSON *root = cJSON_Parse(response_json);
    if (!root) {
        return NULL;
    }

    cJSON *output_text = cJSON_GetObjectItemCaseSensitive(root, "output_text");
    if (cJSON_IsString(output_text) && output_text->valuestring && output_text->valuestring[0] != '\0') {
        char *result = strdup(output_text->valuestring);
        cJSON_Delete(root);
        return result;
    }

    char *result = NULL;
    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    int output_count = cJSON_IsArray(output) ? cJSON_GetArraySize(output) : 0;
    for (int i = 0; i < output_count && !result; i++) {
        cJSON *message = cJSON_GetArrayItem(output, i);
        cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
        int content_count = cJSON_IsArray(content) ? cJSON_GetArraySize(content) : 0;
        for (int j = 0; j < content_count; j++) {
            cJSON *part = cJSON_GetArrayItem(content, j);
            cJSON *type = part ? cJSON_GetObjectItemCaseSensitive(part, "type") : NULL;
            cJSON *text = part ? cJSON_GetObjectItemCaseSensitive(part, "text") : NULL;
            if (cJSON_IsString(type) && strcmp(type->valuestring, "output_text") != 0) {
                continue;
            }
            if (cJSON_IsString(text) && text->valuestring && text->valuestring[0] != '\0') {
                result = strdup(text->valuestring);
                break;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

static char *extract_openai_chat_text(const char *response_json) {
    cJSON *root = cJSON_Parse(response_json);
    if (!root) {
        return NULL;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

    char *result = NULL;
    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0] != '\0') {
        result = strdup(content->valuestring);
    } else if (cJSON_IsArray(content)) {
        int count = cJSON_GetArraySize(content);
        for (int i = 0; i < count; i++) {
            cJSON *part = cJSON_GetArrayItem(content, i);
            cJSON *text = part ? cJSON_GetObjectItemCaseSensitive(part, "text") : NULL;
            if (cJSON_IsString(text) && text->valuestring && text->valuestring[0] != '\0') {
                result = strdup(text->valuestring);
                break;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}

static char *build_jpeg_data_url(const char *image_base64) {
    static const char prefix[] = "data:image/jpeg;base64,";
    if (!image_base64 || image_base64[0] == '\0') {
        return NULL;
    }

    size_t prefix_len = sizeof(prefix) - 1;
    size_t image_len = strlen(image_base64);
    char *image_url = malloc(prefix_len + image_len + 1);
    if (!image_url) {
        return NULL;
    }

    memcpy(image_url, prefix, prefix_len);
    memcpy(image_url + prefix_len, image_base64, image_len + 1);
    return image_url;
}

static char *build_gemini_request(const detection_event_t *event, const char *image_base64) {
    cJSON *root = cJSON_CreateObject();
    cJSON *contents = cJSON_AddArrayToObject(root, "contents");
    cJSON *content = cJSON_CreateObject();
    cJSON *parts = cJSON_AddArrayToObject(content, "parts");
    cJSON *text_part = cJSON_CreateObject();
    cJSON *generation_config = cJSON_AddObjectToObject(root, "generationConfig");

    if (!root || !contents || !content || !parts || !text_part || !generation_config) {
        cJSON_Delete(root);
        return NULL;
    }

    char prompt[768];
    snprintf(prompt, sizeof(prompt),
             "You are summarizing a home security camera event. "
             "Return one concise sentence under 25 words. "
             "Object label: %s. Stream: %s. Confidence: %.2f. "
             "If an image is provided, describe the visible event only.",
             event->label,
             event->stream_name,
             event->best_confidence);

    cJSON_AddStringToObject(content, "role", "user");
    cJSON_AddStringToObject(text_part, "text", prompt);
    cJSON_AddItemToArray(parts, text_part);

    if (image_base64 && image_base64[0] != '\0') {
        cJSON *image_part = cJSON_CreateObject();
        cJSON *inline_data = cJSON_CreateObject();
        if (!image_part || !inline_data) {
            cJSON_Delete(image_part);
            cJSON_Delete(inline_data);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(inline_data, "mime_type", "image/jpeg");
        cJSON_AddStringToObject(inline_data, "data", image_base64);
        cJSON_AddItemToObject(image_part, "inline_data", inline_data);
        cJSON_AddItemToArray(parts, image_part);
    }

    cJSON_AddItemToArray(contents, content);
    cJSON_AddNumberToObject(generation_config, "temperature", 0.2);
    cJSON_AddNumberToObject(generation_config, "maxOutputTokens", 80);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *build_openai_responses_request(const detection_event_t *event,
                                            const char *model,
                                            const char *image_base64) {
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_AddArrayToObject(root, "input");
    cJSON *message = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(message, "content");
    cJSON *text_part = cJSON_CreateObject();

    if (!root || !input || !message || !content || !text_part) {
        cJSON_Delete(root);
        return NULL;
    }

    char prompt[768];
    snprintf(prompt, sizeof(prompt),
             "You are summarizing a home security camera event. "
             "Return one concise sentence under 25 words. "
             "Object label: %s. Stream: %s. Confidence: %.2f. "
             "If an image is provided, describe the visible event only.",
             event->label,
             event->stream_name,
             event->best_confidence);

    cJSON_AddStringToObject(root, "model", model && model[0] ? model : OPENAI_DEFAULT_MODEL);
    cJSON_AddNumberToObject(root, "max_output_tokens", 80);
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(text_part, "type", "input_text");
    cJSON_AddStringToObject(text_part, "text", prompt);
    cJSON_AddItemToArray(content, text_part);

    if (image_base64 && image_base64[0] != '\0') {
        cJSON *image_part = cJSON_CreateObject();
        if (!image_part) {
            cJSON_Delete(root);
            return NULL;
        }
        char *image_url = build_jpeg_data_url(image_base64);
        if (!image_url) {
            cJSON_Delete(image_part);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(image_part, "type", "input_image");
        cJSON_AddStringToObject(image_part, "image_url", image_url);
        free(image_url);
        cJSON_AddItemToArray(content, image_part);
    }

    cJSON_AddItemToArray(input, message);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *build_openai_chat_request(const detection_event_t *event,
                                       const char *model,
                                       const char *image_base64) {
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *message = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(message, "content");
    cJSON *text_part = cJSON_CreateObject();

    if (!root || !messages || !message || !content || !text_part) {
        cJSON_Delete(root);
        return NULL;
    }

    char prompt[768];
    snprintf(prompt, sizeof(prompt),
             "You are summarizing a home security camera event. "
             "Return one concise sentence under 25 words. "
             "Object label: %s. Stream: %s. Confidence: %.2f. "
             "If an image is provided, describe the visible event only.",
             event->label,
             event->stream_name,
             event->best_confidence);

    cJSON_AddStringToObject(root, "model",
                            model && model[0] ? model : OPENAI_COMPAT_DEFAULT_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 80);
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", prompt);
    cJSON_AddItemToArray(content, text_part);

    if (image_base64 && image_base64[0] != '\0') {
        cJSON *image_part = cJSON_CreateObject();
        cJSON *image_url_obj = cJSON_CreateObject();
        if (!image_part || !image_url_obj) {
            cJSON_Delete(image_part);
            cJSON_Delete(image_url_obj);
            cJSON_Delete(root);
            return NULL;
        }
        char *image_url = build_jpeg_data_url(image_base64);
        if (!image_url) {
            cJSON_Delete(image_part);
            cJSON_Delete(image_url_obj);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(image_url_obj, "url", image_url);
        cJSON_AddStringToObject(image_part, "type", "image_url");
        cJSON_AddItemToObject(image_part, "image_url", image_url_obj);
        free(image_url);
        cJSON_AddItemToArray(content, image_part);
    }

    cJSON_AddItemToArray(messages, message);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void build_openai_chat_url(const char *api_url, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (!api_url || api_url[0] == '\0') {
        snprintf(out, out_size, "http://localhost:11434/v1/chat/completions");
        return;
    }
    if (strstr(api_url, "/chat/completions")) {
        snprintf(out, out_size, "%s", api_url);
        return;
    }

    size_t len = strlen(api_url);
    while (len > 0 && api_url[len - 1] == '/') {
        len--;
    }
    snprintf(out, out_size, "%.*s/chat/completions", (int)len, api_url);
}

static int call_gemini(const char *api_url,
                       const char *api_key,
                       const char *request_json,
                       char **description,
                       char **raw_response,
                       char *error,
                       size_t error_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(error, error_size, "Failed to initialize curl");
        return -1;
    }

    http_buffer_t response = {0};
    struct curl_slist *headers = NULL;
    char api_key_header[512];
    snprintf(api_key_header, sizeof(api_key_header), "x-goog-api-key: %s", api_key);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, api_key_header);
    if (!headers) {
        curl_easy_cleanup(curl);
        snprintf(error, error_size, "Failed to build Gemini request headers");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_json));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, GEMINI_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode curl_result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_result != CURLE_OK) {
        snprintf(error, error_size, "Gemini request failed: %s", curl_easy_strerror(curl_result));
        free(response.data);
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        snprintf(error, error_size, "Gemini returned HTTP %ld", http_code);
        if (raw_response) {
            *raw_response = response.data;
        } else {
            free(response.data);
        }
        return -1;
    }

    char *text = extract_gemini_text(response.data ? response.data : "");
    if (!text) {
        snprintf(error, error_size, "Gemini response did not contain text");
        if (raw_response) {
            *raw_response = response.data;
        } else {
            free(response.data);
        }
        return -1;
    }

    *description = text;
    if (raw_response) {
        *raw_response = response.data;
    } else {
        free(response.data);
    }
    return 0;
}

static int call_openai_responses(const char *api_url,
                                 const char *api_key,
                                 const char *request_json,
                                 char **description,
                                 char **raw_response,
                                 char *error,
                                 size_t error_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(error, error_size, "Failed to initialize curl");
        return -1;
    }

    http_buffer_t response = {0};
    struct curl_slist *headers = NULL;
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);
    if (!headers) {
        curl_easy_cleanup(curl);
        snprintf(error, error_size, "Failed to build OpenAI request headers");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, api_url && api_url[0] ? api_url : OPENAI_DEFAULT_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_json));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, OPENAI_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode curl_result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_result != CURLE_OK) {
        snprintf(error, error_size, "OpenAI request failed: %s", curl_easy_strerror(curl_result));
        free(response.data);
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        snprintf(error, error_size, "OpenAI returned HTTP %ld", http_code);
        if (raw_response) {
            *raw_response = response.data;
        } else {
            free(response.data);
        }
        return -1;
    }

    char *text = extract_openai_text(response.data ? response.data : "");
    if (!text) {
        snprintf(error, error_size, "OpenAI response did not contain text");
        if (raw_response) {
            *raw_response = response.data;
        } else {
            free(response.data);
        }
        return -1;
    }

    *description = text;
    if (raw_response) {
        *raw_response = response.data;
    } else {
        free(response.data);
    }
    return 0;
}

static int call_openai_chat_completions(const char *api_url,
                                        const char *api_key,
                                        const char *request_json,
                                        char **description,
                                        char **raw_response,
                                        char *error,
                                        size_t error_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(error, error_size, "Failed to initialize curl");
        return -1;
    }

    http_buffer_t response = {0};
    struct curl_slist *headers = NULL;
    char auth_header[512];

    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (api_key && api_key[0] != '\0') {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth_header);
    }
    if (!headers) {
        curl_easy_cleanup(curl);
        snprintf(error, error_size, "Failed to build OpenAI-compatible request headers");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_json));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, OPENAI_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode curl_result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_result != CURLE_OK) {
        snprintf(error, error_size, "OpenAI-compatible request failed: %s",
                 curl_easy_strerror(curl_result));
        free(response.data);
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        snprintf(error, error_size, "OpenAI-compatible API returned HTTP %ld", http_code);
        if (raw_response) {
            *raw_response = response.data;
        } else {
            free(response.data);
        }
        return -1;
    }

    char *text = extract_openai_chat_text(response.data ? response.data : "");
    if (!text) {
        snprintf(error, error_size, "OpenAI-compatible response did not contain text");
        if (raw_response) {
            *raw_response = response.data;
        } else {
            free(response.data);
        }
        return -1;
    }

    *description = text;
    if (raw_response) {
        *raw_response = response.data;
    } else {
        free(response.data);
    }
    return 0;
}

static int process_genai_job(const enrichment_job_t *queued_job) {
    enrichment_job_t job;
    if (db_enrichment_job_claim(queued_job->id, &job) != 0) {
        return 0;
    }

    cJSON *payload = cJSON_Parse(job.payload_json);
    if (!payload) {
        db_enrichment_job_fail(job.id, "Invalid GenAI job payload", 0);
        return -1;
    }

    const char *provider = json_string_value(payload, "provider");
    const char *api_url = json_string_value(payload, "api_url");
    const char *api_key_ref = json_string_value(payload, "api_key_env");
    const char *api_key = resolve_api_key(api_key_ref);

    bool is_gemini = strcmp(provider, "gemini") == 0;
    bool is_openai = strcmp(provider, "openai") == 0 || strcmp(provider, "chatgpt") == 0;
    bool is_openai_compatible = strcmp(provider, "openai_compatible") == 0;

    if (!is_gemini && !is_openai && !is_openai_compatible) {
        db_enrichment_job_fail(job.id, "Built-in worker supports Gemini, OpenAI, and OpenAI-compatible APIs only", 0);
        cJSON_Delete(payload);
        return -1;
    }
    if (is_gemini && (!api_url || api_url[0] == '\0')) {
        db_enrichment_job_fail(job.id, "Gemini API URL is not configured", 0);
        cJSON_Delete(payload);
        return -1;
    }
    if ((is_gemini || is_openai) && (!api_key || api_key[0] == '\0')) {
        db_enrichment_job_fail(job.id, "GenAI API key environment variable is not set", 0);
        cJSON_Delete(payload);
        return -1;
    }

    detection_event_t event;
    if (db_detection_event_get(job.event_id, &event) != 0) {
        db_enrichment_job_fail(job.id, "Detection event not found", 0);
        cJSON_Delete(payload);
        return -1;
    }

    unsigned char *jpeg_data = NULL;
    size_t jpeg_size = 0;
    char *image_base64 = NULL;
    if (get_or_create_event_snapshot(&event, &jpeg_data, &jpeg_size)) {
        image_base64 = base64_encode(jpeg_data, jpeg_size);
        free(jpeg_data);
    } else {
        log_warn("GenAI enrichment job %llu will run without snapshot image",
                 (unsigned long long)job.id);
    }

    const char *model = json_string_value(payload, "model");
    char *request_json = NULL;
    if (is_openai) {
        request_json = build_openai_responses_request(&event, model, image_base64);
    } else if (is_openai_compatible) {
        request_json = build_openai_chat_request(&event, model, image_base64);
    } else {
        request_json = build_gemini_request(&event, image_base64);
    }
    free(image_base64);
    if (!request_json) {
        db_enrichment_job_fail(job.id, "Failed to build GenAI request", 300);
        cJSON_Delete(payload);
        return -1;
    }

    char *description = NULL;
    char *raw_response = NULL;
    char error[512] = {0};
    char chat_url[MAX_URL_LENGTH];
    if (is_openai_compatible) {
        build_openai_chat_url(api_url, chat_url, sizeof(chat_url));
    }

    int rc = 0;
    if (is_openai) {
        rc = call_openai_responses(api_url && api_url[0] ? api_url : OPENAI_DEFAULT_URL,
                                   api_key,
                                   request_json,
                                   &description,
                                   &raw_response,
                                   error,
                                   sizeof(error));
    } else if (is_openai_compatible) {
        rc = call_openai_chat_completions(chat_url,
                                          api_key,
                                          request_json,
                                          &description,
                                          &raw_response,
                                          error,
                                          sizeof(error));
    } else {
        rc = call_gemini(api_url, api_key, request_json, &description, &raw_response, error, sizeof(error));
    }
    free(request_json);

    if (rc == 0) {
        const char *result_provider = is_openai ? "openai" : (is_openai_compatible ? "openai_compatible" : "gemini");
        db_enrichment_job_complete(job.id, result_provider, 1.0f,
                                   description, raw_response ? raw_response : "", "");
        log_info("Completed %s enrichment job %llu for event %llu",
                 is_openai ? "OpenAI" : (is_openai_compatible ? "OpenAI-compatible" : "Gemini"),
                 (unsigned long long)job.id,
                 (unsigned long long)job.event_id);
    } else {
        int retry_after = strstr(error, "HTTP 4") ? 0 : 300;
        db_enrichment_job_fail(job.id, error, retry_after);
        log_warn("%s enrichment job %llu failed: %s",
                 is_openai ? "OpenAI" : (is_openai_compatible ? "OpenAI-compatible" : "Gemini"),
                 (unsigned long long)job.id,
                 error);
    }

    free(description);
    free(raw_response);
    cJSON_Delete(payload);
    return rc;
}

static int process_face_recognition_job(const enrichment_job_t *queued_job) {
    enrichment_job_t job;
    if (db_enrichment_job_claim(queued_job->id, &job) != 0) {
        return 0;
    }

    if (!g_config.face_recognition_api_url[0]) {
        db_enrichment_job_fail(job.id, "Face Recognition API URL is not configured", 0);
        return -1;
    }

    detection_event_t event;
    if (db_detection_event_get(job.event_id, &event) != 0) {
        db_enrichment_job_fail(job.id, "Detection event not found", 0);
        return -1;
    }

    unsigned char *jpeg_data = NULL;
    size_t jpeg_size = 0;
    if (!get_or_create_event_snapshot(&event, &jpeg_data, &jpeg_size)) {
        db_enrichment_job_fail(job.id, "Failed to get event snapshot", 0);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(jpeg_data);
        db_enrichment_job_fail(job.id, "Failed to initialize CURL", 0);
        return -1;
    }

    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, "snapshot.jpg");
    curl_mime_type(part, "image/jpeg");
    curl_mime_data(part, (const char *)jpeg_data, jpeg_size);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, g_config.face_recognition_api_url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    http_buffer_t chunk;
    chunk.capacity = 4096;
    chunk.data = malloc(chunk.capacity);
    if (!chunk.data) {
        free(jpeg_data);
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        db_enrichment_job_fail(job.id, "Out of memory", 0);
        return -1;
    }
    chunk.size = 0;
    chunk.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    int rc = -1;
    bool matched_face = false;
    if (res == CURLE_OK && http_code == 200) {
        cJSON *json = cJSON_Parse(chunk.data);
        if (json) {
            /* API response shape: {"success": bool, "name": str, "confidence": float, "process_time_ms": int} */
            cJSON *success_val = cJSON_GetObjectItem(json, "success");
            if (success_val && cJSON_IsTrue(success_val)) {
                cJSON *name_val = cJSON_GetObjectItem(json, "name");
                cJSON *conf_val = cJSON_GetObjectItem(json, "confidence");
                if (name_val && cJSON_IsString(name_val) && name_val->valuestring && name_val->valuestring[0]) {
                    float confidence = conf_val && cJSON_IsNumber(conf_val)
                                       ? (float)conf_val->valuedouble : 1.0f;
                    db_enrichment_job_complete(job.id, "light-object-detect", confidence,
                                              name_val->valuestring, chunk.data, NULL);
                    rc = 0;
                    matched_face = true;
                    log_info("Face recognition job %llu matched: %s (conf=%.2f)",
                             (unsigned long long)job.id, name_val->valuestring, confidence);
                }
            }
            if (rc != 0) {
                /* success=false or no name: no known face in snapshot — still a valid outcome */
                db_enrichment_job_complete(job.id, "light-object-detect", 0.0f,
                                           "Unknown", chunk.data, NULL);
                rc = 0;
                log_info("Face recognition job %llu: no face matched", (unsigned long long)job.id);
            }
            cJSON_Delete(json);
        } else {
            db_enrichment_job_fail(job.id, "Failed to parse JSON response from face recognition API", 0);
        }
    } else {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Face recognition API error: %s (HTTP %ld)",
                 curl_easy_strerror(res), http_code);
        db_enrichment_job_fail(job.id, err_msg, 300);
    }

    if (rc == 0 && !matched_face) {
        detect_and_save_face_crops(event.id, jpeg_data, jpeg_size);
    }

    free(chunk.data);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    free(jpeg_data);
    return rc;
}

static bool should_stop_worker(void) {
    pthread_mutex_lock(&g_worker_mutex);
    bool should_stop = g_worker_stop_requested || is_shutdown_initiated();
    pthread_mutex_unlock(&g_worker_mutex);
    return should_stop;
}

static void *enrichment_worker_thread(void *arg) {
    (void)arg;
    log_info("Enrichment worker started");

    while (!should_stop_worker()) {
        enrichment_job_t jobs[ENRICHMENT_WORKER_MAX_JOBS];
        int count = db_enrichment_jobs_list("queued", "genai_description", jobs, ENRICHMENT_WORKER_MAX_JOBS);
        if (count > 0) {
            for (int i = 0; i < count && !should_stop_worker(); i++) {
                process_genai_job(&jobs[i]);
            }
        }

        int fr_count = db_enrichment_jobs_list("queued", "face_recognition", jobs, ENRICHMENT_WORKER_MAX_JOBS);
        if (fr_count > 0) {
            for (int i = 0; i < fr_count && !should_stop_worker(); i++) {
                process_face_recognition_job(&jobs[i]);
            }
        }

        for (int i = 0; i < ENRICHMENT_WORKER_POLL_SECONDS * 10 && !should_stop_worker(); i++) {
            sleep_milliseconds(100);
        }
    }

    go2rtc_snapshot_cleanup_thread();
    log_info("Enrichment worker stopped");
    return NULL;
}

int start_enrichment_worker(void) {
    pthread_mutex_lock(&g_worker_mutex);
    if (g_worker_running) {
        pthread_mutex_unlock(&g_worker_mutex);
        return 0;
    }
    g_worker_stop_requested = false;
    pthread_mutex_unlock(&g_worker_mutex);

    if (pthread_create(&g_enrichment_thread, NULL, enrichment_worker_thread, NULL) != 0) {
        log_error("Failed to start enrichment worker thread");
        return -1;
    }

    pthread_mutex_lock(&g_worker_mutex);
    g_worker_running = true;
    pthread_mutex_unlock(&g_worker_mutex);
    return 0;
}

void stop_enrichment_worker(void) {
    pthread_mutex_lock(&g_worker_mutex);
    if (!g_worker_running) {
        pthread_mutex_unlock(&g_worker_mutex);
        return;
    }
    g_worker_stop_requested = true;
    pthread_mutex_unlock(&g_worker_mutex);

    pthread_join(g_enrichment_thread, NULL);

    pthread_mutex_lock(&g_worker_mutex);
    g_worker_running = false;
    pthread_mutex_unlock(&g_worker_mutex);
}
