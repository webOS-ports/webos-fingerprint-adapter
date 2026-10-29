// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
//
// Just enough of libluna-service2 for src/identify_post.c: subscriptions on
// one key, each a message from a sender connection with a call token, and
// what was sent on each.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <luna-service2/lunaservice.h>

struct LSMessage
{
    std::string sender;  // unique connection name, e.g. "com.example:1.42"
    unsigned long token; // the call's token on that connection
    std::string unique;  // LSMessageGetUniqueToken(): "<sender>.<token>"
    std::vector<std::string> sent;
};

namespace fakels2
{
// Subscribes a new call to key and returns it; the fake owns it.
LSMessage *subscribe(const std::string &key, const std::string &sender, unsigned long token);
// A message that is not subscribed to anything.
LSMessage *message(const std::string &sender, unsigned long token);
// Drops every subscription and message.
void reset();
// Makes LSSubscriptionAcquire / LSMessageRespond fail.
void failAcquire(bool fail);
void failRespond(bool fail);
}
