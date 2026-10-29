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

#ifndef IDENTIFY_POST_H_
#define IDENTIFY_POST_H_

#include <stdbool.h>
#include <luna-service2/lunaservice.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether a and b are the same call: the same sender connection and the
 * same message token (LSMessageGetUniqueToken). */
bool identify_same_call(LSMessage *a, LSMessage *b);

/*
 * Posts the identify update for a matched finger to every subscriber of
 * /identify. luna-authmanager bound the unlock token to the one that asked to
 * identify (caller), so that is the only subscription it is sent on; the
 * others learn that a finger matched, and nothing they could unlock with.
 * With no token (luna-authmanager wants the passcode) everyone gets
 * "passcodeRequired": true.
 */
void identify_post_match(LSHandle *handle, LSMessage *caller, const char *finger,
                         const char *token);

#ifdef __cplusplus
}
#endif

#endif
