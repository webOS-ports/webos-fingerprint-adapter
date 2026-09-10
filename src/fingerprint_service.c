/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <luna-service2/lunaservice.h>

#include "fingerprint_service.h"
#include "fpd_client.h"
#include "luna_service_utils.h"

#define FINGERPRINT_SERVICE_NAME	"com.webos.service.fingerprint"

#define FPD_STATE_ENROLLING		"FPSTATE_ENROLLING"
#define FPD_STATE_IDENTIFYING	"FPSTATE_IDENTIFYING"

extern GMainLoop *event_loop;

struct fingerprint_service {
	LSHandle *handle;
	struct fpd_client *client;

	/* The most recent ErrorInfo, so that the terminal Failed signal can
	 * report what actually went wrong (fpd sends the reason first) */
	gchar *last_error;
};

/* Keeps a message alive across an asynchronous fpd call */
struct fingerprint_request {
	LSHandle *handle;
	LSMessage *message;
	bool subscribed;
};

static struct fingerprint_request *fingerprint_request_new(LSHandle *handle,
                                                           LSMessage *message,
                                                           bool subscribed)
{
	struct fingerprint_request *req = g_new0(struct fingerprint_request, 1);

	req->handle = handle;
	req->message = message;
	req->subscribed = subscribed;
	LSMessageRef(message);

	return req;
}

static void fingerprint_request_free(struct fingerprint_request *req)
{
	if (!req)
		return;

	LSMessageUnref(req->message);
	g_free(req);
}

/* The adapter's own reply vocabulary, kept because the LS2 API and its
 * clients speak it. biomd answers each call with a plain boolean, so the
 * client layer now only ever produces STARTED and FAILED; the remaining
 * codes are retained so existing error text stays addressable. */
static const char *fpd_reply_to_error_text(int reply)
{
	switch (reply) {
	case FPD_REPLY_FAILED:
		return "Operation failed";
	case FPD_REPLY_ALREADY_IDLE:
		return "No operation in progress";
	case FPD_REPLY_ALREADY_BUSY:
		return "Another fingerprint operation is in progress";
	case FPD_REPLY_DENIED:
		return "Operation denied";
	case FPD_REPLY_KEY_ALREADY_EXISTS:
		return "A fingerprint with that name already exists";
	case FPD_REPLY_KEY_DOES_NOT_EXIST:
		return "No fingerprint with that name exists";
	case FPD_REPLY_NO_KEYS_AVAILABLE:
		return "No fingerprints are enrolled";
	case FPD_REPLY_KEY_IS_INVALID:
		return "Invalid fingerprint name";
	default:
		return "Operation failed";
	}
}

/*
 * Reply builders. getStatus is subscribable, so the same body is used for the
 * direct reply and for subscription updates.
 */

static jvalue_ref build_status(struct fingerprint_service *service)
{
	struct fpd_client *client = service->client;
	jvalue_ref reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("available"),
	            jboolean_create(fpd_client_is_available(client)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("state"),
	            jstring_create(fpd_client_get_state(client)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("fingerprints"),
	            fpd_client_get_fingerprints_json(client));

	return reply_obj;
}

static void state_changed_cb(struct fpd_client *client, void *user_data)
{
	struct fingerprint_service *service = user_data;
	const char *state = fpd_client_get_state(client);
	jvalue_ref reply_obj;

	/* Fresh operation: drop any error left over from the previous one. */
	if (!g_strcmp0(state, FPD_STATE_ENROLLING) ||
	    !g_strcmp0(state, FPD_STATE_IDENTIFYING)) {
		g_free(service->last_error);
		service->last_error = NULL;
	}

	reply_obj = build_status(service);
	luna_service_post_subscription(service->handle, "/", "getStatus", reply_obj);
	j_release(&reply_obj);
}

/*
 * Progress events. Which LS2 subscription channel an event belongs to follows
 * from the daemon state: fpd emits the terminal Added/Identified/Failed
 * signals before it flips back to FPSTATE_IDLE, so the cached state still
 * names the operation they belong to.
 */

static void post_finished_error(struct fingerprint_service *service,
                                const char *method, gboolean identify,
                                const char *error_text)
{
	jvalue_ref reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("finished"), jboolean_create(true));
	if (identify)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("identified"), jboolean_create(false));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create(error_text));

	luna_service_post_subscription(service->handle, "/", method, reply_obj);
	j_release(&reply_obj);
}

/*
 * A single rejected touch while identifying. fpd keeps the sensor armed and
 * says FINGER_NOT_RECOGNIZED per capture rather than ending the operation, so
 * this is posted as a NON-terminal miss - the client shows feedback and counts
 * it, but the subscription stays open and no re-arm is needed.
 */
static void post_identify_miss(struct fingerprint_service *service)
{
	jvalue_ref reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("identified"), jboolean_create(false));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("finished"), jboolean_create(false));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"),
	            jstring_create("FINGER_NOT_RECOGNIZED"));

	luna_service_post_subscription(service->handle, "/", "identify", reply_obj);
	j_release(&reply_obj);
}

static void event_cb(struct fpd_client *client, enum fpd_event event,
                     const char *finger_or_info, int progress, void *user_data)
{
	struct fingerprint_service *service = user_data;
	const char *state = fpd_client_get_state(client);
	gboolean enrolling = !g_strcmp0(state, FPD_STATE_ENROLLING);
	gboolean identifying = !g_strcmp0(state, FPD_STATE_IDENTIFYING);
	jvalue_ref reply_obj;

	switch (event) {
	case FPD_EVENT_ENROLL_PROGRESS:
		reply_obj = jobject_create();
		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("progress"), jnumber_create_i32(progress));
		luna_service_post_subscription(service->handle, "/", "enroll", reply_obj);
		j_release(&reply_obj);
		break;

	case FPD_EVENT_ACQUISITION_INFO:
		if (!enrolling && !identifying)
			break;

		reply_obj = jobject_create();
		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("acquisitionInfo"),
		            jstring_create(finger_or_info));
		luna_service_post_subscription(service->handle, "/",
		                               enrolling ? "enroll" : "identify", reply_obj);
		j_release(&reply_obj);
		break;

	case FPD_EVENT_ERROR_INFO:
		g_free(service->last_error);
		service->last_error = g_strdup(finger_or_info);
		/* fpd reports each non-matching capture as FINGER_NOT_RECOGNIZED and
		 * stays armed; surface it so the lockscreen gives per-touch feedback. */
		if (identifying && !g_strcmp0(finger_or_info, "FINGER_NOT_RECOGNIZED"))
			post_identify_miss(service);
		break;

	case FPD_EVENT_ADDED:
		reply_obj = jobject_create();
		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("finished"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("finger"), jstring_create(finger_or_info));
		luna_service_post_subscription(service->handle, "/", "enroll", reply_obj);
		j_release(&reply_obj);

		g_free(service->last_error);
		service->last_error = NULL;
		break;

	case FPD_EVENT_IDENTIFIED:
		reply_obj = jobject_create();
		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("finished"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("identified"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("finger"), jstring_create(finger_or_info));
		luna_service_post_subscription(service->handle, "/", "identify", reply_obj);
		j_release(&reply_obj);

		g_free(service->last_error);
		service->last_error = NULL;
		break;

	case FPD_EVENT_FAILED:
		if (enrolling)
			post_finished_error(service, "enroll", FALSE,
			                    service->last_error ? service->last_error : "Enrollment failed");
		else if (identifying)
			post_finished_error(service, "identify", TRUE, "Aborted");

		g_free(service->last_error);
		service->last_error = NULL;
		break;

	case FPD_EVENT_ABORTED:
		/* Cancel/timeout: always "Aborted" so the client never counts it as a
		 * failed read, whatever the last ErrorInfo happened to be. */
		if (enrolling)
			post_finished_error(service, "enroll", FALSE, "Aborted");
		else if (identifying)
			post_finished_error(service, "identify", TRUE, "Aborted");

		g_free(service->last_error);
		service->last_error = NULL;
		break;

	case FPD_EVENT_REMOVED:
	case FPD_EVENT_VERIFIED:
		/* The list update reaches getStatus subscribers via ListChanged */
		break;
	}
}

/*
 * Methods
 */

static bool _service_get_status_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;
	jvalue_ref reply_obj;
	bool subscribed;

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	reply_obj = build_status(service);
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	luna_service_message_validate_and_send(handle, message, reply_obj);

	j_release(&reply_obj);

	return true;
}

static bool _service_get_fingerprints_cb(LSHandle *handle, LSMessage *message,
                                         void *user_data)
{
	struct fingerprint_service *service = user_data;
	jvalue_ref reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("fingerprints"),
	            fpd_client_get_fingerprints_json(service->client));

	luna_service_message_validate_and_send(handle, message, reply_obj);

	j_release(&reply_obj);

	return true;
}

/* Pulls a string field out of the request, or NULL when it isn't a string */
static gchar *get_string_param(jvalue_ref parsed_obj, const char *name)
{
	jvalue_ref value_obj = NULL;
	raw_buffer buf;

	if (!jobject_get_exists(parsed_obj, j_cstr_to_buffer(name), &value_obj))
		return NULL;

	if (!jis_string(value_obj))
		return NULL;

	buf = jstring_get_fast(value_obj);

	return g_strndup(buf.m_str, buf.m_len);
}

static void started_reply_cb(int reply, const char *error_text, void *user_data)
{
	struct fingerprint_request *req = user_data;

	if (reply == FPD_REPLY_STARTED) {
		jvalue_ref reply_obj = jobject_create();

		jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"),
		            jboolean_create(req->subscribed));

		luna_service_message_validate_and_send(req->handle, req->message, reply_obj);
		j_release(&reply_obj);
	} else {
		luna_service_message_reply_custom_error(req->handle, req->message,
			error_text ? error_text : fpd_reply_to_error_text(reply));
	}

	fingerprint_request_free(req);
}

static bool _service_enroll_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;
	jvalue_ref parsed_obj = NULL;
	gchar *finger = NULL;
	bool subscribed;

	parsed_obj = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed_obj) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	finger = get_string_param(parsed_obj, "finger");
	j_release(&parsed_obj);

	if (!finger || !strlen(finger)) {
		luna_service_message_reply_error_invalid_params(handle, message);
		g_free(finger);
		return true;
	}

	/* Enrollment takes many touches: the progress and the result come as
	 * updates on this subscription */
	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	fpd_client_enroll(service->client, finger, started_reply_cb,
	                  fingerprint_request_new(handle, message, subscribed));

	g_free(finger);

	return true;
}

static bool _service_identify_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;
	bool subscribed;

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	fpd_client_identify(service->client, started_reply_cb,
	                    fingerprint_request_new(handle, message, subscribed));

	return true;
}

static void abort_reply_cb(int reply, const char *error_text, void *user_data)
{
	struct fingerprint_request *req = user_data;

	/* Aborting while nothing is running is as aborted as it gets */
	if (reply == FPD_REPLY_STARTED || reply == FPD_REPLY_ALREADY_IDLE)
		luna_service_message_reply_success(req->handle, req->message);
	else
		luna_service_message_reply_custom_error(req->handle, req->message,
			error_text ? error_text : fpd_reply_to_error_text(reply));

	fingerprint_request_free(req);
}

static bool _service_abort_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;

	fpd_client_abort(service->client, abort_reply_cb,
	                 fingerprint_request_new(handle, message, false));

	return true;
}

static void simple_reply_cb(int reply, const char *error_text, void *user_data)
{
	struct fingerprint_request *req = user_data;

	if (reply == FPD_REPLY_STARTED)
		luna_service_message_reply_success(req->handle, req->message);
	else
		luna_service_message_reply_custom_error(req->handle, req->message,
			error_text ? error_text : fpd_reply_to_error_text(reply));

	fingerprint_request_free(req);
}

static bool _service_remove_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;
	jvalue_ref parsed_obj = NULL;
	gchar *finger = NULL;

	parsed_obj = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed_obj) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	finger = get_string_param(parsed_obj, "finger");
	j_release(&parsed_obj);

	if (!finger || !strlen(finger)) {
		luna_service_message_reply_error_invalid_params(handle, message);
		g_free(finger);
		return true;
	}

	fpd_client_remove(service->client, finger, simple_reply_cb,
	                  fingerprint_request_new(handle, message, false));

	g_free(finger);

	return true;
}

static bool _service_rename_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;
	jvalue_ref parsed_obj = NULL;
	gchar *finger = NULL;
	gchar *new_name = NULL;

	parsed_obj = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed_obj) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	finger = get_string_param(parsed_obj, "finger");
	new_name = get_string_param(parsed_obj, "newName");
	j_release(&parsed_obj);

	if (!finger || !strlen(finger) || !new_name || !strlen(new_name)) {
		luna_service_message_reply_error_invalid_params(handle, message);
		g_free(finger);
		g_free(new_name);
		return true;
	}

	fpd_client_rename(service->client, finger, new_name, simple_reply_cb,
	                  fingerprint_request_new(handle, message, false));

	g_free(finger);
	g_free(new_name);

	return true;
}

static bool _service_clear_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct fingerprint_service *service = user_data;

	fpd_client_clear(service->client, simple_reply_cb,
	                 fingerprint_request_new(handle, message, false));

	return true;
}

static LSMethod _fingerprint_service_methods[] = {
	{ "getStatus", _service_get_status_cb },
	{ "getFingerprints", _service_get_fingerprints_cb },
	{ "enroll", _service_enroll_cb },
	{ "identify", _service_identify_cb },
	{ "abort", _service_abort_cb },
	{ "remove", _service_remove_cb },
	{ "rename", _service_rename_cb },
	{ "clear", _service_clear_cb },
	{ NULL, NULL },
};

struct fingerprint_service *fingerprint_service_create(void)
{
	struct fingerprint_service *service;
	LSError error;

	LSErrorInit(&error);

	service = g_new0(struct fingerprint_service, 1);

	if (!LSRegister(FINGERPRINT_SERVICE_NAME, &service->handle, &error)) {
		g_critical("Failed to register %s: %s", FINGERPRINT_SERVICE_NAME, error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSGmainAttach(service->handle, event_loop, &error)) {
		g_critical("Failed to attach %s to the main loop: %s", FINGERPRINT_SERVICE_NAME,
		           error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSRegisterCategory(service->handle, "/", _fingerprint_service_methods, NULL,
	                        NULL, &error)) {
		g_critical("Failed to register the service category: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSCategorySetData(service->handle, "/", service, &error)) {
		g_critical("Failed to set the service category data: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	service->client = fpd_client_create(state_changed_cb, event_cb, service);

	return service;

failed:
	if (service->handle) {
		LSError unregister_error;

		LSErrorInit(&unregister_error);

		if (!LSUnregister(service->handle, &unregister_error))
			LSErrorFree(&unregister_error);
	}

	g_free(service);

	return NULL;
}

void fingerprint_service_free(struct fingerprint_service *service)
{
	LSError error;

	if (!service)
		return;

	LSErrorInit(&error);

	if (service->client)
		fpd_client_free(service->client);

	if (service->handle && !LSUnregister(service->handle, &error)) {
		LSErrorPrint(&error, stderr);
		LSErrorFree(&error);
	}

	g_free(service->last_error);
	g_free(service);
}

// vim:ts=4:sw=4:noexpandtab
