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

#include <string.h>
#include <gio/gio.h>

#include "fpd-interface.h"
#include "fpd_client.h"

#define FPD_SERVICE		"org.droidian.fingerprint"
#define FPD_OBJECT_PATH	"/org/droidian/fingerprint"

#define FPD_STATE_UNKNOWN	"FPSTATE_UNKNOWN"

struct fpd_client {
	guint daemon_watch;

	FpdInterfaceFingerprint *daemon;

	/* Cancelled when the client goes away, so in-flight calls don't come
	 * back to freed memory */
	GCancellable *cancellable;

	gboolean available;
	gchar *state;
	gchar **fingers;

	fpd_state_cb state_cb;
	fpd_event_cb event_cb;
	void *user_data;
};

/* Keeps a completion callback alive across an asynchronous fpd call */
struct fpd_req {
	struct fpd_client *client;
	fpd_reply_cb cb;
	void *user_data;
};

static void notify_state(struct fpd_client *client)
{
	if (client->state_cb)
		client->state_cb(client, client->user_data);
}

static void notify_event(struct fpd_client *client, enum fpd_event event,
                         const char *finger_or_info, int progress)
{
	if (client->event_cb)
		client->event_cb(client, event, finger_or_info, progress, client->user_data);
}

/*
 * Daemon state and finger list
 */

static void get_all_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_client *client = user_data;
	FpdInterfaceFingerprint *daemon = FPD_INTERFACE_FINGERPRINT(source);
	GError *error = NULL;
	gchar **fingers = NULL;

	if (!fpd_interface_fingerprint_call_get_all_finish(daemon, &fingers, res, &error)) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning("Failed to query enrolled fingerprints: %s", error->message);
		g_error_free(error);
		return;
	}

	g_strfreev(client->fingers);
	client->fingers = fingers;

	notify_state(client);
}

static void refresh_fingers(struct fpd_client *client)
{
	if (!client->daemon)
		return;

	fpd_interface_fingerprint_call_get_all(client->daemon, client->cancellable,
	                                       get_all_ready, client);
}

static void get_state_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_client *client = user_data;
	FpdInterfaceFingerprint *daemon = FPD_INTERFACE_FINGERPRINT(source);
	GError *error = NULL;
	gchar *state = NULL;

	if (!fpd_interface_fingerprint_call_get_state_finish(daemon, &state, res, &error)) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning("Failed to query fpd state: %s", error->message);
		g_error_free(error);
		return;
	}

	g_free(client->state);
	client->state = state;
	client->available = TRUE;

	notify_state(client);
}

/*
 * Signals
 */

static void on_added(FpdInterfaceFingerprint *daemon, const gchar *finger,
                     gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_ADDED, finger, 0);
}

static void on_removed(FpdInterfaceFingerprint *daemon, const gchar *finger,
                       gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_REMOVED, finger, 0);
}

static void on_identified(FpdInterfaceFingerprint *daemon, const gchar *finger,
                          gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_IDENTIFIED, finger, 0);
}

static void on_aborted(FpdInterfaceFingerprint *daemon, gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_ABORTED, NULL, 0);
}

static void on_failed(FpdInterfaceFingerprint *daemon, gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_FAILED, NULL, 0);
}

static void on_verified(FpdInterfaceFingerprint *daemon, gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_VERIFIED, NULL, 0);
}

static void on_state_changed(FpdInterfaceFingerprint *daemon, const gchar *state,
                             gpointer user_data)
{
	struct fpd_client *client = user_data;

	g_free(client->state);
	client->state = g_strdup(state);

	notify_state(client);
}

static void on_enroll_progress_changed(FpdInterfaceFingerprint *daemon, gint progress,
                                       gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_ENROLL_PROGRESS, NULL, progress);
}

static void on_acquisition_info(FpdInterfaceFingerprint *daemon, const gchar *info,
                                gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_ACQUISITION_INFO, info, 0);
}

static void on_error_info(FpdInterfaceFingerprint *daemon, const gchar *info,
                          gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_ERROR_INFO, info, 0);
}

static void on_list_changed(FpdInterfaceFingerprint *daemon, gpointer user_data)
{
	struct fpd_client *client = user_data;

	refresh_fingers(client);
}

/*
 * Bus name watching
 */

static void daemon_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_client *client = user_data;
	GError *error = NULL;
	FpdInterfaceFingerprint *daemon;

	daemon = fpd_interface_fingerprint_proxy_new_for_bus_finish(res, &error);

	if (!daemon) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning("Failed to create fpd proxy: %s", error->message);
		g_error_free(error);
		return;
	}

	if (client->daemon)
		g_object_unref(client->daemon);

	client->daemon = daemon;

	g_signal_connect(daemon, "added", G_CALLBACK(on_added), client);
	g_signal_connect(daemon, "removed", G_CALLBACK(on_removed), client);
	g_signal_connect(daemon, "identified", G_CALLBACK(on_identified), client);
	g_signal_connect(daemon, "aborted", G_CALLBACK(on_aborted), client);
	g_signal_connect(daemon, "failed", G_CALLBACK(on_failed), client);
	g_signal_connect(daemon, "verified", G_CALLBACK(on_verified), client);
	g_signal_connect(daemon, "state-changed", G_CALLBACK(on_state_changed), client);
	g_signal_connect(daemon, "enroll-progress-changed",
	                 G_CALLBACK(on_enroll_progress_changed), client);
	g_signal_connect(daemon, "acquisition-info",
	                 G_CALLBACK(on_acquisition_info), client);
	g_signal_connect(daemon, "error-info", G_CALLBACK(on_error_info), client);
	g_signal_connect(daemon, "list-changed", G_CALLBACK(on_list_changed), client);

	/* available flips to true once GetState answers */
	fpd_interface_fingerprint_call_get_state(daemon, client->cancellable,
	                                         get_state_ready, client);
	refresh_fingers(client);
}

static void daemon_appeared(GDBusConnection *connection, const gchar *name,
                            const gchar *name_owner, gpointer user_data)
{
	struct fpd_client *client = user_data;

	g_message("fpd appeared on the bus");

	fpd_interface_fingerprint_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
	                                            G_DBUS_PROXY_FLAGS_NONE,
	                                            FPD_SERVICE, FPD_OBJECT_PATH,
	                                            client->cancellable,
	                                            daemon_proxy_ready, client);
}

static void daemon_vanished(GDBusConnection *connection, const gchar *name,
                            gpointer user_data)
{
	struct fpd_client *client = user_data;

	g_message("fpd disappeared from the bus");

	if (client->daemon) {
		g_object_unref(client->daemon);
		client->daemon = NULL;
	}

	client->available = FALSE;

	g_free(client->state);
	client->state = NULL;

	g_strfreev(client->fingers);
	client->fingers = NULL;

	notify_state(client);
}

/*
 * Public interface
 */

struct fpd_client *fpd_client_create(fpd_state_cb state_cb, fpd_event_cb event_cb,
                                     void *user_data)
{
	struct fpd_client *client;

	client = g_new0(struct fpd_client, 1);
	client->state_cb = state_cb;
	client->event_cb = event_cb;
	client->user_data = user_data;
	client->cancellable = g_cancellable_new();

	client->daemon_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, FPD_SERVICE,
	                                        G_BUS_NAME_WATCHER_FLAGS_NONE,
	                                        daemon_appeared, daemon_vanished,
	                                        client, NULL);

	return client;
}

void fpd_client_free(struct fpd_client *client)
{
	if (!client)
		return;

	g_cancellable_cancel(client->cancellable);

	if (client->daemon_watch)
		g_bus_unwatch_name(client->daemon_watch);

	if (client->daemon)
		g_object_unref(client->daemon);

	g_object_unref(client->cancellable);
	g_strfreev(client->fingers);
	g_free(client->state);
	g_free(client);
}

gboolean fpd_client_is_available(struct fpd_client *client)
{
	return client->available;
}

const char *fpd_client_get_state(struct fpd_client *client)
{
	return client->state ? client->state : FPD_STATE_UNKNOWN;
}

jvalue_ref fpd_client_get_fingerprints_json(struct fpd_client *client)
{
	jvalue_ref arr = jarray_create(NULL);
	guint n;

	if (client->fingers) {
		for (n = 0; client->fingers[n]; n++)
			jarray_append(arr, jstring_create(client->fingers[n]));
	}

	return arr;
}

/*
 * Method calls. All of them complete with an fpreply code; the D-Bus level
 * failing (fpd crashed mid-call, request timed out) is reported as -1 with
 * the GError text.
 */

static struct fpd_req *fpd_req_new(struct fpd_client *client, fpd_reply_cb cb,
                                   void *user_data)
{
	struct fpd_req *req = g_new0(struct fpd_req, 1);

	req->client = client;
	req->cb = cb;
	req->user_data = user_data;

	return req;
}

static void reply_call_ready(struct fpd_req *req, gboolean success, gint reply,
                             GError *error)
{
	if (success)
		req->cb(reply, NULL, req->user_data);
	else
		req->cb(-1, error->message, req->user_data);

	if (error)
		g_error_free(error);

	g_free(req);
}

static void enroll_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gint reply = 0;
	gboolean success;

	success = fpd_interface_fingerprint_call_enroll_finish(
		FPD_INTERFACE_FINGERPRINT(source), &reply, res, &error);

	reply_call_ready(req, success, reply, error);
}

void fpd_client_enroll(struct fpd_client *client, const char *finger,
                       fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	fpd_interface_fingerprint_call_enroll(client->daemon, finger,
	                                      client->cancellable, enroll_ready,
	                                      fpd_req_new(client, cb, user_data));
}

static void identify_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gint reply = 0;
	gboolean success;

	success = fpd_interface_fingerprint_call_identify_finish(
		FPD_INTERFACE_FINGERPRINT(source), &reply, res, &error);

	reply_call_ready(req, success, reply, error);
}

void fpd_client_identify(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	fpd_interface_fingerprint_call_identify(client->daemon, client->cancellable,
	                                        identify_ready,
	                                        fpd_req_new(client, cb, user_data));
}

static void verify_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gint reply = 0;
	gboolean success;

	success = fpd_interface_fingerprint_call_verify_finish(
		FPD_INTERFACE_FINGERPRINT(source), &reply, res, &error);

	reply_call_ready(req, success, reply, error);
}

void fpd_client_verify(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	fpd_interface_fingerprint_call_verify(client->daemon, client->cancellable,
	                                      verify_ready,
	                                      fpd_req_new(client, cb, user_data));
}

static void abort_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gint reply = 0;
	gboolean success;

	success = fpd_interface_fingerprint_call_abort_finish(
		FPD_INTERFACE_FINGERPRINT(source), &reply, res, &error);

	reply_call_ready(req, success, reply, error);
}

void fpd_client_abort(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	fpd_interface_fingerprint_call_abort(client->daemon, client->cancellable,
	                                     abort_ready,
	                                     fpd_req_new(client, cb, user_data));
}

static void remove_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gint reply = 0;
	gboolean success;

	success = fpd_interface_fingerprint_call_remove_finish(
		FPD_INTERFACE_FINGERPRINT(source), &reply, res, &error);

	reply_call_ready(req, success, reply, error);
}

void fpd_client_remove(struct fpd_client *client, const char *finger,
                       fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	fpd_interface_fingerprint_call_remove(client->daemon, finger,
	                                      client->cancellable, remove_ready,
	                                      fpd_req_new(client, cb, user_data));
}

static void clear_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success;

	success = fpd_interface_fingerprint_call_clear_finish(
		FPD_INTERFACE_FINGERPRINT(source), res, &error);

	reply_call_ready(req, success, FPD_REPLY_STARTED, error);
}

void fpd_client_clear(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	fpd_interface_fingerprint_call_clear(client->daemon, client->cancellable,
	                                     clear_ready,
	                                     fpd_req_new(client, cb, user_data));
}

// vim:ts=4:sw=4:noexpandtab
