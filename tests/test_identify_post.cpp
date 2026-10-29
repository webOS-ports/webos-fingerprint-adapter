// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
//
// A matched finger's unlock token goes on the subscription of the call that
// asked to identify, and on no other: the identify updates are broadcast, and
// any other subscriber (an app with fingerprint.identify, a second shell
// component) must learn only that a finger matched.

#include <gtest/gtest.h>
#include <pbnjson.h>

#include "fake_ls2.h"
#include "identify_post.h"

namespace
{
const char kToken[] = "0123456789abcdef0123456789abcdef";

LSHandle *handle()
{
    return reinterpret_cast<LSHandle *>(0x1);
}

// The one update a subscriber got, parsed
jvalue_ref only(const LSMessage *m)
{
    EXPECT_EQ(1u, m->sent.size());
    if (m->sent.size() != 1)
        return jinvalid();
    JSchemaInfo info;
    jschema_info_init(&info, jschema_all(), nullptr, nullptr);
    jvalue_ref parsed = jdom_parse(j_cstr_to_buffer(m->sent[0].c_str()), DOMOPT_NOOPT, &info);
    EXPECT_TRUE(jis_object(parsed)) << m->sent[0];
    return parsed;
}

bool has(jvalue_ref obj, const char *key)
{
    return jobject_containskey(obj, j_cstr_to_buffer(key));
}

std::string text(jvalue_ref obj, const char *key)
{
    jvalue_ref v = jobject_get(obj, j_cstr_to_buffer(key));
    raw_buffer b = jstring_get_fast(v);
    return b.m_str ? std::string(b.m_str, b.m_len) : std::string();
}

bool flag(jvalue_ref obj, const char *key)
{
    bool v = false;
    jvalue_ref j = jobject_get(obj, j_cstr_to_buffer(key));
    return jis_boolean(j) && jboolean_get(j, &v) == CONV_OK && v;
}

class IdentifyPost : public ::testing::Test
{
protected:
    void TearDown() override { fakels2::reset(); }
};
} // namespace

TEST_F(IdentifyPost, TheTokenGoesToTheCallerOnly)
{
    LSMessage *shell = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *app = fakels2::subscribe("/identify", "com.example.app:1.20", 3);
    LSMessage *other = fakels2::subscribe("/identify", "com.example.other:1.30", 7);

    identify_post_match(handle(), shell, "right-index", kToken);

    jvalue_ref s = only(shell);
    EXPECT_EQ(kToken, text(s, "unlockToken"));
    EXPECT_TRUE(flag(s, "identified"));
    EXPECT_FALSE(has(s, "passcodeRequired"));

    for (LSMessage *m : {app, other})
    {
        jvalue_ref o = only(m);
        const std::string sent = m->sent.empty() ? std::string() : m->sent[0];
        EXPECT_FALSE(has(o, "unlockToken")) << sent;
        EXPECT_EQ(std::string::npos, sent.find(kToken)) << sent;
        EXPECT_TRUE(flag(o, "identified"));
        EXPECT_EQ("right-index", text(o, "finger"));
        EXPECT_FALSE(has(o, "passcodeRequired"));
        j_release(&o);
    }
    j_release(&s);
}

TEST_F(IdentifyPost, AnotherCallOnTheSameConnectionGetsNoToken)
{
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *again = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 8);

    identify_post_match(handle(), caller, "f", kToken);

    jvalue_ref c = only(caller);
    jvalue_ref a = only(again);
    EXPECT_TRUE(has(c, "unlockToken"));
    EXPECT_FALSE(has(a, "unlockToken"));
    j_release(&c);
    j_release(&a);
}

TEST_F(IdentifyPost, TheCallerIsKnownByItsCallNotItsPointer)
{
    // The subscription list may hold its own LSMessage for the same call
    LSMessage *subscribed = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *held = fakels2::message("com.webos.surfacemanager:1.10", 7);

    identify_post_match(handle(), held, "f", kToken);

    jvalue_ref s = only(subscribed);
    EXPECT_EQ(kToken, text(s, "unlockToken"));
    EXPECT_TRUE(held->sent.empty());
    j_release(&s);
}

TEST_F(IdentifyPost, WithoutACallerNobodyGetsTheToken)
{
    LSMessage *a = fakels2::subscribe("/identify", "com.example.a:1.1", 1);
    LSMessage *b = fakels2::subscribe("/identify", "com.example.b:1.2", 1);

    identify_post_match(handle(), nullptr, "f", kToken);

    for (LSMessage *m : {a, b})
    {
        jvalue_ref o = only(m);
        EXPECT_FALSE(has(o, "unlockToken"));
        j_release(&o);
    }
}

TEST_F(IdentifyPost, WithoutATokenEveryoneIsToldThePasscodeIsNeeded)
{
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *other = fakels2::subscribe("/identify", "com.example.app:1.20", 3);

    identify_post_match(handle(), caller, "f", nullptr);

    for (LSMessage *m : {caller, other})
    {
        jvalue_ref o = only(m);
        EXPECT_TRUE(flag(o, "passcodeRequired"));
        EXPECT_FALSE(has(o, "unlockToken"));
        j_release(&o);
    }
}

TEST_F(IdentifyPost, OtherKeysAreNotTouched)
{
    LSMessage *status = fakels2::subscribe("/getStatus", "com.webos.surfacemanager:1.10", 7);
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 8);

    identify_post_match(handle(), caller, "f", kToken);

    EXPECT_TRUE(status->sent.empty());
    EXPECT_EQ(1u, caller->sent.size());
}

TEST_F(IdentifyPost, BusFailuresAreSurvived)
{
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);

    fakels2::failAcquire(true);
    identify_post_match(handle(), caller, "f", kToken);
    EXPECT_TRUE(caller->sent.empty());

    fakels2::failAcquire(false);
    fakels2::failRespond(true);
    identify_post_match(handle(), caller, "f", kToken);
    EXPECT_TRUE(caller->sent.empty());
}

TEST_F(IdentifyPost, SameCall)
{
    LSMessage *a = fakels2::message("x:1.1", 5);
    LSMessage *a2 = fakels2::message("x:1.1", 5);
    LSMessage *b = fakels2::message("x:1.1", 6);
    LSMessage *c = fakels2::message("y:1.2", 5);

    EXPECT_TRUE(identify_same_call(a, a));
    EXPECT_TRUE(identify_same_call(a, a2));
    EXPECT_FALSE(identify_same_call(a, b));
    EXPECT_FALSE(identify_same_call(a, c));
    EXPECT_FALSE(identify_same_call(a, nullptr));
    EXPECT_FALSE(identify_same_call(nullptr, a));
}
