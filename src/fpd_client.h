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

#ifndef FPD_CLIENT_H_
#define FPD_CLIENT_H_

#include <glib.h>
#include <pbnjson.h>

/**
 * Client for the fingerprint daemon's D-Bus API, one bus name on the system
 * bus:
 *
 *   io.FuriOS.Biomd    FuriLabs' biomd, on /io/FuriOS/Biomd/Fingerprint
 *
 * The name is watched, so the daemon restarting or not being installed at all
 * is a normal state rather than an error: the client simply reports
 * unavailable and re-attaches when it comes back.
 *
 * This used to talk to droidian-fpd on org.droidian.fingerprint. That path is
 * dead on Halium here: fpd reaches the fingerprint HAL through libhybris, and
 * the shim it dlopens (libbiometry_fp_api.so) is not built into the Android
 * system image, so it segfaults on startup. biomd reaches the same HAL over
 * binder through libgbinder instead, needing nothing from the Android side.
 *
 * The names below are still fpd's. The interface this header describes is the
 * adapter's own, the service layer is written against it, and its vocabulary
 * (FPSTATE_*, FPERROR_*, FPACQUIRED_*, the fpreply codes) is what the LS2 API
 * and its clients already speak. Renaming it would churn the webOS-facing API
 * for nothing, so fpd_client.c maps biomd's integer enums onto it instead.
 */

struct fpd_client;

/* fpreply codes returned by the daemon's Enroll/Identify/Abort/Remove calls */
enum fpd_reply {
	FPD_REPLY_STARTED = 0,
	FPD_REPLY_FAILED = 1,
	FPD_REPLY_ALREADY_IDLE = 2,
	FPD_REPLY_ALREADY_BUSY = 3,
	FPD_REPLY_DENIED = 4,
	FPD_REPLY_KEY_ALREADY_EXISTS = 5,
	FPD_REPLY_KEY_DOES_NOT_EXIST = 6,
	FPD_REPLY_NO_KEYS_AVAILABLE = 7,
	FPD_REPLY_KEY_IS_INVALID = 8,
};

/* Operation progress events the daemon broadcasts while enrolling,
 * identifying or removing. The finger/info/progress arguments are only
 * meaningful for the events that carry them. */
enum fpd_event {
	FPD_EVENT_ADDED,			/* finger */
	FPD_EVENT_REMOVED,			/* finger */
	FPD_EVENT_IDENTIFIED,		/* finger */
	FPD_EVENT_ABORTED,
	FPD_EVENT_FAILED,
	FPD_EVENT_VERIFIED,
	FPD_EVENT_ENROLL_PROGRESS,	/* progress, 0-100 */
	FPD_EVENT_ACQUISITION_INFO,	/* info, FPACQUIRED_* string */
	FPD_EVENT_ERROR_INFO,		/* info, FPERROR_* or FINGER_NOT_RECOGNIZED */
};

/* The daemon availability, state string or finger list changed */
typedef void (*fpd_state_cb)(struct fpd_client *client, void *user_data);

/* An operation progress signal arrived */
typedef void (*fpd_event_cb)(struct fpd_client *client, enum fpd_event event,
                             const char *finger_or_info, int progress,
                             void *user_data);

/* Completion of a method call: reply is an fpd_reply code, or -1 when the
 * D-Bus call itself failed, in which case error_text says why */
typedef void (*fpd_reply_cb)(int reply, const char *error_text, void *user_data);

struct fpd_client *fpd_client_create(fpd_state_cb state_cb, fpd_event_cb event_cb,
                                     void *user_data);
void fpd_client_free(struct fpd_client *client);

/* True once fpd is running on the bus and answered our first GetState */
gboolean fpd_client_is_available(struct fpd_client *client);

/* FPSTATE_* string, "FPSTATE_UNKNOWN" while fpd is unavailable */
const char *fpd_client_get_state(struct fpd_client *client);

/* The enrolled finger names as a JSON array. The caller takes ownership. */
jvalue_ref fpd_client_get_fingerprints_json(struct fpd_client *client);

void fpd_client_enroll(struct fpd_client *client, const char *finger,
                       fpd_reply_cb cb, void *user_data);
void fpd_client_identify(struct fpd_client *client, fpd_reply_cb cb, void *user_data);
void fpd_client_verify(struct fpd_client *client, fpd_reply_cb cb, void *user_data);
void fpd_client_abort(struct fpd_client *client, fpd_reply_cb cb, void *user_data);
void fpd_client_remove(struct fpd_client *client, const char *finger,
                       fpd_reply_cb cb, void *user_data);
void fpd_client_rename(struct fpd_client *client, const char *finger,
                       const char *new_name, fpd_reply_cb cb, void *user_data);
/* Clear has no reply value on the wire; reply is 0 on delivery */
void fpd_client_clear(struct fpd_client *client, fpd_reply_cb cb, void *user_data);

#endif

// vim:ts=4:sw=4:noexpandtab
