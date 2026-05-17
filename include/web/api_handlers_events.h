#ifndef API_HANDLERS_EVENTS_H
#define API_HANDLERS_EVENTS_H

#include "web/request_response.h"

void handle_get_events(const http_request_t *req, http_response_t *res);
void handle_get_event(const http_request_t *req, http_response_t *res);
void handle_get_event_snapshot(const http_request_t *req, http_response_t *res);
void handle_get_event_enrichments(const http_request_t *req, http_response_t *res);
void handle_post_event_enrichment(const http_request_t *req, http_response_t *res);
void handle_get_enrichment_jobs(const http_request_t *req, http_response_t *res);
void handle_post_enrichment_job_claim(const http_request_t *req, http_response_t *res);
void handle_post_enrichment_job_complete(const http_request_t *req, http_response_t *res);
void handle_post_enrichment_job_fail(const http_request_t *req, http_response_t *res);

#endif // API_HANDLERS_EVENTS_H
