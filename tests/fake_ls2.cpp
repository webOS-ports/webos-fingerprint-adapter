// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

#include "fake_ls2.h"

#include <cstdlib>
#include <cstring>
#include <map>

struct LSSubscriptionIter
{
    std::vector<LSMessage *> messages;
    size_t next = 0;
};

namespace
{
std::vector<std::unique_ptr<LSMessage>> g_messages;
std::map<std::string, std::vector<LSMessage *>> g_subscriptions;
bool g_failAcquire = false;
bool g_failRespond = false;
char g_errorText[] = "fake failure";

LSMessage *make(const std::string &sender, unsigned long token)
{
    g_messages.push_back(std::unique_ptr<LSMessage>(
        new LSMessage{sender, token, sender + "." + std::to_string(token), {}}));
    return g_messages.back().get();
}

void setError(LSError *error)
{
    if (error)
    {
        error->error_code = -1;
        error->message = g_errorText;
    }
}
} // namespace

namespace fakels2
{
LSMessage *subscribe(const std::string &key, const std::string &sender, unsigned long token)
{
    LSMessage *m = make(sender, token);
    g_subscriptions[key].push_back(m);
    return m;
}

LSMessage *message(const std::string &sender, unsigned long token)
{
    return make(sender, token);
}

void reset()
{
    g_subscriptions.clear();
    g_messages.clear();
    g_failAcquire = g_failRespond = false;
}

void failAcquire(bool fail) { g_failAcquire = fail; }
void failRespond(bool fail) { g_failRespond = fail; }
} // namespace fakels2

extern "C" {

bool LSErrorInit(LSError *error)
{
    if (error)
        std::memset(error, 0, sizeof(*error));
    return true;
}

void LSErrorFree(LSError *error)
{
    if (error)
        std::memset(error, 0, sizeof(*error));
}

const char *LSMessageGetUniqueToken(LSMessage *message)
{
    // libluna-service2 dereferences the message; so does any caller that
    // gets here with NULL.
    if (!message)
        std::abort();
    return message->unique.c_str();
}

bool LSSubscriptionAcquire(LSHandle *, const char *key, LSSubscriptionIter **iter, LSError *error)
{
    if (g_failAcquire)
    {
        setError(error);
        return false;
    }
    auto *it = new LSSubscriptionIter;
    auto found = g_subscriptions.find(key ? key : "");
    if (found != g_subscriptions.end())
        it->messages = found->second;
    *iter = it;
    return true;
}

bool LSSubscriptionHasNext(LSSubscriptionIter *iter)
{
    return iter->next < iter->messages.size();
}

LSMessage *LSSubscriptionNext(LSSubscriptionIter *iter)
{
    return iter->messages[iter->next++];
}

void LSSubscriptionRelease(LSSubscriptionIter *iter)
{
    delete iter;
}

bool LSMessageRespond(LSMessage *message, const char *payload, LSError *error)
{
    if (g_failRespond)
    {
        setError(error);
        return false;
    }
    message->sent.push_back(payload ? payload : "");
    return true;
}
}
