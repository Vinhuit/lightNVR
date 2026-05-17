#pragma once
/**
 * @file api_handlers_faces.h
 * @brief Face library proxy API handlers for LightNVR
 *
 * These handlers proxy /api/faces/* requests to the configured
 * face_recognition_api_url (light-object-detect container).
 */

#include "web/request_response.h"

/**
 * GET /api/faces/list
 * Returns the list of trained face profiles from the external API.
 */
void handle_get_faces_list(const http_request_t *req, http_response_t *res);

/**
 * POST /api/faces/train
 * Forwards a multipart training upload (name + image) to the external API.
 */
void handle_post_faces_train(const http_request_t *req, http_response_t *res);

/**
 * POST /api/faces/train-event
 * Trains a face from an existing event snapshot with JSON { event_id, name }.
 */
void handle_post_faces_train_event(const http_request_t *req, http_response_t *res);

/**
 * GET /api/faces/unknown-crops
 * Lists stored unknown face crops discovered from camera events.
 */
void handle_get_faces_unknown_crops(const http_request_t *req, http_response_t *res);

/**
 * GET /api/faces/crops/{id}/image
 * Serves a stored face crop JPEG.
 */
void handle_get_face_crop_image(const http_request_t *req, http_response_t *res);

/**
 * POST /api/faces/train-crop
 * Trains a face from an existing face crop with JSON { crop_id, name }.
 */
void handle_post_faces_train_crop(const http_request_t *req, http_response_t *res);

/**
 * POST /api/faces/recognize
 * Forwards an image upload to the recognition endpoint and returns the match result.
 */
void handle_post_faces_recognize(const http_request_t *req, http_response_t *res);

/**
 * DELETE /api/faces/{id}
 * Removes a trained face from the external face database.
 */
void handle_delete_face(const http_request_t *req, http_response_t *res);
