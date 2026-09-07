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

#include "biomd-interface.h"
#include "fpd_client.h"

#define BIOMD_SERVICE		"io.FuriOS.Biomd"
#define BIOMD_OBJECT_PATH	"/io/FuriOS/Biomd/Fingerprint"

#define FPD_STATE_UNKNOWN		"FPSTATE_UNKNOWN"
#define FPD_STATE_IDLE			"FPSTATE_IDLE"
#define FPD_STATE_ENROLLING		"FPSTATE_ENROLLING"
#define FPD_STATE_IDENTIFYING	"FPSTATE_IDENTIFYING"

/* biomd's BiometricState */
enum biomd_state {
	BIOMD_STATE_IDLE = 0,
	BIOMD_STATE_ENROLLING = 1,
	BIOMD_STATE_IDENTIFYING = 2,
};

/* biomd's BiometricError */
enum biomd_error {
	BIOMD_ERROR_NONE = 0,
	BIOMD_ERROR_CANCELED = 5,
	BIOMD_ERROR_FINGER_NOT_RECOGNIZED = 9,
};

/*
 * biomd reports the terminal outcome of an operation and the return to idle as
 * two separate signals, and not always in that order: a cancel arrives as
 * StateChanged(IDLE) first and ErrorInfoChanged(CANCELED) after it. fpd emitted
 * its terminal signal while still in the operation's state, and
 * fingerprint_service.c relies on that - it decides which subscription an event
 * belongs to from fpd_client_get_state().
 *
 * So the operation is tracked here rather than read back from the daemon:
 * active_op is set when Enroll/Identify is accepted and cleared only when a
 * terminal event has been handed to the service, and get_state() reports it in
 * preference to the daemon's own state. The daemon state is still tracked and
 * still drives getStatus, it just does not decide event routing.
 */
enum active_op {
	OP_NONE = 0,
	OP_ENROLL,
	OP_IDENTIFY,
};

/* If the daemon goes idle and no terminal signal follows, give up waiting after
 * this long and abort the operation ourselves, so a subscriber can never be
 * left hanging on a request that will never complete. */
#define TERMINAL_GRACE_MS 250

struct fpd_client {
	guint daemon_watch;

	BiomdInterfaceFingerprint *daemon;

	/* Cancelled when the client goes away, so in-flight calls don't come
	 * back to freed memory */
	GCancellable *cancellable;

	gboolean available;
	gint state;
	gchar **fingers;

	enum active_op active_op;
	guint terminal_grace_id;

	fpd_state_cb state_cb;
	fpd_event_cb event_cb;
	void *user_data;
};

/* Keeps a completion callback alive across an asynchronous biomd call */
struct fpd_req {
	struct fpd_client *client;
	fpd_reply_cb cb;
	void *user_data;

	/* Which operation this call claimed, so it can be given back if the
	 * daemon refuses; OP_NONE for calls that do not start one */
	enum active_op op_started;

	/* fpd_client_abort only: which of the two stop methods was called, so
	 * the reply is finished with the matching one */
	gboolean stop_enroll;

	/* fpd_client_clear only: how many RemoveFinger calls are still out, and
	 * the first failure seen, so the caller gets exactly one reply */
	guint outstanding;
	gint failed_reply;
	gchar *failed_text;
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
 * Enum translation. The service and its LS2 clients speak fpd's string
 * vocabulary, so biomd's integer codes are mapped back onto it.
 */

static const char *error_to_string(gint error)
{
	switch (error) {
	case 1: return "FPERROR_HW_UNAVAILABLE";
	case 2: return "FPERROR_UNABLE_TO_PROCESS";
	case 3: return "FPERROR_TIMEOUT";
	case 4: return "FPERROR_NO_SPACE";
	case 5: return "FPERROR_CANCELED";
	case 6: return "FPERROR_UNABLE_TO_REMOVE";
	case 7: return "FPERROR_LOCKOUT";
	case 9: return "FINGER_NOT_RECOGNIZED";
	default: return "FPERROR_GENERAL";
	}
}

/* NULL for codes that carry no information worth forwarding */
static const char *acquisition_to_string(gint info)
{
	switch (info) {
	case 1: return "FPACQUIRED_GOOD";
	case 2: return "FPACQUIRED_PARTIAL";
	case 3: return "FPACQUIRED_INSUFFICIENT";
	case 4: return "FPACQUIRED_IMAGER_DIRTY";
	case 5: return "FPACQUIRED_TOO_SLOW";
	case 6: return "FPACQUIRED_TOO_FAST";
	default: return NULL;
	}
}

/*
 * Operation tracking
 */

static void cancel_terminal_grace(struct fpd_client *client)
{
	if (client->terminal_grace_id) {
		g_source_remove(client->terminal_grace_id);
		client->terminal_grace_id = 0;
	}
}

/* Dispatch a terminal event and end the operation it belongs to */
static void finish_op(struct fpd_client *client, enum fpd_event event,
                      const char *finger_or_info)
{
	cancel_terminal_grace(client);

	if (client->active_op == OP_NONE)
		return;

	/* Cleared after the event so get_state() still names the operation
	 * while the service is routing it */
	notify_event(client, event, finger_or_info, 0);
	client->active_op = OP_NONE;

	notify_state(client);
}

static gboolean terminal_grace_expired(gpointer user_data)
{
	struct fpd_client *client = user_data;

	client->terminal_grace_id = 0;

	if (client->active_op != OP_NONE) {
		g_debug("biomd went idle without a terminal signal, aborting");
		finish_op(client, FPD_EVENT_ABORTED, NULL);
	}

	return G_SOURCE_REMOVE;
}

/*
 * Daemon state and finger list
 */

static void set_fingers(struct fpd_client *client, const gchar *const *fingers)
{
	gchar **old = client->fingers;
	guint n;

	client->fingers = g_strdupv((gchar **) fingers);

	/* An enrollment completing shows up as a new name in the list; biomd has
	 * no equivalent of fpd's Added signal, so synthesise it from the diff. */
	if (client->fingers) {
		for (n = 0; client->fingers[n]; n++) {
			if (old && g_strv_contains((const gchar *const *) old, client->fingers[n]))
				continue;

			if (client->active_op == OP_ENROLL)
				finish_op(client, FPD_EVENT_ADDED, client->fingers[n]);
		}
	}

	if (old) {
		for (n = 0; old[n]; n++) {
			if (client->fingers &&
			    g_strv_contains((const gchar *const *) client->fingers, old[n]))
				continue;

			notify_event(client, FPD_EVENT_REMOVED, old[n], 0);
		}
	}

	g_strfreev(old);

	notify_state(client);
}

/*
 * Signals
 */

static void on_state_changed(BiomdInterfaceFingerprint *daemon, gint state,
                             gpointer user_data)
{
	struct fpd_client *client = user_data;

	client->state = state;

	/* The terminal signal for this operation may still be on its way, so do
	 * not end the operation here - just make sure it cannot hang forever. */
	if (state == BIOMD_STATE_IDLE && client->active_op != OP_NONE &&
	    !client->terminal_grace_id)
		client->terminal_grace_id = g_timeout_add(TERMINAL_GRACE_MS,
		                                          terminal_grace_expired, client);

	notify_state(client);
}

static void on_identified(BiomdInterfaceFingerprint *daemon, const gchar *finger,
                          gpointer user_data)
{
	struct fpd_client *client = user_data;

	finish_op(client, FPD_EVENT_IDENTIFIED, finger);
}

static void on_enrollment_progress_changed(BiomdInterfaceFingerprint *daemon,
                                           gint progress, gpointer user_data)
{
	struct fpd_client *client = user_data;

	notify_event(client, FPD_EVENT_ENROLL_PROGRESS, NULL, progress);
}

static void on_enrolled_fingers_changed(BiomdInterfaceFingerprint *daemon,
                                        const gchar *const *fingers,
                                        gpointer user_data)
{
	struct fpd_client *client = user_data;

	set_fingers(client, fingers);
}

static void on_acquisition_info_changed(BiomdInterfaceFingerprint *daemon, gint info,
                                        gpointer user_data)
{
	struct fpd_client *client = user_data;
	const char *str = acquisition_to_string(info);

	if (str)
		notify_event(client, FPD_EVENT_ACQUISITION_INFO, str, 0);
}

static void on_error_info_changed(BiomdInterfaceFingerprint *daemon, gint error,
                                  gpointer user_data)
{
	struct fpd_client *client = user_data;
	const char *str;

	if (error == BIOMD_ERROR_NONE)
		return;

	str = error_to_string(error);

	/* A rejected touch while identifying: biomd keeps the sensor armed, so
	 * this is progress, not an outcome. The service turns it into a
	 * non-terminal miss. */
	if (error == BIOMD_ERROR_FINGER_NOT_RECOGNIZED) {
		notify_event(client, FPD_EVENT_ERROR_INFO, str, 0);
		return;
	}

	/* A cancel is reported as "Aborted" rather than as a failed read, which
	 * is what FPD_EVENT_ABORTED means to the service. */
	if (error == BIOMD_ERROR_CANCELED) {
		finish_op(client, FPD_EVENT_ABORTED, NULL);
		return;
	}

	/* Record the reason first: the service quotes the last ErrorInfo when it
	 * reports the failure. */
	notify_event(client, FPD_EVENT_ERROR_INFO, str, 0);
	finish_op(client, FPD_EVENT_FAILED, NULL);
}

/*
 * Bus name watching
 */

static void daemon_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_client *client = user_data;
	GError *error = NULL;
	BiomdInterfaceFingerprint *daemon;
	const gchar *const *fingers;

	daemon = biomd_interface_fingerprint_proxy_new_for_bus_finish(res, &error);

	if (!daemon) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning("Failed to create biomd proxy: %s", error->message);
		g_error_free(error);
		return;
	}

	if (client->daemon)
		g_object_unref(client->daemon);

	client->daemon = daemon;

	g_signal_connect(daemon, "state-changed", G_CALLBACK(on_state_changed), client);
	g_signal_connect(daemon, "identified", G_CALLBACK(on_identified), client);
	g_signal_connect(daemon, "enrollment-progress-changed",
	                 G_CALLBACK(on_enrollment_progress_changed), client);
	g_signal_connect(daemon, "enrolled-fingers-changed",
	                 G_CALLBACK(on_enrolled_fingers_changed), client);
	g_signal_connect(daemon, "acquisition-info-changed",
	                 G_CALLBACK(on_acquisition_info_changed), client);
	g_signal_connect(daemon, "error-info-changed",
	                 G_CALLBACK(on_error_info_changed), client);

	/* The proxy fetched every property while it was being created, so the
	 * daemon is usable as soon as it exists - there is no equivalent of fpd's
	 * GetState round trip to wait for. */
	client->state = biomd_interface_fingerprint_get_state(daemon);
	client->available = biomd_interface_fingerprint_get_hardware_available(daemon);

	fingers = biomd_interface_fingerprint_get_enrolled_fingers(daemon);
	set_fingers(client, fingers);
}

static void daemon_appeared(GDBusConnection *connection, const gchar *name,
                            const gchar *name_owner, gpointer user_data)
{
	struct fpd_client *client = user_data;

	g_message("biomd appeared on the bus");

	biomd_interface_fingerprint_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
	                                              G_DBUS_PROXY_FLAGS_NONE,
	                                              BIOMD_SERVICE, BIOMD_OBJECT_PATH,
	                                              client->cancellable,
	                                              daemon_proxy_ready, client);
}

static void daemon_vanished(GDBusConnection *connection, const gchar *name,
                            gpointer user_data)
{
	struct fpd_client *client = user_data;

	g_message("biomd disappeared from the bus");

	/* Anything in flight died with it */
	if (client->active_op != OP_NONE)
		finish_op(client, FPD_EVENT_ABORTED, NULL);

	cancel_terminal_grace(client);

	if (client->daemon) {
		g_object_unref(client->daemon);
		client->daemon = NULL;
	}

	client->available = FALSE;
	client->state = BIOMD_STATE_IDLE;

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

	client->daemon_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, BIOMD_SERVICE,
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

	cancel_terminal_grace(client);

	if (client->daemon_watch)
		g_bus_unwatch_name(client->daemon_watch);

	if (client->daemon)
		g_object_unref(client->daemon);

	g_object_unref(client->cancellable);
	g_strfreev(client->fingers);
	g_free(client);
}

gboolean fpd_client_is_available(struct fpd_client *client)
{
	return client->available;
}

const char *fpd_client_get_state(struct fpd_client *client)
{
	if (!client->daemon)
		return FPD_STATE_UNKNOWN;

	/* An operation that has not delivered its terminal event yet still owns
	 * the state, whatever the daemon has moved on to. */
	switch (client->active_op) {
	case OP_ENROLL:
		return FPD_STATE_ENROLLING;
	case OP_IDENTIFY:
		return FPD_STATE_IDENTIFYING;
	case OP_NONE:
		break;
	}

	switch (client->state) {
	case BIOMD_STATE_ENROLLING:
		return FPD_STATE_ENROLLING;
	case BIOMD_STATE_IDENTIFYING:
		return FPD_STATE_IDENTIFYING;
	case BIOMD_STATE_IDLE:
		return FPD_STATE_IDLE;
	default:
		return FPD_STATE_UNKNOWN;
	}
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
 * Method calls. biomd answers with a plain boolean rather than fpd's fpreply
 * code, so success becomes FPD_REPLY_STARTED and a refusal FPD_REPLY_FAILED.
 * The D-Bus level failing (biomd crashed mid-call, request timed out, or it
 * rejected the request outright with an error) is reported as -1 with the
 * GError text, as before.
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

static void reply_call_ready(struct fpd_req *req, gboolean called, gboolean success)
{
	/* The operation is claimed before the call goes out, because biomd can
	 * signal against it before the method reply gets back to us. Hand it back
	 * if the daemon refused or never answered. */
	if ((!called || !success) && req->op_started != OP_NONE &&
	    req->client->active_op == req->op_started)
		req->client->active_op = OP_NONE;

	if (called)
		req->cb(success ? FPD_REPLY_STARTED : FPD_REPLY_FAILED, NULL, req->user_data);

	g_free(req);
}

/* Same, for a call that failed at the D-Bus level */
static void reply_call_error(struct fpd_req *req, GError *error)
{
	if (req->op_started != OP_NONE && req->client->active_op == req->op_started)
		req->client->active_op = OP_NONE;

	req->cb(-1, error->message, req->user_data);

	g_error_free(error);
	g_free(req);
}

static void reply_call_done(struct fpd_req *req, gboolean called, gboolean success,
                            GError *error)
{
	if (!called)
		reply_call_error(req, error);
	else
		reply_call_ready(req, called, success);
}

static void enroll_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	gboolean called;

	called = biomd_interface_fingerprint_call_enroll_finish(
		BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);

	reply_call_done(req, called, success, error);
}

void fpd_client_enroll(struct fpd_client *client, const char *finger,
                       fpd_reply_cb cb, void *user_data)
{
	struct fpd_req *req;

	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	req = fpd_req_new(client, cb, user_data);
	req->op_started = OP_ENROLL;
	client->active_op = OP_ENROLL;

	biomd_interface_fingerprint_call_enroll(client->daemon, finger,
	                                        client->cancellable, enroll_ready, req);
}

static void identify_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	gboolean called;

	called = biomd_interface_fingerprint_call_identify_finish(
		BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);

	reply_call_done(req, called, success, error);
}

void fpd_client_identify(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	struct fpd_req *req;

	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	req = fpd_req_new(client, cb, user_data);
	req->op_started = OP_IDENTIFY;
	client->active_op = OP_IDENTIFY;

	biomd_interface_fingerprint_call_identify(client->daemon, client->cancellable,
	                                          identify_ready, req);
}

/*
 * biomd has no separate Verify: Identify already answers "which of the enrolled
 * fingers is this, if any", which is what the caller wants either way. fpd's
 * Verify existed to check against the currently selected finger only, and
 * nothing in the service depends on that distinction.
 */
void fpd_client_verify(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	fpd_client_identify(client, cb, user_data);
}

static void stop_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	gboolean called;

	/* Which method was called, not what is running now: the operation may
	 * already have ended by the time the reply gets back. */
	if (req->stop_enroll)
		called = biomd_interface_fingerprint_call_stop_enroll_finish(
			BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);
	else
		called = biomd_interface_fingerprint_call_stop_identify_finish(
			BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);

	reply_call_done(req, called, success, error);
}

/*
 * fpd had one Abort for both operations; biomd splits it, so pick by what is
 * running. The ErrorInfoChanged(CANCELED) that follows is what actually ends
 * the operation for subscribers.
 */
void fpd_client_abort(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	struct fpd_req *req;
	gboolean enrolling;

	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	enrolling = (client->active_op == OP_ENROLL) ||
	            (client->active_op == OP_NONE && client->state == BIOMD_STATE_ENROLLING);

	req = fpd_req_new(client, cb, user_data);
	req->stop_enroll = enrolling;

	if (enrolling)
		biomd_interface_fingerprint_call_stop_enroll(client->daemon,
		                                             client->cancellable, stop_ready, req);
	else
		biomd_interface_fingerprint_call_stop_identify(client->daemon,
		                                               client->cancellable, stop_ready, req);
}

static void remove_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	gboolean called;

	called = biomd_interface_fingerprint_call_remove_finger_finish(
		BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);

	reply_call_done(req, called, success, error);
}

void fpd_client_remove(struct fpd_client *client, const char *finger,
                       fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	biomd_interface_fingerprint_call_remove_finger(client->daemon, finger,
	                                               client->cancellable, remove_ready,
	                                               fpd_req_new(client, cb, user_data));
}

static void rename_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	gboolean called;

	called = biomd_interface_fingerprint_call_rename_finger_finish(
		BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);

	reply_call_done(req, called, success, error);
}

void fpd_client_rename(struct fpd_client *client, const char *finger,
                       const char *new_name, fpd_reply_cb cb, void *user_data)
{
	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	biomd_interface_fingerprint_call_rename_finger(client->daemon, finger, new_name,
	                                               client->cancellable, rename_ready,
	                                               fpd_req_new(client, cb, user_data));
}

/*
 * biomd has no Clear, so this removes the enrolled fingers one by one and
 * answers once the last one has come back. The first failure is what gets
 * reported; the remaining calls are still allowed to finish.
 */

static void clear_one_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct fpd_req *req = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	gboolean called;

	called = biomd_interface_fingerprint_call_remove_finger_finish(
		BIOMD_INTERFACE_FINGERPRINT(source), &success, res, &error);

	if (req->failed_reply == FPD_REPLY_STARTED) {
		if (!called) {
			req->failed_reply = -1;
			req->failed_text = g_strdup(error->message);
		} else if (!success) {
			req->failed_reply = FPD_REPLY_FAILED;
		}
	}

	if (error)
		g_error_free(error);

	if (--req->outstanding > 0)
		return;

	req->cb(req->failed_reply, req->failed_text, req->user_data);

	g_free(req->failed_text);
	g_free(req);
}

void fpd_client_clear(struct fpd_client *client, fpd_reply_cb cb, void *user_data)
{
	struct fpd_req *req;
	guint n;

	if (!client->daemon) {
		cb(-1, "Fingerprint daemon not available", user_data);
		return;
	}

	if (!client->fingers || !client->fingers[0]) {
		cb(FPD_REPLY_STARTED, NULL, user_data);
		return;
	}

	req = fpd_req_new(client, cb, user_data);
	req->failed_reply = FPD_REPLY_STARTED;

	for (n = 0; client->fingers[n]; n++)
		req->outstanding++;

	/* Counted first: a call can complete before the loop below ends, and
	 * outstanding reaching zero early would answer the caller twice. */
	for (n = 0; client->fingers[n]; n++)
		biomd_interface_fingerprint_call_remove_finger(client->daemon,
		                                               client->fingers[n],
		                                               client->cancellable,
		                                               clear_one_ready, req);
}

// vim:ts=4:sw=4:noexpandtab
