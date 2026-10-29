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
#include <glib.h>
#include <pbnjson.h>

#include "identify_post.h"

#define IDENTIFY_KEY	"/identify"

bool identify_same_call(LSMessage *a, LSMessage *b)
{
	const char *ua, *ub;

	if (!a || !b)
		return false;
	if (a == b)
		return true;

	ua = LSMessageGetUniqueToken(a);
	ub = LSMessageGetUniqueToken(b);
	return ua && ub && strcmp(ua, ub) == 0;
}

/* The update as text, owned by the caller; NULL when it cannot be built */
static gchar *match_payload(const char *finger, const char *token, bool for_caller)
{
	jvalue_ref reply_obj = jobject_create();
	const char *text;
	gchar *payload;

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("finished"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("identified"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("finger"), jstring_create(finger ? finger : ""));
	if (!token)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("passcodeRequired"), jboolean_create(true));
	else if (for_caller)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("unlockToken"), jstring_create(token));

	text = jvalue_stringify(reply_obj);
	payload = text ? g_strdup(text) : NULL;
	j_release(&reply_obj);
	return payload;
}

static void wipe_free(gchar *text)
{
	if (text) {
		explicit_bzero(text, strlen(text));
		g_free(text);
	}
}

void identify_post_match(LSHandle *handle, LSMessage *caller, const char *finger,
                         const char *token)
{
	gchar *for_caller = match_payload(finger, token, true);
	gchar *for_others = match_payload(finger, token, false);
	LSSubscriptionIter *iter = NULL;
	LSError error;

	LSErrorInit(&error);
	if (!for_caller || !for_others) {
		g_warning("Cannot build the identify update");
	} else if (!LSSubscriptionAcquire(handle, IDENTIFY_KEY, &iter, &error)) {
		g_warning("Cannot reach the identify subscribers: %s", error.message);
		LSErrorFree(&error);
	} else {
		while (LSSubscriptionHasNext(iter)) {
			LSMessage *message = LSSubscriptionNext(iter);
			const char *payload = identify_same_call(message, caller) ? for_caller
			                                                         : for_others;

			if (!LSMessageRespond(message, payload, &error)) {
				g_warning("Cannot post the identify update: %s", error.message);
				LSErrorFree(&error);
			}
		}
		LSSubscriptionRelease(iter);
	}

	wipe_free(for_caller);
	g_free(for_others);
}

// vim:ts=4:sw=4:noexpandtab
